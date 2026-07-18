# AOSP Federated Graph Contract

This contract defines workspace-level symbol edges that cross AOSP manifest
repository boundaries. Repository-local nodes and edges remain owned by their
shard databases. Cross-repository edges are owned by the workspace Master.

## Identity

Every edge has a deterministic 64-character SHA-256 ID derived from:

1. workspace ID;
2. source symbol global ID;
3. resolved target symbol global ID, or unresolved target name;
4. edge type.

Status, confidence, evidence, generation, and properties are mutable attributes
and do not change edge identity. Repeating the same refresh therefore cannot
create duplicate edges. Workspace and symbol IDs are root-scoped, so identical
source trees in different checkout roots do not share edge IDs.

## Ownership

`source_repo_id` identifies the repository that owns refresh and invalidation for
an edge. A resolved or ambiguous target must be a cataloged symbol in a different
repository in the same workspace. Local edges remain in the source shard and are
rejected by this API.

## Status

| Status | Target global ID | Target repository | Target name | Meaning |
|---|---|---|---|---|
| `resolved` | required | required | retained | One supported target was selected. |
| `ambiguous` | required | required | retained | This row is one evidence-backed candidate in an unresolved candidate set. |
| `unresolved` | null | null | required | No supported catalog target was selected. |

Ambiguous candidates are stored as separate rows. Consumers must not silently
choose one of them. Unresolved rows must not use synthetic values in
`target_global_id`.

## Edge Types

The initial contract accepts these symbol-to-symbol relationships:

- `IMPORTS`
- `INCLUDES`
- `USES_TYPE`
- `EXTENDS`
- `IMPLEMENTS`
- `ANNOTATED_BY`
- `CALLS`
- `USAGE`

Adding a type requires updating the public contract and validation tests before a
producer emits it.

## Evidence

Every candidate requires a confidence in the inclusive range `0.0..1.0` and a
non-empty evidence value. Producers use evidence to identify the resolver rule,
not to store an unsupported conclusion. Additional structured facts may be stored
in `properties` as JSON.

The structural producer records `resolution`, numeric `score`, and
`candidate_count` in `properties`. Exact qualified names and qualified suffixes
have higher confidence than unique unqualified short names. A qualified reference
never falls back to a short-name-only target when its qualifier does not match.
Multiple best candidates are retained as separate `ambiguous` rows with the same
candidate count; no consumer may promote one of those rows to `resolved`.

## Refresh Lifecycle

`cbm_aosp_cross_edges_refresh` atomically replaces all outgoing cross-repository
edges owned by one source repository:

1. open the workspace Master and begin an immediate transaction;
2. verify that the source repository belongs to the workspace;
3. delete the source repository's previous cross edges;
4. validate every source, target, status, type, confidence, and evidence value;
5. insert the deterministic replacement set;
6. calculate committed status counts and commit.

An empty replacement set is valid and removes stale edges. Any validation or
database error rolls back the delete and all inserts. Refreshing one repository
does not delete edges owned by another repository.

Repository catalog refreshes enqueue only affected source repositories: the
reindexed source itself, sources with edges targeting it, and sources whose
unresolved names may match its new catalog. `aosp federate` scans queued or
never-refreshed repositories only. A successful per-repository refresh records
refresh state and clears its queue entry in the same transaction; failures retain
the entry for retry and record the latest error. A later successful refresh clears
both the queue entry and failure record atomically. Unqueued repository edges and
refresh state remain unchanged.

`aosp status` and the read-only MCP `aosp_get_status` tool expose the same coverage
dimensions: total/present/missing/indexed/failed repositories; total/resolved/
ambiguous/unresolved cross edges; queued stale repositories; edges owned by those
stale repositories; and repositories whose latest refresh failed. A stale edge is
the previously committed edge of a queued source repository. It remains queryable
until that repository refresh succeeds and atomically replaces its outgoing edges.

`source_generation` records the repository generation observed for the refresh.
Content-aware invalidation and dependency propagation are implemented by later
tasks in the AOSP support plan.

The structural producer uses each indexed shard's File nodes as its work boundary,
replays the existing AST extractor for those files, and resolves only targets in a
different manifest repository. A best local target suppresses Master edge creation;
that relationship remains owned by the repository shard. Run `aosp federate` only
after all desired repository shards have been indexed or refreshed.

Call collection prefers a type-aware `CBMResolvedCall` when it matches the AST call
site and falls back to the textual AST callee otherwise. C++ `::`/`->` and Rust `::`
qualified references are normalized to the catalog's dotted qualified-name form.
`CALLS` targets are restricted to functions and methods; `USAGE` targets are
restricted to callable, type-like, variable, field, and macro definitions. Calls
and usages that have no supported catalog target remain explicit unresolved rows.

## Symbol Resolution

`cbm_aosp_resolve_symbol` resolves one workspace Master-catalog reference using
deterministic tiers, in order:

1. exact global symbol ID;
2. exact qualified name;
3. qualified-name suffix at a component boundary;
4. exact unqualified name.

C++/Rust `::`, C++ `->`, and path separators are normalized to qualified-name
components. A reference containing a qualifier never falls back to an unrelated
short-name match. Only candidates from the highest matching tier are returned. A
single candidate is `RESOLVED`; multiple best-tier candidates are `AMBIGUOUS`; no
candidate is `NOT_FOUND`. Duplicate exact qualified names across repositories are
therefore ambiguous rather than selected by repository order.

Each candidate carries its global ID, repository ID and path, shard project name,
local node ID, label, language, qualified name, file, and source range. Results are
ordered by qualified name, repository path, and global ID. At most 200 candidates
are materialized; `total_candidate_count` and `truncated` preserve explicit
ambiguity when a common short name has more matches. The resolver is the Q1
contract used by shard routing and traversal and is exposed by the Q6 CLI/MCP
workspace query surfaces.

## Shard Routing

`cbm_aosp_shard_route` routes a Master global symbol ID to the repository shard
that owns it. The Master `symbols` table provides the `repo_id` and
`local_node_id`; the Master `repos` table provides the shard `db_path` and
`status`. A route is opened only when the source repository is `indexed` and
has a non-empty `db_path`. The shard database is opened read-only and must be
closed with `cbm_aosp_shard_route_close`.

`cbm_aosp_shard_route_symbol` is a convenience entry point that accepts a Q1
resolved `cbm_aosp_symbol_t` directly, skipping the Master global-ID lookup. It
still queries the Master `repos` table for the shard path.

`cbm_aosp_shard_read_node` reads the routed node from its shard's `nodes` table
by `local_node_id`, returning name, qualified name, label, file path, and line
range. The Master-level `global_id` and `repo_id` are carried from the route.

`cbm_aosp_shard_read_edges` reads edges from the routed node's shard in a given
direction (`OUTGOING` or `INCOMING`). For outgoing edges, the neighbor is the
target node; for incoming edges, the neighbor is the source node. Each result
carries the edge type, properties, and the neighbor's local node ID, name,
qualified name, label, and file path. This is the internal Q2 contract used by
later cross-shard traversal tasks (Q3).

## Trace Path Traversal

`cbm_aosp_trace_path` traverses the federated graph starting from a Master
global symbol ID using breadth-first search. It follows local shard edges
(via Q2 shard routing) and resolved cross-repository edges (from the Master
`cross_symbol_edges` table) up to a configurable depth.

Traversal controls:

- **max_depth**: maximum hops from the start node (0 = start only, -1 = unlimited)
- **direction**: `OUTGOING`, `INCOMING`, or `BOTH`
- **result_budget**: maximum nodes to return (0 = unlimited); sets `truncated`
  when hit before exhaustion
- **cancel_flag**: optional pointer to a volatile bool; traversal stops early
  when set to true
- **cycle detection**: a visited set keyed by global symbol ID prevents revisiting
  nodes across both local and cross-repository hops

Each result node carries the symbol's global ID, repository ID, qualified name,
label, file path, start line, the edge type and evidence that led to it, the edge
confidence (1.0 for local edges, Master-stored confidence for cross-repository
edges), the hop depth from start, and a `cross_repo` flag indicating whether the
hop crossed a repository boundary. The start node has a NULL edge type, NULL
evidence, 0.0 confidence, depth 0, and `cross_repo` false.

This is the internal Q3 contract used by later federated query tasks (Q4).

## Federated Query Graph

`cbm_aosp_query_graph` executes a multi-hop pattern query starting from a code
symbol. Each hop in the pattern specifies a relation kind, direction, and
optional edge type filter. The query traverses local shard edges, cross-
repository edges, build module dependencies, and protocol edges in a single
BFS pass.

Supported hop kinds and transitions:

| Current node | Hop kind | Action |
|---|---|---|
| SYMBOL | SYMBOL | Follow code edges (local shard + cross-repository) |
| SYMBOL | MODULE | Find containing module(s) by matching file path |
| SYMBOL | PROTOCOL | Find linked protocol node(s) by `symbol_global_id` |
| MODULE | MODULE | Follow resolved `module_dependencies` |
| PROTOCOL | PROTOCOL | Follow `protocol_edges` |

Each result node carries its node ID (global_id, module_id, or protocol_id),
kind, repository ID, name, qualified name, file path, the edge type and
confidence that led to it, the hop index in the pattern, and a `cross_repo`
flag. The start node has a NULL edge type, 0.0 confidence, hop index 0, and
`cross_repo` false.

Cycle detection uses a visited set keyed by `"K:node_id"` where K is the node
kind prefix (S/M/P), preventing revisits across different relationship types.
The result budget provides early termination with a `truncated` flag.

This Q4 contract is exposed by the Q6 CLI/MCP workspace query surfaces.

## Workspace Source Snippets

`cbm_aosp_read_source_snippet` accepts an exact Master `global_id`, including the
IDs returned by `cbm_aosp_search_symbols`, and returns the symbol's inclusive
source range from its owning manifest repository. The result carries the
authoritative shard node, repository-relative and workspace-relative file paths,
the canonical absolute path, and the verbatim source bytes for `start_line`
through `end_line`.

The reader resolves the global ID through the Master, opens the indexed shard
read-only, and re-reads the local node before opening source. A qualified-name or
label mismatch between Master and shard is reported as a stale catalog instead of
returning source for a different node that reused the local ID. File paths are
canonicalized and must remain beneath the owning repository root, so a resolved
path outside that repository cannot expose another repository or workspace file.
Missing repositories, unindexed or unreadable shards, absent files, invalid or
outdated line ranges, binary NUL bytes, and oversized snippets are explicit
errors; the API never substitutes a guessed range or partial source.

This Q5 contract is exposed by the Q6 CLI/MCP workspace query surfaces.

## Public Workspace Query Contracts

Q6 exposes Q1-Q5 through AOSP-specific names, leaving the existing single-project
`query_graph`, `trace_path`, and `get_code_snippet` tools unchanged.

CLI commands:

```text
codebase-memory-mcp aosp resolve <reference> [root]
codebase-memory-mcp aosp snippet <global-id> [root]
codebase-memory-mcp aosp trace <reference> [root] [--depth N] [--direction outgoing|incoming|both] [--limit N]
codebase-memory-mcp aosp query <reference> [root] --hop kind:direction[:edge-type] [--hop ...] [--limit N]
```

MCP tools:

| Tool | Contract |
|---|---|
| `aosp_resolve_symbol` | Returns deterministic resolution status, match tier, truncation, and all best-tier candidates. |
| `aosp_get_source_snippet` | Accepts an exact search/resolution `global_id` and returns the verified source range. |
| `aosp_trace_path` | Accepts a global ID or unambiguous reference and returns bounded code traversal with evidence. |
| `aosp_query_graph` | Accepts a global ID or unambiguous reference plus ordered symbol/module/protocol hops. |

`aosp_search_symbols` now includes `global_id` and `repo_id` in every result, so a
caller can pass a search hit directly to `aosp_get_source_snippet`. Trace and query
starts may also use qualified references, but ambiguous references fail with an
explicit instruction to call `aosp_resolve_symbol`; no candidate is selected by
repository order. The AOSP-specific tools are read-only and do not alter the
behavior or schemas of single-project callers.

## Schema Compatibility

Master schema v4 introduces the structured edge identity and status fields. On
initialization, the v3 resolved-only table is migrated transactionally. Cataloged
legacy targets remain resolved; missing legacy targets become unresolved records
whose original target ID is retained as the target name and evidence.

Master schema v5 adds the indexed `target_leaf`, per-source refresh queue, and
refresh state. Existing v4 databases gain the leaf column in place; legacy rows
use the compatibility matcher until their source repository is refreshed, after
which the indexed leaf is populated.

Master schema v6 adds `cross_edge_refresh_failures`, keyed by workspace and source
repository. The row retains the latest error and failure timestamp for status
reporting while the refresh queue remains the source of retry work.

Master schema v7 adds the indexed `symbols.qualified_leaf` resolver key. Existing
symbol rows are backfilled from their qualified names before the version is
recorded. Resolver candidate lookup uses this index and then applies exact
component-boundary matching in memory, avoiding a full symbol-table suffix scan.
