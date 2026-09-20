# Agent Operating Rules

This repository is designed for hierarchical agent-driven development.

## Durable invariants

- Before planning, editing, delegation, or project commands in a new primary-agent or Task-Orchestrator session, read `.automation/INIT.md` and complete the `initialize` skill. Initialization is read-only and failures block work.
- Depth-2 leaves inherit the parent Task Orchestrator's completed Task worktree initialization and Task Contract validation. They must not start `initialize` or the full workflow, and must not run `just agent::doctor`, `just agent::context`, or mandatory `just project::doctor` as startup prerequisites; already-allowed `project::*` commands remain usable when the bounded objective/check requires them.
- Use the repository-local Just API for Task lifecycle, publication, and integration operations.
- Main accepts exactly one startup handoff: a newly started pristine Task must use `just agent::task-start-from-issue <numeric-issue> <slug>` followed by `just agent::contract-check <task>` returning exactly `status: READY` with `mode: initial`; an existing already-launched resumable Task must use only `just agent::contract-resume-check <task>` returning exactly `status: READY` with `mode: resume`. Main launches exactly one Task Orchestrator only after the matching handoff. Placeholder Tasks and the low-level `task-start` path are not normal Main authority; `integration-pending`, `merged`, and `cancelled` are not resumable.
- Do not bypass guarded Just recipes with raw state-changing Git or GitHub commands.
- Ordinary implementation Tasks must not modify Automation Core files: `opencode.json`, `AGENTS.md`, `Justfile`, `.opencode/**`, `.automation/**`, or `.github/workflows/**`.
- `flake.nix` and `flake.lock` may be modified only when the active Task explicitly includes environment or dependency changes.
- One Task owns one branch, one worktree, one disposable Task State, and one Task Orchestrator.
- Task State is hydrated before the initial Task Orchestrator starts; resume does not rehydrate or reset it. The Task Orchestrator has no start, hydration, pre-initialization, or generic `.task-state` write authority, and must not delete or reopen Work Units during resume.
- Leaf agents execute bounded Work Units and never update `.task-state/task.md` directly.
- Leaf agents must not create subagents.
- Main Orchestrator owns Task scheduling and final integration. Task Orchestrators own implementation and publication preparation for exactly one Task.
- Task Orchestrators must not merge pull requests.
- Depth-2 leaf agents are non-interactive; they must start their final response with exactly one `status: COMPLETED`, `status: BLOCKED`, `status: NEEDS_APPROVAL`, or `status: NEEDS_DECISION` field and never perform their own permission requests.
- Task Orchestrator is the approval and decision boundary for any delegated escalation (`NEEDS_APPROVAL`/`NEEDS_DECISION`) and must re-evaluate scope, authority, least privilege, safety, alternatives, and current evidence before deciding.
- A leaf denial is not automatically promoted to Ask. After independent re-evaluation, the Task Orchestrator may originate a new Depth-1 request only when that operation is already Ask/allow under its own configured authority; the leaf profile remains unchanged.
- A user-rejected Depth-1 permission decision is final for that exact operation within the Task. It must not be retried, rephrased, re-delegated, or replaced by an equivalent operation; use recorded permission evidence and a safe alternative or BLOCKED result.
- A command or check that was not executed must never be reported as PASS.
- Unresolved leaf requests must not be inferred as approval. For `NEEDS_DECISION`, the Task Orchestrator resolves from the Task Contract/evidence when possible; if human judgment remains necessary, it asks from Depth 1 with options, tradeoffs, known facts, and a recommendation, then applies the answer.
- Each configured role model is authoritative. If it is unavailable, do not substitute another model or retry the same objective under another model; preserve relevant evidence, report the exact provider/model failure, and return `BLOCKED`.

## Initialization layers

- `AGENTS.md`: durable repository rules.
- `.automation/INIT.md`: mandatory per-session read-only initialization sequence.
- `.task-state/task.md`: active Task contract, progress, and evidence.
- bootstrap: one-time state-changing repository creation/configuration, separate from `/init`.
- `/init`: read-only validation/context resolution only; it must not rewrite `AGENTS.md` or repair Automation Core.

## Agent call graph

```text
build
├── task-orchestrator
├── architect
├── reviewer
├── investigator
├── security-reviewer
└── scout

task-orchestrator
├── general
├── explore
├── verifier
├── reviewer
├── investigator
├── security-reviewer
└── scout

leaf agents
└── no further delegation
```

The call graph is intentionally non-cyclic. `task-orchestrator` may never invoke another `task-orchestrator`.

## Permission boundary

Automatically permitted operations are restricted to repository inspection, selected read-only Git/GitHub commands, safe initialization and `project::*` checks, Task-local commit, and constrained PR create/edit/ready operations through Just.

User approval is required for Task branch push, final merge, cleanup, `/tmp/opencode/**` access, and unclassified shell commands.

Raw Git/GitHub mutations, force push, amend, rebase, destructive reset/clean, direct default-branch push, admin merge, privilege escalation, and destructive store/filesystem operations are prohibited.

## External paths

The only generally requestable external path is `/tmp/opencode/**`, and it requires approval. Other paths outside the current OpenCode workspace are denied by default.

## Worktree isolation

Agents must operate only inside the worktree assigned to their current Task. Access to sibling Task worktrees is prohibited. Static OpenCode permissions provide the default boundary; Task/worktree lifecycle guards add the dynamic sibling-worktree checks.
