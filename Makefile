PLUGIN_NAME ?= codex-auto-ping
BUILD_DIR ?= dist
OS ?= $(shell uname -s | tr '[:upper:]' '[:lower:]')

EXT_linux = so
EXT_darwin = dylib
EXT_windows_nt = dll
PLUGIN_EXT = $(or $(EXT_$(OS)),so)
PLUGIN_OUTPUT ?= $(BUILD_DIR)/$(PLUGIN_NAME).$(PLUGIN_EXT)

.PHONY: build test lint clean

build:
	mkdir -p $(dir $(PLUGIN_OUTPUT))
	cargo build --locked --release
	cp target/release/libcodex_auto_ping.$(PLUGIN_EXT) $(PLUGIN_OUTPUT)

test:
	cargo test --locked

lint:
	cargo fmt --check
	cargo clippy --locked --all-targets -- -D warnings

clean:
	cargo clean
	rm -rf $(BUILD_DIR)
