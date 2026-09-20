# Agent Core automation

This directory contains the language-independent Task lifecycle, publication,
and integration layer shared by every Agent-ready template.

Public operations are exposed through the top-level Just modules rather than by
calling these scripts directly. Project-specific build, lint, test, and toolchain
behavior belongs under `just/project/` in the selected Project Adapter.

Task Orchestrators use the persisted Work Unit API: select with
`work-unit-next`, create with `work-unit-create`, and verify dispatch with
`work-unit-dispatch-check`. These Task-local APIs are denied globally and
allowed only to the Task Orchestrator. Leaf completion is limited to the four
canonical statuses; failed evidence requires a fresh corrective Work Unit.

The current implementation provides guarded Task-local commit/push/PR operations,
integration head-SHA checkpoints, disposable Task State templates, and common
safety policy. Repository-local OpenCode agents and permissions are added by the
separate OpenCode configuration work.

## Post-merge finalization

Task Orchestrators stop at `integration-pending`. After the Main Orchestrator or
a human merges the PR, reconcile the external merge from the unique clean
default-branch worktree:

```sh
just integrate::finalize <task> <pr>
```

This dedicated operation requires one registered Task worktree with matching
persisted identity and status `integration-pending` (or the idempotent `merged`
case). It requires one unambiguous, same-repository GitHub PR for that Task
branch, a default-branch base, state `MERGED`, and a valid GitHub
`mergeCommit.oid`. It narrowly fetches only `origin`'s default branch with a
non-force refspec, rejects a dirty Main worktree, non-fast-forward remote
movement, local-only commits, divergence, detached/wrong worktrees, and ref
movement during synchronization. It then fast-forwards the local default branch
without reset, rebase, force, conflict resolution, or merge-commit creation.

Only after the fetched local and remote-tracking refs agree, the GitHub merge
commit is contained in that revision, and GitHub/Task evidence is revalidated
does the dedicated internal path transition `integration-pending -> merged`.
Re-running a valid finalized Task reports `already-finalized`. Generic Main
`agent::state-set` and raw Git fetch/pull/merge/ref mutation remain denied.

Cleanup is deliberately separate and destructive:

```text
integration-pending -> PR merge -> guarded finalize -> merged
  -> approval-gated cleanup -> dependency re-evaluation -> next Task
```

For a merged Task, cleanup reconstructs safety from the unique same-repository
merged PR: its branch and `headRefOid` must match the registered clean local
Task branch exactly, and the local branch must contain no later commit. A live
origin Task branch must have that same OID. A deleted origin branch is accepted
only after those merged-PR checks, which allows a finalized Task such as
AgentKnowledgeVault #12 to retry normal cleanup without recreating a remote
branch. Cancelled Tasks retain the conservative unpublished-commit check and
never use the merged-PR exception. Cleanup records a private administrative
receipt before worktree removal and deletes the local branch with an
expected-head compare-and-swap, so a partial failure remains retryable.
Cleanup remains Main-owned and approval-gated; no raw cleanup authority is
added.

`agent::task-start` uses the same guarded default-branch synchronization at
execution time before creating a branch/worktree, so it does not trust a stale
remote-tracking ref even if the default branch advanced after finalization.

## Automation Maintenance workflow

An Agent Core upgrade is permitted only from a dedicated registered, non-default
Automation Maintenance Task. From that Task worktree, use a trusted local
Templates checkout and the exact full immutable source revision:

```sh
AUTOMATION_MAINTENANCE=1 just automation::upgrade <trusted local Templates checkout> <expected-revision>
```

The source must be a trusted clean Git worktree root with a full, non-null
`HEAD`. `automation::check-update <source> [expected-revision]` optionally
asserts the expected revision and always reports actual `HEAD`; upgrade and
`automation::bootstrap-receipt` require it. An exact actual-HEAD mismatch
fails before tracked consumer mutation or receipt/authority publication, even
for byte-identical trees: commit identity is provenance. Both operations reject
tracked modifications and non-ignored untracked paths under
`components/agent-core`; ignored generated artifacts are structurally absent.
They pin the source `HEAD`, materialize only tracked Agent Core objects into a
temporary snapshot, and plan/copy only from that snapshot. Compatible
`VERSION` drift remains detectable, while a source race fails closed.

The environment variable is an upgrade opt-in only. It does not authorize a
commit; ordinary `just agent::commit <task>` rejects Automation Core changes.
Upgrade does not commit, push, or merge and writes the ignored receipt
`.task-state/automation-maintenance.json`.

Before publication, execute `git diff --check`, `just agent::doctor`,
`just project::check`, and the repository CI/smoke suite. Then publish only with:

```sh
just automation::commit <task> [message]
just agent::push <task>
just agent::pr-create <task>
just agent::pr-prepare <task>
just agent::pr-edit <task>
just agent::pr-ready <task>
```

`pr-prepare` renders acceptance criteria as authoritative requirements and
renders non-empty `Current state` Blockers/Unverified values in the PR risk
section. Missing Current-state evidence, unresolved placeholders, stale
verification, missing completed reviewer evidence, an incomplete declared
security review, or contradictory validation claims block publication.

The normal upgrade flow creates its receipt before verification. If a self-hosted
pre-receipt upgrade leaves the exact upgraded diff without that receipt, perform
normal verification and, before `automation::commit`, run the strict bootstrap
bridge with its expected revision:

```sh
AUTOMATION_MAINTENANCE=1 just automation::bootstrap-receipt <trusted clean Git Templates checkout> <expected-revision>
```

The bootstrap bridge still supports exactly two strict cases: canonical
pre-receipt reconstruction, and recovery of an exact active receipt whose
authority is missing. It does not trust `NO_CHANGES`, the current diff, or the
environment. A receipt with authority, or stale, forged, or tampered state,
fails closed. Product, Adapter, repository, secret-pattern, or `.task-state`
paths also fail closed. This remains the Issue #83/#85 route.

The canonical consumer correction is narrower than generic editing or deletion:

```sh
AUTOMATION_MAINTENANCE=1 just automation::rebind-maintenance-provenance <trusted-source-at-expected-HEAD> <expected-revision>
```

For older consumers, the Templates source bridge is:

```sh
just agent-core::rebind-maintenance-provenance <consumer-worktree> <expected-revision>
```

It verifies bootstrap/engine trust, reconstructs old and expected immutable
objects from the same Templates object database, requires the expected
canonical diff and exact safe pending paths/fingerprints, and leaves tracked
files unchanged. It reports `PROVENANCE_REBOUND` or idempotent
`PROVENANCE_ALREADY_BOUND`; then run ordinary consumer verification and the
existing `automation::commit`.

Rebind is an operator-serialized maintenance transition: no commit or other
authority mutation may run concurrently in the target worktree. Current Agent
Core revisions additionally fence rebind and commit with the same ordered
common/admin migration locks; the explicit quiescence requirement preserves the
older pending consumer needed to finish an in-flight upgrade.

Eligibility is exact: registered maintenance Task/worktree/branch/HEAD; one
standard active receipt matching exactly one authority; safe pending Agent
Core paths/fingerprints only; no consumed, source-recovery, or ambiguous state;
the old and expected revisions in the same Templates object database; and an
identical expected canonical diff. Missing authority remains the Issue #85
route. Committed or consumed state, including a crossed guarded publication
boundary, is rejected. Handled
failures roll back safely; operations by current revisions fail closed under the
shared fence. No cross-filesystem
atomicity or hard-crash durability is claimed; that remains Issue #89 scope.

The receipt is schema-1 JSON containing Task identity (`task_id`, `branch`, and
`worktree`), source/source revision, current/upstream versions, sorted unique
`changed_paths`, `authority_head`, and exact per-path content/state
`path_fingerprints`. Commit fails closed if the receipt is absent, malformed,
stale, from another Task/worktree, has a different `HEAD`, changed fingerprints,
or does not exactly equal all pending paths. Receipt and authority publication
is a logical pair. Authority records live under the Git-resolved per-worktree
administrative directory returned by `--absolute-git-dir`, not an assumed
visible `.git` or shared Git directory; linked and special administrative
topologies are supported, and worktrees do not share authority. Existing safe
legacy shared-common-dir hashed records remain validation/commit compatible. A
protected record binds it to the preceding successful upgrade; a fabricated
Task State receipt is not authority. Ambient Git repository/index overrides are
scrubbed. The exact paths are staged and rechecked in a private Task State
index, then the commit is created from that verified tree without hooks and the
expected Task branch HEAD is advanced atomically. Only Agent Core-managed paths
are accepted; mixed Adapter, repository, product, secret-pattern, or
`.task-state` scopes are rejected. A handled authority-write failure removes the
newly written receipt if it is unchanged; an interruption half-state is
recoverable only through the strict bootstrap path. No cross-filesystem
atomicity is claimed. After a successful commit it is consumed at
`.task-state/automation-maintenance.consumed.json`; a subsequent successful
upgrade with changes replaces the active receipt and removes the previous consumed
receipt. A no-change invocation returns `NO_CHANGES` without discarding existing
receipt lifecycle evidence.

## Git-private runtime state

Agent Core owns only the `agent-core/` namespace beneath Git administrative
directories. Shared cleanup receipts, pristine-discard receipts, integration
checkpoints, the cleanup lock, the migration lock, and historical hashed
maintenance authorities live beneath `<git-common-dir>/agent-core/`.
Worktree-specific maintenance `authority.json` and
`source-recovery-proof.json` live beneath
`<absolute-git-dir>/agent-core/automation-maintenance/`. Repository Task State
under `.task-state/`, Git's `info/exclude`, and committed `.opencode/` and
`opencode.json` configuration are separate ownership surfaces.

The path `<git-common-dir>/opencode` is OpenCode-owned when it is a regular
file and Agent Core never changes its bytes, metadata, or identity. A legacy
directory at that name is accepted only when every entry satisfies the exact
historical path, schema, Task/branch/worktree identity, repository syntax, and
authority/proof relationship that Agent Core produced. The legacy namespace,
subdirectories, records, and lock must be owned by the current effective user.
Directories require owner access and reject group/other mutation while retaining
historically produced read/traverse modes such as `0755`; records and locks reject
group/other mutation and executable bits. Canonical directories
are exactly mode `0700`, and canonical records, locks, and publication
artifacts are exactly mode `0600`.

Known records are copied byte for byte with durable no-overwrite publication,
all conflicts are rejected before migration mutation, and legacy records are
removed only after every canonical destination is mode-, owner-, and
content-equivalent. The legacy cleanup-lock inode is acquired nonblocking and
hard-linked into the canonical cleanup-lock path before its legacy name is
removed. Existing waiters therefore remain fenced on the same inode. A
contended legacy lock, or distinct legacy and canonical lock inodes, reports a
precise `BLOCKED` condition rather than claiming migration success. Successful
cutover removes the legacy lock last and then removes the empty `opencode/`
directory, leaving the path available to OpenCode.

Canonical reads and writes validate the actual descriptor chain. The opened Git
administrative boundary must be owned by the effective user and must not be
group- or world-writable; ordinary non-writable modes such as `0755` remain
valid. Every opened directory below `agent-core/` must be owned by that user at
exact mode `0700`, and every opened authority record must be a regular file
owned by that user at exact mode `0600`. Traversal and record access remain
descriptor-anchored with `O_NOFOLLOW`, and reads retain device, inode, and size
stability checks. Recognized legacy `opencode/` state keeps its separate bounded
migration mode policy. Hostile processes running as the same effective user are
outside this local-filesystem trust boundary.

Canonical publication is serialized by `migration.lock`. Only exact owned
`.migrate.<pid>.<16-lowercase-hex>` and
`.record.<pid>.<16-lowercase-hex>` artifacts are recoverable. On the next
operation they are validated and durably removed under that lock before the
legacy source is retried or an already-durable destination is used. Equivalent
dual state, partial legacy removal, and interruption before or after durable
publication are therefore retryable. Unknown entries, malformed temporary
names, unsafe modes/ownership, unexpected nesting, symlinks, and special files
fail closed without promotion.

Do not bypass these guards with raw Git/GitHub commands. Merge is not part of
this workflow and remains the separately gated Main Orchestrator operation.
