# Agent Initialization Contract

This file defines the mandatory read-only initialization sequence for every primary agent session and every Task Orchestrator before planning, editing, delegation, or project commands. It does not require a Depth-2 leaf to start a new initialization sequence: the parent Task Orchestrator supplies the completed worktree initialization and Task Contract validation handoff.

## Read-only boundary

Initialization validates and reports state. It must not:

- create or modify `AGENTS.md`;
- edit Automation Core files;
- install packages or tools;
- create branches or worktrees;
- update Task State;
- repair configuration;
- start implementation or delegation.

Repository bootstrap and Automation Core upgrade are separate state-changing workflows.

## Runtime preflight

`just agent::preflight` is a narrower read-only readiness check for bootstrap, adoption, and upgrade verification from the root of an installed Agent Core. It validates required runtime tools, required Agent Core files, the supported Agent Core version, and a non-empty Adapter marker without requiring a Git repository, resolving the default branch, or validating branch/worktree/Task identity.

A successful preflight does not establish a valid Agent session and does not replace `/init`, `agent::doctor`, or `agent::context`. A non-default bootstrap branch without Task State may pass preflight while strict doctor/context initialization remains blocked; this separation is intentional and is not an identity-check bypass.

## Mandatory sequence

1. Read `AGENTS.md` and this file.
2. Read `.automation/INIT.fragment.md`; this adapter fragment is part of the initialization contract.
3. If `.task-state/task.md` exists in the current worktree, read it before continuing.
4. Run `just agent::doctor`.
5. Run `just agent::context` and retain the returned JSON context.
6. Run `just project::doctor`.
7. Record the baseline from the context output: repository root, current worktree, branch, default branch, Task ID, Automation Core version, adapter, HEAD, and Git status.
8. For a Task worktree, confirm the Task Contract in `.task-state/task.md`: Purpose, Scope, Prohibited changes, Dependencies, Acceptance criteria, Test plan, Stop conditions, Coordination surfaces, and External resources.
9. Only after every required check succeeds may the parent workflow plan Work Units, edit files, delegate, or run project build/test commands.

## Planning-only sessions

The repository-local `plan` agent is read-only and has `bash: deny`. It must still read `AGENTS.md`, this file, `.automation/INIT.fragment.md`, and optional `.task-state/task.md`, but it must not run `just agent::doctor`, `just agent::context`, `just project::doctor`, project-controlled commands, or executable verification.

A planning-only session reports `PLANNING_INITIALIZATION_HANDOFF` with explicit `execution_prerequisites` and `verification_handoff` entries for every unexecuted doctor, context, project, build, test, or verification check. It must not report `INITIALIZED`, PASS, or successful verification for checks it did not execute.

This exception applies only to the repository-local `plan` agent. Execution-capable primary agents and Task Orchestrators must complete the mandatory sequence above before editing, implementation delegation, or project commands.

## Depth-2 leaf sessions

Depth-2 leaves do not start `initialize` or the full initialization workflow. They must not run `just agent::doctor`, `just agent::context`, or mandatory `just project::doctor` as leaf-session startup prerequisites. They execute only already-allowed operations necessary for the bounded Work Unit. Already-allowed `project::*` commands, including `just project::doctor`, remain usable when the objective or check requires them, but are not startup initialization. The parent Task Orchestrator remains responsible for durable initialization evidence and Task Contract validation.

## Stop conditions

Stop initialization and report `BLOCKED` when any of the following is true:

- required Agent Core files or tools are missing;
- `.automation/VERSION` does not match the runtime-supported Agent Core version;
- `.automation/ADAPTER` or `.automation/INIT.fragment.md` is missing or invalid;
- the current branch, worktree, and Task State identity do not agree;
- a non-default branch is not a registered Task branch with Task State;
- a Task State is present on the default branch;
- `.task-state/` is not ignored in a Task worktree;
- `just project::doctor` fails;
- a required Task Contract field is absent or still unresolved.

The initialization guard is strict and fail-closed. For a Task worktree, `agent::doctor` and `agent::context` validate the already-materialized Task Contract without mutation; unresolved placeholders remain a blocking error. Do not weaken a permission, select another Task, substitute an unconfigured model, or repair the repository in order to pass initialization.

## Project Adapter fragment

`.automation/INIT.fragment.md` is generated by the selected Project Adapter and is logically composed with this core contract. It may define adapter-specific doctor expectations and stop conditions, but must use stable public Just APIs rather than embedding broad raw command sequences.
