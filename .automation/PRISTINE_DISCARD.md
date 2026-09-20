# Guarded pristine Task discard

`just agent::discard-pristine <task>` is the narrow recovery operation for an
Issue-shaped Task worktree that was materialized but never received a canonical
Task Contract and never began implementation.

It is intentionally separate from `state-set` and ordinary terminal `cleanup`.
The operation is destructive, runs only from the default-branch/Main
Orchestrator worktree, and remains approval-gated by OpenCode policy.

The discard succeeds only when Agent Core can prove all of the following:

- Task status is exactly `initialized`.
- The Task State is byte-for-byte the unresolved initialization template for
  that Task identity and Base revision.
- No canonical Task Contract metadata, Work Unit state, or other Task State
  evidence exists.
- The worktree is clean.
- The registered Task branch and local branch both point exactly at the
  recorded Base revision, with no additional commits.
- The recorded Base branch is the repository default branch and the Base
  revision is trusted default-branch history.
- The Task branch has no configured upstream/publication metadata, no
  remote-tracking ref, no live remote branch, and no historical pull request.
- Repository identity and publication absence still match immediately before
  local branch deletion.

A private receipt under the common Git directory makes the destructive tail
retryable. If worktree removal succeeds but local branch deletion fails, a
later retry revalidates the Base, repository, remote branch, upstream
configuration, and pull-request absence before deleting the expected local ref.
If any evidence appears, retry fails closed and preserves the local branch.

After successful discard, synchronize the default branch and restart the same
Issue through the normal Issue-backed lifecycle:

```text
just agent::discard-pristine <task>
just agent::task-start-from-issue <issue> <slug>
```

Do not use this operation for a resolved Task Contract, a Task with Work Units,
a dirty or advanced branch, any pushed branch/PR, or any status other than
`initialized`. Do not fabricate a contract or use raw `git worktree remove` /
branch deletion as a substitute.
