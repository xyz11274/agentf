use crate::{CallError, Error, LoopDecision, PointCallResult};
use serde_json::{json, Map, Value};
use std::collections::HashSet;
use std::ffi::{c_char, c_int, c_void, CStr, CString, OsStr};
use std::path::Path;
use std::ptr;
use std::sync::Mutex;

type OpenFn = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    *mut *mut c_char,
) -> *mut c_void;
type InvokeFn = unsafe extern "C" fn(*mut c_void, *const c_char, usize) -> *mut c_char;
type MetricsFn = unsafe extern "C" fn(*mut c_void) -> *mut c_char;
type EnabledFn = unsafe extern "C" fn(*mut c_void, c_int);
type HandleFn = unsafe extern "C" fn(*mut c_void);
type FreeFn = unsafe extern "C" fn(*mut c_char);

struct NativeApi {
    _library: platform::Library,
    open: OpenFn,
    invoke: InvokeFn,
    manage: InvokeFn,
    metrics: MetricsFn,
    set_enabled: EnabledFn,
    drain: HandleFn,
    close: HandleFn,
    free: FreeFn,
}

impl NativeApi {
    fn load(path: &Path) -> Result<Self, Error> {
        let library = platform::Library::open(path)?;
        unsafe {
            Ok(Self {
                open: library.symbol(b"te_local_open\0")?,
                invoke: library.symbol(b"te_local_invoke\0")?,
                manage: library.symbol(b"te_local_manage\0")?,
                metrics: library.symbol(b"te_local_metrics\0")?,
                set_enabled: library.symbol(b"te_local_set_enabled\0")?,
                drain: library.symbol(b"te_local_drain\0")?,
                close: library.symbol(b"te_local_close\0")?,
                free: library.symbol(b"te_local_free\0")?,
                _library: library,
            })
        }
    }
}

/// In-process binding for `libtrajectory_evolution_local`.
///
/// Construct this once at application startup. Agent boundaries never use
/// HTTP or spawn a subprocess; the stable C ABI exchanges bounded UTF-8 JSON.
pub struct LocalEvolution {
    api: NativeApi,
    handle: *mut c_void,
    process_id: u32,
    max_request_bytes: usize,
    point_gaps: Mutex<(HashSet<String>, bool)>,
}

// LocalRuntime's C ABI is concurrent. The handle is closed only after Rust has
// exclusive ownership in Drop, and the loaded function table is immutable.
unsafe impl Send for LocalEvolution {}
unsafe impl Sync for LocalEvolution {}

impl LocalEvolution {
    pub fn new(
        library: impl AsRef<Path>,
        config: impl AsRef<Path>,
        token: &str,
        options: Value,
    ) -> Result<Self, Error> {
        let options = options
            .as_object()
            .ok_or_else(|| Error::new("object_required"))?;
        let max_request_bytes = options
            .get("max_request_bytes")
            .and_then(Value::as_u64)
            .unwrap_or(65536);
        if max_request_bytes == 0 || max_request_bytes > usize::MAX as u64 {
            return Err(Error::new("client_limits_invalid"));
        }
        let api = NativeApi::load(library.as_ref())?;
        let config = path_cstring(config.as_ref())?;
        let token = CString::new(token).map_err(|_| Error::new("token_invalid"))?;
        let options =
            CString::new(serde_json::to_vec(options).map_err(|_| Error::new("request_invalid"))?)
                .map_err(|_| Error::new("request_invalid"))?;
        let mut native_error = ptr::null_mut();
        let handle = unsafe {
            (api.open)(
                config.as_ptr(),
                token.as_ptr(),
                options.as_ptr(),
                &mut native_error,
            )
        };
        if !native_error.is_null() {
            unsafe { (api.free)(native_error) };
        }
        if handle.is_null() {
            return Err(Error::new("local_initialization_failed"));
        }
        Ok(Self {
            api,
            handle,
            process_id: std::process::id(),
            max_request_bytes: max_request_bytes as usize,
            point_gaps: Mutex::new((HashSet::new(), false)),
        })
    }

    pub fn invoke(&self, operation: &str, fields: Value) -> Value {
        self.invoke_id(operation, fields, &uuid::Uuid::new_v4().to_string())
    }

    pub fn invoke_id(&self, operation: &str, fields: Value, request_id: &str) -> Value {
        let fallback = fallback(operation, &fields, request_id);
        match self.invoke_checked(self.api.invoke, operation, &fields, request_id) {
            Ok(result) if result.get("degraded").and_then(Value::as_bool) == Some(true) => {
                merge_fallback(fallback, result)
            }
            Ok(result) => result,
            Err(_) => {
                self.report_gap(operation, &fields);
                fallback
            }
        }
    }

    /// Blocking administration entry point. Only `agent_evaluation.*`
    /// operations belong here; keep this outside Agent boundaries.
    pub fn manage(&self, operation: &str, fields: Value) -> Result<Value, Error> {
        self.invoke_checked(
            self.api.manage,
            operation,
            &fields,
            &uuid::Uuid::new_v4().to_string(),
        )
    }

    pub fn point(&self, fields: Value) -> Value {
        let mut request = fields.clone();
        if let Some(object) = request.as_object_mut() {
            if let Some(run) = point_run(&fields) {
                let gaps = self.point_gaps.lock().unwrap_or_else(|e| e.into_inner());
                if gaps.1 || gaps.0.contains(run) {
                    object.insert("capture_gap".into(), json!(true));
                }
            }
        }
        let result = self.invoke("point", request);
        self.point_observed(&fields, result)
    }

    /// Wrap one existing JSON-in/JSON-out operation. The business closure is
    /// executed at most once and is skipped when policy rejects the boundary.
    pub fn call<F, E>(&self, mut fields: Value, execute: F) -> Result<PointCallResult, CallError<E>>
    where
        F: FnOnce(Value) -> Result<Value, E>,
    {
        fields
            .as_object_mut()
            .ok_or_else(|| CallError::Integration(Error::new("object_required")))?
            .insert("phase".into(), json!("before"));
        let before = self.point(fields.clone());
        let actual = before
            .get("input")
            .cloned()
            .or_else(|| fields.get("input").cloned())
            .unwrap_or(Value::Null);
        let mut terminal = finish_identity(&fields, &before);
        terminal.insert(
            "kind".into(),
            fields
                .get("kind")
                .cloned()
                .unwrap_or_else(|| json!("custom")),
        );
        if before.get("decision").and_then(Value::as_str) == Some("reject") {
            let reason = before
                .get("reason")
                .and_then(Value::as_str)
                .unwrap_or("policy_rejected");
            let output = json!({"status": "rejected", "reason": reason, "error": reason});
            terminal.insert("phase".into(), json!("after"));
            terminal.insert("executed".into(), json!(false));
            terminal.insert("actual_input".into(), actual);
            terminal.insert("output".into(), output.clone());
            let after = self.point(Value::Object(terminal));
            return Ok(PointCallResult {
                output,
                before,
                after,
                executed: false,
            });
        }
        match execute(actual.clone()) {
            Ok(output) => {
                terminal.insert("phase".into(), json!("after"));
                terminal.insert("executed".into(), json!(true));
                terminal.insert("actual_input".into(), actual);
                terminal.insert("output".into(), output.clone());
                let after = self.point(Value::Object(terminal));
                Ok(PointCallResult {
                    output,
                    before,
                    after,
                    executed: true,
                })
            }
            Err(source) => {
                terminal.insert("phase".into(), json!("abort"));
                terminal.insert("status".into(), json!("unknown"));
                terminal.insert("output".into(), Value::Null);
                let abort = self.point(Value::Object(terminal));
                Err(CallError::Host {
                    source,
                    before,
                    abort,
                })
            }
        }
    }

    pub fn check_loop(&self, mut fields: Value) -> Result<LoopDecision, Error> {
        let referenced = fields.get("ref").is_some();
        let object = fields
            .as_object_mut()
            .ok_or_else(|| Error::new("object_required"))?;
        object.insert("kind".into(), json!("loop"));
        object.insert(
            "phase".into(),
            json!(if referenced { "guard" } else { "before" }),
        );
        let before = self.point(fields.clone());
        let allow = before.get("decision").and_then(Value::as_str) != Some("stop");
        if referenced {
            return Ok(LoopDecision {
                allow,
                before,
                after: None,
            });
        }
        let mut terminal = finish_identity(&fields, &before);
        terminal.insert("kind".into(), json!("loop"));
        terminal.insert("phase".into(), json!("after"));
        terminal.insert(
            "decision".into(),
            before
                .get("decision")
                .cloned()
                .unwrap_or_else(|| json!("continue")),
        );
        let after = self.point(Value::Object(terminal));
        Ok(LoopDecision {
            allow,
            before,
            after: Some(after),
        })
    }

    pub fn allow_loop(&self, fields: Value) -> Result<bool, Error> {
        Ok(self.check_loop(fields)?.allow)
    }

    pub fn capture_gap(&self, mut fields: Value, reason: &str) -> Result<Value, Error> {
        if fields.get("ref").is_some() {
            let object = fields
                .as_object_mut()
                .ok_or_else(|| Error::new("object_required"))?;
            object.insert("kind".into(), json!("run"));
            object.insert("phase".into(), json!("gap"));
            return Ok(self.point(fields));
        }
        if reason.is_empty() {
            return Err(Error::new("reason_required"));
        }
        fields
            .as_object_mut()
            .ok_or_else(|| Error::new("object_required"))?
            .insert("reason".into(), json!(reason));
        Ok(self.invoke("capture.gap", fields))
    }

    pub fn metrics(&self) -> Value {
        if !self.available() {
            return json!({});
        }
        let raw = unsafe { (self.api.metrics)(self.handle) };
        self.decode_owned(raw).unwrap_or_else(|_| json!({}))
    }

    pub fn set_enabled(&self, enabled: bool) {
        if self.available() {
            unsafe { (self.api.set_enabled)(self.handle, i32::from(enabled)) };
        }
    }

    /// Maintenance/shutdown only; this may wait for persistence.
    pub fn drain(&self) {
        if self.available() {
            unsafe { (self.api.drain)(self.handle) };
        }
    }

    fn available(&self) -> bool {
        !self.handle.is_null() && self.process_id == std::process::id()
    }

    fn invoke_checked(
        &self,
        call: InvokeFn,
        operation: &str,
        fields: &Value,
        request_id: &str,
    ) -> Result<Value, Error> {
        if !self.available() {
            return Err(Error::new("local_unavailable"));
        }
        let mut packet = fields
            .as_object()
            .cloned()
            .ok_or_else(|| Error::new("object_required"))?;
        packet.insert("operation".into(), json!(operation));
        packet.insert("request_id".into(), json!(request_id));
        let bytes = serde_json::to_vec(&packet).map_err(|_| Error::new("request_invalid"))?;
        if bytes.len() > self.max_request_bytes {
            return Err(Error::new("local_request_too_large"));
        }
        let raw = unsafe { call(self.handle, bytes.as_ptr().cast(), bytes.len()) };
        let envelope = self.decode_owned(raw)?;
        if envelope.get("ok").and_then(Value::as_bool) != Some(true) {
            let code = envelope
                .get("error")
                .and_then(Value::as_str)
                .filter(|value| valid_error_code(value))
                .unwrap_or("local_unavailable");
            return Err(Error::new(code));
        }
        envelope
            .get("result")
            .cloned()
            .ok_or_else(|| Error::new("response_invalid"))
    }

    fn decode_owned(&self, raw: *mut c_char) -> Result<Value, Error> {
        if raw.is_null() {
            return Err(Error::new("local_unavailable"));
        }
        let result = unsafe { serde_json::from_slice(CStr::from_ptr(raw).to_bytes()) };
        unsafe { (self.api.free)(raw) };
        result.map_err(|_| Error::new("response_invalid"))
    }

    fn report_gap(&self, operation: &str, fields: &Value) {
        if !self.available() {
            return;
        }
        let referenced = operation == "point" && fields.get("ref").is_some();
        let gap = if referenced {
            json!({"phase": "gap", "kind": "run", "ref": {"run": fields["ref"]["run"]}})
        } else {
            let run = fields
                .get("run_id")
                .cloned()
                .or_else(|| fields.get("context").and_then(|v| v.get("run_id")).cloned())
                .unwrap_or(Value::Null);
            json!({"run_id": run, "reason": "observer_unavailable"})
        };
        let _ = self.invoke_checked(
            self.api.invoke,
            if referenced { "point" } else { "capture.gap" },
            &gap,
            &uuid::Uuid::new_v4().to_string(),
        );
    }

    fn point_observed(&self, fields: &Value, result: Value) -> Value {
        if let Some(run) = point_run(fields).filter(|run| run.len() <= 1024) {
            let mut gaps = self.point_gaps.lock().unwrap_or_else(|e| e.into_inner());
            if result.get("degraded").and_then(Value::as_bool) == Some(true) {
                if gaps.0.len() < 1024 {
                    gaps.0.insert(run.to_owned());
                } else {
                    gaps.1 = true;
                }
            } else if result.get("closed").and_then(Value::as_bool) == Some(true)
                || result.get("run_closed").and_then(Value::as_bool) == Some(true)
            {
                gaps.0.remove(run);
            }
        }
        result
    }
}

impl Drop for LocalEvolution {
    fn drop(&mut self) {
        let handle = self.handle;
        self.handle = ptr::null_mut();
        if !handle.is_null() && self.process_id == std::process::id() {
            unsafe { (self.api.close)(handle) };
        }
    }
}

fn finish_identity(fields: &Value, prepared: &Value) -> Map<String, Value> {
    let mut identity = Map::new();
    if let Some(reference) = fields.get("ref") {
        identity.insert("ref".into(), reference.clone());
    } else if let Some(context) = prepared.get("context") {
        identity.insert("context".into(), context.clone());
    }
    identity
}

fn point_run(fields: &Value) -> Option<&str> {
    fields.get("ref")?.get("run")?.as_str()
}

fn valid_error_code(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 96
        && value
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte == b'_')
}

fn merge_fallback(mut fallback: Value, result: Value) -> Value {
    if let (Some(base), Some(observed)) = (fallback.as_object_mut(), result.as_object()) {
        base.extend(observed.clone());
    }
    fallback
}

fn fallback(operation: &str, fields: &Value, request_id: &str) -> Value {
    let mut result = json!({
        "degraded": true,
        "reason": "local_unavailable",
        "capture_complete": false,
        "policy_applied": false,
        "input": fields.get("input").cloned().unwrap_or(Value::Null),
        "event_id": ""
    });
    if operation == "point" {
        let mut context = fields
            .get("context")
            .and_then(Value::as_object)
            .cloned()
            .unwrap_or_default();
        let kind = fields
            .get("kind")
            .and_then(Value::as_str)
            .or_else(|| context.get("kind").and_then(Value::as_str))
            .unwrap_or("custom")
            .to_owned();
        context.insert("kind".into(), json!(kind));
        context.insert("degraded".into(), json!(true));
        context.insert("caused_by".into(), json!([]));
        context
            .entry("run_id")
            .or_insert_with(|| json!(format!("unobserved-{request_id}")));
        result["context"] = Value::Object(context);
        result["decision"] = json!(if kind == "loop" { "continue" } else { "allow" });
        result["closed"] = json!(false);
        result["ended"] = json!(false);
    } else if operation == "run.begin" {
        result["run_id"] = json!(format!("unobserved-{request_id}"));
    } else if operation == "activity.begin" {
        let activity = format!("unobserved-{request_id}");
        result["activity_id"] = json!(activity);
        result["decision"] = json!("allow");
        result["context"] = json!({
            "run_id": fields.get("run_id").cloned().unwrap_or(Value::Null),
            "activity_id": activity,
            "degraded": true
        });
    } else if operation == "loop.check" {
        result["control_id"] = json!(format!("unobserved-{request_id}"));
        result["decision"] = json!("continue");
        result["context"] = json!("");
    } else if matches!(operation, "activity.end" | "activity.abort" | "loop.commit") {
        result["ended"] = json!(false);
    } else if matches!(operation, "run.end" | "run.abort") {
        result["closed"] = json!(false);
        result["evaluation_status"] = json!("quarantined");
    }
    result
}

fn path_cstring(path: &Path) -> Result<CString, Error> {
    platform::path_cstring(path.as_os_str())
}

#[cfg(unix)]
mod platform {
    use super::*;
    use std::os::unix::ffi::OsStrExt;

    #[cfg_attr(any(target_os = "linux", target_os = "android"), link(name = "dl"))]
    extern "C" {
        fn dlopen(filename: *const c_char, flags: c_int) -> *mut c_void;
        fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
        fn dlclose(handle: *mut c_void) -> c_int;
    }

    pub struct Library(*mut c_void);
    unsafe impl Send for Library {}
    unsafe impl Sync for Library {}

    impl Library {
        pub fn open(path: &Path) -> Result<Self, Error> {
            let path = CString::new(path.as_os_str().as_bytes())
                .map_err(|_| Error::new("local_library_path_invalid"))?;
            let handle = unsafe { dlopen(path.as_ptr(), 2) };
            if handle.is_null() {
                Err(Error::new("local_library_load_failed"))
            } else {
                Ok(Self(handle))
            }
        }

        pub unsafe fn symbol<T: Copy>(&self, name: &[u8]) -> Result<T, Error> {
            let value = dlsym(self.0, name.as_ptr().cast());
            if value.is_null() {
                return Err(Error::new("local_library_symbol_missing"));
            }
            Ok(std::mem::transmute_copy(&value))
        }
    }

    impl Drop for Library {
        fn drop(&mut self) {
            if !self.0.is_null() {
                unsafe { dlclose(self.0) };
            }
        }
    }

    pub fn path_cstring(path: &OsStr) -> Result<CString, Error> {
        CString::new(path.as_bytes()).map_err(|_| Error::new("local_configuration_invalid"))
    }
}

#[cfg(windows)]
mod platform {
    use super::*;
    use std::os::windows::ffi::OsStrExt;

    #[link(name = "kernel32")]
    extern "system" {
        fn LoadLibraryW(filename: *const u16) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, symbol: *const c_char) -> *mut c_void;
        fn FreeLibrary(module: *mut c_void) -> c_int;
    }

    pub struct Library(*mut c_void);
    unsafe impl Send for Library {}
    unsafe impl Sync for Library {}

    impl Library {
        pub fn open(path: &Path) -> Result<Self, Error> {
            let mut path: Vec<u16> = path.as_os_str().encode_wide().collect();
            if path.contains(&0) {
                return Err(Error::new("local_library_path_invalid"));
            }
            path.push(0);
            let handle = unsafe { LoadLibraryW(path.as_ptr()) };
            if handle.is_null() {
                Err(Error::new("local_library_load_failed"))
            } else {
                Ok(Self(handle))
            }
        }

        pub unsafe fn symbol<T: Copy>(&self, name: &[u8]) -> Result<T, Error> {
            let value = GetProcAddress(self.0, name.as_ptr().cast());
            if value.is_null() {
                return Err(Error::new("local_library_symbol_missing"));
            }
            Ok(std::mem::transmute_copy(&value))
        }
    }

    impl Drop for Library {
        fn drop(&mut self) {
            if !self.0.is_null() {
                unsafe { FreeLibrary(self.0) };
            }
        }
    }

    pub fn path_cstring(path: &OsStr) -> Result<CString, Error> {
        CString::new(path.to_string_lossy().as_bytes())
            .map_err(|_| Error::new("local_configuration_invalid"))
    }
}

#[cfg(not(any(unix, windows)))]
compile_error!("LocalEvolution requires a Unix or Windows dynamic loader");
