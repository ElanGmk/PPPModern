# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

PPP Modern is a C++20 job management core library (`ppp_core`) being extracted from a legacy VCL application. It provides job lifecycle management, persistence, scheduling, and a CLI tool (`ppp_jobctl`). Targets both Windows (MSVC) and Linux (GCC/Ninja).

## Build Commands

**Windows (MSVC + vcpkg):**
```bash
cmake --preset windows-vcpkg
cmake --build --preset windows-vcpkg-release
ctest --preset windows-vcpkg-release
```

**Linux/WSL (Ninja):**
```bash
cmake -S . -B build -G Ninja -DPPP_BUILD_TESTS=ON -DPPP_ENABLE_SQLITE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

**Multi-config (MSVC without preset):**
```bash
cmake -S . -B build -G"Visual Studio 17 2022" -DPPP_BUILD_TESTS=ON -DPPP_ENABLE_SQLITE=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug
```

**Makefile shortcuts (WSL):** `make configure && make build && make test`

**Format:** `clang-format -i <files>` (config in `.clang-format`, LLVM-based)

## CMake Options

| Option | Default | Purpose |
|---|---|---|
| `PPP_BUILD_TOOLS` | ON | Build `ppp_jobctl` CLI and `ppp_jobviewer` (Win32) |
| `PPP_BUILD_TESTS` | ON | Build test executable, enables CTest |
| `PPP_ENABLE_SQLITE` | ON | SQLite repository backend |
| `PPP_ENABLE_SQLSERVER` | OFF | SQL Server repository via ODBC |
| `PPP_SQLITE_VENDOR` | OFF | Use vendored amalgamation from `third_party/sqlite/` |

## Architecture

All library code lives in namespace `ppp::core`.

**Core domain** (`include/ppp/core/job.h`): `JobState` enum (Submitted → Validating → Rendering → Completed/Exception/Cancelled), `JobPayload` (source path, profile, attachments, tags), `JobRecord` (full persisted state including priority, attempt count, due date, correlation ID).

**Repository layer** (`include/ppp/core/job_repository.h`): Abstract `JobRepository` interface with four implementations:
- `InMemoryJobRepository` — tests and prototyping
- `FileJobRepository` — human-readable directory layout, env var `PPP_JOBCTL_STORE`
- `SqliteJobRepository` — guarded by `#if PPP_CORE_HAVE_SQLITE`, env var `PPP_JOBCTL_SQLITE`
- `SqlServerJobRepository` — guarded by `#if PPP_CORE_HAVE_SQLSERVER`, env var `PPP_JOBCTL_SQLSERVER`

All repositories use the pimpl pattern (`struct Impl`).

**Service layer** (`include/ppp/core/job_service.h`): `JobService` wraps a `JobRepository&` and provides lifecycle transitions, tag/attachment management, scheduling policy application, purge, and an event sink callback.

**Processor** (`include/ppp/core/job_processor.h`): `JobProcessor` implements a pull-based worker loop on top of `JobService`, delegating to a `Handler` callback.

**Scheduling** (`include/ppp/core/scheduling_policy.h`, `scheduling_policy_io.h`): Policy-driven priority escalation based on due-date windows. Policies can be loaded from config files or directories.

**Serialization** (`include/ppp/core/job_serialization.h`): JSON serialization for jobs (used by export/import commands).

**CLI** (`tools/ppp_jobctl.cpp`): Feature-rich CLI exercising all service operations. Repository selection order: SQL Server → SQLite → File → in-memory.

## Testing

Tests are in `tests/` using a custom lightweight assertion framework (no external test library). Registered via CTest. Run a single test with `ctest --test-dir build -R <test_name>`.

## Code Style

- C++20, 4-space indent, 120-char line limit, `snake_case` filenames
- Formatting enforced by `.clang-format` (LLVM base); enable git hook with `git config core.hooksPath .githooks`
- Public headers go in `include/ppp/core/`, internal-only details stay in `src/`
- Commit messages: imperative mood, ≤72 chars, optional scope prefix (`core:`, `tools:`, `tests:`)
