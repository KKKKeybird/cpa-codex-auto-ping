#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

struct Buffer { void *ptr; std::size_t len; };
using HostCall = int (*)(void *, const char *, const std::uint8_t *, std::size_t, Buffer *);
using Free = void (*)(void *, std::size_t);
struct Host { std::uint32_t version; void *context; HostCall call; Free free; };
struct Plugin {
  std::uint32_t version;
  int (*call)(const char *, const std::uint8_t *, std::size_t, Buffer *);
  Free free;
  void (*shutdown)();
};
int unused_call(void *, const char *, const std::uint8_t *, std::size_t, Buffer *) {
  return 1;
}
void free_buffer(void *ptr, std::size_t) { std::free(ptr); }

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
  auto init = reinterpret_cast<int (*)(const Host *, Plugin *)>(
      dlsym(library, "cliproxy_plugin_init"));
  if (!init || !dlsym(library, "cliproxy_plugin_call") ||
      !dlsym(library, "cliproxy_plugin_free") ||
      !dlsym(library, "cliproxy_plugin_shutdown")) return 1;
  Host host{1, nullptr, unused_call, free_buffer};
  Plugin plugin{};
  if (init(&host, &plugin) != 0 || plugin.version != 1 ||
      !plugin.call || !plugin.free || !plugin.shutdown) return 1;
  Buffer response{};
  const auto *request = reinterpret_cast<const std::uint8_t *>("{}");
  if (plugin.call("management.register", request, 2, &response) != 0 ||
      !response.ptr || response.len == 0) return 1;
  plugin.free(response.ptr, response.len);
  plugin.shutdown();
  if (dlclose(library) != 0) return 1;
  std::puts("plugin dynamic load and ABI checks passed");
  return 0;
}
