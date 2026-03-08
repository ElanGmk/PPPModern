.PHONY: help vendor-sqlite configure build test clean win-configure win-build win-clean fmt

# Defaults (override on command line):
SQLITE_YEAR ?= 2025
SQLITE_VER  ?= 3500400
ENABLE_SQLITE ?= ON
SQLITE_VENDOR ?= ON

help:
	@echo "Common targets:"
	@echo "  make vendor-sqlite      # fetch sqlite3.c/.h into third_party/sqlite"
	@echo "  make configure          # CMake configure (Linux, Ninja)"
	@echo "  make build              # Build (Linux)"
	@echo "  make test               # Run tests (Linux)"
	@echo "  make win-configure      # Configure Windows cross-build (MinGW)"
	@echo "  make win-build          # Build Windows cross artifacts"
	@echo "  make fmt                # clang-format (if available)"
	@echo "  make clean / win-clean  # remove build dirs"

vendor-sqlite:
	@mkdir -p third_party/sqlite
	@SQLITE_YEAR=$(SQLITE_YEAR) SQLITE_VER=$(SQLITE_VER) bash scripts/fetch_sqlite.sh || true

configure:
	@cmake -S . -B build -G Ninja -DPPP_BUILD_TESTS=ON -DPPP_ENABLE_SQLITE=$(ENABLE_SQLITE) -DPPP_SQLITE_VENDOR=$(SQLITE_VENDOR)

build:
	@cmake --build build -j

test:
	@ctest --test-dir build --output-on-failure || true

clean:
	@rm -rf build

win-configure:
	@cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-w64-mingw32.cmake -DPPP_BUILD_TESTS=ON -DPPP_ENABLE_SQLITE=$(ENABLE_SQLITE) -DPPP_SQLITE_VENDOR=$(SQLITE_VENDOR)

win-build:
	@cmake --build build-win -j

win-clean:
	@rm -rf build-win

fmt:
	@command -v clang-format >/dev/null 2>&1 && \
	  clang-format -i $$(rg --files -g '!build' -g '!build-win' -e '\\.(c|cc|cpp|h|hh|hpp)$$') || \
	  echo "clang-format not found; skipping"

