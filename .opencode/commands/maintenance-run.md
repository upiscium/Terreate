---
description: Run an explicit Automation Maintenance Task with its dedicated orchestrator
agent: build
---

First load the `initialize` skill and complete the mandatory read-only initialization checks for the current Main worktree.

Then load the `maintenance-orchestration` skill. The arguments must identify exactly:
1. an existing Task ID,
2. a trusted local Templates source worktree,
3. the expected full immutable Templates revision.

Run `just automation::maintenance-check <task>` from Main. Launch exactly one `maintenance-orchestrator` only on a complete `status: READY`, `mode: maintenance` result, and pass the complete evidence plus the exact source/revision arguments.

If committed-stage `reviewEvidence` is incomplete, Main must first delegate each exact returned reviewer/security-reviewer objective and record only the canonical completed leaf result through `just automation::maintenance-review-record`. Re-run `maintenance-check`; the Maintenance Orchestrator has no review-recording authority.

Do not route this Task through normal `/task-run`, `contract-resume-check`, or product Task State transitions. Stop before merge.
