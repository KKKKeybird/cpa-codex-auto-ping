PLUGIN_NAME ?= codex-auto-ping
BUILD_DIR ?= dist

.PHONY: build test lint clean

build:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
	cmake --build build --config Release
	cmake -E make_directory $(BUILD_DIR)
	cmake -E copy_if_different build/$(PLUGIN_NAME).* $(BUILD_DIR)/

test:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
	cmake --build build --config Debug
	ctest --test-dir build --output-on-failure -C Debug

lint:
	clang-format --dry-run --Werror src/plugin.cpp tests/plugin_test.cpp

clean:
	rm -rf $(BUILD_DIR)
	rm -rf build
