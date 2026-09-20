#!/usr/bin/env python3
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
import git_private_state as private_state

TASK_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
WORK_UNIT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
WORK_UNIT_ROLES = {"general", "explore", "verifier", "reviewer", "investigator", "security-reviewer", "scout"}
WORK_UNIT_STATES = {"in-flight", "failed", "completed", "blocked", "needs-approval", "needs-decision"}
WORK_UNIT_TRANSITIONS = {
    "in-flight": {"failed", "completed", "blocked", "needs-approval", "needs-decision"},
    "failed": set(),
    "completed": set(),
    "blocked": set(),
    "needs-approval": set(),
    "needs-decision": set(),
}
VALID_STATES = {
    "initialized",
    "researching",
    "planning",
    "implementing",
    "verification-pending",
    "local-verified",
    "review-pending",
    "publication-ready",
    "draft-pr-created",
    "integration-pending",
    "merged",
    "blocked",
    "cancelled",
}
TERMINAL_STATES = {"merged", "cancelled"}
REQUIRED_TASK_CONTRACT_SECTIONS = (
    "Purpose",
    "Scope",
    "Prohibited changes",
    "Dependencies",
    "Acceptance criteria",
    "Test plan",
    "Stop conditions",
    "Coordination surfaces",
    "External resources",
)
LINEAR_TRANSITIONS = {
    "initialized": {"researching", "planning", "blocked", "cancelled"},
    "researching": {"planning", "blocked", "cancelled"},
    "planning": {"implementing", "blocked", "cancelled"},
    "implementing": {"verification-pending", "blocked", "cancelled"},
    "verification-pending": {"implementing", "local-verified", "blocked", "cancelled"},
    "local-verified": {"review-pending", "implementing", "blocked", "cancelled"},
    "review-pending": {"publication-ready", "implementing", "blocked", "cancelled"},
    "publication-ready": {"draft-pr-created", "implementing", "blocked", "cancelled"},
    "draft-pr-created": {"integration-pending", "implementing", "blocked", "cancelled"},
    "integration-pending": {"merged", "implementing", "blocked", "cancelled"},
    "blocked": {"planning", "implementing", "verification-pending", "cancelled"},
    "merged": set(),
    "cancelled": set(),
}


class LifecycleError(RuntimeError):
    pass


@dataclass(frozen=True)
class WorktreeRecord:
    path: Path
    branch: str | None
    head: str | None


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    check: bool = True,
    remove_env: tuple[str, ...] = (),
) -> subprocess.CompletedProcess[str]:
    environment = {
        key: value for key, value in os.environ.items() if not key.startswith("GIT_")
    }
    if command and command[0] == "git":
        environment.update(
            {
                "GIT_CONFIG_NOSYSTEM": "1",
                "GIT_CONFIG_GLOBAL": os.devnull,
                "GIT_OPTIONAL_LOCKS": "0",
                "GIT_TERMINAL_PROMPT": "0",
            }
        )
    for name in remove_env:
        environment.pop(name, None)
    result = subprocess.run(
        command, cwd=cwd, text=True, capture_output=True, env=environment
    )
    if check and result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit {result.returncode}"
        raise LifecycleError(f"{' '.join(command)}: {detail}")
    return result


def git(*args: str, cwd: Path, check: bool = True) -> str:
    return run(["git", *args], cwd=cwd, check=check).stdout.strip()


def gh(*args: str, cwd: Path, check: bool = True) -> subprocess.CompletedProcess[str]:
    return run(["gh", *args], cwd=cwd, check=check, remove_env=("GH_REPO",))


def repo_root(cwd: Path | None = None) -> Path:
    result = run(["git", "rev-parse", "--show-toplevel"], cwd=cwd)
    return Path(result.stdout.strip()).resolve()


def common_git_dir(root: Path) -> Path:
    value = Path(git("rev-parse", "--git-common-dir", cwd=root))
    return value if value.is_absolute() else (root / value).resolve()


def default_branch(root: Path) -> str:
    symbolic = git(
        "symbolic-ref",
        "--quiet",
        "--short",
        "refs/remotes/origin/HEAD",
        cwd=root,
        check=False,
    )
    if symbolic.startswith("origin/"):
        return symbolic.removeprefix("origin/")
    result = gh("repo", "view", "--json", "defaultBranchRef", cwd=root, check=False)
    if result.returncode == 0:
        try:
            name = json.loads(result.stdout).get("defaultBranchRef", {}).get("name")
        except json.JSONDecodeError:
            name = None
        if name:
            return name
    raise LifecycleError(
        "cannot resolve default branch; configure origin/HEAD or GitHub CLI access"
    )


def validate_task(task: str) -> None:
    if not TASK_RE.fullmatch(task):
        raise LifecycleError(f"invalid Task ID: {task!r}")


def validate_slug(slug: str) -> None:
    if not SLUG_RE.fullmatch(slug):
        raise LifecycleError(f"invalid Task slug: {slug!r}")


def branch_matches_task(branch: str | None, task: str) -> bool:
    if not branch:
        return False
    return branch.startswith(f"task/{task}-") or branch.startswith(f"fix/{task}-")


def parse_worktrees(root: Path) -> list[WorktreeRecord]:
    records: list[WorktreeRecord] = []
    current: dict[str, str] = {}
    lines = git("worktree", "list", "--porcelain", cwd=root).splitlines() + [""]
    for line in lines:
        if not line:
            if current:
                branch = current.get("branch")
                records.append(
                    WorktreeRecord(
                        path=Path(current["worktree"]).resolve(),
                        branch=branch.removeprefix("refs/heads/") if branch else None,
                        head=current.get("HEAD"),
                    )
                )
                current = {}
            continue
        key, _, value = line.partition(" ")
        current[key] = value
    return records


def current_worktree(root: Path) -> WorktreeRecord:
    matches = [record for record in parse_worktrees(root) if record.path == root.resolve()]
    if len(matches) != 1:
        raise LifecycleError(
            f"cannot uniquely resolve current worktree {root}: found {len(matches)}"
        )
    return matches[0]


def main_worktree(root: Path) -> WorktreeRecord:
    base = default_branch(root)
    matches = [record for record in parse_worktrees(root) if record.branch == base]
    if len(matches) != 1:
        raise LifecycleError(
            f"cannot uniquely resolve default-branch worktree for {base}: found {len(matches)}"
        )
    return matches[0]


def require_main_worktree(root: Path) -> WorktreeRecord:
    current = current_worktree(root)
    main = main_worktree(root)
    if current.path != main.path or current.branch != main.branch:
        raise LifecycleError(
            f"operation must run from the default-branch worktree: {main.path}"
        )
    return current


def validate_branch_name(branch: str) -> None:
    if not branch or branch.startswith("-"):
        raise LifecycleError(f"invalid default branch name: {branch!r}")
    result = run(
        ["git", "check-ref-format", "--branch", branch],
        check=False,
    )
    if result.returncode != 0:
        raise LifecycleError(f"invalid default branch name: {branch!r}")


def synchronize_default_branch(root: Path) -> dict:
    """Fetch and fast-forward only the checked-out origin default branch."""
    main = require_main_worktree(root)
    base = main.branch
    if base is None:
        raise LifecycleError("default-branch worktree is detached")
    validate_branch_name(base)
    if not git("remote", "get-url", "origin", cwd=root, check=False):
        raise LifecycleError("configured origin remote is required")
    if git("status", "--porcelain", "--untracked-files=all", cwd=root):
        raise LifecycleError("default-branch worktree must be clean before synchronization")

    local_ref = f"refs/heads/{base}"
    remote_ref = f"refs/remotes/origin/{base}"
    local_before = git("rev-parse", "--verify", local_ref, cwd=root)
    remote_before = git("rev-parse", "--verify", remote_ref, cwd=root, check=False)
    run(
        [
            "git",
            "fetch",
            "--no-tags",
            "origin",
            f"refs/heads/{base}:{remote_ref}",
        ],
        cwd=root,
    )
    fetched = git("rev-parse", "--verify", remote_ref, cwd=root)

    if remote_before and run(
        ["git", "merge-base", "--is-ancestor", remote_before, fetched],
        cwd=root,
        check=False,
    ).returncode != 0:
        raise LifecycleError("origin default branch moved non-fast-forward")
    if run(
        ["git", "merge-base", "--is-ancestor", local_before, fetched],
        cwd=root,
        check=False,
    ).returncode != 0:
        raise LifecycleError(
            "local default branch cannot be fast-forwarded; local-only commits or divergence exist"
        )
    if git("rev-parse", "--verify", remote_ref, cwd=root) != fetched:
        raise LifecycleError("origin default-branch ref moved during synchronization")
    if git("status", "--porcelain", "--untracked-files=all", cwd=root):
        raise LifecycleError("default-branch worktree changed during synchronization")

    if local_before != fetched:
        run(["git", "merge", "--ff-only", "--no-edit", fetched], cwd=root)

    local_after = git("rev-parse", "--verify", local_ref, cwd=root)
    remote_after = git("rev-parse", "--verify", remote_ref, cwd=root)
    if local_after != fetched or remote_after != fetched:
        raise LifecycleError("default-branch refs changed during synchronization")
    if git("status", "--porcelain", "--untracked-files=all", cwd=root):
        raise LifecycleError("default-branch worktree is not clean after synchronization")
    return {
        "branch": base,
        "revision": fetched,
        "previousRevision": local_before,
        "updated": local_before != fetched,
    }


def require_synchronized_default_branch_revision(
    root: Path, branch: str, revision: str
) -> None:
    main = require_main_worktree(root)
    if main.branch != branch:
        raise LifecycleError("default branch changed during guarded operation")
    local = git("rev-parse", "--verify", f"refs/heads/{branch}", cwd=root)
    remote = git(
        "rev-parse", "--verify", f"refs/remotes/origin/{branch}", cwd=root
    )
    if local != revision or remote != revision:
        raise LifecycleError("default-branch refs moved after synchronization")
    if git("status", "--porcelain", "--untracked-files=all", cwd=root):
        raise LifecycleError("default-branch worktree changed after synchronization")


def worktree_for_task(root: Path, task: str) -> WorktreeRecord:
    validate_task(task)
    candidates = [
        record for record in parse_worktrees(root) if branch_matches_task(record.branch, task)
    ]
    if len(candidates) != 1:
        raise LifecycleError(
            f"expected exactly one registered worktree for {task}, found {len(candidates)}"
        )
    return candidates[0]


def require_local_task(root: Path, task: str) -> WorktreeRecord:
    record = worktree_for_task(root, task)
    if record.path != root.resolve():
        raise LifecycleError(
            f"Task {task} belongs to sibling worktree {record.path}; current worktree is {root}"
        )
    assert_task_identity(record, task)
    return record


def ensure_excludes(root: Path) -> None:
    exclude = common_git_dir(root) / "info" / "exclude"
    exclude.parent.mkdir(parents=True, exist_ok=True)
    existing = exclude.read_text(encoding="utf-8").splitlines() if exclude.exists() else []
    if "/.task-state/" not in existing:
        with exclude.open("a", encoding="utf-8") as handle:
            if existing and existing[-1] != "":
                handle.write("\n")
            handle.write("/.task-state/\n")


def state_path(worktree: Path) -> Path:
    return worktree / ".task-state" / "task.md"


def work_units_path(worktree: Path) -> Path:
    return worktree / ".task-state" / "work-units.json"


def work_units_lock_path(worktree: Path) -> Path:
    return worktree / ".task-state" / "work-units.lock"


@contextmanager
def work_units_lock(record: WorktreeRecord):
    path = work_units_lock_path(record.path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def atomic_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as handle:
        handle.write(text)
        temporary = Path(handle.name)
    temporary.replace(path)


def atomic_json(path: Path, value: dict) -> None:
    atomic_text(path, json.dumps(value, sort_keys=True) + "\n")


def append_task_evidence(path: Path, heading: str, line: str) -> None:
    text = path.read_text(encoding="utf-8")
    placeholder = heading + "\n\nNone yet."
    if placeholder in text:
        text = text.replace(placeholder, heading + "\n\n- " + line, 1)
    elif heading in text:
        text = text.replace(heading, heading + "\n\n- " + line, 1)
    elif "## Evidence" in text:
        text = text.replace("## Evidence", "## Evidence\n\n" + heading + "\n- " + line, 1)
    else:
        text += "\n## Evidence\n\n" + heading + "\n- " + line
    atomic_text(path, text)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def semantic_digest(objective: str) -> str:
    return hashlib.sha256(objective.encode("utf-8")).hexdigest()


def validate_objective(objective: str) -> None:
    if not objective or len(objective) > 2000 or any(ord(char) < 32 for char in objective):
        raise LifecycleError("Work Unit objective must be a non-empty single line of at most 2000 characters")


def validate_evidence(evidence: str) -> None:
    if not evidence or len(evidence) > 4000 or any(ord(char) < 32 for char in evidence):
        raise LifecycleError("Work Unit evidence must be a non-empty single line of at most 4000 characters")


def validate_failure_field(name: str, value: str, maximum: int) -> None:
    if not value or len(value) > maximum or any(ord(char) < 32 for char in value):
        raise LifecycleError(
            f"provider failure {name} must be a non-empty single line of at most {maximum} characters"
        )


def configured_agent_model(worktree: Path, role: str) -> str:
    if role not in WORK_UNIT_ROLES:
        raise LifecycleError(f"invalid persisted Work Unit role: {role!r}")
    path = worktree / ".opencode" / "agents" / f"{role}.md"
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise LifecycleError(f"cannot read configured agent for role {role}: {path}") from exc
    if not lines or lines[0] != "---":
        raise LifecycleError(f"configured agent has invalid frontmatter: {path}")
    try:
        frontmatter_end = lines.index("---", 1)
    except ValueError as exc:
        raise LifecycleError(f"configured agent has invalid frontmatter: {path}") from exc
    declarations = []
    for line in lines[1:frontmatter_end]:
        match = re.fullmatch(r"model:\s*(.*?)\s*", line)
        if match is not None:
            declarations.append(match.group(1))
    if len(declarations) != 1:
        raise LifecycleError(
            f"configured agent must declare exactly one model for role {role}: {path}"
        )
    configured = declarations[0]
    if len(configured) >= 2 and configured[0] == configured[-1] and configured[0] in {'"', "'"}:
        configured = configured[1:-1]
    if not re.fullmatch(r"[^\s/]+/[^\s/]+", configured):
        raise LifecycleError(f"configured agent has invalid model for role {role}: {path}")
    return configured


def empty_work_units(record: WorktreeRecord, task: str) -> dict:
    return {
        "schema_version": 1,
        "task_id": task,
        "worktree": str(record.path),
        "branch": record.branch,
        "units": {},
    }


def read_work_units(record: WorktreeRecord, task: str) -> dict:
    path = work_units_path(record.path)
    if not path.is_file():
        return empty_work_units(record, task)
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise LifecycleError(f"invalid Work Unit state JSON: {path}") from exc
    expected = {"schema_version": 1, "task_id": task, "worktree": str(record.path), "branch": record.branch}
    mismatches = [key for key, expected_value in expected.items() if value.get(key) != expected_value]
    if mismatches or not isinstance(value.get("units"), dict):
        raise LifecycleError("Work Unit state identity mismatch: " + ", ".join(mismatches or ["units"]))
    return value


def canonical_work_unit_sequence(task: str, work_unit: str) -> int | None:
    match = re.fullmatch(rf"WU-{re.escape(task)}-([0-9]+)", work_unit)
    if match is None:
        return None
    sequence = int(match.group(1))
    if sequence < 1 or match.group(1) != f"{sequence:02d}":
        return None
    return sequence


def next_work_unit_id(value: dict, task: str) -> str:
    sequences = [
        sequence
        for work_unit in value["units"]
        if (sequence := canonical_work_unit_sequence(task, work_unit)) is not None
    ]
    work_unit = f"WU-{task}-{max(sequences, default=0) + 1:02d}"
    if not WORK_UNIT_RE.fullmatch(work_unit):
        raise LifecycleError(f"generated Work Unit ID is invalid: {work_unit!r}")
    return work_unit


def new_work_unit(work_unit: str, role: str, objective: str) -> dict:
    now = utc_now()
    return {
        "id": work_unit,
        "requested_role": role,
        "objective": objective,
        "semantic_sha256": semantic_digest(objective),
        "state": "in-flight",
        "transitions": [],
        "created_at": now,
        "updated_at": now,
    }


def persist_work_units(record: WorktreeRecord, value: dict, evidence: str) -> None:
    units_path = work_units_path(record.path)
    previous_units = units_path.read_text(encoding="utf-8") if units_path.exists() else None
    atomic_json(units_path, value)
    try:
        append_task_evidence(state_path(record.path), "## Work Units", evidence)
    except Exception:
        if previous_units is None:
            units_path.unlink(missing_ok=True)
        else:
            atomic_text(units_path, previous_units)
        raise


def validate_work_unit_request(role: str, objective: str) -> None:
    if role not in WORK_UNIT_ROLES:
        raise LifecycleError(f"invalid Work Unit role: {role}")
    validate_objective(objective)


def work_unit_next(root: Path, task: str) -> None:
    record = require_local_task(root, task)
    work_unit = next_work_unit_id(read_work_units(record, task), task)
    print(json.dumps({"task_id": task, "next_work_unit": work_unit}, sort_keys=True))


def work_unit_create(root: Path, task: str, role: str, objective: str) -> None:
    record = require_local_task(root, task)
    require_resolved_contract(record, task)
    validate_work_unit_request(role, objective)
    with work_units_lock(record):
        assert_task_identity(record, task)
        value = read_work_units(record, task)
        work_unit = next_work_unit_id(value, task)
        if work_unit in value["units"]:
            raise LifecycleError(f"Work Unit already exists: {work_unit}")
        unit = new_work_unit(work_unit, role, objective)
        value["units"][work_unit] = unit
        persist_work_units(
            record,
            value,
            f"work_unit_created={work_unit}; requested_role={role}; semantic_sha256={unit['semantic_sha256']}; state=in-flight",
        )
    print(json.dumps(unit, sort_keys=True))


def work_unit_register(root: Path, task: str, work_unit: str, role: str, objective: str) -> None:
    record = require_local_task(root, task)
    require_resolved_contract(record, task)
    if not WORK_UNIT_RE.fullmatch(work_unit):
        raise LifecycleError(f"invalid Work Unit ID: {work_unit!r}")
    validate_work_unit_request(role, objective)
    with work_units_lock(record):
        assert_task_identity(record, task)
        value = read_work_units(record, task)
        if work_unit in value["units"]:
            raise LifecycleError(f"Work Unit already exists: {work_unit}")
        unit = new_work_unit(work_unit, role, objective)
        value["units"][work_unit] = unit
        persist_work_units(
            record,
            value,
            f"work_unit_registered={work_unit}; requested_role={role}; semantic_sha256={unit['semantic_sha256']}; state=in-flight",
        )
    print(json.dumps(unit, sort_keys=True))


def work_unit_status(root: Path, task: str, work_unit: str) -> None:
    record = require_local_task(root, task)
    unit = read_work_units(record, task)["units"].get(work_unit)
    if unit is None:
        raise LifecycleError(f"unknown Work Unit: {work_unit}")
    print(json.dumps(unit, sort_keys=True))


def work_unit_dispatch_check(
    root: Path, task: str, work_unit: str, role: str, objective: str
) -> None:
    record = require_local_task(root, task)
    validate_work_unit_request(role, objective)
    unit = read_work_units(record, task)["units"].get(work_unit)
    if not isinstance(unit, dict):
        raise LifecycleError(f"unknown Work Unit: {work_unit}")
    if unit.get("id") != work_unit:
        raise LifecycleError(f"Work Unit persisted identity mismatch: {work_unit}")
    if unit.get("state") != "in-flight":
        raise LifecycleError(
            f"Work Unit is not dispatchable: {work_unit} state={unit.get('state')}"
        )
    if unit.get("requested_role") != role:
        raise LifecycleError(
            "Work Unit dispatch role mismatch: "
            f"registered={unit.get('requested_role')}, delegated={role}"
        )
    if unit.get("objective") != objective:
        raise LifecycleError("Work Unit dispatch objective mismatch")
    digest = semantic_digest(objective)
    if unit.get("semantic_sha256") != digest:
        raise LifecycleError("Work Unit persisted objective digest mismatch")
    configured_model = configured_agent_model(record.path, role)
    print(
        json.dumps(
            {
                "status": "READY",
                "task_id": task,
                "work_unit": work_unit,
                "requested_role": role,
                "objective": objective,
                "semantic_sha256": digest,
                "configured_model": configured_model,
            },
            sort_keys=True,
        )
    )


def work_unit_state_set(
    root: Path,
    task: str,
    work_unit: str,
    status: str,
    evidence: str,
    provider: str | None,
    model: str | None,
    error: str | None,
) -> None:
    record = require_local_task(root, task)
    require_resolved_contract(record, task)
    if status not in WORK_UNIT_STATES:
        raise LifecycleError(f"invalid Work Unit state: {status}")
    validate_evidence(evidence)
    failure_fields = (provider, model, error)
    supplied_failure_fields = tuple(value is not None for value in failure_fields)
    if any(supplied_failure_fields) and not all(supplied_failure_fields):
        raise LifecycleError("provider failure evidence requires provider, model, and error together")
    if all(supplied_failure_fields):
        if status != "blocked":
            raise LifecycleError("provider failure evidence is only valid for a blocked Work Unit")
        assert provider is not None and model is not None and error is not None
        validate_failure_field("provider", provider, 200)
        validate_failure_field("model", model, 200)
        validate_failure_field("error", error, 4000)
    with work_units_lock(record):
        assert_task_identity(record, task)
        value = read_work_units(record, task)
        unit = value["units"].get(work_unit)
        if unit is None:
            raise LifecycleError(f"unknown Work Unit: {work_unit}")
        previous = unit.get("state")
        if status == previous:
            print(json.dumps(unit, sort_keys=True))
            return
        if status not in WORK_UNIT_TRANSITIONS.get(previous, set()):
            raise LifecycleError(f"invalid Work Unit transition: {previous} -> {status}")
        if all(supplied_failure_fields):
            assert provider is not None and model is not None
            configured = configured_agent_model(record.path, unit.get("requested_role"))
            reported = f"{provider}/{model}"
            if reported != configured:
                raise LifecycleError(
                    "provider failure model does not match configured Work Unit role: "
                    f"role={unit.get('requested_role')}, configured={configured}, reported={reported}"
                )
        now = utc_now()
        transition = {
            "from": previous,
            "to": status,
            "evidence": evidence,
            "evidence_sha256": semantic_digest(evidence),
            "recorded_at": now,
        }
        if all(supplied_failure_fields):
            transition["provider_failure"] = {
                "provider": provider,
                "model": model,
                "error": error,
            }
        unit["state"] = status
        unit.setdefault("transitions", []).append(transition)
        unit["updated_at"] = now
        persist_work_units(
            record,
            value,
            f"work_unit_state={work_unit}; previous={previous}; state={status}; evidence_sha256={transition['evidence_sha256']}; evidence={json.dumps(evidence)}",
        )
    print(json.dumps(unit, sort_keys=True))


def state_status(path: Path) -> str:
    if not path.is_file():
        raise LifecycleError(f"missing Task State: {path}")
    text = path.read_text(encoding="utf-8")
    sections = re.findall(r"(?ms)^## Current state\n\n(.*?)(?=^## |\Z)", text)
    statuses = re.findall(r"(?m)^- Status: ([A-Za-z0-9._-]+)$", text)
    section_statuses = (
        re.findall(r"(?m)^- Status: ([A-Za-z0-9._-]+)$", sections[0])
        if len(sections) == 1
        else []
    )
    if (
        len(sections) != 1
        or len(statuses) != 1
        or len(section_statuses) != 1
        or statuses != section_statuses
        or statuses[0] not in VALID_STATES
    ):
        raise LifecycleError(f"invalid or missing Task State status in {path}")
    return statuses[0]


def set_state_status(path: Path, status: str) -> None:
    if status not in VALID_STATES:
        raise LifecycleError(f"invalid Task State status: {status}")
    previous = state_status(path)
    if status == previous:
        return
    if status not in LINEAR_TRANSITIONS[previous]:
        raise LifecycleError(f"invalid Task State transition: {previous} -> {status}")
    text = path.read_text(encoding="utf-8")
    updated, count = re.subn(
        r"(?m)^- Status: [A-Za-z0-9._-]+$",
        f"- Status: {status}",
        text,
        count=1,
    )
    if count != 1:
        raise LifecycleError(f"cannot update Task State status in {path}")
    atomic_text(path, updated)


def initialize_state(
    worktree: Path, task: str, branch: str, base: str, base_revision: str
) -> None:
    template = worktree / ".automation" / "templates" / "task-state.md"
    if not template.is_file():
        raise LifecycleError(f"missing Task State template: {template}")
    destination = state_path(worktree)
    destination.parent.mkdir(parents=True, exist_ok=True)
    text = template.read_text(encoding="utf-8")
    values = {
        "@@TASK_ID@@": task,
        "@@BRANCH@@": branch,
        "@@WORKTREE@@": str(worktree),
        "@@BASE_BRANCH@@": base,
        "@@BASE_REVISION@@": base_revision,
    }
    for marker, value in values.items():
        text = text.replace(marker, value)
    destination.write_text(text, encoding="utf-8")


def assert_task_identity(record: WorktreeRecord, task: str) -> None:
    if not branch_matches_task(record.branch, task):
        raise LifecycleError(
            f"worktree branch does not match Task {task}: {record.branch}"
        )
    state = state_path(record.path)
    if not state.is_file():
        raise LifecycleError(f"missing Task State for {task}: {state}")
    text = state.read_text(encoding="utf-8")
    expected = {
        f"- Task ID: {task}",
        f"- Branch: {record.branch}",
        f"- Worktree: {record.path}",
    }
    missing = [line for line in expected if line not in text]
    if missing:
        raise LifecycleError("Task State identity mismatch: " + ", ".join(missing))


def require_resolved_contract(record: WorktreeRecord, task: str) -> None:
    """Block every Task mutation until strict read-only initialization can pass."""
    path = state_path(record.path)
    text = path.read_text(encoding="utf-8")
    missing = [
        section
        for section in REQUIRED_TASK_CONTRACT_SECTIONS
        if f"## {section}" not in text
    ]
    if missing:
        raise LifecycleError(
            "Task Contract is unresolved; missing required sections: "
            + ", ".join(missing)
        )
    if any(token in text for token in ("TBD", "Define Task-specific", "- Unverified: Task contract")):
        raise LifecycleError("Task Contract is unresolved; mutation is forbidden before initialization")
    canonical = "canonical-contract sha256=" in text
    metadata = any((record.path / relative).is_file() for relative in (".task-state/issue.json", ".task-state/contract.json"))
    if canonical or metadata:
        from task_contract import validate_contract

        validate_contract(record.path, task)


def task_start(root: Path, task: str, slug: str, *, quiet: bool = False) -> WorktreeRecord:
    require_main_worktree(root)
    validate_task(task)
    validate_slug(slug)
    branch = f"task/{task}-{slug}"
    worktree = root / ".worktrees" / f"{task}-{slug}"
    records = parse_worktrees(root)

    if any(branch_matches_task(record.branch, task) for record in records):
        raise LifecycleError(f"Task already has a registered worktree: {task}")
    if any(record.branch == branch for record in records):
        raise LifecycleError(f"branch is already registered in a worktree: {branch}")
    if any(record.path == worktree.resolve() for record in records):
        raise LifecycleError(f"worktree is already registered: {worktree}")
    if worktree.exists():
        raise LifecycleError(f"worktree path already exists: {worktree}")
    if (
        run(
            ["git", "show-ref", "--verify", "--quiet", f"refs/heads/{branch}"],
            cwd=root,
            check=False,
        ).returncode
        == 0
    ):
        raise LifecycleError(f"branch already exists: {branch}")

    synchronized = synchronize_default_branch(root)
    base = synchronized["branch"]
    base_revision = synchronized["revision"]

    worktree.parent.mkdir(parents=True, exist_ok=True)
    run(
        ["git", "worktree", "add", "-b", branch, str(worktree), base_revision],
        cwd=root,
    )
    try:
        ensure_excludes(worktree)
        initialize_state(worktree, task, branch, base, base_revision)
    except Exception:
        run(
            ["git", "worktree", "remove", "--force", str(worktree)],
            cwd=root,
            check=False,
        )
        run(["git", "branch", "-D", branch], cwd=root, check=False)
        raise
    if not quiet:
        print(
            json.dumps(
            {
                "task": task,
                "branch": branch,
                "worktree": str(worktree),
                "base": base,
                "baseRevision": base_revision,
                "status": "initialized",
            }
            )
        )
    return current_worktree(worktree)


def task_start_from_issue(root: Path, issue: str, slug: str) -> None:
    """Atomically create and hydrate an Issue-backed Task Contract."""
    from task_contract import ContractError, fetch_issue, hydrate_task_contract

    require_main_worktree(root)
    identity, payload = fetch_issue(root, issue)
    task = issue
    validate_task(task)
    worktree = root / ".worktrees" / f"{task}-{slug}"
    created: WorktreeRecord | None = None
    try:
        created = task_start(root, task, slug, quiet=True)
        hydrate_task_contract(worktree, task, issue, payload, identity)
    except Exception:
        if created is not None:
            run(["git", "worktree", "remove", "--force", str(worktree)], cwd=root, check=False)
            if created.branch:
                run(["git", "branch", "-D", created.branch], cwd=root, check=False)
        raise
    print(json.dumps({"task": task, "issue": int(issue), "repository": identity, "worktree": str(worktree), "status": "initialized", "contract": "canonical"}))


def task_status(root: Path, task: str) -> None:
    current = current_worktree(root)
    main = main_worktree(root)
    record = worktree_for_task(root, task)
    if current.path != main.path and current.path != record.path:
        raise LifecycleError(
            f"cannot inspect sibling Task worktree {record.path} from {current.path}"
        )
    assert_task_identity(record, task)
    status = state_status(state_path(record.path))
    dirty = git("status", "--short", cwd=record.path).splitlines()
    print(
        json.dumps(
            {
                "task": task,
                "branch": record.branch,
                "worktree": str(record.path),
                "head": record.head,
                "status": status,
                "dirty": dirty,
            }
        )
    )


def task_state_set(root: Path, task: str, status: str) -> None:
    record = require_local_task(root, task)
    require_resolved_contract(record, task)
    if status in {"draft-pr-created", "integration-pending"}:
        raise LifecycleError(
            f"{status} is reserved for the guarded pull request publication boundary"
        )
    with work_units_lock(record):
        assert_task_identity(record, task)
        set_state_status(state_path(record.path), status)
    print(json.dumps({"task": task, "status": status}))


def mark_task_publication_state(
    record: WorktreeRecord, task: str, expected: str, target: str
) -> str:
    """Narrow transition authority for validated PR creation/readiness."""
    allowed = {
        ("publication-ready", "draft-pr-created"),
        ("draft-pr-created", "integration-pending"),
    }
    if (expected, target) not in allowed:
        raise LifecycleError("invalid guarded publication transition")
    validate_task(task)
    require_resolved_contract(record, task)
    with work_units_lock(record):
        assert_task_identity(record, task)
        path = state_path(record.path)
        previous = state_status(path)
        if previous == target:
            return "already-transitioned"
        if previous != expected:
            raise LifecycleError(
                f"guarded publication transition requires {expected}; found {previous}"
            )
        set_state_status(path, target)
    return "transitioned"


def recover_blocked_publication_ready(
    record: WorktreeRecord,
    task: str,
    expected_state: bytes,
    expected_evidence: dict[str, bytes | None],
    recovery_receipt: bytes,
) -> str:
    """CAS a proven publication-only blocked Task to publication-ready.

    This deliberately is not part of the general transition table: recovery
    may use it only after its external publication evidence has been checked.
    """
    validate_task(task)
    if not isinstance(expected_state, bytes):
        raise LifecycleError("expected Task State CAS value must be bytes")
    evidence_names = {"work-units.json", "verification.json", "contract.json", "issue.json"}
    if set(expected_evidence) != evidence_names or any(
        value is not None and not isinstance(value, bytes)
        for value in expected_evidence.values()
    ):
        raise LifecycleError("expected publication evidence CAS values are invalid")
    require_resolved_contract(record, task)
    import task_contract

    receipt_path = private_state.publication_recovery_receipt(record.path)
    try:
        private_state.prepare(record.path, admin=True)
        receipt_value = json.loads(recovery_receipt.decode("utf-8"))
        private_state._validate_legacy_content(receipt_path, recovery_receipt)
        private_state._validate_publication_recovery_topology(
            receipt_path, receipt_value, private_state.topology(record.path)
        )
        with private_state.mutation_lock(record.path, admin=True):
            try:
                receipt_path.lstat()
            except FileNotFoundError:
                private_state.exclusive_write_bytes(
                    receipt_path, recovery_receipt, _lock_held=True
                )
            else:
                existing = private_state.read_bytes(
                    receipt_path, "publication recovery receipt"
                )
                if existing != recovery_receipt:
                    raise LifecycleError(
                        "conflicting publication recovery receipt already exists"
                    )
    except LifecycleError:
        raise
    except Exception as exc:
        raise LifecycleError("cannot durably bind blocked publication recovery") from exc

    try:
        state_lock = task_contract.contract_state_lock(record.path)
    except AttributeError as exc:  # pragma: no cover - verified module contract
        raise LifecycleError("canonical Task State lock is unavailable") from exc
    with state_lock as directory_fd:
        current = current_worktree(record.path)
        if current != record:
            raise LifecycleError("local Task worktree identity changed")
        registered = worktree_for_task(record.path, task)
        if registered != record:
            raise LifecycleError("Task worktree registration identity changed")
        assert_task_identity(record, task)
        path = state_path(record.path)
        try:
            actual = task_contract._read_state_file(directory_fd, "task.md")
            actual_evidence = {
                name: task_contract._read_state_file(directory_fd, name)
                for name in evidence_names
            }
            task_contract._assert_state_dir_binding(record.path, directory_fd)
        except Exception as exc:
            raise LifecycleError(f"cannot read Task State for guarded recovery: {path}") from exc
        if actual != expected_state:
            raise LifecycleError("Task State changed before guarded blocked recovery")
        if actual_evidence != expected_evidence:
            raise LifecycleError("publication evidence changed before guarded blocked recovery")
        try:
            text = actual.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise LifecycleError("Task State is not valid UTF-8") from exc
        updated, count = re.subn(
            r"(?m)^- Status: blocked$", "- Status: publication-ready", text, count=1
        )
        if count != 1:
            raise LifecycleError("cannot update blocked Task State status")
        try:
            task_contract._write_state_file(
                directory_fd, "task.md", updated.encode("utf-8")
            )
            task_contract._assert_state_dir_binding(record.path, directory_fd)
        except Exception as exc:
            raise LifecycleError(f"cannot update Task State for guarded recovery: {path}") from exc
    return "transitioned"


def complete_blocked_publication_recovery(
    record: WorktreeRecord,
    task: str,
    expected_state: bytes,
    expected_evidence: dict[str, bytes | None],
    recovery_receipt: bytes,
) -> str:
    """Consume the exact recovery receipt only after Draft convergence."""
    validate_task(task)
    evidence_names = {"work-units.json", "verification.json", "contract.json", "issue.json"}
    if not isinstance(expected_state, bytes) or set(expected_evidence) != evidence_names:
        raise LifecycleError("expected recovered publication subject is invalid")
    require_resolved_contract(record, task)
    import task_contract

    receipt_path = private_state.publication_recovery_receipt(record.path)
    try:
        private_state.prepare(record.path, admin=True)
        with private_state.mutation_lock(record.path, admin=True):
            receipt_content, receipt_identity = private_state.read_bytes_identity(
                receipt_path, "publication recovery receipt"
            )
            if receipt_content != recovery_receipt:
                raise LifecycleError("publication recovery receipt changed before consumption")
            with task_contract.contract_state_lock(record.path) as directory_fd:
                current = current_worktree(record.path)
                registered = worktree_for_task(record.path, task)
                if current != record or registered != record:
                    raise LifecycleError("Task worktree identity changed before receipt consumption")
                assert_task_identity(record, task)
                actual_state = task_contract._read_state_file(directory_fd, "task.md")
                actual_evidence = {
                    name: task_contract._read_state_file(directory_fd, name)
                    for name in evidence_names
                }
                task_contract._assert_state_dir_binding(record.path, directory_fd)
                if actual_state != expected_state or actual_evidence != expected_evidence:
                    raise LifecycleError(
                        "recovered publication subject changed before receipt consumption"
                    )
                private_state.unlink(
                    receipt_path,
                    expected_identity=receipt_identity,
                    expected_content=recovery_receipt,
                    _lock_held=True,
                )
    except LifecycleError:
        raise
    except Exception as exc:
        raise LifecycleError("cannot consume blocked publication recovery receipt") from exc
    return "consumed"


def mark_task_merged_from_integration(record: WorktreeRecord, task: str) -> str:
    """Dedicated terminal transition used only after guarded merge reconciliation."""
    validate_task(task)
    require_resolved_contract(record, task)
    with work_units_lock(record):
        assert_task_identity(record, task)
        path = state_path(record.path)
        previous = state_status(path)
        if previous == "merged":
            return "already-finalized"
        if previous != "integration-pending":
            raise LifecycleError(
                "post-merge finalization requires Task status integration-pending or merged; "
                f"found {previous}"
            )
        set_state_status(path, "merged")
    return "finalized"


def extract_identity_value(path: Path, label: str) -> str | None:
    match = re.search(
        rf"(?m)^- {re.escape(label)}: (.+)$", path.read_text(encoding="utf-8")
    )
    return match.group(1).strip() if match else None


def remote_branch_head(record: WorktreeRecord) -> str | None:
    """Resolve the live origin branch, distinguishing deletion from failures."""
    assert record.branch is not None
    result = run(
        [
            "git",
            "ls-remote",
            "--exit-code",
            "--heads",
            "origin",
            f"refs/heads/{record.branch}",
        ],
        cwd=record.path,
        check=False,
    )
    if result.returncode == 2 and not result.stdout.strip():
        return None
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit {result.returncode}"
        raise LifecycleError(f"cleanup refused: cannot inspect live Task branch: {detail}")
    lines = [line.split() for line in result.stdout.splitlines() if line.strip()]
    expected_ref = f"refs/heads/{record.branch}"
    if (
        len(lines) != 1
        or len(lines[0]) != 2
        or lines[0][1] != expected_ref
        or not re.fullmatch(r"[0-9a-fA-F]{40,64}", lines[0][0])
    ):
        raise LifecycleError("cleanup refused: live Task branch response is invalid or ambiguous")
    return lines[0][0].lower()


def unpushed_commits_from_base(record: WorktreeRecord, base_revision: str) -> int:
    assert record.branch is not None
    remote_head = remote_branch_head(record)
    if remote_head is not None:
        count = git(
            "rev-list", "--count", f"{remote_head}..{record.branch}", cwd=record.path
        )
        return int(count or "0")
    count = git(
        "rev-list",
        "--count",
        f"{base_revision}..{record.branch}",
        cwd=record.path,
    )
    return int(count or "0")


def unpushed_commits(record: WorktreeRecord, state: Path) -> int:
    base_revision = extract_identity_value(state, "Base revision")
    if not base_revision:
        raise LifecycleError("Task State is missing Base revision")
    return unpushed_commits_from_base(record, base_revision)


def require_cleanup_base_revision(root: Path, base_revision: str) -> None:
    if not re.fullmatch(r"[0-9a-fA-F]{40,64}", base_revision):
        raise LifecycleError("cleanup refused: Task Base revision is missing or invalid")
    result = run(
        ["git", "merge-base", "--is-ancestor", base_revision, default_branch(root)],
        cwd=root,
        check=False,
    )
    if result.returncode != 0:
        raise LifecycleError(
            "cleanup refused: Task Base revision is not trusted default-branch history"
        )


def cleanup_receipt_path(root: Path, task: str) -> Path:
    validate_task(task)
    try:
        return private_state.cleanup_receipt(root, task)
    except private_state.GitPrivateStateError as exc:
        raise LifecycleError(str(exc)) from exc


@contextmanager
def cleanup_lock(root: Path):
    try:
        with private_state.cleanup_lock(root):
            yield
    except private_state.GitPrivateStateError as exc:
        raise LifecycleError(str(exc)) from exc


def cleanup_repository(root: Path) -> str:
    result = gh("repo", "view", "--json", "nameWithOwner", cwd=root, check=False)
    if result.returncode != 0:
        raise LifecycleError("cleanup refused: cannot resolve GitHub repository identity")
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise LifecycleError("cleanup refused: GitHub repository identity is invalid") from exc
    repository = value.get("nameWithOwner") if isinstance(value, dict) else None
    if not isinstance(repository, str) or not re.fullmatch(
        r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository
    ):
        raise LifecycleError("cleanup refused: GitHub repository identity is invalid")
    return repository


def cleanup_prs(root: Path, branch: str, repository: str) -> list[dict]:
    owner = repository.split("/", 1)[0]
    result = gh(
        "api",
        "--method",
        "GET",
        "--paginate",
        "--slurp",
        f"repos/{repository}/pulls",
        "-f",
        "state=all",
        "-f",
        f"head={owner}:{branch}",
        "-f",
        "per_page=100",
        cwd=root,
        check=False,
    )
    if result.returncode != 0:
        raise LifecycleError("cleanup refused: cannot reconstruct GitHub pull request evidence")
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise LifecycleError("cleanup refused: GitHub pull request evidence is invalid") from exc
    if not isinstance(value, list) or any(not isinstance(page, list) for page in value):
        raise LifecycleError("cleanup refused: GitHub pull request evidence is invalid")
    matches = []
    for item in (entry for page in value for entry in page):
        if not isinstance(item, dict):
            raise LifecycleError("cleanup refused: GitHub pull request evidence is invalid")
        head = item.get("head")
        base = item.get("base")
        head_repo = head.get("repo") if isinstance(head, dict) else None
        if not isinstance(head, dict) or not isinstance(base, dict):
            raise LifecycleError("cleanup refused: GitHub pull request evidence is invalid")
        if head.get("ref") != branch:
            continue
        matches.append(
            {
                "number": item.get("number"),
                "state": "MERGED" if item.get("merged_at") else str(item.get("state", "")).upper(),
                "headRefName": head.get("ref"),
                "headRefOid": head.get("sha"),
                "baseRefName": base.get("ref"),
                "isCrossRepository": not isinstance(head_repo, dict)
                or str(head_repo.get("full_name", "")).casefold() != repository.casefold(),
                "mergeCommit": {"oid": item.get("merge_commit_sha")},
            }
        )
    return matches


def merged_cleanup_evidence(
    root: Path, record: WorktreeRecord, state: Path, local_head: str
) -> dict:
    assert record.branch is not None
    repository = cleanup_repository(root)
    matches = cleanup_prs(root, record.branch, repository)
    if len(matches) != 1:
        raise LifecycleError("cleanup refused: merged Task pull request identity is missing or ambiguous")
    pr = matches[0]
    published_head = pr.get("headRefOid")
    if (
        pr.get("state") != "MERGED"
        or pr.get("headRefName") != record.branch
        or pr.get("baseRefName") != default_branch(root)
        or pr.get("isCrossRepository") is not False
        or not isinstance(pr.get("number"), int)
        or not isinstance(published_head, str)
        or not re.fullmatch(r"[0-9a-fA-F]{40,64}", published_head)
    ):
        raise LifecycleError("cleanup refused: merged pull request evidence does not match the Task")
    published_head = published_head.lower()
    if local_head.lower() != published_head:
        raise LifecycleError("cleanup refused: local Task head does not match published PR head")
    ahead = git(
        "rev-list", "--count", f"{published_head}..{record.branch}", cwd=record.path
    )
    if int(ahead or "0") != 0:
        raise LifecycleError("cleanup refused: local Task branch is ahead of published PR head")
    recorded = extract_identity_value(state, "Published head SHA")
    if recorded and recorded.casefold() != "none":
        if not re.fullmatch(r"[0-9a-fA-F]{40,64}", recorded) or recorded.lower() != published_head:
            raise LifecycleError("cleanup refused: Task State published head does not match GitHub")
    remote_head = remote_branch_head(record)
    if remote_head is not None and remote_head != published_head:
        raise LifecycleError("cleanup refused: live remote Task branch does not match published PR head")
    return {
        "repository": repository,
        "pr": pr["number"],
        "published_head": published_head,
        "upstream": "deleted" if remote_head is None else "live",
    }


def cleanup_plan(root: Path, task: str) -> dict:
    record = worktree_for_task(root, task)
    assert_task_identity(record, task)
    state = state_path(record.path)
    status = state_status(state)
    if status not in TERMINAL_STATES:
        raise LifecycleError(
            f"cleanup refused while Task status is {status}; expected one of {sorted(TERMINAL_STATES)}"
        )
    if git("status", "--porcelain", "--untracked-files=all", cwd=record.path):
        raise LifecycleError("cleanup refused: Task worktree has uncommitted changes")
    branch = record.branch
    assert branch is not None
    local_head = git("rev-parse", "--verify", f"refs/heads/{branch}", cwd=record.path).lower()
    if record.head is None or record.head.lower() != local_head:
        raise LifecycleError("cleanup refused: registered Task head changed")
    evidence: dict = {}
    if status == "merged":
        evidence = merged_cleanup_evidence(root, record, state, local_head)
    else:
        base_revision = extract_identity_value(state, "Base revision")
        if not base_revision:
            raise LifecycleError("Task State Base revision is missing or invalid")
        base_revision = base_revision.lower()
        require_cleanup_base_revision(root, base_revision)
        ahead = unpushed_commits_from_base(record, base_revision)
        if ahead:
            raise LifecycleError(f"cleanup refused: Task branch has {ahead} unpushed commit(s)")
        repository = cleanup_repository(root)
        if any(pr.get("state") == "OPEN" for pr in cleanup_prs(root, branch, repository)):
            raise LifecycleError("cleanup refused: cancelled Task still has an open pull request")
        evidence = {
            "repository": repository,
            "upstream": "cancelled-safe",
            "base_revision": base_revision,
        }
    return {
        "schema_version": 1,
        "task": task,
        "status": status,
        "worktree": str(record.path),
        "branch": branch,
        "local_head": local_head,
        "evidence": evidence,
    }


def read_cleanup_receipt(path: Path, task: str) -> dict:
    if path.is_symlink() or not path.is_file():
        raise LifecycleError("cleanup receipt is not a regular local file")
    try:
        value = json.loads(private_state.read_bytes(path, "cleanup receipt").decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, private_state.GitPrivateStateError) as exc:
        raise LifecycleError("cleanup receipt is invalid") from exc
    required = {"schema_version", "task", "status", "worktree", "branch", "local_head", "evidence"}
    if (
        not isinstance(value, dict)
        or set(value) != required
        or value.get("schema_version") != 1
        or value.get("task") != task
        or value.get("status") not in TERMINAL_STATES
        or not isinstance(value.get("worktree"), str)
        or not branch_matches_task(value.get("branch"), task)
        or not isinstance(value.get("local_head"), str)
        or not re.fullmatch(r"[0-9a-fA-F]{40,64}", value["local_head"])
        or not isinstance(value.get("evidence"), dict)
    ):
        raise LifecycleError("cleanup receipt is invalid")
    evidence = value["evidence"]
    if value["status"] == "cancelled" and (
        set(evidence) != {"repository", "upstream", "base_revision"}
        or not isinstance(evidence.get("repository"), str)
        or evidence.get("upstream") != "cancelled-safe"
        or not isinstance(evidence.get("base_revision"), str)
        or not re.fullmatch(r"[0-9a-fA-F]{40,64}", evidence["base_revision"])
    ):
        raise LifecycleError("cleanup receipt is invalid")
    return value


def finish_cleanup(root: Path, plan: dict, receipt: Path) -> None:
    task = plan["task"]
    branch = plan["branch"]
    expected_path = Path(plan["worktree"]).resolve()
    expected_head = plan["local_head"].lower()
    records = parse_worktrees(root)
    registered = [r for r in records if r.path == expected_path or r.branch == branch]
    if registered:
        if len(registered) != 1 or registered[0].path != expected_path or registered[0].branch != branch:
            raise LifecycleError("cleanup receipt conflicts with current worktree registration")
        current = cleanup_plan(root, task)
        if current != plan:
            raise LifecycleError("cleanup evidence changed before worktree removal")
        run(["git", "worktree", "remove", str(expected_path)], cwd=root)
    if any(r.path == expected_path or r.branch == branch for r in parse_worktrees(root)):
        raise LifecycleError("cleanup failed to remove the expected worktree registration")

    branch_ref = f"refs/heads/{branch}"
    actual = git("rev-parse", "--verify", branch_ref, cwd=root, check=False).lower()
    if actual:
        if actual != expected_head:
            raise LifecycleError("cleanup refused: local Task branch moved after validation")
        if plan["status"] == "merged":
            # Reconstruct the externally authoritative evidence again after
            # Task State/worktree removal, using the receipt-bound identity.
            repository = cleanup_repository(root)
            if repository.casefold() != plan["evidence"].get("repository", "").casefold():
                raise LifecycleError("cleanup repository identity changed after worktree removal")
            matches = cleanup_prs(root, branch, repository)
            if (
                len(matches) != 1
                or matches[0].get("state") != "MERGED"
                or matches[0].get("headRefOid", "").lower() != expected_head
                or matches[0].get("headRefName") != branch
                or matches[0].get("baseRefName") != default_branch(root)
                or matches[0].get("isCrossRepository") is not False
                or matches[0].get("number") != plan["evidence"].get("pr")
            ):
                raise LifecycleError("cleanup merged PR evidence changed after worktree removal")
            remote_head = remote_branch_head(WorktreeRecord(root, branch, expected_head))
            if remote_head is not None and remote_head != expected_head:
                raise LifecycleError("cleanup remote Task branch changed after worktree removal")
        else:
            require_cleanup_base_revision(root, plan["evidence"]["base_revision"])
            repository = cleanup_repository(root)
            if repository.casefold() != plan["evidence"].get("repository", "").casefold():
                raise LifecycleError("cleanup repository identity changed after worktree removal")
            if any(pr.get("state") == "OPEN" for pr in cleanup_prs(root, branch, repository)):
                raise LifecycleError("cleanup refused: cancelled Task still has an open pull request")
            record = WorktreeRecord(root, branch, expected_head)
            ahead = unpushed_commits_from_base(record, plan["evidence"]["base_revision"])
            if ahead:
                raise LifecycleError(
                    f"cleanup refused: cancelled Task branch has {ahead} unpublished commit(s)"
                )
        run(["git", "update-ref", "-d", branch_ref, expected_head], cwd=root)
    if git("rev-parse", "--verify", branch_ref, cwd=root, check=False):
        raise LifecycleError("cleanup failed to delete the expected local Task branch")
    try:
        private_state.unlink(receipt)
    except private_state.GitPrivateStateError as exc:
        raise LifecycleError(str(exc)) from exc
    print(
        json.dumps(
            {
                "task": task,
                "removedWorktree": str(expected_path),
                "removedBranch": branch,
                "taskStateDiscarded": True,
            }
        )
    )


def task_cleanup(root: Path, task: str) -> None:
    require_main_worktree(root)
    with cleanup_lock(root):
        receipt = cleanup_receipt_path(root, task)
        if receipt.exists():
            finish_cleanup(root, read_cleanup_receipt(receipt, task), receipt)
            return
        plan = cleanup_plan(root, task)
        try:
            private_state.write_bytes(
                receipt, (json.dumps(plan, sort_keys=True) + "\n").encode("utf-8")
            )
        except private_state.GitPrivateStateError as exc:
            raise LifecycleError(str(exc)) from exc
        finish_cleanup(root, plan, receipt)


def extract_list(path: Path, heading: str) -> list[str]:
    text = path.read_text(encoding="utf-8")
    pattern = rf"(?ms)^## {re.escape(heading)}\n\n(.*?)(?=^## |\Z)"
    match = re.search(pattern, text)
    if not match:
        return []
    values: list[str] = []
    for line in match.group(1).splitlines():
        line = line.strip()
        if line.startswith("- "):
            value = line[2:].strip()
            if value and value.lower() not in {"none", "none recorded", "tbd"}:
                values.append(value)
    return values


def task_summary(root: Path, task: str) -> dict:
    record = worktree_for_task(root, task)
    assert_task_identity(record, task)
    path = state_path(record.path)
    return {
        "task": task,
        "branch": record.branch,
        "worktree": str(record.path),
        "status": state_status(path),
        "dependencies": extract_list(path, "Dependencies"),
        "scope": extract_list(path, "Scope"),
        "coordinationSurfaces": extract_list(path, "Coordination surfaces"),
        "externalResources": extract_list(path, "External resources"),
    }


def normalized(values: list[str]) -> set[str]:
    return {value.strip().lower() for value in values if value.strip()}


def overlap_reason(label: str, left: list[str], right: list[str]) -> str | None:
    overlap = sorted(normalized(left) & normalized(right))
    if not overlap:
        return None
    return f"overlapping {label}: " + ", ".join(overlap)


def batch_conflicts(summaries: list[dict]) -> list[dict]:
    conflicts: list[dict] = []
    for index, left in enumerate(summaries):
        for right in summaries[index + 1 :]:
            reasons: list[str] = []
            left_deps = normalized(left["dependencies"])
            right_deps = normalized(right["dependencies"])
            if right["task"].lower() in left_deps or left["task"].lower() in right_deps:
                reasons.append("declared dependency")
            for label, key in (
                ("declared scope", "scope"),
                ("coordination surface", "coordinationSurfaces"),
                ("external resource", "externalResources"),
            ):
                reason = overlap_reason(label, left[key], right[key])
                if reason:
                    reasons.append(reason)
            if reasons:
                conflicts.append(
                    {"tasks": [left["task"], right["task"]], "reasons": reasons}
                )
    return conflicts


def batch_plan(root: Path, tasks: list[str]) -> None:
    require_main_worktree(root)
    if len(tasks) < 2:
        raise LifecycleError("batch-plan requires at least two explicit Task IDs")
    if len(set(tasks)) != len(tasks):
        raise LifecycleError("batch-plan contains duplicate Task IDs")
    summaries = [task_summary(root, task) for task in tasks]
    conflicts = batch_conflicts(summaries)
    print(
        json.dumps(
            {
                "tasks": summaries,
                "parallelSafe": not conflicts,
                "conflicts": conflicts,
            }
        )
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Task/worktree lifecycle")
    sub = result.add_subparsers(dest="command", required=True)
    start = sub.add_parser("start")
    start.add_argument("task")
    start.add_argument("slug")
    issue_start = sub.add_parser("start-from-issue")
    issue_start.add_argument("issue")
    issue_start.add_argument("slug")
    contract = sub.add_parser("contract-check")
    contract.add_argument("task", nargs="?")
    resume_contract = sub.add_parser("contract-resume-check")
    resume_contract.add_argument("task", nargs="?")
    status = sub.add_parser("status")
    status.add_argument("task")
    state = sub.add_parser("state-set")
    state.add_argument("task")
    state.add_argument("status")
    cleanup = sub.add_parser("cleanup")
    cleanup.add_argument("task")
    batch = sub.add_parser("batch-plan")
    batch.add_argument("tasks", nargs="+")
    work_unit_register_parser = sub.add_parser("work-unit-register")
    work_unit_register_parser.add_argument("task")
    work_unit_register_parser.add_argument("work_unit")
    work_unit_register_parser.add_argument("role")
    work_unit_register_parser.add_argument("objective")
    work_unit_next_parser = sub.add_parser("work-unit-next")
    work_unit_next_parser.add_argument("task")
    work_unit_create_parser = sub.add_parser("work-unit-create")
    work_unit_create_parser.add_argument("task")
    work_unit_create_parser.add_argument("role")
    work_unit_create_parser.add_argument("objective")
    work_unit_status_parser = sub.add_parser("work-unit-status")
    work_unit_status_parser.add_argument("task")
    work_unit_status_parser.add_argument("work_unit")
    work_unit_dispatch_parser = sub.add_parser("work-unit-dispatch-check")
    work_unit_dispatch_parser.add_argument("task")
    work_unit_dispatch_parser.add_argument("work_unit")
    work_unit_dispatch_parser.add_argument("role")
    work_unit_dispatch_parser.add_argument("objective")
    work_unit_state_parser = sub.add_parser("work-unit-state-set")
    work_unit_state_parser.add_argument("task")
    work_unit_state_parser.add_argument("work_unit")
    work_unit_state_parser.add_argument("status", choices=sorted(WORK_UNIT_STATES))
    work_unit_state_parser.add_argument("evidence")
    work_unit_state_parser.add_argument("--provider")
    work_unit_state_parser.add_argument("--model")
    work_unit_state_parser.add_argument("--error")
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        root = repo_root()
        if args.command == "start":
            task_start(root, args.task, args.slug)
        elif args.command == "start-from-issue":
            task_start_from_issue(root, args.issue, args.slug)
        elif args.command == "contract-check":
            from task_contract import check_contract
            print(json.dumps(check_contract(root, args.task), sort_keys=True))
        elif args.command == "contract-resume-check":
            from task_contract import check_resume_contract
            print(json.dumps(check_resume_contract(root, args.task), sort_keys=True))
        elif args.command == "status":
            task_status(root, args.task)
        elif args.command == "state-set":
            task_state_set(root, args.task, args.status)
        elif args.command == "cleanup":
            task_cleanup(root, args.task)
        elif args.command == "batch-plan":
            batch_plan(root, args.tasks)
        elif args.command == "work-unit-register":
            work_unit_register(root, args.task, args.work_unit, args.role, args.objective)
        elif args.command == "work-unit-next":
            work_unit_next(root, args.task)
        elif args.command == "work-unit-create":
            work_unit_create(root, args.task, args.role, args.objective)
        elif args.command == "work-unit-status":
            work_unit_status(root, args.task, args.work_unit)
        elif args.command == "work-unit-dispatch-check":
            work_unit_dispatch_check(
                root, args.task, args.work_unit, args.role, args.objective
            )
        elif args.command == "work-unit-state-set":
            work_unit_state_set(
                root,
                args.task,
                args.work_unit,
                args.status,
                args.evidence,
                args.provider,
                args.model,
                args.error,
            )
        else:  # pragma: no cover
            raise LifecycleError(f"unsupported command: {args.command}")
    except LifecycleError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
