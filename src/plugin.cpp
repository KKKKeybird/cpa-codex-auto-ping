#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "dev"
#endif

#if defined(_WIN32)
#define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct cliproxy_buffer {
  void *ptr;
  std::size_t len;
};

using cliproxy_host_call_fn = int (*)(void *, const char *, const std::uint8_t *,
                                      std::size_t, cliproxy_buffer *);
using cliproxy_host_free_fn = void (*)(void *, std::size_t);

struct cliproxy_host_api {
  std::uint32_t abi_version;
  void *host_ctx;
  cliproxy_host_call_fn call;
  cliproxy_host_free_fn free_buffer;
};

using cliproxy_plugin_call_fn = int (*)(const char *, const std::uint8_t *,
                                        std::size_t, cliproxy_buffer *);
using cliproxy_plugin_free_fn = void (*)(void *, std::size_t);
using cliproxy_plugin_shutdown_fn = void (*)();

struct cliproxy_plugin_api {
  std::uint32_t abi_version;
  cliproxy_plugin_call_fn call;
  cliproxy_plugin_free_fn free_buffer;
  cliproxy_plugin_shutdown_fn shutdown;
};

namespace {

using Clock = std::chrono::system_clock;
using Duration = std::chrono::nanoseconds;

constexpr std::uint32_t kABIVersion = 1;
constexpr const char *kPluginID = "codex-auto-ping";

struct Config {
  Duration interval = std::chrono::hours(5);
  Duration startup_delay = std::chrono::seconds(10);
  bool run_on_start = true;
  std::string model = "gpt-5.6";
  std::string prompt = "1";
  std::uint64_t max_output_tokens = 1;
  std::uint64_t pings_per_cycle = 1;
  Duration ping_spacing = std::chrono::seconds(3);
};

struct State {
  Config cfg;
  Clock::time_point last_attempt{};
  Clock::time_point last_success{};
  Clock::time_point next_run{};
  int last_status = 0;
  std::string last_error;
  std::uint64_t total_attempts = 0;
  std::uint64_t total_success = 0;
};

struct StopSignal {
  std::mutex mutex;
  std::condition_variable wake;
  bool stopped = false;
};

std::mutex g_state_mutex;
State g_state;
std::mutex g_scheduler_mutex;
std::shared_ptr<StopSignal> g_stop;
std::thread g_scheduler;
cliproxy_host_api g_host{};
std::atomic<bool> g_host_ready{false};

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (const unsigned char character : value) {
    switch (character) {
    case '"': out << "\\\""; break;
    case '\\': out << "\\\\"; break;
    case '\b': out << "\\b"; break;
    case '\f': out << "\\f"; break;
    case '\n': out << "\\n"; break;
    case '\r': out << "\\r"; break;
    case '\t': out << "\\t"; break;
    default:
      if (character < 0x20) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<int>(character) << std::dec;
      } else {
        out << character;
      }
    }
  }
  return out.str();
}

std::string json_string_field(const std::string &json, const std::string &name) {
  const std::string needle = "\"" + name + "\"";
  auto cursor = json.find(needle);
  if (cursor == std::string::npos) {
    return {};
  }
  cursor = json.find(':', cursor + needle.size());
  if (cursor == std::string::npos) {
    return {};
  }
  cursor = json.find('"', cursor + 1);
  if (cursor == std::string::npos) {
    return {};
  }
  ++cursor;
  std::string value;
  while (cursor < json.size()) {
    const char character = json[cursor++];
    if (character == '"') {
      return value;
    }
    if (character != '\\') {
      value.push_back(character);
      continue;
    }
    if (cursor >= json.size()) {
      return {};
    }
    const char escaped = json[cursor++];
    switch (escaped) {
    case '"': value.push_back('"'); break;
    case '\\': value.push_back('\\'); break;
    case '/': value.push_back('/'); break;
    case 'b': value.push_back('\b'); break;
    case 'f': value.push_back('\f'); break;
    case 'n': value.push_back('\n'); break;
    case 'r': value.push_back('\r'); break;
    case 't': value.push_back('\t'); break;
    default: return {};
    }
  }
  return {};
}

bool json_bool_field(const std::string &json, const std::string &name, bool fallback) {
  const std::string needle = "\"" + name + "\"";
  auto cursor = json.find(needle);
  if (cursor == std::string::npos ||
      (cursor = json.find(':', cursor + needle.size())) == std::string::npos) {
    return fallback;
  }
  const auto value = trim(json.substr(cursor + 1, 5));
  if (value.rfind("true", 0) == 0) return true;
  if (value.rfind("false", 0) == 0) return false;
  return fallback;
}

int json_int_field(const std::string &json, const std::string &name, int fallback) {
  const std::string needle = "\"" + name + "\"";
  auto cursor = json.find(needle);
  if (cursor == std::string::npos ||
      (cursor = json.find(':', cursor + needle.size())) == std::string::npos) {
    return fallback;
  }
  ++cursor;
  while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
  char *end = nullptr;
  const long value = std::strtol(json.c_str() + cursor, &end, 10);
  return end == json.c_str() + cursor ? fallback : static_cast<int>(value);
}

const std::string kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string &input) {
  std::string output;
  std::uint32_t accumulator = 0;
  int bits = -6;
  for (const unsigned char byte : input) {
    accumulator = (accumulator << 8) | byte;
    bits += 8;
    while (bits >= 0) {
      output.push_back(kBase64Alphabet[(accumulator >> bits) & 0x3f]);
      bits -= 6;
    }
  }
  if (bits > -6) output.push_back(kBase64Alphabet[(accumulator << 8 >> (bits + 8)) & 0x3f]);
  while (output.size() % 4 != 0) output.push_back('=');
  return output;
}

std::string base64_decode(const std::string &input) {
  std::string output;
  std::uint32_t accumulator = 0;
  int bits = -8;
  for (const unsigned char character : input) {
    if (character == '=') break;
    const auto position = kBase64Alphabet.find(static_cast<char>(character));
    if (position == std::string::npos) throw std::runtime_error("invalid config_yaml base64");
    accumulator = (accumulator << 6) | static_cast<std::uint32_t>(position);
    bits += 6;
    if (bits >= 0) {
      output.push_back(static_cast<char>((accumulator >> bits) & 0xff));
      bits -= 8;
    }
  }
  return output;
}

Duration parse_duration(const std::string &raw) {
  const std::string value = trim(raw);
  if (value.empty() || value.front() == '-') throw std::runtime_error("invalid duration");
  std::size_t cursor = value.front() == '+' ? 1 : 0;
  long double nanoseconds = 0;
  while (cursor < value.size()) {
    const std::size_t number_start = cursor;
    while (cursor < value.size() &&
           (std::isdigit(static_cast<unsigned char>(value[cursor])) || value[cursor] == '.')) {
      ++cursor;
    }
    if (cursor == number_start) throw std::runtime_error("invalid duration");
    const long double amount = std::stold(value.substr(number_start, cursor - number_start));
    const std::pair<const char *, long double> units[] = {
        {"ns", 1.0L}, {"us", 1'000.0L}, {"ms", 1'000'000.0L},
        {"s", 1'000'000'000.0L}, {"m", 60'000'000'000.0L},
        {"h", 3'600'000'000'000.0L}};
    bool matched = false;
    for (const auto &[unit, multiplier] : units) {
      const std::size_t length = std::strlen(unit);
      if (value.compare(cursor, length, unit) == 0) {
        nanoseconds += amount * multiplier;
        cursor += length;
        matched = true;
        break;
      }
    }
    if (!matched) throw std::runtime_error("invalid duration");
  }
  if (nanoseconds < 0 || nanoseconds > static_cast<long double>(Duration::max().count())) {
    throw std::runtime_error("duration out of range");
  }
  return Duration(static_cast<Duration::rep>(nanoseconds));
}

std::string unquote_yaml(std::string value) {
  value = trim(std::move(value));
  if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
    return value.substr(1, value.size() - 2);
  }
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    std::string output;
    for (std::size_t index = 1; index + 1 < value.size(); ++index) {
      if (value[index] == '\\' && index + 2 < value.size()) {
        const char escaped = value[++index];
        if (escaped == 'n') output.push_back('\n');
        else if (escaped == 'r') output.push_back('\r');
        else if (escaped == 't') output.push_back('\t');
        else output.push_back(escaped);
      } else {
        output.push_back(value[index]);
      }
    }
    return output;
  }
  const auto comment = value.find(" #");
  return trim(value.substr(0, comment));
}

Config parse_config(const std::string &request) {
  Config cfg;
  const auto encoded = json_string_field(request, "config_yaml");
  if (encoded.empty()) return cfg;
  std::istringstream lines(base64_decode(encoded));
  std::string line;
  while (std::getline(lines, line)) {
    line = trim(line);
    if (line.empty() || line.front() == '#') continue;
    const auto colon = line.find(':');
    if (colon == std::string::npos) throw std::runtime_error("invalid plugin config");
    const auto key = trim(line.substr(0, colon));
    const auto value = unquote_yaml(line.substr(colon + 1));
    if (key == "interval" && !value.empty()) {
      cfg.interval = parse_duration(value);
      if (cfg.interval.count() <= 0) throw std::runtime_error("invalid interval");
    } else if (key == "startup_delay" && !value.empty()) {
      cfg.startup_delay = parse_duration(value);
    } else if (key == "run_on_start" && !value.empty()) {
      if (value == "true") cfg.run_on_start = true;
      else if (value == "false") cfg.run_on_start = false;
      else throw std::runtime_error("invalid run_on_start");
    } else if (key == "model" && !value.empty()) {
      cfg.model = value;
    } else if (key == "prompt" && !value.empty()) {
      cfg.prompt = value;
    } else if (key == "max_output_tokens" && !value.empty()) {
      cfg.max_output_tokens = std::stoull(value);
      if (cfg.max_output_tokens == 0) cfg.max_output_tokens = 1;
    } else if (key == "pings_per_cycle" && !value.empty()) {
      cfg.pings_per_cycle = std::stoull(value);
      if (cfg.pings_per_cycle == 0) cfg.pings_per_cycle = 1;
    } else if (key == "ping_spacing" && !value.empty()) {
      cfg.ping_spacing = parse_duration(value);
    }
  }
  return cfg;
}

std::string ok_envelope(const std::string &result) {
  return "{\"ok\":true,\"result\":" + result + "}";
}

std::string error_envelope(const std::string &code, const std::string &message) {
  return "{\"ok\":false,\"error\":{\"code\":\"" + json_escape(code) +
         "\",\"message\":\"" + json_escape(message) + "\"}}";
}

bool wait_or_stop(const std::shared_ptr<StopSignal> &stop, Duration duration) {
  std::unique_lock<std::mutex> lock(stop->mutex);
  return !stop->wake.wait_for(lock, duration, [&] { return stop->stopped; });
}

std::string call_host(const std::string &method, const std::string &request) {
  if (!g_host_ready.load() || g_host.call == nullptr || g_host.free_buffer == nullptr) {
    throw std::runtime_error("host API is unavailable");
  }
  cliproxy_buffer response{};
  const int code = g_host.call(g_host.host_ctx, method.c_str(),
                               reinterpret_cast<const std::uint8_t *>(request.data()),
                               request.size(), &response);
  std::string output;
  if (response.ptr != nullptr && response.len > 0) {
    output.assign(static_cast<const char *>(response.ptr), response.len);
  }
  if (response.ptr != nullptr) g_host.free_buffer(response.ptr, response.len);
  if (output.empty()) throw std::runtime_error("host callback returned no response");
  if (!json_bool_field(output, "ok", false)) {
    const auto code_value = json_string_field(output, "code");
    const auto message = json_string_field(output, "message");
    throw std::runtime_error((code_value.empty() ? "host_error" : code_value) + ": " +
                             (message.empty() ? "host callback failed" : message));
  }
  if (code != 0) throw std::runtime_error("host callback returned code=" + std::to_string(code));
  return output;
}

void record_result(int status, std::string error) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_state.last_status = status;
  g_state.last_error = std::move(error);
  if (g_state.last_error.empty()) {
    g_state.last_success = Clock::now();
    ++g_state.total_success;
  }
}

void run_ping(const Config &cfg) {
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_state.last_attempt = Clock::now();
    ++g_state.total_attempts;
  }
  const std::string body = "{\"model\":\"" + json_escape(cfg.model) +
                           "\",\"input\":\"" + json_escape(cfg.prompt) +
                           "\",\"max_output_tokens\":" +
                           std::to_string(cfg.max_output_tokens) + "}";
  const std::string request =
      "{\"entry_protocol\":\"openai-response\",\"exit_protocol\":\"openai-response\"," 
      "\"model\":\"" + json_escape(cfg.model) +
      "\",\"stream\":false,\"body\":\"" + base64_encode(body) +
      "\",\"headers\":null,\"query\":null,\"alt\":\"\"}";
  try {
    const auto response = call_host("host.model.execute", request);
    const int status = json_int_field(response, "status_code", 0);
    if (status < 200 || status >= 300) {
      record_result(status, "model request returned status " + std::to_string(status));
    } else {
      record_result(status, {});
    }
  } catch (const std::exception &error) {
    record_result(0, error.what());
  }
}

void scheduler_loop(const std::shared_ptr<StopSignal> &stop, Config cfg) {
  if (cfg.run_on_start) {
    if (!wait_or_stop(stop, cfg.startup_delay)) return;
    for (std::uint64_t index = 0; index < cfg.pings_per_cycle; ++index) {
      run_ping(cfg);
      if (index + 1 < cfg.pings_per_cycle && !wait_or_stop(stop, cfg.ping_spacing)) return;
    }
  }
  while (true) {
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      g_state.next_run =
          Clock::now() + std::chrono::duration_cast<Clock::duration>(cfg.interval);
    }
    if (!wait_or_stop(stop, cfg.interval)) return;
    for (std::uint64_t index = 0; index < cfg.pings_per_cycle; ++index) {
      run_ping(cfg);
      if (index + 1 < cfg.pings_per_cycle && !wait_or_stop(stop, cfg.ping_spacing)) return;
    }
  }
}

void stop_scheduler() {
  std::lock_guard<std::mutex> scheduler_lock(g_scheduler_mutex);
  if (g_stop) {
    {
      std::lock_guard<std::mutex> stop_lock(g_stop->mutex);
      g_stop->stopped = true;
    }
    g_stop->wake.notify_all();
  }
  if (g_scheduler.joinable()) g_scheduler.join();
  g_stop.reset();
}

void start_scheduler(Config cfg) {
  stop_scheduler();
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_state.cfg = cfg;
    g_state.next_run = {};
  }
  std::lock_guard<std::mutex> scheduler_lock(g_scheduler_mutex);
  g_stop = std::make_shared<StopSignal>();
  g_scheduler = std::thread(scheduler_loop, g_stop, std::move(cfg));
}

std::string format_time(Clock::time_point value) {
  const std::time_t timestamp = Clock::to_time_t(value);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &timestamp);
#else
  gmtime_r(&timestamp, &utc);
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

std::string format_duration(Duration duration) {
  const auto nanos = duration.count();
  if (nanos % 3'600'000'000'000LL == 0) return std::to_string(nanos / 3'600'000'000'000LL) + "h0m0s";
  if (nanos % 60'000'000'000LL == 0) return std::to_string(nanos / 60'000'000'000LL) + "m0s";
  if (nanos % 1'000'000'000LL == 0) return std::to_string(nanos / 1'000'000'000LL) + "s";
  if (nanos % 1'000'000LL == 0) return std::to_string(nanos / 1'000'000LL) + "ms";
  return std::to_string(nanos) + "ns";
}

std::string status_response() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  std::ostringstream body;
  body << "{\n  \"plugin\": \"" << kPluginID << "\",\n"
       << "  \"version\": \"" << PLUGIN_VERSION << "\",\n"
       << "  \"model\": \"" << json_escape(g_state.cfg.model) << "\",\n"
       << "  \"interval\": \"" << format_duration(g_state.cfg.interval) << "\",\n"
       << "  \"pings_per_cycle\": " << g_state.cfg.pings_per_cycle << ",\n"
       << "  \"last_status\": " << g_state.last_status << ",\n"
       << "  \"last_error\": \"" << json_escape(g_state.last_error) << "\",\n"
       << "  \"total_attempts\": " << g_state.total_attempts << ",\n"
       << "  \"total_success\": " << g_state.total_success;
  if (g_state.last_attempt.time_since_epoch().count() != 0) body << ",\n  \"last_attempt\": \"" << format_time(g_state.last_attempt) << "\"";
  if (g_state.last_success.time_since_epoch().count() != 0) body << ",\n  \"last_success\": \"" << format_time(g_state.last_success) << "\"";
  if (g_state.next_run.time_since_epoch().count() != 0) body << ",\n  \"next_run\": \"" << format_time(g_state.next_run) << "\"";
  body << "\n}";
  return "{\"StatusCode\":200,\"Headers\":{\"content-type\":[\"application/json; charset=utf-8\"],"
         "\"cache-control\":[\"no-store\"]},\"Body\":\"" + base64_encode(body.str()) + "\"}";
}

std::string handle_method(const std::string &method, const std::string &request) {
  if (method == "plugin.register" || method == "plugin.reconfigure") {
    start_scheduler(parse_config(request));
    return ok_envelope(
        "{\"schema_version\":6,\"metadata\":{\"Name\":\"Codex Auto Ping\","
        "\"Version\":\"" PLUGIN_VERSION "\",\"Author\":\"KKKKeybird\","
        "\"GitHubRepository\":\"https://github.com/KKKKeybird/cpa-codex-auto-ping\","
        "\"Logo\":\"\",\"ConfigFields\":["
        "{\"Name\":\"interval\",\"Type\":\"string\",\"Description\":\"Ping interval, e.g. 5h.\"},"
        "{\"Name\":\"startup_delay\",\"Type\":\"string\",\"Description\":\"Delay before the startup ping.\"},"
        "{\"Name\":\"run_on_start\",\"Type\":\"boolean\",\"Description\":\"Send one ping after startup_delay.\"},"
        "{\"Name\":\"model\",\"Type\":\"string\",\"Description\":\"Model name exposed by CLIProxyAPI.\"},"
        "{\"Name\":\"prompt\",\"Type\":\"string\",\"Description\":\"Tiny input used for the ping.\"},"
        "{\"Name\":\"max_output_tokens\",\"Type\":\"integer\",\"Description\":\"Maximum output tokens.\"},"
        "{\"Name\":\"pings_per_cycle\",\"Type\":\"integer\",\"Description\":\"Number of tiny requests sent per cycle.\"},"
        "{\"Name\":\"ping_spacing\",\"Type\":\"string\",\"Description\":\"Delay between requests in a cycle.\"}]},"
        "\"capabilities\":{\"management_api\":true}}");
  }
  if (method == "management.register") {
    return ok_envelope("{\"routes\":[{\"Method\":\"GET\",\"Path\":\"/plugins/codex-auto-ping/status\","
                       "\"Description\":\"Show scheduler state and recent ping result.\"}]}");
  }
  if (method == "management.handle") return ok_envelope(status_response());
  return error_envelope("unknown_method", "unknown method: " + method);
}

void write_response(cliproxy_buffer *response, const std::string &value) {
  if (response == nullptr || value.empty()) return;
  void *memory = std::malloc(value.size());
  if (memory == nullptr) throw std::bad_alloc();
  std::memcpy(memory, value.data(), value.size());
  response->ptr = memory;
  response->len = value.size();
}

} // namespace

PLUGIN_EXPORT int cliproxy_plugin_call(const char *method, const std::uint8_t *request,
                                        std::size_t request_len, cliproxy_buffer *response);
PLUGIN_EXPORT void cliproxy_plugin_free(void *ptr, std::size_t len);
PLUGIN_EXPORT void cliproxy_plugin_shutdown();

PLUGIN_EXPORT int cliproxy_plugin_init(const cliproxy_host_api *host,
                                        cliproxy_plugin_api *plugin) {
  if (host == nullptr || plugin == nullptr || host->abi_version != kABIVersion ||
      host->call == nullptr || host->free_buffer == nullptr) {
    return 1;
  }
  g_host = *host;
  g_host_ready.store(true);
  plugin->abi_version = kABIVersion;
  plugin->call = cliproxy_plugin_call;
  plugin->free_buffer = cliproxy_plugin_free;
  plugin->shutdown = cliproxy_plugin_shutdown;
  return 0;
}

PLUGIN_EXPORT int cliproxy_plugin_call(const char *method, const std::uint8_t *request,
                                        std::size_t request_len, cliproxy_buffer *response) {
  if (response == nullptr) return 1;
  response->ptr = nullptr;
  response->len = 0;
  try {
    if (method == nullptr || (request == nullptr && request_len != 0)) {
      write_response(response, error_envelope("invalid_request", "invalid method or request"));
      return 1;
    }
    const std::string payload = request_len == 0
                                    ? std::string()
                                    : std::string(reinterpret_cast<const char *>(request), request_len);
    write_response(response, handle_method(method, payload));
    return 0;
  } catch (const std::exception &error) {
    try {
      write_response(response, error_envelope("plugin_error", error.what()));
    } catch (...) {
    }
    return 1;
  }
}

PLUGIN_EXPORT void cliproxy_plugin_free(void *ptr, std::size_t) { std::free(ptr); }

PLUGIN_EXPORT void cliproxy_plugin_shutdown() {
  stop_scheduler();
  g_host_ready.store(false);
}
