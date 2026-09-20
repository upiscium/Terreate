---
name: maintenance-orchestration
description: Run one registered Automation Maintenance Task through receipt-driven publication without normal Task lifecycle state fabrication
---

# Automation Maintenance orchestration

Use this skill only for an explicitly selected registered Issue-backed Automation Maintenance Task.

Inputs are exact and mandatory:
1. Task ID.
2. Trusted clean local Templates source worktree.
3. Expected full immutable Templates revision.

From Main, run `just automation::maintenance-check <task>`. Launch exactly one `maintenance-orchestrator` only when it returns `status: READY` and `mode: maintenance`. Pass the complete result plus the exact source path and revision. This dedicated readiness path is valid for pristine, applied, committed, pushed, and Draft-PR maintenance stages and does not alter normal `contract-check` / `contract-resume-check` semantics.

At committed stage, Main owns reviewer and security-reviewer delegation. For each false `reviewEvidence` role, delegate the exact returned objective, accept only the canonical completed leaf result, and persist it with `just automation::maintenance-review-record <task> <role> <evidence>`. The Maintenance Orchestrator cannot invoke that recorder. Re-run the check before launch.

The maintenance orchestrator must:
- reconstruct progress from guarded maintenance evidence, not synthetic Task State transitions;
- bind `check-update` and `upgrade` to the exact expected source revision;
- request human approval for upgrade and push;
- require actual diff/check/reviewer/security-review evidence before publication;
- use `automation::commit`, never ordinary Task commit, for Agent Core managed changes;
- use `automation::maintenance-pr-create`, never normal `agent::pr-create`, for the Draft PR;
- stop at Draft PR or any later remote stage and never merge.

Resume is evidence-driven:
- active maintenance receipt -> applied;
- consumed receipt + exact local commit -> committed;
- exact remote branch at that commit -> pushed;
- exact Draft PR at that head -> draft-pr-created;
- exact merged PR -> merged-remote;
- dedicated Main finalization -> merged.

After human merge, Main runs `just automation::maintenance-finalize <task> <pr>`. That operation must validate the consumed receipt, exact published head, merged PR identity, merge commit, and synchronized default branch before the dedicated `initialized -> merged` terminal transition. Then cleanup remains separately approval-gated.

Never weaken normal Task transitions, never make `initialized` generally resumable, never use raw Git/GitHub writes as a substitute, and never manually edit `.task-state`.
