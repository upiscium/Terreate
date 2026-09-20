---
description: Create an authoritative Issue-backed Task branch, worktree, and Task State
agent: build
---

For a newly started pristine Task, create the explicitly supplied numeric GitHub Issue-backed Task with `just agent::task-start-from-issue <numeric-issue> <slug>`. The Issue in the current repository is authoritative; do not use an arbitrary URL, repository, low-level `task-start`, placeholder Task, or interpretation step.

Run this initial path only from the default-branch worktree. Run `just agent::contract-check <task>` and require exactly `status: READY` with `mode: initial` before launching exactly one Task Orchestrator. Do not implement the Task in the Main Orchestrator. After creation and the READY check, report the resolved branch and worktree and stop unless the user also asked to run the Task.

For an Issue-number invocation, the same initial check is `just agent::contract-check <numeric-issue>`.

To resume an existing already-launched resumable Task, do not run task-start or hydrate it. Main must run the dedicated `just agent::contract-resume-check <task>` and require exactly `status: READY` with `mode: resume`; only Main may launch exactly one Task Orchestrator after that handoff. `integration-pending`, `merged`, and `cancelled` are not resumable.
