//! Distributed callback integration. This crate never executes an Agent object.
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{collections::HashSet, fmt, future::Future, sync::{Arc, Mutex, atomic::{AtomicBool, Ordering}}, time::Duration};

#[derive(Debug, Clone)]
pub struct Error { pub code: String, pub status: u16 }
impl Error { pub fn new(code: &str) -> Self { Self { code: code.into(), status: 0 } } }
impl fmt::Display for Error { fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { write!(f, "{}", self.code) } }
impl std::error::Error for Error {}
fn decode<T: for<'a> Deserialize<'a>>(value: Value) -> Result<T, Error> {
    serde_json::from_value(value).map_err(|_| Error::new("response_invalid"))
}

/// Bind an existing async receiver on the caller's runtime. Inactive online.
/// Acknowledgement is not completion; normal run-end points unlock each turn.
pub async fn evaluate<F, Fut, E>(mut receive: F) -> Result<bool, Error>
where F: FnMut(Value) -> Fut, Fut: Future<Output = Result<(), E>> {
    if std::env::var("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY").as_deref() != Ok("1") { return Ok(false); }
    let client = Evolution::with_limits(&std::env::var("TRAJECTORY_EVOLUTION_TRIAL_URL").unwrap_or_default(),
        &std::env::var("TRAJECTORY_EVOLUTION_TRIAL_TOKEN").unwrap_or_default(), Duration::from_secs(5), 0, 2097152)?;
    let consumer = uuid::Uuid::new_v4().to_string();
    let result: Result<bool, Error> = async {
        let mut index = 0usize;
        loop {
            let packet = client.invoke("evaluation.next", json!({"consumer": consumer, "index": index})).await?;
            match packet["state"].as_str() {
                Some("done") => return Ok(true),
                Some("waiting") => tokio::time::sleep(Duration::from_millis(100)).await,
                Some("input") if packet["index"].as_u64() == Some(index as u64) => {
                    receive(packet["input"].clone()).await.map_err(|_| Error::new("evaluation_entry_failed"))?;
                    index += 1;
                },
                _ => return Err(Error::new("evaluation_entry_failed")),
            }
        }
    }.await;
    if result.is_err() {
        let _ = client.invoke("evaluation.fail", json!({"consumer": consumer})).await;
        return Err(Error::new("evaluation_entry_failed"));
    }
    result
}

#[derive(Clone)]
pub struct Evolution {
    http: reqwest::Client, url: String, token: Arc<String>, retries: u8, max_bytes: usize,
    point_gaps: Arc<Mutex<(HashSet<String>, bool)>>,
}
impl Evolution {
    pub fn new(url: &str, token: &str, timeout: Duration) -> Result<Self, Error> {
        Self::with_limits(url, token, timeout, 1, 2 * 1024 * 1024)
    }
    pub fn with_limits(url: &str, token: &str, timeout: Duration, retries: u8, max_bytes: usize) -> Result<Self, Error> {
        let trial = std::env::var("TRAJECTORY_EVOLUTION_TRIAL_URL").ok().zip(std::env::var("TRAJECTORY_EVOLUTION_TRIAL_TOKEN").ok());
        let (url, token) = trial.as_ref().map(|(u, t)| (u.as_str(), t.as_str())).unwrap_or((url, token));
        let base = reqwest::Url::parse(url).map_err(|_| Error::new("url_invalid"))?;
        let local = [Some("localhost"), Some("127.0.0.1"), Some("[::1]")].contains(&base.host_str());
        if !base.username().is_empty() || base.password().is_some() || base.query().is_some() || base.fragment().is_some()
            || base.path() != "/" || !(base.scheme() == "https" || (base.scheme() == "http" && local)) {
            return Err(Error::new("https_or_loopback_url_required"));
        }
        if token.len() < 24 || token.contains(['\r', '\n']) { return Err(Error::new("token_invalid")); }
        if !(Duration::from_millis(100)..=Duration::from_secs(300)).contains(&timeout)
            || retries > 3 || !(1024..=16 * 1024 * 1024).contains(&max_bytes) { return Err(Error::new("client_limits_invalid")); }
        let http = reqwest::Client::builder().no_proxy().redirect(reqwest::redirect::Policy::none())
            .timeout(timeout).build().map_err(|_| Error::new("transport_initialization_failed"))?;
        Ok(Self { http, url: format!("{}v1/invoke", base), token: Arc::new(token.into()), retries, max_bytes,
            point_gaps: Arc::new(Mutex::new((HashSet::new(), false))) })
    }
    pub async fn invoke(&self, operation: &str, fields: Value) -> Result<Value, Error> {
        self.invoke_id(operation, fields, &uuid::Uuid::new_v4().to_string()).await
    }
    pub async fn invoke_id(&self, operation: &str, mut fields: Value, request_id: &str) -> Result<Value, Error> {
        let object = fields.as_object_mut().ok_or_else(|| Error::new("object_required"))?;
        object.insert("operation".into(), json!(operation)); object.insert("request_id".into(), json!(request_id));
        let bytes = serde_json::to_vec(&fields).map_err(|_| Error::new("request_invalid"))?;
        if bytes.len() > 2 * 1024 * 1024 { return Err(Error::new("request_too_large")); }
        for attempt in 0..=self.retries {
            match self.send(&bytes, None).await {
                Err(error) if attempt < self.retries && (error.status == 0 || error.status >= 500) => continue,
                result => return result,
            }
        }
        Err(Error::new("observer_unavailable"))
    }
    pub async fn point(&self, fields: Value) -> Value {
        self.point_with_timeout(fields, Duration::from_millis(20)).await
    }
    pub async fn point_with_timeout(&self, fields: Value, timeout: Duration) -> Value {
        let id = uuid::Uuid::new_v4().to_string();
        let mut packet = fields.clone();
        if let Some(run) = fields["ref"]["run"].as_str() {
            let gaps = self.point_gaps.lock().unwrap_or_else(|e| e.into_inner());
            if gaps.1 || gaps.0.contains(run) { packet["capture_gap"] = json!(true); }
        }
        if let Some(object) = packet.as_object_mut() {
            object.insert("operation".into(), json!("point")); object.insert("request_id".into(), json!(id));
            if (Duration::from_millis(1)..=Duration::from_secs(5)).contains(&timeout) {
                if let Ok(bytes) = serde_json::to_vec(&packet) {
                    if bytes.len() <= 2 * 1024 * 1024 {
                        if let Ok(result) = self.send(&bytes, Some(timeout)).await { return self.point_observed(&fields, result); }
                    }
                }
            }
        }
        let mut context = fields["context"].as_object().cloned().unwrap_or_default();
        let kind = fields["kind"].as_str().or_else(|| context.get("kind").and_then(Value::as_str)).unwrap_or("custom").to_owned();
        context.insert("kind".into(), json!(kind)); context.insert("degraded".into(), json!(true));
        context.insert("caused_by".into(), json!([])); context.entry("run_id".to_owned()).or_insert_with(|| json!(format!("unobserved-{id}")));
        self.point_observed(&fields, json!({"context": context, "input": fields["input"], "degraded": true, "capture_complete": false,
            "policy_applied": false, "decision": if kind == "loop" { "continue" } else { "allow" },
            "closed": false, "ended": false, "reason": "observer_unavailable"}))
    }
    fn point_observed(&self, fields: &Value, result: Value) -> Value {
        if let Some(run) = fields["ref"]["run"].as_str().filter(|r| r.len() <= 1024) {
            let mut gaps = self.point_gaps.lock().unwrap_or_else(|e| e.into_inner());
            if result["degraded"] == true {
                if gaps.0.len() < 1024 { gaps.0.insert(run.to_owned()); } else { gaps.1 = true; }
            } else if result["closed"] == true || result["run_closed"] == true { gaps.0.remove(run); }
        }
        result
    }
    async fn send(&self, body: &[u8], timeout: Option<Duration>) -> Result<Value, Error> {
        let mut request = self.http.post(&self.url).bearer_auth(self.token.as_str())
            .header("Content-Type", "application/json").body(body.to_vec());
        if let Some(budget) = timeout { request = request.timeout(budget); }
        let mut response = request.send().await
            .map_err(|_| Error::new("observer_unavailable"))?;
        let status = response.status().as_u16();
        if response.content_length().unwrap_or(0) > self.max_bytes as u64 { return Err(Error::new("response_too_large")); }
        let mut bytes = Vec::new();
        while let Some(chunk) = response.chunk().await.map_err(|_| Error::new("response_incomplete"))? {
            if bytes.len() + chunk.len() > self.max_bytes { return Err(Error::new("response_too_large")); }
            bytes.extend_from_slice(&chunk);
        }
        let value: Value = serde_json::from_slice(&bytes).map_err(|_| Error::new("response_invalid"))?;
        if status != 200 || value["ok"] != true {
            let code = value["error"].as_str().filter(|s| s.len() <= 96 && s.bytes().all(|c| c.is_ascii_lowercase() || c == b'_')).unwrap_or("rpc_failed");
            return Err(Error { code: code.into(), status });
        }
        value.get("result").cloned().ok_or_else(|| Error::new("response_invalid"))
    }
    pub async fn start(&self, conversation_id: &str, input: Value, level: &str, capabilities: &[&str]) -> Result<Run, Error> {
        let result = self.invoke("task.begin", json!({"conversation_id": conversation_id, "input": input,
            "level": level, "capabilities": capabilities})).await?;
        Ok(self.resume(result["run_id"].as_str().ok_or_else(|| Error::new("response_invalid"))?))
    }
    pub fn resume(&self, run_id: &str) -> Run { Run { client: self.clone(), id: run_id.into(), incomplete: Arc::new(AtomicBool::new(false)) } }
}

#[derive(Clone)]
pub struct Run { client: Evolution, pub id: String, incomplete: Arc<AtomicBool> }
impl Run {
    pub async fn invoke(&self, operation: &str, mut fields: Value) -> Result<Value, Error> {
        fields.as_object_mut().ok_or_else(|| Error::new("object_required"))?.insert("run_id".into(), json!(self.id));
        self.client.invoke(operation, fields).await
    }
    pub async fn step(&self, state: Value, execution_id: &str, actor_id: &str) -> Result<Step, Error> {
        self.step_after(state, execution_id, actor_id, &[]).await
    }
    pub async fn step_after(&self, state: Value, execution_id: &str, actor_id: &str, caused_by: &[String]) -> Result<Step, Error> {
        let handle = decode(self.invoke("step.begin", json!({"state": state, "execution_id": execution_id,
            "actor_id": actor_id, "caused_by": caused_by})).await?)?;
        Ok(self.resume_step(handle))
    }
    pub fn resume_step(&self, handle: StepHandle) -> Step { Step { run: self.clone(), handle } }
    pub async fn gap(&self, reason: &str) -> Result<Value, Error> {
        self.incomplete.store(true, Ordering::SeqCst); self.invoke("capture.gap", json!({"reason": reason})).await
    }
    async fn observe(&self, operation: &str, fields: Value) -> Option<Value> {
        match self.invoke(operation, fields.clone()).await {
            Ok(value) => Some(value),
            Err(error) => {
                let privacy = ["sensitive_field_rejected", "credential_value_rejected"].contains(&error.code.as_str());
                let _ = self.gap(if privacy { "privacy_rejected" } else { "observer_unavailable" }).await;
                if operation == "model.observe" || operation == "action.observe" {
                    let mut fallback = json!({"step_id": fields["step_id"], "call_id": fields["call_id"],
                        "status": "unknown", "output": null, "error": {"code": "observation_unavailable", "retryability": "unknown"}});
                    if operation == "model.observe" { fallback["request"] = json!({}); fallback["transport_allowed"] = json!(fields["transport_allowed"] == true); }
                    else { fallback["executed"] = json!(fields["executed"] == true); }
                    let _ = self.invoke(operation, fallback).await;
                }
                None
            }
        }
    }
    pub fn capture_degraded(&self) -> bool { self.incomplete.load(Ordering::SeqCst) }
    pub async fn status(&self) -> Result<Value, Error> { self.invoke("task.status", json!({})).await }
    pub async fn retry_evaluation(&self) -> Result<Value, Error> { self.invoke("evaluation.retry", json!({})).await }
    pub async fn finish(&self, output: Value, status: &str, business_status: &str) -> Result<Value, Error> {
        self.invoke(if self.capture_degraded() { "task.abort" } else { "task.end" }, json!({"output": output,
            "status": if self.capture_degraded() { "unknown" } else { status },
            "business_status": business_status, "capture_complete": !self.capture_degraded()})).await
    }
    pub async fn abort(&self, output: Value, status: &str) -> Result<Value, Error> {
        self.invoke("task.abort", json!({"output": output, "status": status})).await
    }
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct StepHandle { pub step_id: String, pub decision: String, pub event_id: String }
#[derive(Serialize, Deserialize, Clone)]
pub struct ModelHandle { pub call_id: String, pub request: Value, pub event_id: String }
#[derive(Serialize, Deserialize, Clone)]
pub struct Action { pub id: String, pub operation: String, pub arguments: Value }
#[derive(Serialize, Deserialize, Clone)]
pub struct ActionHandle { pub id: String, pub call_id: String, pub decision: String, pub reason: String, pub event_id: String }
#[derive(Clone)]
pub struct Step { pub run: Run, pub handle: StepHandle }
pub struct PlannedAction { pub step: Step, pub handle: ActionHandle, pub action: Action }
#[derive(Serialize, Deserialize, Debug)]
pub struct ActionResult { pub id: String, pub status: String, pub output: Value, pub event_id: Option<String> }
pub enum ModelError<E> { Integration(Error), Host(E) }
impl<E> fmt::Debug for ModelError<E> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self { Self::Integration(error) => error.fmt(f), Self::Host(_) => write!(f, "host_model_failed") }
    }
}
impl Step {
    pub async fn prepare_model(&self, request: Value) -> Result<ModelHandle, Error> {
        decode(self.run.invoke("model.prepare", json!({"step_id": self.handle.step_id, "request": request})).await?)
    }
    pub async fn observe_model(&self, handle: &ModelHandle, actual_request: Value, output: Value, status: &str, transport_allowed: bool) -> Option<Value> {
        self.run.observe("model.observe", json!({"step_id": self.handle.step_id, "call_id": handle.call_id,
            "request": actual_request, "output": output, "status": status, "transport_allowed": transport_allowed})).await
    }
    pub async fn model<F, Fut, E>(&self, request: Value, send: F) -> Result<Value, ModelError<E>>
    where F: FnOnce(Value) -> Fut, Fut: Future<Output = Result<Value, E>> {
        let handle = self.prepare_model(request).await.map_err(ModelError::Integration)?;
        match send(handle.request.clone()).await {
            Ok(output) => { self.observe_model(&handle, handle.request.clone(), output.clone(), "completed", true).await; Ok(output) }
            Err(error) => { self.observe_model(&handle, handle.request.clone(), Value::Null, "failed", true).await; Err(ModelError::Host(error)) }
        }
    }
    pub async fn actions(&self, actions: Vec<Action>) -> Result<Vec<PlannedAction>, Error> {
        if actions.is_empty() { return Ok(Vec::new()); }
        let result = self.run.invoke("actions.prepare", json!({"step_id": self.handle.step_id, "actions": actions})).await?;
        let handles: Vec<ActionHandle> = decode(result["actions"].clone())?;
        let count = actions.len();
        let mut originals: std::collections::HashMap<_, _> = actions.into_iter().map(|a| (a.id.clone(), a)).collect();
        if handles.len() != count || originals.len() != count { return Err(Error::new("action_plan_invalid")); }
        handles.into_iter().map(|handle| {
            let action = originals.remove(&handle.id).ok_or_else(|| Error::new("action_plan_invalid"))?;
            Ok(PlannedAction { step: self.clone(), handle, action })
        }).collect()
    }
    pub async fn record_state(&self, before: Value, after: Value) -> Result<Value, Error> {
        self.run.invoke("state.record", json!({"step_id": self.handle.step_id, "before": before, "after": after})).await
    }
    pub async fn end(&self, output: Value, status: &str) -> Result<Value, Error> {
        self.run.invoke("step.end", json!({"step_id": self.handle.step_id, "output": output,
            "status": status, "decision": self.handle.decision})).await
    }
}
impl PlannedAction {
    pub async fn observe(&self, output: Value, status: &str, executed: bool) -> Option<Value> {
        self.step.run.observe("action.observe", json!({"step_id": self.step.handle.step_id, "call_id": self.handle.call_id,
            "output": output, "status": status, "executed": executed})).await
    }
    pub async fn execute<F, Fut, E>(self, send: F) -> ActionResult
    where F: FnOnce(Action) -> Fut, Fut: Future<Output = Result<Value, E>> {
        let executed = self.handle.decision == "allow";
        let (status, output) = if !executed { ("cancelled", json!({"error": "policy_rejected", "reason": self.handle.reason})) }
            else { match send(self.action.clone()).await {
                Ok(value) => ("completed", value), Err(_) => ("failed", json!({"error": "action_failed"})),
            }};
        let receipt = self.observe(output.clone(), status, executed).await;
        ActionResult { id: self.action.id, status: status.into(), output,
            event_id: receipt.and_then(|v| v["event_id"].as_str().map(str::to_string)) }
    }
}
