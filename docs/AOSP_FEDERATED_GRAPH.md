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
evidence and confidence that led to it, the hop index in the pattern, and a
`cross_repo` flag. Local code edges use `local` evidence; cross-repository and
protocol edges retain stored evidence; module and symbol-to-protocol transitions
return their Master-table provenance. The start node has NULL edge type/evidence,
0.0 confidence, hop index 0, and `cross_repo` false.

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
| `aosp_query_graph` | Accepts a global ID or unambiguous reference plus ordered symbol/module/protocol hops and returns evidence at each boundary. |

`aosp_search_symbols` now includes `global_id` and `repo_id` in every result, so a
caller can pass a search hit directly to `aosp_get_source_snippet`. Trace and query
starts may also use qualified references, but ambiguous references fail with an
explicit instruction to call `aosp_resolve_symbol`; no candidate is selected by
repository order. The AOSP-specific tools are read-only and do not alter the
behavior or schemas of single-project callers.

## Query-Plane Fault Coverage

Q7 uses a four-entry manifest fixture with three indexed repositories and one
missing repository. Its golden path starts in alpha, follows a local edge, crosses
to beta, follows a beta-local edge, and crosses again to gamma. Both `trace_path`
and `query_graph` must return the repository, file, line, edge type, confidence,
and evidence for all four boundaries. The same fixture verifies that ambiguous
public starts fail without candidate selection, a missing shard rejects exact
source reads, a Master/shard identity mismatch reports a stale catalog, and status
retains the partial workspace's present/indexed/missing counts.

## Build Namespace and Visibility Boundaries

Blueprint modules carry their workspace-relative package and nearest enclosing
`soong_namespace`. Unqualified dependencies resolve in deterministic tiers: the
current namespace, imported namespaces, then the global namespace. Explicit
`//namespace:module` references bypass those tiers. Multiple candidates in the
selected tier remain unresolved as ambiguous; no repository-order fallback is
allowed.

Module visibility is taken from the module declaration, or from the closest
ancestor package with `default_visibility`, with Soong's legacy-public default
when neither is present. Public, private, `__pkg__`, and `__subpackages__` rules
are evaluated against workspace-relative package paths. A blocked or unsupported
visibility expression retains the declared dependency and structured failure
evidence but does not create a `module_edge`.

Master schema v8 stores explicit namespace declarations and imports in
`build_namespaces`, and every discovered Blueprint package boundary plus its
declared default visibility in `build_packages`. Module properties retain the
effective namespace, imports, package, visibility, and visibility origin;
dependency properties retain the resolution tier, candidate count, target
namespace, matching visibility rule, or failure reason.

## Generated Build Inputs and Outputs

Filegroup and genrule modules remain ordinary build modules, but their literal
file declarations are stored separately from module dependencies. `srcs` paths,
genrule `out` entries, and literal `tool_files` become `SOURCE`, `OUTPUT`, and
`TOOL_FILE` rows in `module_files`. Module references in those properties become
`SOURCE`, `FILEGROUP_INPUT`, `GENRULE_INPUT`, or `TOOL_FILE` dependency edges.
Generated sources, generated headers, exported generated headers, and tools use
dedicated module-edge types.

References beginning with `:` or an explicit namespace may select a tagged output,
for example `:generator{.header}`. Resolution uses the normalized module name,
while dependency properties retain every declared spelling and output tag.
Literal file declarations and tagged dependencies inherited through defaults keep
the same variant and inheritance provenance as other build relationships.

Master schema v9 adds `module_files`, keyed by module, declared path, and role.
This table remains the declaration layer; schema v13 adds the resolved links
described below without changing the original evidence.

## Build Files, Outputs, And Definitions

B9 resolves module definition files and literal `SOURCE` and `TOOL_FILE`
declarations against the Master symbol catalog. Declared paths are relative to
the module's build-file directory; a filegroup's `path` is applied before its
source paths. `.` and `..` components are normalized lexically, but a path that
escapes its manifest project, is absolute, or retains a Make expression is
rejected as `invalid_path`.

A physical path links to a catalog `File` only when the repository is indexed and
there is exactly one `File` row with the same repository-relative path. The
stored states distinguish `resolved`, `file_not_found`, `repository_unindexed`,
`symbol_not_found`, `ambiguous`, and `invalid_path`. Each resolved source/tool
file links the module to all definition symbols cataloged in that exact file.
Build-file symbols are narrower: the symbol must also have the module's exact
Soong-visible name, preventing unrelated declarations in the same `Android.bp` or
`Android.mk` from being attached to the module.

Every `OUTPUT` declaration receives a stable generated-file ID scoped by the
workspace, producer module, and normalized output path. A resolved module
dependency can link to one of those outputs. An empty output tag requires the
producer to have one output; a dot-prefixed tag matches an exact path suffix, and
other tags match the declared output path. Zero or multiple matches remain
`output_not_found` or `ambiguous`. This applies across manifest repositories once
the producer module dependency itself has resolved.

Master schema v13 adds `module_file_links`, `build_generated_files`,
`module_generated_links`, and `module_symbol_links`. Link refresh is part of the
same transaction that replaces the module graph, including cleanup for modules
removed by the current scan. CLI and MCP architecture coverage report physical
file resolution, missing/unindexed/ambiguous states, generated-output consumer
links, and definition-symbol links.

## Android Make Evaluation

Android.mk extraction evaluates a bounded, deterministic Make subset rather than
claiming Kati compatibility. Recursive and simple variables support `=`, `:=`,
`+=`, and `?=`. Expansion covers local variable references, `define` macros,
`$(call ...)`, `my-dir`, `strip`, `subst`, `addprefix`, `addsuffix`, and `if`.
Line continuation and relative `include`, `-include`, and `sinclude` fragments are
processed with include-cycle detection.

`ifeq`, `ifneq`, `ifdef`, `ifndef`, `else`, and `endif` are evaluated when their
inputs are locally known. Conditions that depend on external product or target
variables are not guessed: both branches remain unmodeled and the expression is
reported as a coverage gap. Unsupported functions, statements, missing required
includes, and malformed or cyclic constructs are retained the same way.

Common `LOCAL_*_LIBRARIES`, runtime/uses-library, JNI, and required-module
properties become typed module dependencies. Source and prebuilt file declarations
remain `module_files` rows. `BUILD_*` rules and `LOCAL_MODULE_CLASS` map common
native, Java, app, test, and prebuilt declarations to semantic module types while
retaining the original rule and effective class in module properties.

Master schema v10 adds `build_make_files`. Each root Android.mk row stores included
fragments, condition and macro-expansion counts, and the exact unsupported
expressions used by CLI and MCP coverage reporting.

## Product And Board Semantics

Product extraction scans Make fragments that declare `PRODUCT_*` values and treats
files with `PRODUCT_NAME` as selectable products. `inherit-product` and
`inherit-product-if-exists` create explicit file-level edges. Resolved inheritance
expands metadata, package declarations, and partition evidence with provenance;
missing, optional-missing, cyclic, and unsupported-expression edges remain stored
with distinct statuses.

`PRODUCT_PACKAGES*` entries resolve only when a module name is unique across the
workspace. Direct, inherited, removed, ambiguous, and missing declarations remain
queryable. Partition ownership is recorded only from explicit partition-qualified
package variables, `PRODUCT_COPY_FILES` destinations, and supported BoardConfig
variables. Plain `PRODUCT_PACKAGES` entries use `unspecified` rather than assuming
an install partition.

BoardConfig extraction retains supported `BOARD_*`, `TARGET_BOARD_*`, bootloader,
and VINTF variables as an evidence object. Device and vendor ownership comes from
the manifest-scoped workspace path of the declaring product or BoardConfig file.
Unknown Make values are preserved as coverage gaps instead of being evaluated from
ambient build state.

Master schema v11 adds `build_products`, `build_product_inheritance`,
`build_product_packages`, and `build_board_configs`. CLI and MCP architecture
coverage report product, inheritance, package-resolution, BoardConfig, and
partition totals from these tables.

## Bazel Mixed-Build Metadata

B8 imports generated metadata rather than parsing or evaluating Starlark. A
manifest project may contain one or more files named
`aosp_bazel_mixed_build.json`; each file must use the versioned schema below.
Labels are canonicalized to `//package:target`, including package-relative
dependency labels. The external producer is responsible for exporting configured
targets from the Soong/Bazel mixed build.

```json
{
  "schema": "aosp_mixed_build_metadata",
  "version": 1,
  "configuration": "android_arm64",
  "unsupported": ["select_provider:CustomInfo"],
  "targets": [
    {
      "label": "//frameworks/base:libservices",
      "configuration": "android_arm64",
      "kind": "cc_library",
      "module_name": "libservices",
      "dependencies": [
        "//system/core:libbase",
        {
          "label": "//external/zlib:zlib",
          "configuration": "android_arm64",
          "type": "BAZEL_LINK",
          "transition": "target"
        }
      ],
      "unsupported": ["provider:AndroidIdeInfo"]
    }
  ]
}
```

`schema`, `version`, and the `targets` array are required. Every supported target
requires `label` and `module_name`; `configuration`, `kind`, `dependencies`, and
`unsupported` are optional. A dependency may be a label string or an object with
`label` plus optional `configuration`, `type`, and `transition`. Version 1 does
not interpret providers, `select`, toolchains, aspects, or configuration
transitions. Producers list any such omitted semantics in `unsupported`, and the
importer returns those entries as coverage gaps.

Target-to-module mapping succeeds only when `module_name` identifies one module
in the federated workspace. Dependency mapping then requires one configured Bazel
target for the normalized label and one resolved target module. Cross-manifest
project edges are allowed after both boundaries resolve. Duplicate module names,
missing labels or modules, multiple matching configurations, external repository
labels, unsupported format versions, and producer-declared omissions remain
stored with explicit statuses. A malformed artifact fails the scan before the
last committed build graph is replaced.

Master schema v12 adds `build_bazel_artifacts`, `build_bazel_targets`, and
`build_bazel_dependencies`. Resolved dependencies are also projected into
`module_dependencies` and `module_edges`; unresolved evidence remains in the
Bazel tables. CLI and MCP architecture coverage report artifacts, configured
targets, dependencies, resolved counts, ambiguity, missing mappings, and coverage
gaps.

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
