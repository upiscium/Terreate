#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
import task_lifecycle as lifecycle
import git_private_state as private_state


RECEIPT_SCHEMA_VERSION = 1
RECEIPT_OPERATION = "discard-pristine"
EXPECTED_STATE_FILES = {"task.md"}
REPOSITORY_RE = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")


def discard_receipt_path(root: Path, task: str) -> Path:
    lifecycle.validate_task(task)
    try:
        return private_state.discard_receipt(root, task)
    except private_state.GitPrivateStateError as exc:
        raise lifecycle.LifecycleError(str(exc)) from exc


def _branch_publication_configuration(record: lifecycle.WorktreeRecord) -> list[str]:
    assert record.branch is not None
    evidence: list[str] = []
    remote = lifecycle.git(
        "config",
        "--get",
        f"branch.{record.branch}.remote",
        cwd=record.path,
        check=False,
    )
    merge = lifecycle.git(
        "config",
        "--get",
        f"branch.{record.branch}.merge",
        cwd=record.path,
        check=False,
    )
    tracking = lifecycle.git(
        "rev-parse",
        "--verify",
        f"refs/remotes/origin/{record.branch}",
        cwd=record.path,
        check=False,
    )
    if remote:
        evidence.append("configured branch remote")
    if merge:
        evidence.append("configured upstream merge ref")
    if tracking:
        evidence.append("remote-tracking ref")
    return evidence


def _render_expected_pristine_state(
    record: lifecycle.WorktreeRecord,
    task: str,
    base_branch: str,
    base_revision: str,
) -> str:
    template = record.path / ".automation" / "templates" / "task-state.md"
    if template.is_symlink() or not template.is_file():
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task State template is unavailable or unsafe"
        )
    text = template.read_text(encoding="utf-8")
    values = {
        "@@TASK_ID@@": task,
        "@@BRANCH@@": record.branch or "",
        "@@WORKTREE@@": str(record.path),
        "@@BASE_BRANCH@@": base_branch,
        "@@BASE_REVISION@@": base_revision,
    }
    for marker, value in values.items():
        text = text.replace(marker, value)
    if any(marker in text for marker in values):
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task State template contains unresolved identity markers"
        )
    return text


def _require_pristine_unresolved_state(
    root: Path,
    record: lifecycle.WorktreeRecord,
    task: str,
) -> tuple[str, str]:
    lifecycle.assert_task_identity(record, task)
    state = lifecycle.state_path(record.path)
    if state.is_symlink() or not state.is_file():
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task State is unavailable or unsafe"
        )
    if lifecycle.state_status(state) != "initialized":
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task status must be exactly initialized"
        )

    state_dir = state.parent
    if state_dir.is_symlink() or not state_dir.is_dir():
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task State directory is unavailable or unsafe"
        )
    entries = {entry.name for entry in state_dir.iterdir()}
    if entries != EXPECTED_STATE_FILES:
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task State contains contract, Work Unit, or other lifecycle evidence"
        )

    text = state.read_text(encoding="utf-8")
    if "canonical-contract sha256=" in text:
        raise lifecycle.LifecycleError(
            "discard-pristine refused: canonical Task Contract evidence exists"
        )
    for relative in (".task-state/issue.json", ".task-state/contract.json"):
        if (record.path / relative).exists():
            raise lifecycle.LifecycleError(
                "discard-pristine refused: canonical Task Contract metadata exists"
            )

    work_units = lifecycle.read_work_units(record, task)
    if work_units.get("units"):
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Work Units already exist"
        )

    base_branch = lifecycle.extract_identity_value(state, "Base branch")
    base_revision = lifecycle.extract_identity_value(state, "Base revision")
    if (
        not isinstance(base_branch, str)
        or not base_branch
        or not isinstance(base_revision, str)
        or not re.fullmatch(r"[0-9a-fA-F]{40,64}", base_revision)
    ):
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task Base identity is missing or invalid"
        )
    base_revision = base_revision.lower()

    expected = _render_expected_pristine_state(
        record,
        task,
        base_branch,
        base_revision,
    )
    if text != expected:
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task State is not the untouched unresolved initialization state"
        )

    current_default = lifecycle.default_branch(root)
    if base_branch != current_default:
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task Base branch is not the repository default branch"
        )
    lifecycle.require_cleanup_base_revision(root, base_revision)
    return base_branch, base_revision


def _require_no_publication(
    root: Path,
    record: lifecycle.WorktreeRecord,
    repository: str,
) -> None:
    assert record.branch is not None
    configured = _branch_publication_configuration(record)
    if configured:
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task branch has publication configuration: "
            + ", ".join(configured)
        )
    remote_head = lifecycle.remote_branch_head(record)
    if remote_head is not None:
        raise lifecycle.LifecycleError(
            "discard-pristine refused: live remote Task branch exists"
        )
    if lifecycle.cleanup_prs(root, record.branch, repository):
        raise lifecycle.LifecycleError(
            "discard-pristine refused: pull request evidence exists for the Task branch"
        )


def pristine_discard_plan(root: Path, task: str) -> dict:
    lifecycle.require_main_worktree(root)
    record = lifecycle.worktree_for_task(root, task)
    lifecycle.assert_task_identity(record, task)
    if lifecycle.git(
        "status",
        "--porcelain",
        "--untracked-files=all",
        cwd=record.path,
    ):
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task worktree has uncommitted changes"
        )

    base_branch, base_revision = _require_pristine_unresolved_state(
        root,
        record,
        task,
    )
    branch = record.branch
    assert branch is not None
    local_head = lifecycle.git(
        "rev-parse",
        "--verify",
        f"refs/heads/{branch}",
        cwd=record.path,
    ).lower()
    if record.head is None or record.head.lower() != local_head:
        raise lifecycle.LifecycleError(
            "discard-pristine refused: registered Task head changed"
        )
    if local_head != base_revision:
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task branch HEAD differs from Base revision"
        )
    extra = lifecycle.git(
        "rev-list",
        "--count",
        f"{base_revision}..{branch}",
        cwd=record.path,
    )
    if int(extra or "0") != 0:
        raise lifecycle.LifecycleError(
            "discard-pristine refused: Task branch contains commits beyond Base revision"
        )

    repository = lifecycle.cleanup_repository(root)
    _require_no_publication(root, record, repository)
    return {
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "operation": RECEIPT_OPERATION,
        "task": task,
        "status": "initialized",
        "worktree": str(record.path),
        "branch": branch,
        "base_branch": base_branch,
        "base_revision": base_revision,
        "local_head": local_head,
        "repository": repository,
    }


def read_discard_receipt(root: Path, path: Path, task: str) -> dict:
    if path.is_symlink() or not path.is_file():
        raise lifecycle.LifecycleError(
            "discard-pristine receipt is not a regular local file"
        )
    try:
        value = json.loads(private_state.read_bytes(path, "discard-pristine receipt").decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, private_state.GitPrivateStateError) as exc:
        raise lifecycle.LifecycleError("discard-pristine receipt is invalid") from exc
    required = {
        "schema_version",
        "operation",
        "task",
        "status",
        "worktree",
        "branch",
        "base_branch",
        "base_revision",
        "local_head",
        "repository",
    }
    worktree = (
        Path(value.get("worktree", "")).resolve()
        if isinstance(value, dict)
        else Path()
    )
    worktree_is_expected = (
        worktree.parent == (root / ".worktrees").resolve()
        and worktree.name not in {"", ".", ".."}
    )
    if (
        not isinstance(value, dict)
        or set(value) != required
        or value.get("schema_version") != RECEIPT_SCHEMA_VERSION
        or value.get("operation") != RECEIPT_OPERATION
        or value.get("task") != task
        or value.get("status") != "initialized"
        or not isinstance(value.get("branch"), str)
        or not lifecycle.branch_matches_task(value.get("branch"), task)
        or not isinstance(value.get("base_branch"), str)
        or not value.get("base_branch")
        or not isinstance(value.get("base_revision"), str)
        or not re.fullmatch(r"[0-9a-fA-F]{40,64}", value["base_revision"])
        or not isinstance(value.get("local_head"), str)
        or not re.fullmatch(r"[0-9a-fA-F]{40,64}", value["local_head"])
        or value["local_head"].lower() != value["base_revision"].lower()
        or not isinstance(value.get("repository"), str)
        or not REPOSITORY_RE.fullmatch(value["repository"])
        or not worktree_is_expected
    ):
        raise lifecycle.LifecycleError("discard-pristine receipt is invalid")
    value["base_revision"] = value["base_revision"].lower()
    value["local_head"] = value["local_head"].lower()
    value["worktree"] = str(worktree)
    return value


def _revalidate_after_worktree_removal(root: Path, plan: dict) -> None:
    if lifecycle.default_branch(root) != plan["base_branch"]:
        raise lifecycle.LifecycleError(
            "discard-pristine refused: repository default branch changed"
        )
    lifecycle.require_cleanup_base_revision(root, plan["base_revision"])
    repository = lifecycle.cleanup_repository(root)
    if repository.casefold() != plan["repository"].casefold():
        raise lifecycle.LifecycleError(
            "discard-pristine refused: repository identity changed"
        )
    record = lifecycle.WorktreeRecord(
        root,
        plan["branch"],
        plan["local_head"],
    )
    _require_no_publication(root, record, repository)


def finish_pristine_discard(root: Path, plan: dict, receipt: Path) -> None:
    task = plan["task"]
    branch = plan["branch"]
    expected_path = Path(plan["worktree"]).resolve()
    expected_head = plan["local_head"].lower()

    records = lifecycle.parse_worktrees(root)
    registered = [
        record
        for record in records
        if record.path == expected_path or record.branch == branch
    ]
    if registered:
        if (
            len(registered) != 1
            or registered[0].path != expected_path
            or registered[0].branch != branch
        ):
            raise lifecycle.LifecycleError(
                "discard-pristine receipt conflicts with current worktree registration"
            )
        current = pristine_discard_plan(root, task)
        if current != plan:
            raise lifecycle.LifecycleError(
                "discard-pristine evidence changed before worktree removal"
            )
        lifecycle.run(
            ["git", "worktree", "remove", str(expected_path)],
            cwd=root,
        )

    if any(
        record.path == expected_path or record.branch == branch
        for record in lifecycle.parse_worktrees(root)
    ):
        raise lifecycle.LifecycleError(
            "discard-pristine failed to remove the expected worktree registration"
        )

    _revalidate_after_worktree_removal(root, plan)
    branch_ref = f"refs/heads/{branch}"
    actual = lifecycle.git(
        "rev-parse",
        "--verify",
        branch_ref,
        cwd=root,
        check=False,
    ).lower()
    if actual:
        if actual != expected_head:
            raise lifecycle.LifecycleError(
                "discard-pristine refused: local Task branch moved after validation"
            )
        lifecycle.run(
            ["git", "update-ref", "-d", branch_ref, expected_head],
            cwd=root,
        )

    if lifecycle.git(
        "rev-parse",
        "--verify",
        branch_ref,
        cwd=root,
        check=False,
    ):
        raise lifecycle.LifecycleError(
            "discard-pristine failed to delete the expected local Task branch"
        )

    try:
        private_state.unlink(receipt)
    except private_state.GitPrivateStateError as exc:
        raise lifecycle.LifecycleError(str(exc)) from exc
    print(
        json.dumps(
            {
                "task": task,
                "discarded": True,
                "removedWorktree": str(expected_path),
                "removedBranch": branch,
                "baseRevision": plan["base_revision"],
            },
            sort_keys=True,
        )
    )


def task_discard_pristine(root: Path, task: str) -> None:
    lifecycle.require_main_worktree(root)
    receipt = discard_receipt_path(root, task)
    with lifecycle.cleanup_lock(root):
        if receipt.exists():
            finish_pristine_discard(
                root,
                read_discard_receipt(root, receipt, task),
                receipt,
            )
            return
        plan = pristine_discard_plan(root, task)
        try:
            private_state.write_bytes(
                receipt, (json.dumps(plan, sort_keys=True) + "\n").encode("utf-8")
            )
        except private_state.GitPrivateStateError as exc:
            raise lifecycle.LifecycleError(str(exc)) from exc
        finish_pristine_discard(root, plan, receipt)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Guardedly discard an untouched unresolved Task"
    )
    result.add_argument("task")
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        task_discard_pristine(lifecycle.repo_root(), args.task)
    except lifecycle.LifecycleError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
