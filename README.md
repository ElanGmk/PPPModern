# PPP Modern Core Prototype

This directory seeds the modernized C++20 codebase for PPP. It introduces a
self-contained job management core that can be compiled on Windows or Linux via
CMake. The goal is to provide a production-quality seam for migrating business
logic away from the legacy VCL application while staying compatible with the
backend/container scenarios described in the modernization plan.

## Layout

- `include/ppp/core` — public C++20 headers for the job model, repository
  abstraction, lifecycle service, and worker coordination helpers.
- `src` — portable implementations with a thread-safe in-memory repository,
  on-disk repository, and orchestration helpers.
- `tools/ppp_jobctl.cpp` — a minimal CLI that exercises the service, provides
  a smoke-test harness for future automation, and now drives a worker loop for
  submitted jobs. Commands can render structured JSON (`--json`) to simplify
  scripting and backend orchestration. The CLI surfaces attempt counts,
  per-job priority, due timestamps, the most recent attempt timestamp, and the
  `retry`, `prioritize`, `reschedule`, `attach`, `detach`, `clear-attachments`,
  `tag`, `untag`, `clear-tags`, and `purge` commands so operators can resubmit,
  resequence, retime, enrich, label, and retire work while preserving audit trails.
  Queue progress snapshots are available via the `report` command, which emits
  aggregated counts, backlog age, and the next due timestamp in human-readable
  or JSON formats for dashboards. The companion `export` command streams the
  same job metadata as JSON to stdout or a file so batch workflows and backend
  services can ingest consistent payloads without scraping console output. Pair
  exports with the `import` command to hydrate another queue from the exported
  payload while preserving priorities, tags, due dates, and correlation IDs.
- `tests` — lightweight regression tests that validate state transitions and
  persistence behaviour.

## Building

```bash
cmake -S XE/PPPModern -B build/pppmodern -DPPP_BUILD_TOOLS=ON -DPPP_BUILD_TESTS=ON
cmake --build build/pppmodern
ctest --test-dir build/pppmodern
```

On Windows workstations, configure the generator explicitly (for example
`-G"Visual Studio 17 2022"`) and the toolchain recommended in the modernization
playbook. The resulting `ppp_core` static library can be linked by the evolving
Qt/WinUI front-end as well as containerized services compiled on Linux.

## Persistent storage

`SqlServerJobRepository` reaches Microsoft SQL Server through ODBC when the
project is built with `-DPPP_ENABLE_SQLSERVER=ON`. Provide an ODBC connection
string via `PPP_JOBCTL_SQLSERVER` to persist submissions in a centralized
database while reusing the same schema as SQLite deployments.

`SqliteJobRepository` stores jobs in a relational SQLite database when
`PPP_ENABLE_SQLITE` is enabled (default). Point the `PPP_JOBCTL_SQLITE`
environment variable at a database file to keep local queues backed by SQLite
while retaining compatibility with future enterprise databases.

`FileJobRepository` persists job metadata to a human-readable directory layout.
Set the `PPP_JOBCTL_STORE` environment variable to point the `ppp_jobctl`
utility at a repository directory so that submissions survive across process
restarts. This remains useful for air-gapped deployments or smoke testing when
database engines are unavailable. The CLI now evaluates `PPP_JOBCTL_SQLSERVER`
first, then `PPP_JOBCTL_SQLITE`, and finally `PPP_JOBCTL_STORE`.

Run `ppp_jobctl migrate --sqlserver "<connection string>"` (or set
`PPP_JOBCTL_SQLSERVER` and omit the argument) to apply SQL Server schema
migrations ahead of rolling out new worker builds. Use
`ppp_jobctl migrate [path]` or configure `PPP_JOBCTL_SQLITE` to upgrade local
SQLite files. Each command emits the applied schema version so Windows operators
can verify database readiness before deploying services onto air-gapped
machines.

## Windows GUI smoke test

Build the optional `ppp_jobviewer` target on Windows (enabled automatically when
`PPP_BUILD_TOOLS=ON`). The Win32 app uses the same repository selection rules as
the CLI and renders a summary of queue counts for quick validation runs. Set
`PPP_JOBCTL_SQLSERVER`, `PPP_JOBCTL_SQLITE`, or `PPP_JOBCTL_STORE` before
launching to point the viewer at a real repository.

## Worker loop prototype

`JobProcessor` wraps `JobService` with a pull-based worker that claims the next
submitted job, marks its lifecycle transitions, and delegates processing to a
callback. The CLI exposes this via `ppp_jobctl run-next`, which emits a
human-readable trace (or machine-readable JSON) for the claimed job and
completes, fails, or cancels it while reporting the number of attempts, the
current priority, the next due timestamp, and the time of the most recent
claim. Use `--resume` to shove any jobs stuck in validating or rendering states
back to the submitted queue before the worker runs. Pair `--continue` with the
command to keep processing until no submitted jobs remain—handy for batch
testing or smoke runs on Windows workstations. JSON output reflects the resume
count and returns an array of processed jobs when `--continue` is enabled.

Repositories now expose an atomic `claim_next_submitted` operation so workers
running across processes or machines can't double-claim the same job. Each
claim clears stale error messages, increments an attempt counter, records the
latest attempt timestamp, and honors descending priority while favouring the
earliest due timestamps so urgent work reaches handlers first. Lifecycle events
remain accurate even when jobs are retried, reprioritized, rescheduled, or
resubmitted through the CLI.

## Attachment management

Use `ppp_jobctl attach <id> <path> [more...]` to append one or more attachments
to an existing job without resubmitting it. The CLI de-duplicates attachment
paths, keeping repeated invocations idempotent and preserving existing
metadata. Remove specific entries via `ppp_jobctl detach <id> <path> [more...]`
or clear the entire list with `ppp_jobctl clear-attachments <id>` when
downstream processors need to reclaim disk space. Complement attachments with
  labels using `ppp_jobctl tag <id> <label> [more...]` to annotate jobs for
  downstream routing or dashboards, `ppp_jobctl untag <id> <label> [more...]` to
  drop specific labels, and `ppp_jobctl clear-tags <id>` to remove all labels in
  one go. Each helper calls into the shared `JobService` primitives so in-memory,
  file-backed, and SQLite repositories stay in sync while emitting lifecycle
  events for audit subscribers. Jobs submitted through the CLI can also include
  labels up front via repeated `--tag` arguments on `ppp_jobctl submit`.

## Correlation metadata

Associate jobs with upstream workflow batches using the new correlation helpers.
Pass `--correlation <id>` to `ppp_jobctl submit` to persist the identifier when a
job is first enqueued, or adjust existing records with
`ppp_jobctl correlate <id> <value|clear>`. Listings accept
`ppp_jobctl list --correlation <id>` (combine with `--tag` to require multiple
filters) so operators can quickly triage the work associated with a particular
ingest batch, customer ticket, or automation run.

## Bulk exports

Produce portable snapshots of the queue with `ppp_jobctl export`. The command
shares the same filters as `list`—state, repeated `--tag` arguments, and
`--correlation`—so operators can scope exports to the jobs relevant for a given
handoff. By default the JSON array is written to stdout, making it trivial to
pipe into PowerShell, `jq`, or log shippers. Append `--output <path>` to persist
the payload to disk; the helper uses the shared JSON serialization helpers, so
air-gapped environments and containerized services both receive identical
schemas when exchanging work between systems.

## Bulk imports

Use `ppp_jobctl import <path|->` to replay a previously exported JSON payload
into the active repository. The command creates fresh job IDs while copying
over the original priority, due timestamp, correlation ID, attachments, and
tags, enabling air-gapped operators to shuttle batches between machines or
seed a staging queue with production data. Add `--json` to obtain a structured
mapping of source IDs to the new job identifiers for downstream automation.

## Retention hygiene

Operators can trim repositories without manual database access using the
`ppp_jobctl purge --state <state> --before <timestamp>` command. The tool
counts (or with `--dry-run`, previews) jobs whose last update predates the
provided UTC timestamp—useful for clearing completed or cancelled work items in
air-gapped deployments. Purges rely on the new `JobService::purge` helper, so
all repository backends honour the same retention logic.

## Queue telemetry

Run `ppp_jobctl report` to emit a consolidated progress snapshot that lists
per-state job counts, highlights the number of outstanding jobs, and surfaces
the oldest backlog entry alongside the next scheduled due timestamp. Append
`--json` when integrating with dashboards or log aggregation pipelines so the
same metrics can be ingested programmatically across Windows workstations and
containerized workers.

## Policy-driven scheduling

When due dates represent customer SLAs, run `ppp_jobctl rebalance` with one or
more `--within-minutes <minutes> <priority>` rules to promote urgent jobs ahead
of the regular queue. Optionally add `--overdue <priority>` to boost any work
that has crossed its deadline; pass `--no-overdue-escalation` to skip the
fallback when you only want the explicit windows applied. The `rebalance`
command calls into `JobService::apply_scheduling_policy`, which enforces the
same escalation strategy regardless of whether the repository is in-memory,
file-backed, or SQLite powered.

Operators can now store the same rules in one or more configuration files and
apply them with `ppp_jobctl rebalance --policy <path>` or by pointing the
command at a directory of layered configurations via `--policy-dir`. Files are
applied in lexicographic order so shared baselines (for example, `00-global`
and `10-site`) can be combined with change-controlled overrides. Configuration
entries follow a simple `key=value` format:

```
# Nightly SLA policy
within-minutes=120 priority=50
within-minutes=60 priority=80
within-minutes=15 priority=95
overdue=120
escalate-overdue=false
```

Lines beginning with `#`, `;`, or `//` are ignored. Each `within-minutes` entry
requires an accompanying `priority` token (separated by spaces or commas).
Policies combine with any command-line `--within-minutes`, `--overdue`, or
`--no-overdue-escalation` arguments so on-call staff can temporarily override
defaults without editing shared files. Directory-based policies inherit those
layering semantics while ensuring that overdue escalation toggles remain
respected even when no additional escalation windows are defined.

## Next steps

- Extend the scheduling policy loader to pull rules from centralized databases
  or configuration services when air-gapped deployments migrate to managed
  infrastructure.
- Harden the SQL Server adapter with connection pooling and retry policies to
  handle transient network failures in managed environments.
- Automate parity tests that validate schema migrations across SQLite and SQL
  Server so future changes remain portable.
