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

`source_generation` records the repository generation observed for the refresh.
Content-aware invalidation and dependency propagation are implemented by later
tasks in the AOSP support plan.

## Schema Compatibility

Master schema v4 introduces the structured edge identity and status fields. On
initialization, the v3 resolved-only table is migrated transactionally. Cataloged
legacy targets remain resolved; missing legacy targets become unresolved records
whose original target ID is retained as the target name and evidence.
