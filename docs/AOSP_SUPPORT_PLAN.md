# AOSP Support Execution Plan

This document is the single progress baseline for AOSP support. Work is performed
in task ID order unless a dependency recorded here requires a different order.

## Scope

"Complete AOSP support" means that codebase-memory-mcp can discover a repo-based
AOSP checkout, index it as a federated graph, resolve and traverse cross-repository
code relationships, model the main build and Android IPC boundaries, answer
evidence-backed architecture questions, and operate reliably at full-checkout
scale.

It does not mean replacing Soong, Kati, Bazel, the Android compiler toolchain, or
runtime instrumentation. Static analysis limitations must be visible as unresolved
records or coverage metrics instead of being silently treated as resolved facts.

## Progress Rules

- `[ ]` means pending. `[x]` means verified and archived.
- Only one task may be in implementation at a time.
- A task is marked `[x]` only after its acceptance criteria pass.
- Completing a parent milestone does not automatically complete its children.
- A failed or partial verification leaves the task unchecked.
- Every completed task must add an entry to the evidence ledger.
- Scope changes must update this plan before implementation begins.

## Completion Gate

Every implementation task must satisfy all applicable checks before it is marked
complete:

1. Implementation and public contracts are complete.
2. Focused unit and integration tests pass under ASan/UBSan.
3. The production binary builds with warnings treated as errors.
4. A production-binary smoke test exercises the new path.
5. Error, ambiguity, stale-state, and partial-workspace behavior is tested.
6. Documentation and MCP/CLI schemas match actual behavior.
7. `git diff --check` passes and the task is archived in a dedicated commit.

## Current Queue

- Current task: `Q7` - complete federated query-plane fault and mixed-path coverage.
- Next task: `B1` - evaluate Blueprint literal variables, list concatenation, and references.
- Deferred verification:
  - `Q5` implementation, focused Windows tests, production `-Werror` build, and CLI
    smoke pass; Linux ASan/UBSan execution remains required before archival.
  - `Q6` CLI/MCP implementation, 18 AOSP tests, 159 MCP tests, production `-Werror`
    build, and production CLI/MCP smoke pass; Linux ASan/UBSan execution remains
    required before archival.
- Worktree baseline: Q4 query_graph commit on `codex/aosp-federated-graph`.

## Verified Baseline

### W - Workspace Federation

- [x] `W1` Parse `.repo/manifest.xml`, included manifests, and local manifests.
- [x] `W2` Apply `remove-project` and `extend-project` semantics safely.
- [x] `W3` Generate root-scoped stable workspace and repository IDs.
- [x] `W4` Create per-repository graph shards and a workspace Master database.
- [x] `W5` Catalog definition symbols from shards for global lookup.
- [x] `W6` Provide `aosp init/index/status/repos/search` commands.
- [x] `W7` Provide the `aosp_search_symbols` MCP tool.

### B0 - Baseline Build Graph

- [x] `B0.1` Parse literal module declarations from `Android.bp`.
- [x] `B0.2` Parse common module declarations from `Android.mk`.
- [x] `B0.3` Discover AIDL declarations as build modules.
- [x] `B0.4` Resolve module dependencies across repository boundaries.
- [x] `B0.5` Retain unresolved dependency declarations.
- [x] `B0.6` Provide `aosp build/modules` and `aosp_get_architecture`.

### P0 - Baseline Protocol Graph

- [x] `P0.1` Parse AIDL interfaces and methods.
- [x] `P0.2` Link AIDL methods to native `Bn` and `Bp` owners.
- [x] `P0.3` Link AIDL methods to Java `Stub` and `Proxy` owners.
- [x] `P0.4` Link static JNI exported names.
- [x] `P0.5` Link dynamic `JNINativeMethod` tables with source-file evidence.
- [x] `P0.6` Store confidence and evidence on protocol edges.
- [x] `P0.7` Provide `aosp link/protocols` and `aosp_trace_protocol`.

## Remaining Work

### F - Federated Cross-Repository Code Graph

- [x] `F1` Define the `cross_symbol_edges` contract, identity rules, supported edge
  types, evidence fields, unresolved representation, and refresh lifecycle.
  Acceptance: schema/API tests prove deterministic IDs, workspace isolation,
  idempotent refresh, and stale-edge deletion for one refreshed repository.
- [x] `F2` Collect cross-repository import, include, type-reference, inheritance,
  implementation, and annotation candidates from repository shards.
  Acceptance: a multi-repository fixture produces each supported candidate class
  without creating edges for local-only relationships.
- [x] `F3` Resolve cross-repository calls and usages to definition symbols.
  Acceptance: C/C++, Java, Kotlin, and Rust fixtures cover direct, qualified, and
  unresolved references across repositories.
- [x] `F4` Add ambiguity handling, confidence scoring, and evidence retention.
  Acceptance: duplicate-name fixtures never produce an unsupported name-only edge;
  ambiguous candidates remain queryable.
- [x] `F5` Implement per-repository cross-edge refresh and dependency invalidation.
  Acceptance: reindexing one repository updates affected edges without rebuilding
  unrelated workspace state.
- [x] `F6` Report cross-edge coverage, unresolved counts, and resolution errors.
  Acceptance: CLI and MCP status distinguish indexed, resolved, ambiguous,
  unresolved, stale, and failed state.

### Q - Federated Query Plane

- [x] `Q1` Implement a workspace-level symbol resolver with exact, qualified,
  suffix, and explicit ambiguity results.
- [x] `Q2` Implement shard routing from Master symbol IDs to repository databases.
- [x] `Q3` Implement cross-shard `trace_path` traversal with depth, direction,
  cycle, result-budget, and cancellation controls.
- [x] `Q4` Implement a read-only federated `query_graph` surface for supported
  multi-hop patterns across code, module, and protocol relationships.
- [ ] `Q5` Route workspace search results to exact source snippets.
- [ ] `Q6` Add workspace-aware CLI and MCP query contracts without breaking
  single-project callers.
- [ ] `Q7` Test mixed local and cross-repository paths, ambiguous starts, missing
  shards, stale catalogs, and partial workspaces.

Acceptance for `Q1-Q7`: a query beginning in one manifest repository must locate
source and traverse through at least two other repositories, returning repository,
file, line, edge type, confidence, and evidence at every boundary.

### B - Complete Build and Product Semantics

- [ ] `B1` Evaluate Blueprint literal variables, list concatenation, and references.
- [ ] `B2` Expand `defaults` inheritance with provenance and cycle detection.
- [ ] `B3` Model target, arch, multilib, product-variable, and configurable variants.
- [ ] `B4` Model `soong_namespace`, imports, visibility, and package boundaries.
- [ ] `B5` Model filegroups, genrules, generated sources, tools, and output tags.
- [ ] `B6` Expand common Android.mk variables, conditions, includes, macros, and
  module-class semantics without pretending to be a complete Kati evaluator.
- [ ] `B7` Parse product makefiles, `PRODUCT_PACKAGES`, inheritance, BoardConfig,
  device, vendor, and partition ownership.
- [ ] `B8` Import supported Bazel mixed-build module and dependency metadata.
- [ ] `B9` Link build modules to source files, generated files, and definition symbols.
- [ ] `B10` Expose dependency edges, variants, unresolved expressions, and provenance
  through CLI and MCP queries.

Acceptance for `B1-B10`: golden fixtures and selected real AOSP modules must match
Soong-visible module names and direct dependencies for the supported subset; all
unevaluated constructs must be counted and returned as coverage gaps.

### P - Complete Android Protocol and System Boundaries

- [ ] `P1` Support AIDL imports, annotations, parcelables, unions, enums, callbacks,
  one-way methods, and declared stability.
- [ ] `P2` Link generated Binder transaction constants, `onTransact`, proxy transact
  calls, and implementation methods.
- [ ] `P3` Link ServiceManager registration, lookup, wait, client, and server paths.
- [ ] `P4` Cover Java, C++/NDK, and Rust AIDL generated naming conventions.
- [ ] `P5` Decode JNI overload/signature encodings and common registration helpers.
- [ ] `P6` Model HIDL and HwBinder interfaces, clients, and services.
- [ ] `P7` Parse VINTF manifests and compatibility matrices and link HAL instances.
- [ ] `P8` Parse `init.rc` services and connect binaries, interfaces, and startup
  triggers.
- [ ] `P9` Model binder callbacks, death recipients, and asynchronous direction.
- [ ] `P10` Publish per-protocol coverage and unresolved evidence.

Acceptance for `P1-P10`: golden paths must cover Java Binder, NDK Binder, Rust AIDL,
JNI, HIDL/HwBinder, and a VINTF HAL from declaration through client and service
implementation, with confidence and evidence on every inferred edge.

### A - Architecture and Problem Queries

- [ ] `A1` Return actual module dependency edges, reverse dependencies, and unresolved
  declarations instead of counts alone.
- [ ] `A2` Add dependency-path, cycle, hotspot, ownership, partition, and layer queries.
- [ ] `A3` Add impact analysis spanning source symbols, modules, protocols, services,
  products, and repositories.
- [ ] `A4` Generate deterministic architecture summaries with source evidence and
  invalidation hashes.
- [ ] `A5` Add structured problem-query intents for ownership, callers, service path,
  build inclusion, dependency reason, and change impact.
- [ ] `A6` Return coverage and uncertainty alongside every architecture answer.
- [ ] `A7` Add CLI/MCP end-to-end tests for each query intent.

Acceptance for `A1-A7`: every answer must identify the traversed nodes and edges,
cite repository/file evidence, expose missing coverage, and be reproducible without
LLM-generated facts being stored as graph truth.

### O - Scale, Incrementality, and Operations

- [ ] `O1` Add bounded parallel repository indexing with CPU and memory budgets.
- [ ] `O2` Add resumable jobs, checkpoints, retry policy, and per-repository failure
  isolation.
- [ ] `O3` Track manifest revision and content fingerprints for incremental refresh.
- [ ] `O4` Make build, cross-edge, and protocol linking incrementally refreshable.
- [ ] `O5` Use snapshot-friendly transactions so queries remain available during
  long scans and refreshes.
- [ ] `O6` Add schema migration, cache compatibility, cleanup, and corruption recovery.
- [ ] `O7` Add progress, duration, throughput, cache size, coverage, and error metrics.
- [ ] `O8` Add cancellation, file-size, recursion, result, and resource-budget controls.
- [ ] `O9` Add an idempotent orchestration command for discover, index, build, link,
  validate, and resume.
- [ ] `O10` Test interrupted runs, changed manifests, missing repositories, read-only
  queries, database contention, and low-resource operation.

Acceptance for `O1-O10`: an interrupted full-checkout run must resume without
reindexing successful unchanged repositories; readers must continue to return the
last committed snapshot while refresh work is running.

### V - Full AOSP Validation and Release

- [ ] `V1` Build a larger synthetic multi-repository correctness corpus.
- [ ] `V2` Pin representative AOSP platform and vendor revision manifests for
  repeatable external acceptance runs.
- [ ] `V3` Define approved time, memory, disk, query-latency, and coverage budgets.
- [ ] `V4` Run a clean full-checkout index and record repository/file/node/edge counts.
- [ ] `V5` Run golden global symbol, call-path, build, protocol, HAL, and impact queries.
- [ ] `V6` Validate incremental refresh after source, build, protocol, and manifest
  changes.
- [ ] `V7` Run fault-injection, resume, migration, and concurrent-reader tests.
- [ ] `V8` Run the complete sanitizer suite and production-binary CLI/MCP suite.
- [ ] `V9` Publish the AOSP operation, troubleshooting, sizing, backup, and upgrade
  documentation.
- [ ] `V10` Archive the final evidence report and release-ready commit.

The project may be described as having complete AOSP support only after `F`, `Q`,
`B`, `P`, `A`, `O`, and `V` are fully checked and the final evidence report has no
unaccepted critical coverage gap.

## Evidence Ledger

| Tasks | Evidence | Verification |
|---|---|---|
| `W1-W7` | `b998b08` | AOSP manifest, catalog, CLI, and MCP focused tests |
| `B0.1-B0.6` | `f0863a6` | Cross-repository module fixture and sanitizer suite |
| `P0.1-P0.7` | `d57b988` | Binder/JNI fixture, 173 focused tests, production smoke |
| `F1` | `21e7483` | v3 migration, deterministic/isolated refresh tests, 176 focused tests, production schema v4 smoke |
| `F2` | `d53eb01` | Six structural edge classes, local-edge exclusion, 177 focused tests, production `aosp federate` smoke |
| `F3` | `0b1774e` | C/C++, Java, Kotlin, and Rust calls/usages; local, ambiguous, unresolved coverage; 177 focused tests; production two-repository federation smoke |
| `F4` | `60dc77b` | Qualified/short-name confidence tiers, structured candidate evidence, qualifier-mismatch rejection, duplicate-name ambiguity fixture, 177 focused tests, production three-state smoke |
| `F5` | `c57883b` | Schema v5 refresh queue/state and indexed dependency leaf, v3/v4 migrations, three-repository selective refresh, retry retention, 178 focused tests, production incremental federation smoke |
| `F6` | `4c3b29d` | Schema v6 refresh-failure retention, CLI/MCP coverage parity, stale edge reporting, failed-refresh retry cleanup, 178 focused tests, production `-Werror` build and dual-entry status smoke |
| `Q1` | `552dca2` | Deterministic global-ID/QN/suffix/short-name resolver, explicit ambiguity and bounded candidate reporting, schema v7 indexed leaf migration, mixed C++/Rust separator normalization, 179 focused tests, production `-Werror` build and real C++ shard/index-plan smoke |
| `Q2` | `0eee37b` | Master-to-shard routing by global_id and resolved symbol, read-only shard node/edge reads with outgoing/incoming direction, missing-symbol and unindexed-repo error paths, 180 focused tests, production `-Werror` build |
| `Q3` | `dc5cdbb` | BFS cross-shard trace_path with depth/direction/cycle/budget/cancel controls, local shard edges and resolved cross-repository edges, incoming/outgoing/both traversal, 181 focused tests, production `-Werror` build |
| `Q4` | `5d6b4cb` | Multi-hop federated query_graph across code/module/protocol relationships, SYMBOL→SYMBOL/MODULE/PROTOCOL transitions, module dependency and protocol edge traversal, edge-type filter, result budget, 182 focused tests, production `-Werror` build |
