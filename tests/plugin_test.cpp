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

  const auto registration = handle_method(
      "plugin.register", "{\"config_yaml\":\"\",\"schema_version\":6}");
  assert(registration.find("\"management_api\":true") != std::string::npos);
  assert(registration.find("\"schema_version\":6") != std::string::npos);
  stop_scheduler();

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
