use base64::Engine as _;
use chrono::{DateTime, SecondsFormat, Utc};
use serde::Deserialize;
use serde_json::{Value, json};
use std::ffi::{CStr, c_char, c_void};
use std::ptr;
use std::slice;
use std::sync::{Arc, Condvar, Mutex, OnceLock};
use std::thread::{self, JoinHandle};
use std::time::{Duration, SystemTime};

const ABI_VERSION: u32 = 1;
const SCHEMA_VERSION: u32 = 6;
const PLUGIN_ID: &str = "codex-auto-ping";
const PLUGIN_NAME: &str = "Codex Auto Ping";
const PLUGIN_VERSION: &str = env!("CARGO_PKG_VERSION");

const METHOD_PLUGIN_REGISTER: &str = "plugin.register";
const METHOD_PLUGIN_RECONFIGURE: &str = "plugin.reconfigure";
const METHOD_MANAGEMENT_REGISTER: &str = "management.register";
const METHOD_MANAGEMENT_HANDLE: &str = "management.handle";
const METHOD_HOST_MODEL_EXECUTE: &str = "host.model.execute";

#[repr(C)]
pub struct Buffer {
    ptr: *mut c_void,
    len: usize,
}

type HostCall = unsafe extern "C" fn(
    host_ctx: *mut c_void,
    method: *const c_char,
    request: *const u8,
    request_len: usize,
    response: *mut Buffer,
) -> i32;
type HostFree = unsafe extern "C" fn(ptr: *mut c_void, len: usize);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HostApi {
    abi_version: u32,
    host_ctx: *mut c_void,
    call: Option<HostCall>,
    free_buffer: Option<HostFree>,
}

// The host owns this table for the lifetime of the loaded library. CLIProxyAPI may
// invoke the plugin from several threads, so the copied function table is shared.
unsafe impl Send for HostApi {}
unsafe impl Sync for HostApi {}

type PluginCall = unsafe extern "C" fn(
    method: *const c_char,
    request: *const u8,
    request_len: usize,
    response: *mut Buffer,
) -> i32;
type PluginFree = unsafe extern "C" fn(ptr: *mut c_void, len: usize);
type PluginShutdown = unsafe extern "C" fn();

#[repr(C)]
pub struct PluginApi {
    abi_version: u32,
    call: Option<PluginCall>,
    free_buffer: Option<PluginFree>,
    shutdown: Option<PluginShutdown>,
}

#[derive(Clone)]
struct Config {
    interval: Duration,
    startup_delay: Duration,
    run_on_start: bool,
    model: String,
    prompt: String,
    max_output_tokens: u64,
    pings_per_cycle: u64,
    ping_spacing: Duration,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            interval: Duration::from_secs(5 * 60 * 60),
            startup_delay: Duration::from_secs(10),
            run_on_start: true,
            model: "gpt-5.6".into(),
            prompt: "1".into(),
            max_output_tokens: 1,
            pings_per_cycle: 1,
            ping_spacing: Duration::from_secs(3),
        }
    }
}

#[derive(Default, Deserialize)]
struct ConfigInput {
    interval: Option<String>,
    startup_delay: Option<String>,
    run_on_start: Option<bool>,
    model: Option<String>,
    prompt: Option<String>,
    max_output_tokens: Option<u64>,
    pings_per_cycle: Option<u64>,
    ping_spacing: Option<String>,
}

#[derive(Deserialize)]
struct LifecycleRequest {
    #[serde(default)]
    config_yaml: String,
}

#[derive(Default)]
struct State {
    cfg: Config,
    last_attempt: Option<SystemTime>,
    last_success: Option<SystemTime>,
    next_run: Option<SystemTime>,
    last_status: i64,
    last_error: String,
    total_attempts: u64,
    total_success: u64,
}

struct StopSignal {
    stopped: Mutex<bool>,
    wake: Condvar,
}

struct Scheduler {
    stop: Arc<StopSignal>,
    thread: JoinHandle<()>,
}

struct Runtime {
    state: Arc<Mutex<State>>,
    scheduler: Mutex<Option<Scheduler>>,
}

impl Runtime {
    fn new() -> Self {
        Self {
            state: Arc::new(Mutex::new(State::default())),
            scheduler: Mutex::new(None),
        }
    }

    fn start(&self, cfg: Config) {
        self.stop();
        {
            let mut state = lock(&self.state);
            state.cfg = cfg.clone();
            state.next_run = None;
        }

        let stop = Arc::new(StopSignal {
            stopped: Mutex::new(false),
            wake: Condvar::new(),
        });
        let thread_stop = Arc::clone(&stop);
        let state = Arc::clone(&self.state);
        let thread = thread::spawn(move || scheduler_loop(state, thread_stop, cfg));
        *lock(&self.scheduler) = Some(Scheduler { stop, thread });
    }

    fn stop(&self) {
        let scheduler = lock(&self.scheduler).take();
        if let Some(scheduler) = scheduler {
            *lock(&scheduler.stop.stopped) = true;
            scheduler.stop.wake.notify_all();
            let _ = scheduler.thread.join();
        }
    }
}

static HOST: OnceLock<HostApi> = OnceLock::new();
static RUNTIME: OnceLock<Runtime> = OnceLock::new();

fn runtime() -> &'static Runtime {
    RUNTIME.get_or_init(Runtime::new)
}

fn lock<T>(mutex: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

#[unsafe(no_mangle)]
/// Initialize the CLIProxyAPI plugin function table.
///
/// # Safety
/// `host` and `plugin` must point to valid ABI v1 tables owned by CLIProxyAPI.
pub unsafe extern "C" fn cliproxy_plugin_init(host: *const HostApi, plugin: *mut PluginApi) -> i32 {
    if host.is_null() || plugin.is_null() {
        return 1;
    }
    // SAFETY: both pointers are validated above and provided by CLIProxyAPI for
    // the duration of this initialization call.
    let host_api = unsafe { *host };
    if host_api.abi_version != ABI_VERSION
        || host_api.call.is_none()
        || host_api.free_buffer.is_none()
    {
        return 1;
    }
    if HOST.set(host_api).is_err() {
        return 1;
    }
    // SAFETY: plugin is non-null and points to the writable ABI table owned by the host.
    unsafe {
        (*plugin).abi_version = ABI_VERSION;
        (*plugin).call = Some(cliproxy_plugin_call);
        (*plugin).free_buffer = Some(cliproxy_plugin_free);
        (*plugin).shutdown = Some(cliproxy_plugin_shutdown);
    }
    0
}

#[unsafe(no_mangle)]
/// Dispatch one JSON RPC call from CLIProxyAPI.
///
/// # Safety
/// All non-null pointers must remain readable or writable for their declared
/// lengths until this synchronous call returns.
pub unsafe extern "C" fn cliproxy_plugin_call(
    method: *const c_char,
    request: *const u8,
    request_len: usize,
    response: *mut Buffer,
) -> i32 {
    if response.is_null() {
        return 1;
    }
    // SAFETY: response is checked non-null and is host-owned writable memory.
    unsafe {
        (*response).ptr = ptr::null_mut();
        (*response).len = 0;
    }
    if method.is_null() || (request.is_null() && request_len != 0) {
        write_response(
            response,
            error_envelope("invalid_request", "invalid method or request"),
        );
        return 1;
    }

    // SAFETY: the host guarantees NUL-terminated method strings and request_len
    // readable bytes for non-null request pointers.
    let method = unsafe { CStr::from_ptr(method) }.to_string_lossy();
    let request = if request_len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(request, request_len) }
    };

    match handle_method(&method, request) {
        Ok(value) => {
            write_response(response, value);
            0
        }
        Err(message) => {
            write_response(response, error_envelope("plugin_error", &message));
            1
        }
    }
}

#[unsafe(no_mangle)]
/// Release a response previously allocated by `cliproxy_plugin_call`.
///
/// # Safety
/// `ptr` and `len` must be an unchanged response allocation returned by this library.
pub unsafe extern "C" fn cliproxy_plugin_free(ptr: *mut c_void, len: usize) {
    if !ptr.is_null() && len > 0 {
        // SAFETY: responses are allocated as boxed [u8] by write_response and are
        // returned exactly once with their original length.
        unsafe {
            drop(Box::from_raw(ptr::slice_from_raw_parts_mut(
                ptr.cast::<u8>(),
                len,
            )));
        }
    }
}

#[unsafe(no_mangle)]
/// Stop and join the scheduler before CLIProxyAPI unloads the library.
///
/// # Safety
/// The host must not issue concurrent calls after shutdown begins.
pub unsafe extern "C" fn cliproxy_plugin_shutdown() {
    if let Some(runtime) = RUNTIME.get() {
        runtime.stop();
    }
}

fn handle_method(method: &str, request: &[u8]) -> Result<Vec<u8>, String> {
    match method {
        METHOD_PLUGIN_REGISTER | METHOD_PLUGIN_RECONFIGURE => {
            let cfg = parse_lifecycle_config(request)?;
            runtime().start(cfg);
            ok_envelope(json!({
                "schema_version": SCHEMA_VERSION,
                "metadata": {
                    "Name": PLUGIN_NAME,
                    "Version": PLUGIN_VERSION,
                    "Author": "KKKKeybird",
                    "GitHubRepository": "https://github.com/KKKKeybird/cpa-codex-auto-ping",
                    "Logo": "",
                    "ConfigFields": [
                        {"Name":"interval","Type":"string","Description":"Ping interval, e.g. 5h."},
                        {"Name":"startup_delay","Type":"string","Description":"Delay before the startup ping."},
                        {"Name":"run_on_start","Type":"boolean","Description":"Send one ping after startup_delay."},
                        {"Name":"model","Type":"string","Description":"Model name exposed by CLIProxyAPI."},
                        {"Name":"prompt","Type":"string","Description":"Tiny input used for the ping."},
                        {"Name":"max_output_tokens","Type":"integer","Description":"Maximum output tokens."},
                        {"Name":"pings_per_cycle","Type":"integer","Description":"Number of tiny requests sent per cycle."},
                        {"Name":"ping_spacing","Type":"string","Description":"Delay between requests in a cycle."}
                    ]
                },
                "capabilities": {"management_api": true}
            }))
        }
        METHOD_MANAGEMENT_REGISTER => ok_envelope(json!({
            "routes": [{
                "Method": "GET",
                "Path": "/plugins/codex-auto-ping/status",
                "Description": "Show scheduler state and recent ping result."
            }]
        })),
        METHOD_MANAGEMENT_HANDLE => ok_envelope(status_response()),
        _ => Ok(error_envelope(
            "unknown_method",
            &format!("unknown method: {method}"),
        )),
    }
}

fn parse_lifecycle_config(raw: &[u8]) -> Result<Config, String> {
    let mut cfg = Config::default();
    if raw.iter().all(u8::is_ascii_whitespace) {
        return Ok(cfg);
    }
    let lifecycle: LifecycleRequest = serde_json::from_slice(raw)
        .map_err(|error| format!("decode lifecycle request: {error}"))?;
    if lifecycle.config_yaml.trim().is_empty() {
        return Ok(cfg);
    }
    let yaml = base64::engine::general_purpose::STANDARD
        .decode(lifecycle.config_yaml)
        .map_err(|error| format!("decode config_yaml: {error}"))?;
    if yaml.iter().all(u8::is_ascii_whitespace) {
        return Ok(cfg);
    }
    let input: ConfigInput =
        serde_yaml::from_slice(&yaml).map_err(|error| format!("decode plugin config: {error}"))?;

    if let Some(value) = nonempty(input.interval) {
        cfg.interval = parse_duration(&value).ok_or("invalid interval")?;
        if cfg.interval.is_zero() {
            return Err("invalid interval".into());
        }
    }
    if let Some(value) = nonempty(input.startup_delay) {
        cfg.startup_delay = parse_duration(&value).ok_or("invalid startup_delay")?;
    }
    if let Some(value) = input.run_on_start {
        cfg.run_on_start = value;
    }
    if let Some(value) = nonempty(input.model) {
        cfg.model = value;
    }
    if let Some(value) = input.prompt
        && !value.is_empty()
    {
        cfg.prompt = value;
    }
    if let Some(value) = input.max_output_tokens.filter(|value| *value > 0) {
        cfg.max_output_tokens = value;
    }
    if let Some(value) = input.pings_per_cycle.filter(|value| *value > 0) {
        cfg.pings_per_cycle = value;
    }
    if let Some(value) = nonempty(input.ping_spacing) {
        cfg.ping_spacing = parse_duration(&value).ok_or("invalid ping_spacing")?;
    }
    Ok(cfg)
}

fn nonempty(value: Option<String>) -> Option<String> {
    value
        .map(|value| value.trim().to_owned())
        .filter(|value| !value.is_empty())
}

fn parse_duration(raw: &str) -> Option<Duration> {
    let raw = raw.trim();
    if raw.is_empty() || raw.starts_with('-') {
        return None;
    }
    let raw = raw.strip_prefix('+').unwrap_or(raw);
    let mut remaining = raw;
    let mut seconds = 0.0;
    while !remaining.is_empty() {
        let number_end = remaining.char_indices().find_map(|(index, character)| {
            (!character.is_ascii_digit() && character != '.').then_some(index)
        })?;
        if number_end == 0 {
            return None;
        }
        let amount: f64 = remaining[..number_end].parse().ok()?;
        if !amount.is_finite() || amount < 0.0 {
            return None;
        }
        remaining = &remaining[number_end..];
        let (unit, multiplier) = [
            ("ns", 1.0 / 1_000_000_000.0),
            ("us", 1.0 / 1_000_000.0),
            ("µs", 1.0 / 1_000_000.0),
            ("μs", 1.0 / 1_000_000.0),
            ("ms", 1.0 / 1_000.0),
            ("s", 1.0),
            ("m", 60.0),
            ("h", 3_600.0),
        ]
        .into_iter()
        .find(|(unit, _)| remaining.starts_with(unit))?;
        seconds += amount * multiplier;
        remaining = &remaining[unit.len()..];
    }
    Duration::try_from_secs_f64(seconds).ok()
}

fn scheduler_loop(state: Arc<Mutex<State>>, stop: Arc<StopSignal>, cfg: Config) {
    if cfg.run_on_start {
        if !wait_or_stop(&stop, cfg.startup_delay) {
            return;
        }
        run_cycle(&state, &stop, &cfg);
    }
    loop {
        lock(&state).next_run = SystemTime::now().checked_add(cfg.interval);
        if !wait_or_stop(&stop, cfg.interval) {
            return;
        }
        run_cycle(&state, &stop, &cfg);
    }
}

fn wait_or_stop(stop: &StopSignal, duration: Duration) -> bool {
    let stopped = lock(&stop.stopped);
    if *stopped {
        return false;
    }
    let (stopped, _) = stop
        .wake
        .wait_timeout_while(stopped, duration, |stopped| !*stopped)
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    !*stopped
}

fn run_cycle(state: &Arc<Mutex<State>>, stop: &StopSignal, cfg: &Config) {
    for index in 0..cfg.pings_per_cycle {
        run_ping(state, cfg);
        if index + 1 < cfg.pings_per_cycle && !wait_or_stop(stop, cfg.ping_spacing) {
            return;
        }
    }
}

fn run_ping(state: &Arc<Mutex<State>>, cfg: &Config) {
    {
        let mut state = lock(state);
        state.last_attempt = Some(SystemTime::now());
        state.total_attempts += 1;
    }
    let body = json!({
        "model": cfg.model,
        "input": cfg.prompt,
        "max_output_tokens": cfg.max_output_tokens
    });
    let body = match serde_json::to_vec(&body) {
        Ok(body) => body,
        Err(error) => {
            record_result(state, 0, Some(error.to_string()));
            return;
        }
    };
    let request = json!({
        "entry_protocol": "openai-response",
        "exit_protocol": "openai-response",
        "model": cfg.model,
        "stream": false,
        "body": base64::engine::general_purpose::STANDARD.encode(body),
        "headers": null,
        "query": null,
        "alt": ""
    });
    match call_host(METHOD_HOST_MODEL_EXECUTE, &request) {
        Ok(result) => {
            let status = result
                .get("status_code")
                .and_then(Value::as_i64)
                .unwrap_or(0);
            if (200..300).contains(&status) {
                record_result(state, status, None);
            } else {
                record_result(
                    state,
                    status,
                    Some(format!("model request returned status {status}")),
                );
            }
        }
        Err(error) => record_result(state, 0, Some(error)),
    }
}

fn call_host(method: &str, request: &Value) -> Result<Value, String> {
    let host = HOST.get().ok_or("host API is unavailable")?;
    let call = host.call.ok_or("host call function is unavailable")?;
    let free = host
        .free_buffer
        .ok_or("host free function is unavailable")?;
    let method = std::ffi::CString::new(method).map_err(|error| error.to_string())?;
    let request = serde_json::to_vec(request).map_err(|error| error.to_string())?;
    let mut response = Buffer {
        ptr: ptr::null_mut(),
        len: 0,
    };
    // SAFETY: all pointers are valid for the duration of the synchronous host callback.
    let code = unsafe {
        call(
            host.host_ctx,
            method.as_ptr(),
            request.as_ptr(),
            request.len(),
            &mut response,
        )
    };
    let bytes = if response.ptr.is_null() || response.len == 0 {
        Vec::new()
    } else {
        // SAFETY: the host returned response.len readable bytes.
        unsafe { slice::from_raw_parts(response.ptr.cast::<u8>(), response.len) }.to_vec()
    };
    if !response.ptr.is_null() {
        // SAFETY: response memory is released through the matching host callback.
        unsafe { free(response.ptr, response.len) };
    }
    if bytes.is_empty() {
        return Err(format!("host callback returned no response, code={code}"));
    }
    let envelope: Value = serde_json::from_slice(&bytes)
        .map_err(|error| format!("decode host callback envelope: {error}"))?;
    if envelope.get("ok").and_then(Value::as_bool) != Some(true) {
        let error = envelope.get("error").unwrap_or(&Value::Null);
        let error_code = error
            .get("code")
            .and_then(Value::as_str)
            .unwrap_or("host_error");
        let message = error
            .get("message")
            .and_then(Value::as_str)
            .unwrap_or("host callback failed");
        return Err(format!("{error_code}: {message}"));
    }
    if code != 0 {
        return Err(format!("host callback returned code={code}"));
    }
    Ok(envelope.get("result").cloned().unwrap_or(Value::Null))
}

fn record_result(state: &Arc<Mutex<State>>, status: i64, error: Option<String>) {
    let mut state = lock(state);
    state.last_status = status;
    if let Some(error) = error {
        state.last_error = error;
    } else {
        state.last_error.clear();
        state.last_success = Some(SystemTime::now());
        state.total_success += 1;
    }
}

fn status_response() -> Value {
    let state = lock(&runtime().state);
    let mut body = json!({
        "plugin": PLUGIN_ID,
        "version": PLUGIN_VERSION,
        "model": state.cfg.model,
        "interval": format_duration(state.cfg.interval),
        "pings_per_cycle": state.cfg.pings_per_cycle,
        "last_status": state.last_status,
        "last_error": state.last_error,
        "total_attempts": state.total_attempts,
        "total_success": state.total_success
    });
    for (name, value) in [
        ("last_attempt", state.last_attempt),
        ("last_success", state.last_success),
        ("next_run", state.next_run),
    ] {
        if let Some(value) = value {
            body[name] = Value::String(format_time(value));
        }
    }
    let body = serde_json::to_vec_pretty(&body).unwrap_or_default();
    json!({
        "StatusCode": 200,
        "Headers": {
            "content-type": ["application/json; charset=utf-8"],
            "cache-control": ["no-store"]
        },
        "Body": base64::engine::general_purpose::STANDARD.encode(body)
    })
}

fn format_time(value: SystemTime) -> String {
    DateTime::<Utc>::from(value).to_rfc3339_opts(SecondsFormat::Secs, true)
}

fn format_duration(value: Duration) -> String {
    let nanos = value.as_nanos();
    if nanos.is_multiple_of(3_600_000_000_000) {
        format!("{}h0m0s", nanos / 3_600_000_000_000)
    } else if nanos.is_multiple_of(60_000_000_000) {
        format!("{}m0s", nanos / 60_000_000_000)
    } else if nanos.is_multiple_of(1_000_000_000) {
        format!("{}s", nanos / 1_000_000_000)
    } else if nanos.is_multiple_of(1_000_000) {
        format!("{}ms", nanos / 1_000_000)
    } else {
        format!("{}ns", nanos)
    }
}

fn ok_envelope(result: Value) -> Result<Vec<u8>, String> {
    serde_json::to_vec(&json!({"ok": true, "result": result})).map_err(|error| error.to_string())
}

fn error_envelope(code: &str, message: &str) -> Vec<u8> {
    serde_json::to_vec(&json!({
        "ok": false,
        "error": {"code": code, "message": message}
    }))
    .unwrap_or_default()
}

fn write_response(response: *mut Buffer, bytes: Vec<u8>) {
    if response.is_null() || bytes.is_empty() {
        return;
    }
    let boxed = bytes.into_boxed_slice();
    let len = boxed.len();
    let ptr = Box::into_raw(boxed).cast::<u8>().cast::<c_void>();
    // SAFETY: response is validated by the exported caller and ptr/len describe
    // an allocation released by cliproxy_plugin_free.
    unsafe {
        (*response).ptr = ptr;
        (*response).len = len;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_go_style_durations() {
        assert_eq!(parse_duration("5h"), Some(Duration::from_secs(18_000)));
        assert_eq!(parse_duration("250ms"), Some(Duration::from_millis(250)));
        assert_eq!(parse_duration("1h30m"), Some(Duration::from_secs(5_400)));
        assert_eq!(parse_duration("0s"), Some(Duration::ZERO));
        assert_eq!(parse_duration("tomorrow"), None);
    }

    #[test]
    fn lifecycle_config_uses_base64_encoded_yaml() {
        let yaml = b"interval: 2h\nrun_on_start: false\nmodel: gpt-test\n";
        let raw = serde_json::to_vec(&json!({
            "config_yaml": base64::engine::general_purpose::STANDARD.encode(yaml),
            "schema_version": 6
        }))
        .unwrap();
        let cfg = parse_lifecycle_config(&raw).unwrap();
        assert_eq!(cfg.interval, Duration::from_secs(7_200));
        assert!(!cfg.run_on_start);
        assert_eq!(cfg.model, "gpt-test");
    }

    #[test]
    fn registration_uses_host_rpc_field_names() {
        let raw = handle_method(
            METHOD_PLUGIN_REGISTER,
            br#"{"config_yaml":"","schema_version":6}"#,
        )
        .unwrap();
        let envelope: Value = serde_json::from_slice(&raw).unwrap();
        assert_eq!(envelope["result"]["capabilities"]["management_api"], true);
        assert_eq!(envelope["result"]["schema_version"], SCHEMA_VERSION);
        runtime().stop();
    }
}
