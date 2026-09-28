// rust 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

//! 分布式回调接入，本库不执行 Agent 对象。
mod local;

pub use local::LocalEvolution;

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    collections::HashSet,
    fmt,
    future::Future,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
    time::Duration,
};

#[derive(Debug, Clone)]
pub struct Error {
    pub code: String,
    pub status: u16,
}
impl Error {
    // 初始化 new 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    pub fn new(code: &str) -> Self {
        Self {
            code: code.into(),
            status: 0,
        }
    }
}
impl fmt::Display for Error {
    // 生成不含凭据的诊断表示，避免打印对象时泄露敏感配置。
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.code)
    }
}
impl std::error::Error for Error {}
// 解析受限响应，校验状态和结构后再交给调用方。
fn decode<T: for<'a> Deserialize<'a>>(value: Value) -> Result<T, Error> {
    serde_json::from_value(value).map_err(|_| Error::new("response_invalid"))
}

/// 在调用方运行时绑定现有异步接收函数；正常在线请求时不接管执行。
/// 提交确认不代表完成；每轮推进由正常运行结束打点解锁。
// 通过本模块的评测入口处理请求，执行与采集生命周期由对应契约约束。
pub async fn evaluate<F, Fut, E>(mut receive: F) -> Result<bool, Error>
where
    F: FnMut(Value) -> Fut,
    Fut: Future<Output = Result<(), E>>,
{
    if std::env::var("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY").as_deref() != Ok("1") {
        return Ok(false);
    }
    let client = Evolution::with_limits(
        &std::env::var("TRAJECTORY_EVOLUTION_TRIAL_URL").unwrap_or_default(),
        &std::env::var("TRAJECTORY_EVOLUTION_TRIAL_TOKEN").unwrap_or_default(),
        Duration::from_secs(5),
        0,
        2097152,
    )?;
    let consumer = uuid::Uuid::new_v4().to_string();
    let result: Result<bool, Error> = async {
        let mut index = 0usize;
        loop {
            let packet = client
                .invoke(
                    "evaluation.next",
                    json!({"consumer": consumer, "index": index}),
                )
                .await?;
            match packet["state"].as_str() {
                Some("done") => return Ok(true),
                Some("waiting") => tokio::time::sleep(Duration::from_millis(100)).await,
                Some("input") if packet["index"].as_u64() == Some(index as u64) => {
                    receive(packet["input"].clone())
                        .await
                        .map_err(|_| Error::new("evaluation_entry_failed"))?;
                    index += 1;
                }
                _ => return Err(Error::new("evaluation_entry_failed")),
            }
        }
    }
    .await;
    if result.is_err() {
        let _ = client
            .invoke("evaluation.fail", json!({"consumer": consumer}))
            .await;
        return Err(Error::new("evaluation_entry_failed"));
    }
    result
}

#[derive(Clone)]
pub struct Evolution {
    http: reqwest::Client,
    url: String,
    token: Arc<String>,
    retries: u8,
    max_bytes: usize,
    point_gaps: Arc<Mutex<(HashSet<String>, bool)>>,
}

/// 保留一次宿主调用的业务输出及前后两个打点回执。
#[derive(Clone, Debug)]
pub struct PointCallResult {
    pub output: Value,
    pub before: Value,
    pub after: Value,
    pub executed: bool,
}

/// 保存循环边界的决定和回执；引用式打点只使用前置守卫。
/// 引用式阶段为 guard，上下文式打点还需 after 提交。
#[derive(Clone, Debug)]
pub struct LoopDecision {
    pub allow: bool,
    pub before: Value,
    pub after: Option<Value>,
}

impl LoopDecision {
    // 构造或解析标准回执，使宿主可以区分建议、应用、降级和业务结果。
    pub fn receipt(&self) -> &Value {
        self.after.as_ref().unwrap_or(&self.before)
    }
}

/// 接入错误与宿主原始业务错误分开保存。
pub enum CallError<E> {
    Integration(Error),
    Host {
        source: E,
        before: Value,
        abort: Value,
    },
}

impl<E> fmt::Debug for CallError<E> {
    // 生成不含凭据的诊断表示，避免打印对象时泄露敏感配置。
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Integration(error) => error.fmt(f),
            Self::Host { .. } => write!(f, "host_call_failed"),
        }
    }
}

impl<E> CallError<E> {
    // 读取原始业务错误，避免用接入层失败覆盖业务语义。
    pub fn host_error(&self) -> Option<&E> {
        match self {
            Self::Host { source, .. } => Some(source),
            _ => None,
        }
    }

    // 构造或解析标准回执，使宿主可以区分建议、应用、降级和业务结果。
    pub fn receipt(&self) -> Option<&Value> {
        match self {
            Self::Host { abort, .. } => Some(abort),
            _ => None,
        }
    }
}

impl Evolution {
    // 初始化 new 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    pub fn new(url: &str, token: &str, timeout: Duration) -> Result<Self, Error> {
        Self::with_limits(url, token, timeout, 1, 2 * 1024 * 1024)
    }
    // 设置请求时间与大小限制，并验证参数范围。
    pub fn with_limits(
        url: &str,
        token: &str,
        timeout: Duration,
        retries: u8,
        max_bytes: usize,
    ) -> Result<Self, Error> {
        let trial = std::env::var("TRAJECTORY_EVOLUTION_TRIAL_URL")
            .ok()
            .zip(std::env::var("TRAJECTORY_EVOLUTION_TRIAL_TOKEN").ok());
        let (url, token) = trial
            .as_ref()
            .map(|(u, t)| (u.as_str(), t.as_str()))
            .unwrap_or((url, token));
        let base = reqwest::Url::parse(url).map_err(|_| Error::new("url_invalid"))?;
        let local =
            [Some("localhost"), Some("127.0.0.1"), Some("[::1]")].contains(&base.host_str());
        if !base.username().is_empty()
            || base.password().is_some()
            || base.query().is_some()
            || base.fragment().is_some()
            || base.path() != "/"
            || !(base.scheme() == "https" || (base.scheme() == "http" && local))
        {
            return Err(Error::new("https_or_loopback_url_required"));
        }
        if token.len() < 24 || token.contains(['\r', '\n']) {
            return Err(Error::new("token_invalid"));
        }
        if !(Duration::from_millis(100)..=Duration::from_secs(300)).contains(&timeout)
            || retries > 3
            || !(1024..=16 * 1024 * 1024).contains(&max_bytes)
        {
            return Err(Error::new("client_limits_invalid"));
        }
        let http = reqwest::Client::builder()
            .no_proxy()
            .redirect(reqwest::redirect::Policy::none())
            .timeout(timeout)
            .build()
            .map_err(|_| Error::new("transport_initialization_failed"))?;
        Ok(Self {
            http,
            url: format!("{}v1/invoke", base),
            token: Arc::new(token.into()),
            retries,
            max_bytes,
            point_gaps: Arc::new(Mutex::new((HashSet::new(), false))),
        })
    }
    // 执行一次组件协议调用，检查响应信封并保持请求身份一致。
    pub async fn invoke(&self, operation: &str, fields: Value) -> Result<Value, Error> {
        self.invoke_id(operation, fields, &uuid::Uuid::new_v4().to_string())
            .await
    }
    // 使用调用方稳定请求身份执行协议操作，支持不确定传输后的幂等恢复。
    pub async fn invoke_id(
        &self,
        operation: &str,
        mut fields: Value,
        request_id: &str,
    ) -> Result<Value, Error> {
        let object = fields
            .as_object_mut()
            .ok_or_else(|| Error::new("object_required"))?;
        object.insert("operation".into(), json!(operation));
        object.insert("request_id".into(), json!(request_id));
        let bytes = serde_json::to_vec(&fields).map_err(|_| Error::new("request_invalid"))?;
        if bytes.len() > 2 * 1024 * 1024 {
            return Err(Error::new("request_too_large"));
        }
        for attempt in 0..=self.retries {
            match self.send(&bytes, None).await {
                Err(error)
                    if attempt < self.retries && (error.status == 0 || error.status >= 500) =>
                {
                    continue
                }
                result => return result,
            }
        }
        Err(Error::new("observer_unavailable"))
    }
    // 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
    pub async fn point(&self, fields: Value) -> Value {
        self.point_with_timeout(fields, Duration::from_millis(20))
            .await
    }
    // 在调用方指定的总期限内提交打点，超时后保留原业务输入并标记降级。
    pub async fn point_with_timeout(&self, fields: Value, timeout: Duration) -> Value {
        let id = uuid::Uuid::new_v4().to_string();
        let mut packet = fields.clone();
        if let Some(run) = fields["ref"]["run"].as_str() {
            let gaps = self.point_gaps.lock().unwrap_or_else(|e| e.into_inner());
            if gaps.1 || gaps.0.contains(run) {
                packet["capture_gap"] = json!(true);
            }
        }
        if let Some(object) = packet.as_object_mut() {
            object.insert("operation".into(), json!("point"));
            object.insert("request_id".into(), json!(id));
            if (Duration::from_millis(1)..=Duration::from_secs(5)).contains(&timeout) {
                if let Ok(bytes) = serde_json::to_vec(&packet) {
                    if bytes.len() <= 2 * 1024 * 1024 {
                        if let Ok(result) = self.send(&bytes, Some(timeout)).await {
                            return self.point_observed(&fields, result);
                        }
                    }
                }
            }
        }
        let mut context = fields["context"].as_object().cloned().unwrap_or_default();
        let kind = fields["kind"]
            .as_str()
            .or_else(|| context.get("kind").and_then(Value::as_str))
            .unwrap_or("custom")
            .to_owned();
        context.insert("kind".into(), json!(kind));
        context.insert("degraded".into(), json!(true));
        context.insert("caused_by".into(), json!([]));
        context
            .entry("run_id".to_owned())
            .or_insert_with(|| json!(format!("unobserved-{id}")));
        self.point_observed(&fields, json!({"context": context, "input": fields["input"], "degraded": true, "capture_complete": false,
            "policy_applied": false, "decision": if kind == "loop" { "continue" } else { "allow" },
            "closed": false, "ended": false, "reason": "observer_unavailable"}))
    }

    /// 包装一次现有异步 JSON 输入输出操作。
    /// 业务闭包最多执行一次；策略拒绝后跳过执行。
    // 包装宿主业务调用：先查询策略，再最多执行一次原业务函数，最后记录结果。
    pub async fn call<F, Fut, E>(
        &self,
        mut fields: Value,
        execute: F,
    ) -> Result<PointCallResult, CallError<E>>
    where
        F: FnOnce(Value) -> Fut,
        Fut: Future<Output = Result<Value, E>>,
    {
        fields
            .as_object_mut()
            .ok_or_else(|| CallError::Integration(Error::new("object_required")))?
            .insert("phase".into(), json!("before"));
        let before = self.point(fields.clone()).await;
        let actual = before
            .get("input")
            .cloned()
            .or_else(|| fields.get("input").cloned())
            .unwrap_or(Value::Null);
        let mut terminal = if fields.get("ref").is_some() {
            json!({"ref": fields["ref"].clone()})
        } else {
            json!({"context": before["context"].clone()})
        };
        terminal["kind"] = fields
            .get("kind")
            .cloned()
            .unwrap_or_else(|| json!("custom"));
        if before.get("decision").and_then(Value::as_str) == Some("reject") {
            let reason = before
                .get("reason")
                .and_then(Value::as_str)
                .unwrap_or("policy_rejected");
            let output = json!({"status": "rejected", "reason": reason, "error": reason});
            terminal["phase"] = json!("after");
            terminal["executed"] = json!(false);
            terminal["actual_input"] = actual;
            terminal["output"] = output.clone();
            let after = self.point(terminal).await;
            return Ok(PointCallResult {
                output,
                before,
                after,
                executed: false,
            });
        }
        match execute(actual.clone()).await {
            Ok(output) => {
                terminal["phase"] = json!("after");
                terminal["executed"] = json!(true);
                terminal["actual_input"] = actual;
                terminal["output"] = output.clone();
                let after = self.point(terminal).await;
                Ok(PointCallResult {
                    output,
                    before,
                    after,
                    executed: true,
                })
            }
            Err(error) => {
                terminal["phase"] = json!("abort");
                terminal["status"] = json!("unknown");
                terminal["output"] = Value::Null;
                let abort = self.point(terminal).await;
                Err(CallError::Host {
                    source: error,
                    before,
                    abort,
                })
            }
        }
    }

    // 返回循环守卫决定和完整回执，区分仅守卫与前后提交两种协议。
    pub async fn check_loop(&self, mut fields: Value) -> Result<LoopDecision, Error> {
        let referenced = fields.get("ref").is_some();
        let object = fields
            .as_object_mut()
            .ok_or_else(|| Error::new("object_required"))?;
        object.insert("kind".into(), json!("loop"));
        object.insert(
            "phase".into(),
            json!(if referenced { "guard" } else { "before" }),
        );
        let before = self.point(fields).await;
        let allow = before.get("decision").and_then(Value::as_str) != Some("stop");
        if referenced {
            return Ok(LoopDecision {
                allow,
                before,
                after: None,
            });
        }
        let terminal = json!({"context": before["context"].clone(), "kind": "loop",
            "phase": "after", "decision": before.get("decision").cloned().unwrap_or_else(|| json!("continue"))});
        let after = self.point(terminal).await;
        Ok(LoopDecision {
            allow,
            before,
            after: Some(after),
        })
    }

    // 在原循环条件处取得并提交实际控制决定；返回是否允许继续，而不是另起一套循环。
    pub async fn allow_loop(&self, fields: Value) -> Result<bool, Error> {
        Ok(self.check_loop(fields).await?.allow)
    }

    // 显式报告采集缺口，防止不完整轨迹进入后续学习与评测。
    pub async fn capture_gap(&self, fields: Value) -> Result<Value, Error> {
        self.invoke("capture.gap", fields).await
    }

    // 通过现有接收入口取得隔离评测下一轮输入；上一轮尚未结束时不提前交付。
    pub async fn evaluation_next(&self, consumer: &str, index: usize) -> Result<Value, Error> {
        self.invoke(
            "evaluation.next",
            json!({"consumer": consumer, "index": index}),
        )
        .await
    }

    // 报告评测入口执行异常，保留固定错误类型，不发送业务异常正文。
    pub async fn evaluation_fail(&self, consumer: &str) -> Result<Value, Error> {
        self.invoke("evaluation.fail", json!({"consumer": consumer}))
            .await
    }

    // 更新本地观测完整性记录，运行关闭后清理对应跟踪项。
    fn point_observed(&self, fields: &Value, result: Value) -> Value {
        if let Some(run) = fields["ref"]["run"].as_str().filter(|r| r.len() <= 1024) {
            let mut gaps = self.point_gaps.lock().unwrap_or_else(|e| e.into_inner());
            if result["degraded"] == true {
                if gaps.0.len() < 1024 {
                    gaps.0.insert(run.to_owned());
                } else {
                    gaps.1 = true;
                }
            } else if result["closed"] == true || result["run_closed"] == true {
                gaps.0.remove(run);
            }
        }
        result
    }
    // 发送一份已构造的协议请求，保持请求身份、超时和响应上限。
    async fn send(&self, body: &[u8], timeout: Option<Duration>) -> Result<Value, Error> {
        let mut request = self
            .http
            .post(&self.url)
            .bearer_auth(self.token.as_str())
            .header("Content-Type", "application/json")
            .body(body.to_vec());
        if let Some(budget) = timeout {
            request = request.timeout(budget);
        }
        let mut response = request
            .send()
            .await
            .map_err(|_| Error::new("observer_unavailable"))?;
        let status = response.status().as_u16();
        if response.content_length().unwrap_or(0) > self.max_bytes as u64 {
            return Err(Error::new("response_too_large"));
        }
        let mut bytes = Vec::new();
        while let Some(chunk) = response
            .chunk()
            .await
            .map_err(|_| Error::new("response_incomplete"))?
        {
            if bytes.len() + chunk.len() > self.max_bytes {
                return Err(Error::new("response_too_large"));
            }
            bytes.extend_from_slice(&chunk);
        }
        let value: Value =
            serde_json::from_slice(&bytes).map_err(|_| Error::new("response_invalid"))?;
        if status != 200 || value["ok"] != true {
            let code = value["error"]
                .as_str()
                .filter(|s| s.len() <= 96 && s.bytes().all(|c| c.is_ascii_lowercase() || c == b'_'))
                .unwrap_or("rpc_failed");
            return Err(Error {
                code: code.into(),
                status,
            });
        }
        value
            .get("result")
            .cloned()
            .ok_or_else(|| Error::new("response_invalid"))
    }
    // 开始一个新的规范运行上下文，绑定应用、用户与会话身份。
    pub async fn start(
        &self,
        conversation_id: &str,
        input: Value,
        level: &str,
        capabilities: &[&str],
    ) -> Result<Run, Error> {
        let result = self
            .invoke(
                "task.begin",
                json!({"conversation_id": conversation_id, "input": input,
            "level": level, "capabilities": capabilities}),
            )
            .await?;
        Ok(self.resume(
            result["run_id"]
                .as_str()
                .ok_or_else(|| Error::new("response_invalid"))?,
        ))
    }
    // 根据既有运行句柄恢复上下文，适配异步和跨进程任务。
    pub fn resume(&self, run_id: &str) -> Run {
        Run {
            client: self.clone(),
            id: run_id.into(),
            incomplete: Arc::new(AtomicBool::new(false)),
        }
    }
}

#[derive(Clone)]
pub struct Run {
    client: Evolution,
    pub id: String,
    incomplete: Arc<AtomicBool>,
}
impl Run {
    // 执行一次组件协议调用，检查响应信封并保持请求身份一致。
    pub async fn invoke(&self, operation: &str, mut fields: Value) -> Result<Value, Error> {
        fields
            .as_object_mut()
            .ok_or_else(|| Error::new("object_required"))?
            .insert("run_id".into(), json!(self.id));
        self.client.invoke(operation, fields).await
    }
    // 开始当前步骤并取得规范上下文，后续观测和结束复用该身份。
    pub async fn step(
        &self,
        state: Value,
        execution_id: &str,
        actor_id: &str,
    ) -> Result<Step, Error> {
        self.step_after(state, execution_id, actor_id, &[]).await
    }
    // 记录步骤完成后的实际结果和因果关系，不把前置建议当作完成。
    pub async fn step_after(
        &self,
        state: Value,
        execution_id: &str,
        actor_id: &str,
        caused_by: &[String],
    ) -> Result<Step, Error> {
        let handle = decode(
            self.invoke(
                "step.begin",
                json!({"state": state, "execution_id": execution_id,
            "actor_id": actor_id, "caused_by": caused_by}),
            )
            .await?,
        )?;
        Ok(self.resume_step(handle))
    }
    // 根据可序列化上下文恢复步骤，适配队列和跨进程接力。
    pub fn resume_step(&self, handle: StepHandle) -> Step {
        Step {
            run: self.clone(),
            handle,
        }
    }
    // 报告当前上下文的采集缺口，使后续收尾知道记录不完整。
    pub async fn gap(&self, reason: &str) -> Result<Value, Error> {
        self.incomplete.store(true, Ordering::SeqCst);
        self.invoke("capture.gap", json!({"reason": reason})).await
    }
    // 记录真实观测结果并推进当前状态；未执行的建议不能作为观测输入。
    async fn observe(&self, operation: &str, fields: Value) -> Option<Value> {
        match self.invoke(operation, fields.clone()).await {
            Ok(value) => Some(value),
            Err(error) => {
                let privacy = ["sensitive_field_rejected", "credential_value_rejected"]
                    .contains(&error.code.as_str());
                let _ = self
                    .gap(if privacy {
                        "privacy_rejected"
                    } else {
                        "observer_unavailable"
                    })
                    .await;
                if operation == "model.observe" || operation == "action.observe" {
                    let mut fallback = json!({"step_id": fields["step_id"], "call_id": fields["call_id"],
                        "status": "unknown", "output": null, "error": {"code": "observation_unavailable", "retryability": "unknown"}});
                    if operation == "model.observe" {
                        fallback["request"] = json!({});
                        fallback["transport_allowed"] = json!(fields["transport_allowed"] == true);
                    } else {
                        fallback["executed"] = json!(fields["executed"] == true);
                    }
                    let _ = self.invoke(operation, fallback).await;
                }
                None
            }
        }
    }
    // 返回当前上下文的采集降级状态，保证关闭时不会把缺口遗漏。
    pub fn capture_degraded(&self) -> bool {
        self.incomplete.load(Ordering::SeqCst)
    }
    // 返回当前运行或作业状态，不触发额外执行或重新评测。
    pub async fn status(&self) -> Result<Value, Error> {
        self.invoke("task.status", json!({})).await
    }
    // 触发后台评测重试，复用原运行证据和幂等身份。
    pub async fn retry_evaluation(&self) -> Result<Value, Error> {
        self.invoke("evaluation.retry", json!({})).await
    }
    // 提交实际完成状态；未确认的异步任务不能因为派发函数返回就结束。
    pub async fn finish(
        &self,
        output: Value,
        status: &str,
        business_status: &str,
    ) -> Result<Value, Error> {
        self.invoke(
            if self.capture_degraded() {
                "task.abort"
            } else {
                "task.end"
            },
            json!({"output": output,
            "status": if self.capture_degraded() { "unknown" } else { status },
            "business_status": business_status, "capture_complete": !self.capture_degraded()}),
        )
        .await
    }
    // 以明确中止状态收尾，保留采集完整性和业务异常的区别。
    pub async fn abort(&self, output: Value, status: &str) -> Result<Value, Error> {
        self.invoke("task.abort", json!({"output": output, "status": status}))
            .await
    }
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct StepHandle {
    pub step_id: String,
    pub decision: String,
    pub event_id: String,
}
#[derive(Serialize, Deserialize, Clone)]
pub struct ModelHandle {
    pub call_id: String,
    pub request: Value,
    pub event_id: String,
}
#[derive(Serialize, Deserialize, Clone)]
pub struct Action {
    pub id: String,
    pub operation: String,
    pub arguments: Value,
}
#[derive(Serialize, Deserialize, Clone)]
pub struct ActionHandle {
    pub id: String,
    pub call_id: String,
    pub decision: String,
    pub reason: String,
    pub event_id: String,
}
#[derive(Clone)]
pub struct Step {
    pub run: Run,
    pub handle: StepHandle,
}
pub struct PlannedAction {
    pub step: Step,
    pub handle: ActionHandle,
    pub action: Action,
}
#[derive(Serialize, Deserialize, Debug)]
pub struct ActionResult {
    pub id: String,
    pub status: String,
    pub output: Value,
    pub event_id: Option<String>,
}
pub enum ModelError<E> {
    Integration(Error),
    Host(E),
}
impl<E> fmt::Debug for ModelError<E> {
    // 生成不含凭据的诊断表示，避免打印对象时泄露敏感配置。
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Integration(error) => error.fmt(f),
            Self::Host(_) => write!(f, "host_model_failed"),
        }
    }
}
impl Step {
    // 在模型调用前完成策略查询和请求绑定，返回实际应发送的请求。
    pub async fn prepare_model(&self, request: Value) -> Result<ModelHandle, Error> {
        decode(
            self.run
                .invoke(
                    "model.prepare",
                    json!({"step_id": self.handle.step_id, "request": request}),
                )
                .await?,
        )
    }
    // 将实际模型响应关联到准备阶段的身份，并保留真实用量。
    pub async fn observe_model(
        &self,
        handle: &ModelHandle,
        actual_request: Value,
        output: Value,
        status: &str,
        transport_allowed: bool,
    ) -> Option<Value> {
        self.run.observe("model.observe", json!({"step_id": self.handle.step_id, "call_id": handle.call_id,
            "request": actual_request, "output": output, "status": status, "transport_allowed": transport_allowed})).await
    }
    // 包装一次真实模型调用，先准备注入请求，再记录原始调用结果。
    pub async fn model<F, Fut, E>(&self, request: Value, send: F) -> Result<Value, ModelError<E>>
    where
        F: FnOnce(Value) -> Fut,
        Fut: Future<Output = Result<Value, E>>,
    {
        let handle = self
            .prepare_model(request)
            .await
            .map_err(ModelError::Integration)?;
        match send(handle.request.clone()).await {
            Ok(output) => {
                self.observe_model(
                    &handle,
                    handle.request.clone(),
                    output.clone(),
                    "completed",
                    true,
                )
                .await;
                Ok(output)
            }
            Err(error) => {
                self.observe_model(&handle, handle.request.clone(), Value::Null, "failed", true)
                    .await;
                Err(ModelError::Host(error))
            }
        }
    }
    // 按宿主动作接口执行或派发已允许的动作，并记录真实结果。
    pub async fn actions(&self, actions: Vec<Action>) -> Result<Vec<PlannedAction>, Error> {
        if actions.is_empty() {
            return Ok(Vec::new());
        }
        let result = self
            .run
            .invoke(
                "actions.prepare",
                json!({"step_id": self.handle.step_id, "actions": actions}),
            )
            .await?;
        let handles: Vec<ActionHandle> = decode(result["actions"].clone())?;
        let count = actions.len();
        let mut originals: std::collections::HashMap<_, _> =
            actions.into_iter().map(|a| (a.id.clone(), a)).collect();
        if handles.len() != count || originals.len() != count {
            return Err(Error::new("action_plan_invalid"));
        }
        handles
            .into_iter()
            .map(|handle| {
                let action = originals
                    .remove(&handle.id)
                    .ok_or_else(|| Error::new("action_plan_invalid"))?;
                Ok(PlannedAction {
                    step: self.clone(),
                    handle,
                    action,
                })
            })
            .collect()
    }
    // 提交当前业务状态快照，为回放和收敛判定提供依据。
    pub async fn record_state(&self, before: Value, after: Value) -> Result<Value, Error> {
        self.run
            .invoke(
                "state.record",
                json!({"step_id": self.handle.step_id, "before": before, "after": after}),
            )
            .await
    }
    // 关闭当前步骤或运行，保留实际终态及未完成分支检查。
    pub async fn end(&self, output: Value, status: &str) -> Result<Value, Error> {
        self.run
            .invoke(
                "step.end",
                json!({"step_id": self.handle.step_id, "output": output,
            "status": status, "decision": self.handle.decision}),
            )
            .await
    }
}
impl PlannedAction {
    // 记录真实观测结果并推进当前状态；未执行的建议不能作为观测输入。
    pub async fn observe(&self, output: Value, status: &str, executed: bool) -> Option<Value> {
        self.step
            .run
            .observe(
                "action.observe",
                json!({"step_id": self.step.handle.step_id, "call_id": self.handle.call_id,
            "output": output, "status": status, "executed": executed}),
            )
            .await
    }
    // 调用宿主提供的真实执行入口，返回和异常保持原业务语义。
    pub async fn execute<F, Fut, E>(self, send: F) -> ActionResult
    where
        F: FnOnce(Action) -> Fut,
        Fut: Future<Output = Result<Value, E>>,
    {
        let executed = self.handle.decision == "allow";
        let (status, output) = if !executed {
            (
                "cancelled",
                json!({"error": "policy_rejected", "reason": self.handle.reason}),
            )
        } else {
            match send(self.action.clone()).await {
                Ok(value) => ("completed", value),
                Err(_) => ("failed", json!({"error": "action_failed"})),
            }
        };
        let receipt = self.observe(output.clone(), status, executed).await;
        ActionResult {
            id: self.action.id,
            status: status.into(),
            output,
            event_id: receipt.and_then(|v| v["event_id"].as_str().map(str::to_string)),
        }
    }
}
