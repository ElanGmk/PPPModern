# Repository Guidelines

## Project Structure & Module Organization
- Source: `src/` (C++20 implementation), headers in `include/ppp/core/` (public API under `ppp::core`).
- Tests: `tests/` (unit/functional test executables registered via CTest).
- Tools: `tools/` (CLI utilities like `ppp_jobctl`).
- Build output: `build/` (out-of-source; CMake-generated files and binaries).

## Build, Test, and Development Commands
- Configure: `cmake -S . -B build -DPPP_BUILD_TESTS=ON -DPPP_ENABLE_SQLITE=ON`
- Build (multi-config, e.g., MSVC): `cmake --build build --config Debug`
- Build (single-config, e.g., Ninja/Make): `cmake --build build`
- Tests: `ctest --test-dir build -C Debug` (omit `-C` for single-config)
- Run tool: Unix `./build/ppp_jobctl`, Windows `build/Debug/ppp_jobctl.exe`
- Optional SQL Server adapter: add `-DPPP_ENABLE_SQLSERVER=ON` (requires an ODBC driver and headers).
- Optional Win32 viewer: `cmake --build build --target ppp_jobviewer` (Windows only) to launch a GUI job summary.

Vcpkg (Windows, optional)
- Manifest provided (`vcpkg.json`). Ensure vcpkg is installed and set `VCPKG_ROOT`.
- Configure with preset: `cmake --preset windows-vcpkg`
- Build: `cmake --build --preset windows-vcpkg-release`
- Test: `ctest --preset windows-vcpkg-release`

WSL/Linux Development
- Prereqs: `sudo apt install -y build-essential cmake ninja-build sqlite3 libsqlite3-dev`.
- SQL Server builds also need `unixodbc-dev` plus the Microsoft ODBC driver (install from Microsoft's package repository).
- Configure (Ninja): `cmake -S . -B build -G Ninja -DPPP_BUILD_TESTS=ON -DPPP_ENABLE_SQLITE=ON`
- Build: `cmake --build build -j`; Test: `ctest --test-dir build`.

Cross-Compile for Windows (WSL)
- Install: `sudo apt install -y mingw-w64`.
- Toolchain: `cmake/toolchains/x86_64-w64-mingw32.cmake`.
- Configure: `cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-w64-mingw32.cmake`.
- SQLite options:
  - System: provide MinGW sqlite dev libs in the sysroot, OR
  - Vendor: place amalgamation at `third_party/sqlite/sqlite3.c` and configure with `-DPPP_SQLITE_VENDOR=ON -DPPP_ENABLE_SQLITE=ON`.

Fetch SQLite Amalgamation (vendor mode)
- Use helper: `bash scripts/fetch_sqlite.sh 2025 3500400` (installs `sqlite3.c/.h` under `third_party/sqlite/`).
- Requires `curl` or `wget` and `unzip` (`sudo apt install -y curl unzip`).

Make Targets (WSL convenience)
- `make vendor-sqlite` — fetches amalgamation (defaults: `SQLITE_YEAR=2025 SQLITE_VER=3500400`).
- `make configure && make build && make test` — Linux build/test with vendor SQLite by default.
- `make win-configure && make win-build` — Windows cross-build via MinGW.

## Coding Style & Naming Conventions
- C++20, modern STL; prefer RAII and `std::` facilities.
- Indentation: 4 spaces, no tabs. Line length ~120.
- Filenames: `snake_case.cpp/.h`; namespaces `ppp::core` for library code.
- Headers in `include/ppp/core/` form the public surface; keep internal-only details in `src/`.
- Formatting: use `clang-format -i` (LLVM base configured in `.clang-format`).

## Testing Guidelines
- Framework: lightweight assertions via custom test executable registered in `CMakeLists.txt` with `add_test` + `ctest`.
- Place tests in `tests/`, name `<area>_tests.cpp` (e.g., `job_service_tests.cpp`).
- Tests must return nonzero on failure and write diagnostics to `stderr`.
- Aim for coverage of parsing, persistence, and service behavior; include boundary/error cases.

## Commit & Pull Request Guidelines
- Commit messages: imperative mood, concise summary (≤72 chars), optional scope: `core:`, `tools:`, `tests:`.
- PRs: clear description, link issues, note build options used, and include before/after behavior or logs. Add tests for new behavior.

Tip: Enable local formatting hook with `git config core.hooksPath .githooks` (requires `clang-format`). On Windows, a PowerShell variant exists at `.githooks/pre-commit.ps1` if you prefer it.

Windows note: A `.githooks/pre-commit.cmd` shim is included to launch the PowerShell hook when committing from `cmd` or PowerShell.

## Configuration & Security Tips
- SQLite support: toggle with `-DPPP_ENABLE_SQLITE=ON/OFF`. Ensure `SQLite3` is discoverable by CMake; on Windows, `sqlite3.dll` must be available at runtime.
- SQL Server support: toggle with `-DPPP_ENABLE_SQLSERVER=ON/OFF`. Supply an ODBC connection string via `PPP_JOBCTL_SQLSERVER`; keep credentials out of logs and scripts.
- Keep public API stable; document any breaking changes in the PR.
