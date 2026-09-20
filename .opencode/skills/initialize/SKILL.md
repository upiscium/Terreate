---
name: initialize
description: Run mandatory read-only repository and Task initialization checks before work begins
---

# Initialize

Use this skill before editing, implementation delegation, or project commands in a new primary-agent or Task-Orchestrator session. It is not a leaf-session startup prerequisite: a Depth-2 leaf receives the parent Task Orchestrator's completed worktree initialization and Task Contract validation and must not start this skill or the full workflow.

Full initialization for execution-capable primary agents and Task Orchestrators:

1. Read `AGENTS.md` and `.automation/INIT.md`.
2. If `.task-state/task.md` exists in the current worktree, read it.
3. Run `just agent::doctor`; stop on failure.
4. Run `just agent::context`; retain the machine-readable context.
5. Run `just project::doctor`; stop on failure.
6. Confirm the baseline HEAD and Git status reported by context.
7. For a Task worktree, verify the Task Contract is concrete and internally consistent.
8. Return `INITIALIZED` with the resolved context only after all checks pass.

Planning-only initialization for the repository-local `plan` agent:

1. Read `AGENTS.md`, `.automation/INIT.md`, and `.automation/INIT.fragment.md`.
2. If `.task-state/task.md` exists in the current worktree, read it.
3. Do not run `just agent::doctor`, `just agent::context`, `just project::doctor`, Bash, project-controlled commands, or executable verification from `plan`.
4. Return `PLANNING_INITIALIZATION_HANDOFF`, not `INITIALIZED`, with `execution_prerequisites` listing every unexecuted required initialization command.
5. Report unexecuted doctor, context, project, build, test, or verification checks as `UNEXECUTED`; never report them as PASS.

Initialization is read-only. Do not edit `AGENTS.md`, Task State, Automation Core, project files, Git refs, worktrees, or installed packages. Do not perform bootstrap or repair actions. Planning-only handoff does not weaken the full initialization contract for execution-capable workflows.

## Depth-2 leaf applicability

The parent Task Orchestrator already completed Task worktree initialization and Task Contract validation before delegating a leaf Work Unit. A leaf executes only already-allowed operations necessary for its bounded objective. It must not run `just agent::doctor`, `just agent::context`, or mandatory `just project::doctor` as session startup prerequisites. Already-allowed `project::*` commands, including `just project::doctor`, remain usable when the objective or check requires them; that use is not initialization.
