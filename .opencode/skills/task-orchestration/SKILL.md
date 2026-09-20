---
name: task-orchestration
description: Run one already-created Task autonomously through persisted Work Unit state without merging
---

# Task orchestration

Use this skill when the Main Orchestrator is asked to run a specific Task that already has a dedicated branch/worktree and Task State. The workflow is autonomous and state-driven; do not wait for conversational scheduling.

1. Resolve the explicit Task ID and assigned worktree.
2. Confirm the Task is not already owned by another active Task Orchestrator.
3. Launch exactly one `task-orchestrator` for that Task.
4. Require the Task Orchestrator to operate only in the assigned worktree and to use bounded Work Units. Inspect persisted state (`work-unit-next` is read-only planning output), then call `work-unit-create` with the chosen role and exact objective and use its returned ID. Run `work-unit-dispatch-check` with that ID and the exact same role/objective before delegation. Never hand-author an allocation or delegate an uncreated or unverified Work Unit.
5. Require the Task Orchestrator to enforce leaf escalation-only status contract: Depth-2 units may only return `COMPLETED`, `BLOCKED`, `NEEDS_APPROVAL`, or `NEEDS_DECISION`. These are the only canonical leaf statuses.
   - For a normal leaf `status: BLOCKED` result, first persist the blocked Work Unit as terminal and inspect its evidence and the Task Contract.
   - If a fresh bounded in-scope corrective Work Unit can resolve it, create that new Work Unit and continue. Otherwise set the Task `blocked`, surface the blocker, and stop.
   - Never reopen or mutate the blocked Work Unit.
6. Require the Task Orchestrator to treat `NEEDS_APPROVAL`/`NEEDS_DECISION` as its own decision point:
   - re-validate scope, authority, least privilege, safety, alternatives, and evidence;
   - do not automatically relay/launder leaf requests or change a leaf's deny-default profile;
   - only issue a new Depth-1 permission request after independent re-evaluation and when the operation is already Ask/allow within the Task Orchestrator's configured role authority;
   - on rejection, pick a safe alternative or return `BLOCKED` with evidence.
   - treat a user-rejected Depth-1 operation as final for that Task; do not retry, rephrase, re-delegate, or substitute an equivalent operation to verify/bypass rejection.
   - for unresolved `NEEDS_DECISION`, use `question` from Depth 1 with concrete options, tradeoffs, known facts, and a recommendation; apply the answer rather than relaying the Leaf request or escalating it to Depth 0.
7. Treat each configured model as authoritative for its role. Do not substitute another model or retry the same Task or Work Unit under another model.
8. If provider/model execution is unavailable, persist the affected Work Unit as terminal `blocked` with the exact provider, model, and error evidence; report the exact provider/model failure; set and surface the Task as `blocked`; and stop. Do not substitute another role/model or retry the same Task or Work Unit under another model.
9. Accept only actual verification and evidence-backed completion: changed files, verification results, review and security-review results, verifier/check results, commit/PR state, blockers, and explicitly unverified checks.
10. Confirm the Task did not report PASS for any unexecuted command or unperformed Work Unit.
11. Before advancing Task State to `publication-ready`, and again before guarded commit, push, or Draft PR preparation, require actual PASS evidence for every applicable item:
    - relevant focused tests and checks;
    - `git diff --check`;
    - `just project::check`;
    - `just agent::verify <task>`;
    - a reviewer Work Unit for non-trivial changes;
    - a security-reviewer Work Unit when trust- or security-sensitive surfaces changed.
12. No unexecuted check or Work Unit may count as PASS. If any required review, verifier, or check fails and a bounded correction is possible, create a fresh corrective Work Unit and return to verification. Otherwise set the Task `blocked`, surface the evidence, and stop.
13. Only after the verification evidence gate passes may Task State become `publication-ready`; then use guarded `commit`, request approval for `push`, run `just agent::pr-prepare <task>`, and create or edit only the same Draft PR. Publication metadata must be regenerated from persisted evidence and must contain no unresolved placeholder or false `NOT RUN` claim.
14. Persist every transition and continue the loop until integration-pending. Stop for `NEEDS_APPROVAL` or `NEEDS_DECISION` human handling, and otherwise stop at integration-pending. Do not merge from this skill. Never merge.

Do not silently select another Issue or Task. Do not weaken permissions to work around a blocked approval or missing tool/model.
Depth-2 Ask behavior is tracked as a non-gating upstream compatibility canary for anomalyco/opencode#13715; release readiness is determined by correct Leaf→Depth-1 escalation approval/rejection decisions.
