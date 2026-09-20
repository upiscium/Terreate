#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
import tempfile
from collections.abc import Callable
from contextlib import contextmanager
from pathlib import Path

import agent_core
import automation_upgrade as upgrade
import publication_metadata as publication
import task_contract
import task_lifecycle as lifecycle


class MaintenanceError(RuntimeError):
    pass


def root() -> Path:
    installed = Path(__file__).resolve().parents[2]
    discovered = agent_core.repo_root(installed)
    if discovered != installed:
        raise MaintenanceError(
            "installed maintenance lifecycle path does not match the Git worktree root"
        )
    return installed


def _current_or_main_can_inspect(
    root_path: Path, record: lifecycle.WorktreeRecord
) -> None:
    current = lifecycle.current_worktree(root_path)
    main = lifecycle.main_worktree(root_path)
    if current.path not in {main.path, record.path}:
        raise MaintenanceError(
            f"maintenance check cannot inspect sibling Task worktree {record.path} from {current.path}"
        )


def _stored_contract(
    root_path: Path, task: str
) -> tuple[lifecycle.WorktreeRecord, dict]:
    record = lifecycle.worktree_for_task(root_path, task)
    _current_or_main_can_inspect(root_path, record)
    try:
        result = task_contract.validate_contract(record.path, task)
        if task_contract.repository_identity(record.path) != result["repository"]:
            raise MaintenanceError("live repository identity mismatch")
    except task_contract.ContractError as exc:
        raise MaintenanceError(str(exc)) from exc
    return record, result


def _validated_contract(
    root_path: Path, task: str
) -> tuple[lifecycle.WorktreeRecord, dict]:
    record, result = _stored_contract(root_path, task)
    try:
        task_contract._validate_authoritative_issue(
            record.path, task, result["repository"], result["sha256"]
        )
    except task_contract.ContractError as exc:
        raise MaintenanceError(str(exc)) from exc
    return record, result


def _read_json_regular(path: Path, description: str) -> dict:
    if path.is_symlink() or not path.is_file():
        raise MaintenanceError(f"{description} is unavailable or unsafe")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise MaintenanceError(f"{description} is invalid") from exc
    if not isinstance(value, dict):
        raise MaintenanceError(f"{description} is invalid")
    return value


def _validate_active_receipt(record: lifecycle.WorktreeRecord, task: str) -> dict:
    path = upgrade.receipt_path(record.path)
    try:
        receipt = upgrade.validate_receipt_schema(
            _read_json_regular(path, "active maintenance receipt")
        )
        upgrade.validate_authority(record.path, receipt)
    except upgrade.UpgradeError as exc:
        raise MaintenanceError(str(exc)) from exc
    if (
        receipt.get("status") != "active"
        or receipt.get("task_id") != task
        or receipt.get("branch") != record.branch
        or receipt.get("worktree") != str(record.path)
    ):
        raise MaintenanceError("active maintenance receipt identity mismatch")
    if upgrade.consumed_receipt_path(record.path).exists():
        raise MaintenanceError("active and consumed maintenance receipts coexist")
    head = upgrade.git_head(record.path)
    if receipt.get("authority_head") != head or record.head != head:
        raise MaintenanceError("active maintenance receipt authority HEAD is stale")
    try:
        paths = upgrade.receipt_paths(record.path, receipt)
        if upgrade.pending_paths(record.path) != paths:
            raise MaintenanceError(
                "pending paths do not exactly match the active maintenance receipt"
            )
        fingerprints = receipt.get("path_fingerprints")
        if not isinstance(fingerprints, dict) or set(fingerprints) != set(paths):
            raise MaintenanceError(
                "active maintenance receipt fingerprints do not match its paths"
            )
        if any(
            upgrade.file_fingerprint(record.path, item) != fingerprints[item]
            for item in paths
        ):
            raise MaintenanceError(
                "active maintenance receipt path fingerprint changed"
            )
    except upgrade.UpgradeError as exc:
        raise MaintenanceError(str(exc)) from exc
    return receipt


def _validate_consumed_receipt(
    record: lifecycle.WorktreeRecord, task: str
) -> dict:
    if upgrade.receipt_path(record.path).exists():
        raise MaintenanceError("active maintenance receipt still exists after commit")
    path = upgrade.consumed_receipt_path(record.path)
    value = _read_json_regular(path, "consumed maintenance receipt")
    required = set(upgrade.RECEIPT_FIELDS) | {"commit_sha"}
    if (
        set(value) != required
        or value.get("schema_version") != 1
        or value.get("status") != "consumed"
    ):
        raise MaintenanceError("consumed maintenance receipt has an invalid schema")
    if (
        value.get("task_id") != task
        or value.get("branch") != record.branch
        or value.get("worktree") != str(record.path)
    ):
        raise MaintenanceError("consumed maintenance receipt identity mismatch")
    commit = value.get("commit_sha")
    if not isinstance(commit, str):
        raise MaintenanceError("consumed maintenance receipt has no commit SHA")
    try:
        upgrade.validate_commit_oid(record.path, commit, field="maintenance commit")
        upgrade.validate_commit_oid(
            record.path,
            value.get("authority_head"),
            field="maintenance authority HEAD",
        )
    except upgrade.UpgradeError as exc:
        raise MaintenanceError(str(exc)) from exc
    local_head = upgrade.git_head(record.path)
    branch_head = upgrade.run(
        ["git", "rev-parse", "--verify", f"refs/heads/{record.branch}"],
        cwd=record.path,
    ).stdout.strip()
    if local_head != commit or branch_head != commit or record.head != commit:
        raise MaintenanceError(
            "maintenance Task HEAD does not match the consumed receipt commit"
        )
    parent = upgrade.run(
        ["git", "rev-parse", f"{commit}^"], cwd=record.path
    ).stdout.strip()
    if parent != value["authority_head"]:
        raise MaintenanceError(
            "maintenance commit parent does not match the receipt authority HEAD"
        )
    changed = sorted(
        line
        for line in upgrade.run(
            [
                "git",
                "diff-tree",
                "--no-commit-id",
                "--name-only",
                "--no-renames",
                "-r",
                commit,
            ],
            cwd=record.path,
        ).stdout.splitlines()
        if line
    )
    paths = value.get("changed_paths")
    if not isinstance(paths, list) or paths != sorted(set(paths)) or changed != paths:
        raise MaintenanceError(
            "maintenance commit paths do not match the consumed receipt"
        )
    fingerprints = value.get("path_fingerprints")
    if not isinstance(fingerprints, dict) or set(fingerprints) != set(paths):
        raise MaintenanceError(
            "consumed maintenance receipt fingerprints do not match its paths"
        )
    try:
        if any(
            upgrade.file_fingerprint(record.path, item) != fingerprints[item]
            for item in paths
        ):
            raise MaintenanceError("committed maintenance path fingerprint changed")
    except upgrade.UpgradeError as exc:
        raise MaintenanceError(str(exc)) from exc
    status = upgrade.run(
        ["git", "status", "--porcelain", "--untracked-files=all"],
        cwd=record.path,
    ).stdout.strip()
    if status:
        raise MaintenanceError("maintenance Task worktree is not clean after commit")
    return value


def _remote_head(record: lifecycle.WorktreeRecord) -> str | None:
    try:
        return lifecycle.remote_branch_head(record)
    except lifecycle.LifecycleError as exc:
        raise MaintenanceError(str(exc)) from exc


def _remote_relation(
    record: lifecycle.WorktreeRecord, remote: str | None, commit: str
) -> str:
    if remote is None:
        return "absent"
    if remote == commit:
        return "exact"
    result = lifecycle.run(
        ["git", "merge-base", "--is-ancestor", remote, commit],
        cwd=record.path,
        check=False,
    )
    if result.returncode == 0:
        return "ancestor"
    raise MaintenanceError(
        "live remote Task branch is not the maintenance commit or its ancestor"
    )


def _pr_evidence(
    record: lifecycle.WorktreeRecord, repository: str, commit: str
) -> dict | None:
    pr = agent_core.pr_for_branch(record.path, record.branch or "", repository)
    if pr is None:
        return None
    expected = {
        "headRefName": record.branch,
        "baseRefName": agent_core.default_branch(record.path),
        "headRefOid": commit,
        "isCrossRepository": False,
    }
    mismatches = [name for name, wanted in expected.items() if pr.get(name) != wanted]
    if mismatches or not isinstance(pr.get("number"), int):
        raise MaintenanceError(
            "maintenance pull request identity is stale or inconsistent: "
            + ", ".join(mismatches or ["number"])
        )
    if pr.get("state") not in {"OPEN", "MERGED"}:
        raise MaintenanceError(
            f"maintenance pull request has unsupported state: {pr.get('state')}"
        )
    return pr


def _review_subject(receipt: dict) -> str:
    fields = {
        "commit_sha": receipt.get("commit_sha"),
        "authority_head": receipt.get("authority_head"),
        "source": receipt.get("source"),
        "source_revision": receipt.get("source_revision"),
        "changed_paths": receipt.get("changed_paths"),
        "path_fingerprints": receipt.get("path_fingerprints"),
    }
    if (
        not isinstance(fields["commit_sha"], str)
        or not isinstance(fields["authority_head"], str)
        or not isinstance(fields["source"], str)
        or not isinstance(fields["source_revision"], str)
        or not isinstance(fields["changed_paths"], list)
        or not isinstance(fields["path_fingerprints"], dict)
    ):
        raise MaintenanceError("maintenance review subject is incomplete")
    return hashlib.sha256(
        json.dumps(fields, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def _review_objective(task: str, role: str, subject: str) -> str:
    return (
        f"Review Automation Maintenance Task {task} as {role}; "
        f"review_subject_sha256={subject}; report only evidence for this exact immutable upgrade."
    )


def _review_handoff(task: str, receipt: dict) -> dict:
    subject = _review_subject(receipt)
    return {
        "reviewSubjectSha256": subject,
        "reviewObjectives": {
            role: _review_objective(task, role, subject)
            for role in ("reviewer", "security-reviewer")
        },
    }


def _review_status(
    record: lifecycle.WorktreeRecord, task: str, receipt: dict
) -> dict[str, bool]:
    return {
        role: _completed_role(record, task, role, receipt)
        for role in ("reviewer", "security-reviewer")
    }


def _maintenance_stage(
    record: lifecycle.WorktreeRecord, task: str, contract: dict
) -> dict:
    status = lifecycle.state_status(lifecycle.state_path(record.path))
    if status == "merged":
        return {
            **contract,
            "status": "COMPLETED",
            "mode": "maintenance",
            "taskStatus": status,
            "stage": "merged",
        }
    if status != "initialized":
        raise MaintenanceError(
            f"maintenance lifecycle requires Task status initialized or merged; found {status}"
        )

    active = upgrade.receipt_path(record.path)
    consumed = upgrade.consumed_receipt_path(record.path)
    if active.exists():
        receipt = _validate_active_receipt(record, task)
        return {
            **contract,
            "status": "READY",
            "mode": "maintenance",
            "taskStatus": status,
            "stage": "applied",
            "sourceRevision": receipt["source_revision"],
        }
    if consumed.exists():
        receipt = _validate_consumed_receipt(record, task)
        commit = receipt["commit_sha"]
        remote = _remote_head(record)
        relation = _remote_relation(record, remote, commit)
        pr = _pr_evidence(record, contract["repository"], commit)
        stage = "pushed" if relation == "exact" else "committed"
        if pr is not None:
            if pr.get("state") == "MERGED":
                stage = "merged-remote"
            elif pr.get("isDraft") is True:
                stage = "draft-pr-created"
            elif pr.get("isDraft") is False:
                stage = "ready"
            else:
                raise MaintenanceError("maintenance pull request draft state is invalid")
        return {
            **contract,
            "status": "READY",
            "mode": "maintenance",
            "taskStatus": status,
            "stage": stage,
            "commit": commit,
            "sourceRevision": receipt["source_revision"],
            "remoteHead": remote,
            "remoteRelation": relation,
            "pr": pr["number"] if pr is not None else None,
            **_review_handoff(task, receipt),
            "reviewEvidence": _review_status(record, task, receipt),
        }

    try:
        initial = task_contract.check_contract(record.path, task)
    except task_contract.ContractError as exc:
        raise MaintenanceError(
            "maintenance Task has no valid maintenance receipt and is no longer pristine: "
            + str(exc)
        ) from exc
    return {
        **initial,
        "status": "READY",
        "mode": "maintenance",
        "taskStatus": status,
        "stage": "pristine",
    }


def maintenance_check(root_path: Path, task: str) -> dict:
    record, contract = _validated_contract(root_path, task)
    return _maintenance_stage(record, task, contract)


def _completed_role(
    record: lifecycle.WorktreeRecord, task: str, role: str, receipt: dict
) -> bool:
    subject = _review_subject(receipt)
    objective = _review_objective(task, role, subject)
    state = lifecycle.read_work_units(record, task)
    return any(
        isinstance(unit, dict)
        and unit.get("requested_role") == role
        and unit.get("state") == "completed"
        and unit.get("objective") == objective
        and unit.get("semantic_sha256") == lifecycle.semantic_digest(objective)
        and isinstance(unit.get("transitions"), list)
        and bool(unit["transitions"])
        and unit["transitions"][-1].get("to") == "completed"
        and isinstance(unit["transitions"][-1].get("evidence_sha256"), str)
        and re.fullmatch(
            r"[0-9a-f]{64}", unit["transitions"][-1]["evidence_sha256"]
        )
        is not None
        for unit in state.get("units", {}).values()
    )


def _require_review_evidence(
    record: lifecycle.WorktreeRecord, task: str, receipt: dict
) -> None:
    missing = [
        role
        for role in ("reviewer", "security-reviewer")
        if not _completed_role(record, task, role, receipt)
    ]
    if missing:
        raise MaintenanceError(
            "maintenance publication requires completed review evidence: "
            + ", ".join(missing)
        )


def maintenance_review_record(
    root_path: Path, task: str, role: str, evidence: str
) -> dict:
    if role not in {"reviewer", "security-reviewer"}:
        raise MaintenanceError("maintenance review role must be reviewer or security-reviewer")
    lifecycle.validate_evidence(evidence)
    if not evidence.startswith("status: COMPLETED;"):
        raise MaintenanceError("maintenance review evidence must start with status: COMPLETED;")
    record, _ = _validated_contract(root_path, task)
    receipt = _validate_consumed_receipt(record, task)
    subject = _review_subject(receipt)
    objective = _review_objective(task, role, subject)
    with lifecycle.work_units_lock(record):
        lifecycle.assert_task_identity(record, task)
        value = lifecycle.read_work_units(record, task)
        for identifier, unit in value.get("units", {}).items():
            if (
                isinstance(unit, dict)
                and unit.get("requested_role") == role
                and unit.get("objective") == objective
                and unit.get("state") == "completed"
            ):
                transitions = unit.get("transitions")
                transition = transitions[-1] if isinstance(transitions, list) and transitions else None
                if (
                    unit.get("semantic_sha256") == lifecycle.semantic_digest(objective)
                    and isinstance(transition, dict)
                    and transition.get("to") == "completed"
                    and transition.get("evidence") == evidence
                    and transition.get("evidence_sha256") == lifecycle.semantic_digest(evidence)
                ):
                    return {
                        "status": "ALREADY_RECORDED",
                        "task": task,
                        "role": role,
                        "workUnit": identifier,
                        "reviewSubjectSha256": subject,
                    }
                raise MaintenanceError(
                    "maintenance review role already has different or invalid completed evidence "
                    "for the exact review subject"
                )
        identifier = lifecycle.next_work_unit_id(value, task)
        unit = lifecycle.new_work_unit(identifier, role, objective)
        now = lifecycle.utc_now()
        transition = {
            "from": "in-flight",
            "to": "completed",
            "evidence": evidence,
            "evidence_sha256": lifecycle.semantic_digest(evidence),
            "recorded_at": now,
        }
        unit["state"] = "completed"
        unit["transitions"].append(transition)
        unit["updated_at"] = now
        value["units"][identifier] = unit
        lifecycle.persist_work_units(
            record,
            value,
            "maintenance_review_recorded="
            f"{identifier}; requested_role={role}; review_subject_sha256={subject}; "
            f"evidence_sha256={transition['evidence_sha256']}",
        )
    return {
        "status": "RECORDED",
        "task": task,
        "role": role,
        "workUnit": identifier,
        "reviewSubjectSha256": subject,
    }


def _direct_upgrade_publication(
    record: lifecycle.WorktreeRecord, receipt: dict
) -> list[str]:
    """Prove the whole Base..HEAD tree equals one direct upgrade to receipt source."""
    commit = receipt.get("commit_sha")
    source_text = receipt.get("source")
    revision = receipt.get("source_revision")
    if not isinstance(commit, str) or not isinstance(source_text, str) or not source_text:
        raise MaintenanceError(
            "consumed maintenance receipt publication provenance is incomplete"
        )
    try:
        base_revision = upgrade.validate_commit_oid(
            record.path,
            agent_core._base_revision(record.path),
            field="Task Base revision",
        )
        source = upgrade.resolve_source(Path(source_text))
        source_revision = upgrade.validate_commit_oid(
            source, revision, field="receipt source revision"
        )
    except (upgrade.UpgradeError, agent_core.AutomationError) as exc:
        raise MaintenanceError(str(exc)) from exc

    state_dir = upgrade.task_state_dir(record.path)
    try:
        with tempfile.TemporaryDirectory(
            prefix="maintenance-publication-", dir=state_dir
        ) as directory:
            temporary = Path(directory)
            baseline = temporary / "baseline"
            upgrade.materialize_tree(
                record.path, base_revision, baseline, surface_only=True
            )
            before = {
                path.relative_to(baseline).as_posix(): upgrade.bootstrap_fingerprint(
                    baseline, path.relative_to(baseline).as_posix()
                )
                for path in baseline.rglob("*")
                if path.is_file()
            }
            snapshot, source_core = upgrade.materialize_source_snapshot(
                source, source_revision, temporary / "source-snapshot"
            )
            plan = upgrade.build_plan(baseline, snapshot)
            if plan["blockers"]:
                raise MaintenanceError(
                    "direct maintenance publication reconstruction is blocked:\n- "
                    + "\n- ".join(plan["blockers"])
                )
            upgrade.apply_plan_to_tree(baseline, source_core, plan)
            returned = {
                item["path"]
                for item in plan["actions"]
                if item["action"] != "noop"
            }
            expected_paths = sorted(
                path
                for path in returned
                if before.get(
                    path,
                    {"state": "absent", "mode": None, "content_sha256": None},
                )
                != upgrade.bootstrap_fingerprint(baseline, path)
            )
            expected_fingerprints = {
                path: upgrade.bootstrap_fingerprint(baseline, path)
                for path in expected_paths
            }
    except upgrade.UpgradeError as exc:
        raise MaintenanceError(str(exc)) from exc

    actual_paths = sorted(
        line
        for line in upgrade.run(
            [
                "git",
                "diff",
                "--no-ext-diff",
                "--no-renames",
                "--name-only",
                f"{base_revision}...{commit}",
            ],
            cwd=record.path,
        ).stdout.splitlines()
        if line
    )
    if actual_paths != expected_paths:
        raise MaintenanceError(
            "maintenance branch diff does not exactly match a direct upgrade from Task Base"
        )
    try:
        current_fingerprints = {
            path: upgrade.file_fingerprint(record.path, path)
            for path in expected_paths
        }
    except upgrade.UpgradeError as exc:
        raise MaintenanceError(str(exc)) from exc
    if current_fingerprints != expected_fingerprints:
        raise MaintenanceError(
            "maintenance branch content does not match the reconstructed direct upgrade"
        )
    return expected_paths


def _publication_evidence(
    record: lifecycle.WorktreeRecord, task: str, receipt: dict
) -> tuple[list[str], str, str]:
    commit = receipt["commit_sha"]
    _require_review_evidence(record, task, receipt)
    try:
        publication.verification_evidence(record.path, task, commit)
        paths = _direct_upgrade_publication(record, receipt)
        title, body = publication.canonical_metadata(
            record.path, task, head=commit, changed_paths=paths
        )
    except publication.PublicationMetadataError as exc:
        raise MaintenanceError(str(exc)) from exc
    return paths, title, body


def maintenance_pr_create(root_path: Path, task: str) -> dict:
    record = lifecycle.require_local_task(root_path, task)
    ready = maintenance_check(root_path, task)
    if ready["stage"] not in {"pushed", "draft-pr-created"}:
        raise MaintenanceError(
            "maintenance PR creation requires pushed or draft-pr-created stage; "
            f"found {ready['stage']}"
        )
    receipt = _validate_consumed_receipt(record, task)
    commit = receipt["commit_sha"]
    remote = _remote_head(record)
    if remote != commit:
        raise MaintenanceError(
            "maintenance PR creation requires the exact commit on the remote Task branch"
        )
    _require_review_evidence(record, task, receipt)

    try:
        agent_core.verify(root_path, task)
        _, title, body_text = _publication_evidence(record, task, receipt)
        publication.write_metadata(root_path, title, body_text)
        _, body_path, validated_body = agent_core._validated_local_metadata(
            root_path, task, commit
        )
    except (agent_core.AutomationError, publication.PublicationMetadataError) as exc:
        raise MaintenanceError(str(exc)) from exc

    repository = ready["repository"]
    branch = record.branch
    assert branch is not None
    base = agent_core.default_branch(root_path)
    existing = agent_core.pr_for_branch(root_path, branch, repository)
    if existing is None:
        agent_core.gh(
            "pr",
            "create",
            "--repo",
            repository,
            "--draft",
            "--base",
            base,
            "--head",
            branch,
            "--title",
            title,
            "--body-file",
            str(body_path),
            cwd=root_path,
        )
    if agent_core.canonical_repository(root_path).casefold() != repository.casefold():
        raise MaintenanceError(
            "repository identity changed during maintenance pull request creation"
        )
    pr = agent_core.pr_for_branch(root_path, branch, repository)
    if pr is None:
        raise MaintenanceError("created maintenance pull request cannot be re-read")
    try:
        agent_core._validate_live_pr(
            pr,
            branch=branch,
            base=base,
            head=commit,
            title=title,
            body=validated_body,
            draft=True,
        )
    except agent_core.AutomationError as exc:
        raise MaintenanceError(str(exc)) from exc
    return {
        "status": "DRAFT_PR_READY",
        "mode": "maintenance",
        "stage": "draft-pr-created",
        "task": task,
        "pr": pr["number"],
        "head": commit,
        "repository": repository,
    }


def _merged_pr(
    root_path: Path,
    record: lifecycle.WorktreeRecord,
    repository: str,
    pr_number: int,
    commit: str,
    title: str,
    body: str,
) -> dict:
    details = agent_core.pr_details(record.path, str(pr_number))
    expected = {
        "headRefName": record.branch,
        "baseRefName": lifecycle.default_branch(root_path),
        "headRefOid": commit,
        "isCrossRepository": False,
        "state": "MERGED",
        "title": title,
    }
    mismatches = [
        name for name, wanted in expected.items() if details.get(name) != wanted
    ]
    if not publication.canonical_pr_body_matches(body, details.get("body")):
        mismatches.append("body")
    merge = details.get("mergeCommit")
    merge_oid = merge.get("oid") if isinstance(merge, dict) else None
    if (
        mismatches
        or details.get("number") != pr_number
        or not isinstance(merge_oid, str)
        or not re.fullmatch(r"[0-9a-fA-F]{40,64}", merge_oid)
    ):
        raise MaintenanceError(
            "merged maintenance pull request evidence is invalid: "
            + ", ".join(mismatches or ["number/mergeCommit"])
        )
    if agent_core.canonical_repository(record.path).casefold() != repository.casefold():
        raise MaintenanceError("maintenance repository identity changed")
    return {**details, "mergeCommitOid": merge_oid.lower()}


def _mark_maintenance_merged(
    record: lifecycle.WorktreeRecord,
    task: str,
    *,
    publication_line: str | None = None,
    validate_before_write: Callable[[], None] | None = None,
    default_branch: str | None = None,
    default_revision: str | None = None,
) -> str:
    """Atomically finalize Task State and its canonical publication evidence.

    All Git, network, and publication reconstruction must happen before this
    function is called.  The work-units lock is consequently held only while
    the identity, current state, and final byte contents are checked and
    written.
    """
    lifecycle.validate_task(task)
    lifecycle.require_resolved_contract(record, task)
    with lifecycle.work_units_lock(record):
        lifecycle.assert_task_identity(record, task)
        with _terminal_ref_locks(
            record,
            default_branch=default_branch,
            default_revision=default_revision,
        ):
            if validate_before_write is not None:
                validate_before_write()
            path = lifecycle.state_path(record.path)
            previous = lifecycle.state_status(path)
            if previous == "merged":
                if publication_line is not None:
                    _validate_finalized_publication(path.read_text(encoding="utf-8"), publication_line)
                return "already-finalized"
            if previous != "initialized":
                raise MaintenanceError(
                    f"maintenance finalization requires initialized or merged; found {previous}"
                )
            text = path.read_text(encoding="utf-8")
            marker = "- Status: initialized"
            if text.count(marker) != 1:
                raise MaintenanceError("maintenance Task State status marker is ambiguous")
            updated = text.replace(marker, "- Status: merged", 1)
            if publication_line is not None:
                updated = _add_finalized_publication(updated, publication_line)
            lifecycle.atomic_text(path, updated)
    return "finalized"


def _safe_ref_parent(common: Path, branch: str) -> Path:
    refs = common / "refs"
    heads = refs / "heads"
    for directory in (common, refs, heads):
        try:
            metadata = directory.lstat()
        except OSError as exc:
            raise MaintenanceError("maintenance ref directory is unavailable") from exc
        if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
            raise MaintenanceError("maintenance ref directory is unsafe")
    current = heads
    for part in branch.split("/")[:-1]:
        current = current / part
        try:
            current.mkdir(mode=0o700)
        except FileExistsError:
            pass
        try:
            metadata = current.lstat()
        except OSError as exc:
            raise MaintenanceError("maintenance ref directory is unavailable") from exc
        if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
            raise MaintenanceError("maintenance ref directory is unsafe")
    return current


@contextmanager
def _terminal_ref_locks(
    record: lifecycle.WorktreeRecord,
    *,
    default_branch: str | None,
    default_revision: str | None,
):
    """Hold Git's exact Task/default ref locks through terminal state commit."""
    branch = record.branch
    if branch is None or record.head is None:
        raise MaintenanceError("maintenance Task ref identity is incomplete")
    refs = [(branch, record.head)]
    if (default_branch is None) != (default_revision is None):
        raise MaintenanceError("maintenance default ref identity is incomplete")
    if default_branch is not None and default_revision is not None:
        refs.append((default_branch, default_revision))
    if len({name for name, _ in refs}) != len(refs):
        raise MaintenanceError("maintenance Task and default refs must be distinct")
    common = lifecycle.common_git_dir(record.path)
    acquired: list[tuple[int, Path, os.stat_result]] = []
    try:
        for name, revision in sorted(refs):
            lifecycle.validate_branch_name(name)
            parent = _safe_ref_parent(common, name)
            lock = parent / (name.rsplit("/", 1)[-1] + ".lock")
            flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
            if hasattr(os, "O_NOFOLLOW"):
                flags |= os.O_NOFOLLOW
            try:
                descriptor = os.open(lock, flags, 0o600)
            except OSError as exc:
                raise MaintenanceError(
                    "maintenance refs are concurrently locked or unavailable"
                ) from exc
            try:
                os.write(descriptor, (revision + "\n").encode("ascii", "strict"))
                os.fsync(descriptor)
                acquired.append((descriptor, lock, os.fstat(descriptor)))
            except Exception:
                os.close(descriptor)
                lock.unlink(missing_ok=True)
                raise
        yield
    finally:
        cleanup_error: MaintenanceError | None = None
        for descriptor, lock, locked in reversed(acquired):
            os.close(descriptor)
            try:
                current = lock.lstat()
            except OSError as exc:
                cleanup_error = MaintenanceError(
                    "maintenance ref lock changed unexpectedly"
                )
                cleanup_error.__cause__ = exc
                continue
            if not stat.S_ISREG(current.st_mode) or (current.st_dev, current.st_ino) != (
                locked.st_dev,
                locked.st_ino,
            ):
                cleanup_error = MaintenanceError(
                    "maintenance ref lock changed unexpectedly"
                )
                continue
            lock.unlink()
        if cleanup_error is not None:
            raise cleanup_error


def _publication_section(text: str) -> list[str] | None:
    heading = "### Maintenance publication"
    heading_lines = [line for line in text.splitlines() if line == heading]
    if len(heading_lines) > 1:
        raise MaintenanceError("maintenance publication evidence is duplicated")
    if not heading_lines:
        return None
    lines = text.splitlines()
    index = lines.index(heading)
    content: list[str] = []
    for line in lines[index + 1 :]:
        if line.startswith("#"):
            break
        if line.strip():
            content.append(line.strip())
    return content


def _validate_finalized_publication(text: str, publication_line: str) -> None:
    expected = "- " + publication_line
    if text.count(expected) > 1:
        raise MaintenanceError("maintenance publication evidence is duplicated")
    section = _publication_section(text)
    if section != [expected]:
        raise MaintenanceError(
            "maintenance publication evidence is missing or conflicting"
        )


def _add_finalized_publication(text: str, publication_line: str) -> str:
    expected = "- " + publication_line
    if expected in text:
        raise MaintenanceError("maintenance publication evidence is duplicated")
    section = _publication_section(text)
    if section not in (None, ["None yet."]):
        raise MaintenanceError("maintenance publication evidence is conflicting")
    heading = "### Maintenance publication"
    replacement = heading + "\n\n" + expected
    if section == ["None yet."]:
        return text.replace(heading + "\n\nNone yet.", replacement, 1)
    if section is not None:
        return text.replace(heading, replacement, 1)
    if "## Evidence" in text:
        return text.replace("## Evidence", "## Evidence\n\n" + replacement, 1)
    return text.rstrip("\n") + "\n\n## Evidence\n\n" + replacement + "\n"


def maintenance_finalize(root_path: Path, task: str, pr_number: int) -> dict:
    lifecycle.require_main_worktree(root_path)
    record, contract = _stored_contract(root_path, task)
    receipt = _validate_consumed_receipt(record, task)
    commit = receipt["commit_sha"]
    publication_evidence = _publication_evidence(record, task, receipt)
    _, title, body = publication_evidence
    first = _merged_pr(
        root_path, record, contract["repository"], pr_number, commit, title, body
    )
    sync = lifecycle.synchronize_default_branch(root_path)
    merge_oid = first["mergeCommitOid"]
    if (
        lifecycle.run(
            ["git", "merge-base", "--is-ancestor", merge_oid, sync["revision"]],
            cwd=root_path,
            check=False,
        ).returncode
        != 0
    ):
        raise MaintenanceError(
            "maintenance PR merge commit is not present on the synchronized default branch"
        )
    lifecycle.require_synchronized_default_branch_revision(
        root_path, sync["branch"], sync["revision"]
    )
    receipt_after = _validate_consumed_receipt(record, task)
    if _publication_evidence(record, task, receipt_after) != publication_evidence:
        raise MaintenanceError(
            "maintenance publication reconstruction changed during finalization"
        )
    second = _merged_pr(
        root_path, record, contract["repository"], pr_number, commit, title, body
    )
    if second["mergeCommitOid"] != merge_oid:
        raise MaintenanceError(
            "maintenance pull request merge evidence changed during finalization"
        )
    publication_line = (
        f"PR #{pr_number} merged from {commit}; merge commit {merge_oid}; "
        "finalization finalized"
    )

    def validate_terminal_evidence() -> None:
        lifecycle.require_synchronized_default_branch_revision(
            root_path, sync["branch"], sync["revision"]
        )
        terminal_receipt = _validate_consumed_receipt(record, task)
        if terminal_receipt != receipt_after:
            raise MaintenanceError(
                "maintenance receipt changed before terminal transition"
            )
        if _publication_evidence(record, task, terminal_receipt) != publication_evidence:
            raise MaintenanceError(
                "maintenance publication reconstruction changed before terminal transition"
            )
        terminal_pr = _merged_pr(
            root_path, record, contract["repository"], pr_number, commit, title, body
        )
        if terminal_pr["mergeCommitOid"] != merge_oid:
            raise MaintenanceError(
                "maintenance pull request merge evidence changed before terminal transition"
            )

    result = _mark_maintenance_merged(
        record,
        task,
        publication_line=publication_line,
        validate_before_write=validate_terminal_evidence,
        default_branch=sync["branch"],
        default_revision=sync["revision"],
    )
    return {
        "status": "FINALIZED",
        "mode": "maintenance",
        "stage": "merged",
        "task": task,
        "pr": pr_number,
        "publishedHead": commit,
        "mergeCommit": merge_oid,
        "defaultBranchRevision": sync["revision"],
        "transition": result,
    }


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser(
        description="Guarded Automation Maintenance lifecycle"
    )
    sub = value.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check")
    check.add_argument("task")
    create = sub.add_parser("pr-create")
    create.add_argument("task")
    review = sub.add_parser("review-record")
    review.add_argument("task")
    review.add_argument("role")
    review.add_argument("evidence")
    finalize = sub.add_parser("finalize")
    finalize.add_argument("task")
    finalize.add_argument("pr", type=int)
    return value


def main() -> int:
    args = parser().parse_args()
    try:
        root_path = root()
        if args.command == "check":
            result = maintenance_check(root_path, args.task)
        elif args.command == "pr-create":
            result = maintenance_pr_create(root_path, args.task)
        elif args.command == "review-record":
            result = maintenance_review_record(
                root_path, args.task, args.role, args.evidence
            )
        elif args.command == "finalize":
            result = maintenance_finalize(root_path, args.task, args.pr)
        else:
            raise MaintenanceError(f"unsupported command: {args.command}")
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (
        MaintenanceError,
        lifecycle.LifecycleError,
        upgrade.UpgradeError,
    ) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
