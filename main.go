package main

/*
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    void* ptr;
    size_t len;
} cliproxy_buffer;

typedef int (*cliproxy_host_call_fn)(void*, const char*, const uint8_t*, size_t, cliproxy_buffer*);
typedef void (*cliproxy_host_free_fn)(void*, size_t);

typedef struct {
    uint32_t abi_version;
    void* host_ctx;
    cliproxy_host_call_fn call;
    cliproxy_host_free_fn free_buffer;
} cliproxy_host_api;

typedef int (*cliproxy_plugin_call_fn)(char*, uint8_t*, size_t, cliproxy_buffer*);
typedef void (*cliproxy_plugin_free_fn)(void*, size_t);
typedef void (*cliproxy_plugin_shutdown_fn)(void);

typedef struct {
    uint32_t abi_version;
    cliproxy_plugin_call_fn call;
    cliproxy_plugin_free_fn free_buffer;
    cliproxy_plugin_shutdown_fn shutdown;
} cliproxy_plugin_api;

extern int cliproxyPluginCall(char*, uint8_t*, size_t, cliproxy_buffer*);
extern void cliproxyPluginFree(void*, size_t);
extern void cliproxyPluginShutdown(void);

static const cliproxy_host_api* stored_host;

static void store_host_api(const cliproxy_host_api* host) {
    stored_host = host;
}

static int call_host_api(const char* method, const uint8_t* request, size_t request_len, cliproxy_buffer* response) {
    if (stored_host == NULL || stored_host->call == NULL) {
        return 1;
    }
    return stored_host->call(stored_host->host_ctx, method, request, request_len, response);
}

static void free_host_buffer(void* ptr, size_t len) {
    if (stored_host != NULL && stored_host->free_buffer != NULL && ptr != NULL) {
        stored_host->free_buffer(ptr, len);
    }
}
*/
import "C"

import (
    "bytes"
    "encoding/json"
    "fmt"
    "net/http"
    "strings"
    "sync"
    "time"
    "unsafe"

    "github.com/router-for-me/CLIProxyAPI/v7/sdk/pluginabi"
    "github.com/router-for-me/CLIProxyAPI/v7/sdk/pluginapi"
    "gopkg.in/yaml.v3"
)

const (
    pluginName = "Codex Auto Ping"
    pluginID   = "codex-auto-ping"
)

var pluginVersion = "0.1.0"

type pluginConfig struct {
    Interval        string `yaml:"interval"`
    StartupDelay    string `yaml:"startup_delay"`
    RunOnStart      *bool  `yaml:"run_on_start"`
    Model           string `yaml:"model"`
    Prompt          string `yaml:"prompt"`
    MaxOutputTokens int    `yaml:"max_output_tokens"`
    PingsPerCycle   int    `yaml:"pings_per_cycle"`
    PingSpacing     string `yaml:"ping_spacing"`
}

type runtimeConfig struct {
    Interval        time.Duration
    StartupDelay    time.Duration
    RunOnStart      bool
    Model           string
    Prompt          string
    MaxOutputTokens int
    PingsPerCycle   int
    PingSpacing     time.Duration
}

type runtimeState struct {
    mu            sync.RWMutex
    cfg           runtimeConfig
    cancel        chan struct{}
    generation    uint64
    lastAttempt   time.Time
    lastSuccess   time.Time
    nextRun       time.Time
    lastStatus    int
    lastError     string
    totalAttempts uint64
    totalSuccess  uint64
}

var state = &runtimeState{}

type lifecycleRequest struct {
    ConfigYAML    []byte `json:"config_yaml"`
    SchemaVersion uint32 `json:"schema_version"`
}

type envelope struct {
    OK     bool            `json:"ok"`
    Result json.RawMessage `json:"result,omitempty"`
    Error  *rpcError       `json:"error,omitempty"`
}

type rpcError struct {
    Code    string `json:"code"`
    Message string `json:"message"`
}

type registrationResult struct {
    SchemaVersion uint32             `json:"schema_version"`
    Metadata      pluginapi.Metadata `json:"metadata"`
    Capabilities  capabilityFlags    `json:"capabilities"`
}

type capabilityFlags struct {
    ManagementAPI bool `json:"management_api"`
}

type managementRegistrationResponse struct {
    Resources []resourceRoute `json:"resources,omitempty"`
}

type resourceRoute struct {
    Path        string `json:"Path"`
    Menu        string `json:"Menu"`
    Description string `json:"Description"`
}

type managementResponse struct {
    StatusCode int         `json:"StatusCode"`
    Headers    http.Header `json:"Headers,omitempty"`
    Body       []byte      `json:"Body,omitempty"`
}

type hostModelExecutionRequest struct {
    pluginapi.HostModelExecutionRequest
}

func main() {}

//export cliproxy_plugin_init
func cliproxy_plugin_init(host *C.cliproxy_host_api, plugin *C.cliproxy_plugin_api) C.int {
    if plugin == nil {
        return 1
    }
    C.store_host_api(host)
    plugin.abi_version = C.uint32_t(pluginabi.ABIVersion)
    plugin.call = C.cliproxy_plugin_call_fn(C.cliproxyPluginCall)
    plugin.free_buffer = C.cliproxy_plugin_free_fn(C.cliproxyPluginFree)
    plugin.shutdown = C.cliproxy_plugin_shutdown_fn(C.cliproxyPluginShutdown)
    return 0
}

//export cliproxyPluginCall
func cliproxyPluginCall(method *C.char, request *C.uint8_t, requestLen C.size_t, response *C.cliproxy_buffer) C.int {
    if response != nil {
        response.ptr = nil
        response.len = 0
    }
    if method == nil {
        writeResponse(response, errorEnvelope("invalid_method", "method is required"))
        return 1
    }

    var payload []byte
    if request != nil && requestLen > 0 {
        payload = C.GoBytes(unsafe.Pointer(request), C.int(requestLen))
    }

    raw, err := handleMethod(C.GoString(method), payload)
    if err != nil {
        writeResponse(response, errorEnvelope("plugin_error", err.Error()))
        return 1
    }
    writeResponse(response, raw)
    return 0
}

//export cliproxyPluginFree
func cliproxyPluginFree(ptr unsafe.Pointer, length C.size_t) {
    _ = length
    if ptr != nil {
        C.free(ptr)
    }
}

//export cliproxyPluginShutdown
func cliproxyPluginShutdown() {
    stopScheduler()
}

func handleMethod(method string, request []byte) ([]byte, error) {
    switch method {
    case pluginabi.MethodPluginRegister, pluginabi.MethodPluginReconfigure:
        if err := applyConfig(request); err != nil {
            return nil, err
        }
        return okEnvelope(registrationResult{
            SchemaVersion: pluginabi.SchemaVersion,
            Metadata: pluginapi.Metadata{
                Name:             pluginName,
                Version:          pluginVersion,
                Author:           "KKKKeybird",
                GitHubRepository: "https://github.com/KKKKeybird/cpa-codex-auto-ping",
                ConfigFields: []pluginapi.ConfigField{
                    {Name: "interval", Type: pluginapi.ConfigFieldTypeString, Description: "Ping interval, e.g. 5h."},
                    {Name: "startup_delay", Type: pluginapi.ConfigFieldTypeString, Description: "Delay before the startup ping."},
                    {Name: "run_on_start", Type: pluginapi.ConfigFieldTypeBoolean, Description: "Send one ping after startup_delay."},
                    {Name: "model", Type: pluginapi.ConfigFieldTypeString, Description: "Model name exposed by CLIProxyAPI."},
                    {Name: "prompt", Type: pluginapi.ConfigFieldTypeString, Description: "Tiny input used for the ping."},
                    {Name: "max_output_tokens", Type: pluginapi.ConfigFieldTypeInteger, Description: "Maximum output tokens."},
                    {Name: "pings_per_cycle", Type: pluginapi.ConfigFieldTypeInteger, Description: "Number of tiny requests sent per cycle."},
                    {Name: "ping_spacing", Type: pluginapi.ConfigFieldTypeString, Description: "Delay between requests in a cycle."},
                },
            },
            Capabilities: capabilityFlags{ManagementAPI: true},
        })

    case pluginabi.MethodManagementRegister:
        return okEnvelope(managementRegistrationResponse{
            Resources: []resourceRoute{{
                Path:        "/status",
                Menu:        "Codex Auto Ping",
                Description: "Show scheduler state and recent ping result.",
            }},
        })

    case pluginabi.MethodManagementHandle:
        return okEnvelope(currentStatusResponse())

    default:
        return errorEnvelope("unknown_method", "unknown method: "+method), nil
    }
}

func defaults() runtimeConfig {
    return runtimeConfig{
        Interval:        5 * time.Hour,
        StartupDelay:    10 * time.Second,
        RunOnStart:      true,
        Model:           "gpt-5.6",
        Prompt:          "1",
        MaxOutputTokens: 1,
        PingsPerCycle:   1,
        PingSpacing:     3 * time.Second,
    }
}

func applyConfig(raw []byte) error {
    cfg := defaults()

    if len(bytes.TrimSpace(raw)) > 0 {
        var req lifecycleRequest
        if err := json.Unmarshal(raw, &req); err != nil {
            return fmt.Errorf("decode lifecycle request: %w", err)
        }

        if len(bytes.TrimSpace(req.ConfigYAML)) > 0 {
            var in pluginConfig
            if err := yaml.Unmarshal(req.ConfigYAML, &in); err != nil {
                return fmt.Errorf("decode plugin config: %w", err)
            }

            var err error
            if strings.TrimSpace(in.Interval) != "" {
                cfg.Interval, err = time.ParseDuration(strings.TrimSpace(in.Interval))
                if err != nil || cfg.Interval <= 0 {
                    return fmt.Errorf("invalid interval")
                }
            }
            if strings.TrimSpace(in.StartupDelay) != "" {
                cfg.StartupDelay, err = time.ParseDuration(strings.TrimSpace(in.StartupDelay))
                if err != nil || cfg.StartupDelay < 0 {
                    return fmt.Errorf("invalid startup_delay")
                }
            }
            if in.RunOnStart != nil {
                cfg.RunOnStart = *in.RunOnStart
            }
            if strings.TrimSpace(in.Model) != "" {
                cfg.Model = strings.TrimSpace(in.Model)
            }
            if in.Prompt != "" {
                cfg.Prompt = in.Prompt
            }
            if in.MaxOutputTokens > 0 {
                cfg.MaxOutputTokens = in.MaxOutputTokens
            }
            if in.PingsPerCycle > 0 {
                cfg.PingsPerCycle = in.PingsPerCycle
            }
            if strings.TrimSpace(in.PingSpacing) != "" {
                cfg.PingSpacing, err = time.ParseDuration(strings.TrimSpace(in.PingSpacing))
                if err != nil || cfg.PingSpacing < 0 {
                    return fmt.Errorf("invalid ping_spacing")
                }
            }
        }
    }

    startScheduler(cfg)
    return nil
}

func startScheduler(cfg runtimeConfig) {
    state.mu.Lock()
    if state.cancel != nil {
        close(state.cancel)
    }
    state.generation++
    generation := state.generation
    cancel := make(chan struct{})
    state.cancel = cancel
    state.cfg = cfg
    state.nextRun = time.Time{}
    state.mu.Unlock()

    go schedulerLoop(generation, cancel, cfg)
}

func stopScheduler() {
    state.mu.Lock()
    defer state.mu.Unlock()
    if state.cancel != nil {
        close(state.cancel)
        state.cancel = nil
    }
}

func schedulerLoop(generation uint64, cancel <-chan struct{}, cfg runtimeConfig) {
    if cfg.RunOnStart {
        if !waitOrCancel(cancel, cfg.StartupDelay) {
            return
        }
        runCycle(cfg)
    }

    for {
        next := time.Now().Add(cfg.Interval)
        state.mu.Lock()
        if generation != state.generation {
            state.mu.Unlock()
            return
        }
        state.nextRun = next
        state.mu.Unlock()

        if !waitOrCancel(cancel, cfg.Interval) {
            return
        }
        runCycle(cfg)
    }
}

func waitOrCancel(cancel <-chan struct{}, d time.Duration) bool {
    timer := time.NewTimer(d)
    defer timer.Stop()
    select {
    case <-timer.C:
        return true
    case <-cancel:
        return false
    }
}

func runCycle(cfg runtimeConfig) {
    for i := 0; i < cfg.PingsPerCycle; i++ {
        runPing(cfg)
        if i+1 < cfg.PingsPerCycle && cfg.PingSpacing > 0 {
            time.Sleep(cfg.PingSpacing)
        }
    }
}

func runPing(cfg runtimeConfig) {
    body, err := json.Marshal(map[string]any{
        "model":             cfg.Model,
        "input":             cfg.Prompt,
        "max_output_tokens": cfg.MaxOutputTokens,
    })
    if err != nil {
        recordResult(0, err)
        return
    }

    state.mu.Lock()
    state.lastAttempt = time.Now()
    state.totalAttempts++
    state.mu.Unlock()

    result, err := callHost(pluginabi.MethodHostModelExecute, hostModelExecutionRequest{
        HostModelExecutionRequest: pluginapi.HostModelExecutionRequest{
            EntryProtocol: "openai-response",
            ExitProtocol:  "openai-response",
            Model:         cfg.Model,
            Stream:        false,
            Body:          body,
        },
    })
    if err != nil {
        recordResult(0, err)
        return
    }

    var resp pluginapi.HostModelExecutionResponse
    if err := json.Unmarshal(result, &resp); err != nil {
        recordResult(0, fmt.Errorf("decode model response: %w", err))
        return
    }
    if resp.StatusCode < 200 || resp.StatusCode >= 300 {
        recordResult(resp.StatusCode, fmt.Errorf("model request returned status %d", resp.StatusCode))
        return
    }

    recordResult(resp.StatusCode, nil)
}

func recordResult(status int, err error) {
    state.mu.Lock()
    defer state.mu.Unlock()
    state.lastStatus = status
    if err != nil {
        state.lastError = err.Error()
        return
    }
    state.lastError = ""
    state.lastSuccess = time.Now()
    state.totalSuccess++
}

func currentStatusResponse() managementResponse {
    state.mu.RLock()
    snapshot := map[string]any{
        "plugin":          pluginID,
        "version":         pluginVersion,
        "model":           state.cfg.Model,
        "interval":        state.cfg.Interval.String(),
        "pings_per_cycle": state.cfg.PingsPerCycle,
        "last_status":     state.lastStatus,
        "last_error":      state.lastError,
        "total_attempts":  state.totalAttempts,
        "total_success":   state.totalSuccess,
    }
    if !state.lastAttempt.IsZero() {
        snapshot["last_attempt"] = state.lastAttempt.Format(time.RFC3339)
    }
    if !state.lastSuccess.IsZero() {
        snapshot["last_success"] = state.lastSuccess.Format(time.RFC3339)
    }
    if !state.nextRun.IsZero() {
        snapshot["next_run"] = state.nextRun.Format(time.RFC3339)
    }
    state.mu.RUnlock()

    body, _ := json.MarshalIndent(snapshot, "", "  ")
    return managementResponse{
        StatusCode: http.StatusOK,
        Headers: http.Header{
            "content-type":  []string{"application/json; charset=utf-8"},
            "cache-control": []string{"no-store"},
        },
        Body: body,
    }
}

func callHost(method string, payload any) (json.RawMessage, error) {
    rawPayload, err := json.Marshal(payload)
    if err != nil {
        return nil, err
    }

    cMethod := C.CString(method)
    defer C.free(unsafe.Pointer(cMethod))

    var response C.cliproxy_buffer
    var requestPtr *C.uint8_t
    if len(rawPayload) > 0 {
        cPayload := C.CBytes(rawPayload)
        if cPayload == nil {
            return nil, fmt.Errorf("allocate host payload")
        }
        defer C.free(cPayload)
        requestPtr = (*C.uint8_t)(cPayload)
    }

    code := C.call_host_api(cMethod, requestPtr, C.size_t(len(rawPayload)), &response)

    var rawResponse []byte
    if response.ptr != nil && response.len > 0 {
        rawResponse = C.GoBytes(response.ptr, C.int(response.len))
    }
    if response.ptr != nil {
        C.free_host_buffer(response.ptr, response.len)
    }
    if len(rawResponse) == 0 {
        return nil, fmt.Errorf("host callback %s returned no response, code=%d", method, int(code))
    }

    var env envelope
    if err := json.Unmarshal(rawResponse, &env); err != nil {
        return nil, fmt.Errorf("decode host callback envelope: %w", err)
    }
    if !env.OK {
        if env.Error != nil {
            return nil, fmt.Errorf("%s: %s", env.Error.Code, env.Error.Message)
        }
        return nil, fmt.Errorf("host callback %s failed", method)
    }
    if code != 0 {
        return nil, fmt.Errorf("host callback %s returned code=%d", method, int(code))
    }
    return append(json.RawMessage(nil), env.Result...), nil
}

func okEnvelope(v any) ([]byte, error) {
    result, err := json.Marshal(v)
    if err != nil {
        return nil, err
    }
    return json.Marshal(envelope{OK: true, Result: result})
}

func errorEnvelope(code, message string) []byte {
    raw, _ := json.Marshal(envelope{
        OK: false,
        Error: &rpcError{
            Code:    code,
            Message: message,
        },
    })
    return raw
}

func writeResponse(response *C.cliproxy_buffer, raw []byte) {
    if response == nil || len(raw) == 0 {
        return
    }
    ptr := C.CBytes(raw)
    if ptr == nil {
        return
    }
    response.ptr = ptr
    response.len = C.size_t(len(raw))
}
