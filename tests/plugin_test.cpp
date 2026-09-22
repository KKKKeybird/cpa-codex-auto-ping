#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

#include "../src/plugin.cpp"

namespace {

int fake_host_call(void *, const char *method, const std::uint8_t *, std::size_t,
                   cliproxy_buffer *response) {
  assert(std::string(method) == "host.model.execute");
  const std::string value =
      "{\"ok\":true,\"result\":{\"status_code\":200,\"headers\":null,\"body\":null}}";
  response->ptr = std::malloc(value.size());
  assert(response->ptr != nullptr);
  std::memcpy(response->ptr, value.data(), value.size());
  response->len = value.size();
  return 0;
}

void fake_host_free(void *ptr, std::size_t) { std::free(ptr); }

std::string plugin_call(cliproxy_plugin_api &api, const std::string &method,
                        const std::string &request) {
  cliproxy_buffer response{};
  const int code = api.call(method.c_str(),
                            reinterpret_cast<const std::uint8_t *>(request.data()),
                            request.size(), &response);
  assert(code == 0);
  const std::string output(static_cast<const char *>(response.ptr), response.len);
  api.free_buffer(response.ptr, response.len);
  return output;
}

} // namespace

int main() {
  assert(parse_duration("5h") == std::chrono::hours(5));
  assert(parse_duration("250ms") == std::chrono::milliseconds(250));
  assert(parse_duration("1h30m") == std::chrono::minutes(90));
  assert(base64_decode(base64_encode("interval: 2h\n")) == "interval: 2h\n");

  const auto encoded = base64_encode(
      "interval: 2h\nrun_on_start: false\nmodel: gpt-test\n");
  const auto cfg = parse_config("{\"config_yaml\":\"" + encoded + "\",\"schema_version\":6}");
  assert(cfg.interval == std::chrono::hours(2));
  assert(!cfg.run_on_start);
  assert(cfg.model == "gpt-test");

  // A store-managed install hands over the whole subtree: host wrappers plus
  // the store manifest with a YAML sequence (tags) and nested mappings. None of
  // it may be mistaken for plugin settings, and none of it may fail the parse.
  const auto store_managed = base64_encode(
      "enabled: true\n"
      "priority: 1\n"
      "interval: \"5h\"\n"
      "startup_delay: \"10s\"\n"
      "run_on_start: true\n"
      "model: \"gpt-5.6-luna\"\n"
      "prompt: \"1\"\n"
      "max_output_tokens: 1\n"
      "pings_per_cycle: 1\n"
      "ping_spacing: \"3s\"\n"
      "store:\n"
      "  id: codex-auto-ping\n"
      "  name: Codex Auto Ping\n"
      "  version: 0.2.1\n"
      "  release-tag: v0.2.1\n"
      "  source-id: official\n"
      "  source-url: https://example.invalid/registry.json\n"
      "  tags:\n"
      "    - Codex\n"
      "    - Usage\n"
      "  install:\n"
      "    type: github-release\n"
      "    model: shadowed-nested-value\n");
  const auto stored = parse_config(
      "{\"config_yaml\":\"" + store_managed + "\",\"schema_version\":6}");
  assert(stored.interval == std::chrono::hours(5));
  assert(stored.startup_delay == std::chrono::seconds(10));
  assert(stored.run_on_start);
  assert(stored.model == "gpt-5.6-luna");
  assert(stored.prompt == "1");
  assert(stored.max_output_tokens == 1);
  assert(stored.pings_per_cycle == 1);
  assert(stored.ping_spacing == std::chrono::seconds(3));

  // Unknown top-level keys and stray colon-less lines are ignored, not fatal.
  const auto tolerant = base64_encode(
      "model: gpt-tolerant\nunknown-key: value\n- stray sequence item\n"
      "interval: 1h\n");
  const auto tolerant_cfg = parse_config(
      "{\"config_yaml\":\"" + tolerant + "\",\"schema_version\":6}");
  assert(tolerant_cfg.model == "gpt-tolerant");
  assert(tolerant_cfg.interval == std::chrono::hours(1));

  const auto registration = handle_method(
      "plugin.register", "{\"config_yaml\":\"\",\"schema_version\":6}");
  assert(registration.find("\"management_api\":true") != std::string::npos);
  assert(registration.find("\"schema_version\":6") != std::string::npos);
  stop_scheduler();

  // Dynamic runtime state must only be exposed through an authenticated
  // Management API route. Never register it as a public resource.
  const auto management_registration =
      handle_method("management.register", "{}");
  assert(management_registration.find("\"ok\":true") != std::string::npos);
  assert(management_registration.find("\"routes\"") != std::string::npos);
  assert(management_registration.find("\"Method\":\"GET\"") !=
         std::string::npos);
  assert(management_registration.find(
             "\"Path\":\"/plugins/codex-auto-ping/status\"") !=
         std::string::npos);
  assert(management_registration.find("\"resources\"") ==
         std::string::npos);
  assert(management_registration.find("/v0/resource/plugins/") ==
         std::string::npos);

  cliproxy_host_api host{kABIVersion, nullptr, fake_host_call, fake_host_free};
  cliproxy_plugin_api api{};
  assert(cliproxy_plugin_init(&host, &api) == 0);
  assert(api.abi_version == kABIVersion);

  const auto live_config = base64_encode(
      "interval: 5h\nstartup_delay: 0s\nrun_on_start: true\nmodel: gpt-test\n");
  plugin_call(api, "plugin.register",
              "{\"config_yaml\":\"" + live_config + "\",\"schema_version\":6}");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto status = plugin_call(api, "management.handle", "{}");
  const auto body = base64_decode(json_string_field(status, "Body"));
  assert(body.find("\"last_status\": 200") != std::string::npos);
  assert(body.find("\"total_success\": 1") != std::string::npos);
  api.shutdown();

  std::cout << "plugin tests passed\n";
  return 0;
}
