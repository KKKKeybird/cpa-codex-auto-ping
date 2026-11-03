# CLIProxyAPI Codex Auto Ping

A small native plugin for [CLIProxyAPI](https://github.com/router-for-me/CLIProxyAPI) that periodically sends a tiny model request through CLIProxyAPI's own `host.model.execute` callback.

The plugin is implemented in C++ and exposes CLIProxyAPI's language-neutral C ABI. Default Linux release archives are built against Debian Bookworm/glibc for current CLIProxyAPI Docker images. Separate `_musl.zip` archives support Alpine-based images. Both statically link the C++ runtime and avoid embedding a second Go runtime. Do not load a musl archive into a glibc process or vice versa.

It is intended to keep Codex's rolling usage window active with minimal token consumption. The plugin does **not** read, rotate, or refresh OAuth tokens itself.

## Install from the official plugin store

`codex-auto-ping` is listed in the official [CLIProxyAPI Plugins Store](https://github.com/router-for-me/CLIProxyAPI-Plugins-Store).

In CLIProxyAPI Management Center, open the plugin store, search for **Codex Auto Ping**, install it, and enable the plugin. CLIProxyAPI downloads the release asset for the current platform and verifies it using the published checksum.

After installation, apply the configuration below and restart CLIProxyAPI if your deployment does not reload plugins automatically.

## Default behavior

- first ping: 10 seconds after plugin starts
- interval: every 5 hours
- model: `gpt-5.6`
- prompt: `1`
- max output tokens: `1`
- requests per cycle: `1`

## CLIProxyAPI configuration

Choose either example below. Omitting `schedule_mode` defaults to `interval`.

Example 1: run every 5 hours.

```yaml
plugins:
  enabled: true
  dir: "plugins"
  configs:
    codex-auto-ping:
      enabled: true
      priority: 1
      schedule_mode: "interval"
      interval: "5h"
      startup_delay: "10s"
      run_on_start: true
      model: "gpt-5.6"
      prompt: "1"
      max_output_tokens: 1
      pings_per_cycle: 1
      ping_spacing: "3s"
```

Example 2: run at scheduled times.

```yaml
plugins:
  enabled: true
  dir: "plugins"
  configs:
    codex-auto-ping:
      enabled: true
      priority: 1
      schedule_mode: "times"
      schedule_times:
        - "06:55"
        - "12:00"
        - "17:05"
        - "22:10"
      model: "gpt-5.6"
      prompt: "1"
      max_output_tokens: 1
      pings_per_cycle: 1
      ping_spacing: "3s"
```

Daily times use the **CPA process's local timezone**. In Docker, set `TZ=Asia/Shanghai` and ensure the container has timezone data. Status timestamps use UTC (`Z`): Shanghai `06:55` is `22:55Z` on the previous day. Times must be `HH:MM` strings from `00:00` to `23:59`; the list is sorted and deduplicated and must not be empty in `times` mode.

In `times` mode, `interval`, `run_on_start`, and `startup_delay` do not affect scheduling. Startup waits for the next future configured time; missed runs are not replayed. To return to interval mode, set `schedule_mode: interval`, remove `schedule_times`, and restore `interval: 5h`, `run_on_start: true`, and `startup_delay: 10s`.

If your Codex model alias is different, set `model` to the exact model name exposed by your CLIProxyAPI instance.

For multiple Codex accounts, `pings_per_cycle` can be set to the account count when CLIProxyAPI uses round-robin routing. This is currently best-effort; the plugin cannot pin `host.model.execute` to an individual auth ID.

## Build

Linux archive selection:
- `codex-auto-ping_<version>_linux_<arch>.zip`: glibc (Debian Bookworm or newer), also the default plugin-store asset.
- `codex-auto-ping_<version>_linux_<arch>_musl.zip`: musl (Alpine 3.22), install manually in Alpine deployments.

For a reproducible Linux build, use `docker buildx build -f Dockerfile.plugin --build-arg LIBC=glibc --output type=local,dest=dist .`; use `LIBC=musl` for Alpine. The build runs unit tests and a dynamic loading/ABI check. CI also loads each Linux library in its runtime image, including the current CPA Docker image for glibc.

Native builds require CMake and a C++17 compiler:

```bash
make build
```

Linux output:

```text
dist/codex-auto-ping.so
```

Install it in a CLIProxyAPI plugin discovery path, for example:

```bash
mkdir -p /path/to/CLIProxyAPI/plugins/linux/amd64
cp dist/codex-auto-ping.so /path/to/CLIProxyAPI/plugins/linux/amd64/
```

Then restart CLIProxyAPI and enable the plugin configuration above.

## Status endpoint

The plugin registers:

```text
/v0/management/plugins/codex-auto-ping/status
```

The endpoint is protected by CLIProxyAPI's Management API authentication and returns JSON containing the last attempt, last success, last error, counters, and next scheduled run. It does not register any unauthenticated routes under `/v0/resource/plugins/`.

## Current limitation

The plugin deliberately uses CLIProxyAPI's normal model scheduler. It therefore cannot guarantee exactly one ping per individual Codex credential.

A future version can use CLIProxyAPI's `host.auth.*` callbacks together with credential-specific execution if/when auth pinning is available to `host.model.execute`.

## License

MIT
