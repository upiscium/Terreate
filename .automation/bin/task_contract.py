#!/usr/bin/env python3
"""Canonical, non-LLM Task Contract hydration for GitHub Issues."""
from __future__ import annotations

import hashlib
import fcntl
import json
import os
import re
import secrets
import stat
import subprocess
import importlib.util
from contextlib import contextmanager
from pathlib import Path
from urllib.parse import urlparse

try:
    import task_lifecycle as lifecycle
except ModuleNotFoundError:  # direct import by an offline test harness
    _path = Path(__file__).with_name("task_lifecycle.py")
    _spec = importlib.util.spec_from_file_location("task_lifecycle", _path)
    assert _spec and _spec.loader
    lifecycle = importlib.util.module_from_spec(_spec)
    import sys
    sys.modules["task_lifecycle"] = lifecycle
    _spec.loader.exec_module(lifecycle)

ISSUE_RE = re.compile(r"^[1-9][0-9]*$")
SNAPSHOT = ".task-state/issue.json"
CONTRACT = ".task-state/contract.json"
RESUMABLE_STATES = {
    "researching",
    "planning",
    "implementing",
    "verification-pending",
    "local-verified",
    "review-pending",
    "publication-ready",
    "draft-pr-created",
    "blocked",
}


class ContractError(lifecycle.LifecycleError):
    pass


@contextmanager
def contract_state_lock(root: Path):
    """Pin the real Task State directory and share the lifecycle lock inode."""
    directory = root / ".task-state"
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        directory_fd = os.open(directory, flags)
    except OSError as exc:
        raise ContractError("Task State directory must be a real local directory") from exc
    try:
        metadata = os.fstat(directory_fd)
        if not stat.S_ISDIR(metadata.st_mode):
            raise ContractError("Task State path is not a directory")
        lock_flags = os.O_RDWR | os.O_CREAT
        if hasattr(os, "O_NOFOLLOW"):
            lock_flags |= os.O_NOFOLLOW
        lock_fd = os.open("work-units.lock", lock_flags, 0o600, dir_fd=directory_fd)
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX)
            yield directory_fd
        finally:
            os.close(lock_fd)
    finally:
        os.close(directory_fd)


def _assert_state_dir_binding(root: Path, directory_fd: int) -> None:
    try:
        path_metadata = (root / ".task-state").lstat()
    except OSError as exc:
        raise ContractError("Task State directory changed during hydration") from exc
    pinned = os.fstat(directory_fd)
    if (
        not stat.S_ISDIR(path_metadata.st_mode)
        or (path_metadata.st_dev, path_metadata.st_ino) != (pinned.st_dev, pinned.st_ino)
    ):
        raise ContractError("Task State directory changed during hydration")


def _read_state_file(directory_fd: int, name: str) -> bytes | None:
    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        fd = os.open(name, flags, dir_fd=directory_fd)
    except FileNotFoundError:
        return None
    try:
        metadata = os.fstat(fd)
        if not stat.S_ISREG(metadata.st_mode):
            raise ContractError(f"Task State file is not regular: {name}")
        with os.fdopen(fd, "rb") as stream:
            fd = -1
            return stream.read()
    finally:
        if fd >= 0:
            os.close(fd)


def _write_state_file(directory_fd: int, name: str, content: bytes) -> None:
    temporary = f".{name}.{secrets.token_hex(16)}.tmp"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    fd = os.open(temporary, flags, 0o600, dir_fd=directory_fd)
    try:
        with os.fdopen(fd, "wb") as stream:
            fd = -1
            stream.write(content)
        os.rename(temporary, name, src_dir_fd=directory_fd, dst_dir_fd=directory_fd)
    except Exception:
        try:
            os.unlink(temporary, dir_fd=directory_fd)
        except FileNotFoundError:
            pass
        raise
    finally:
        if fd >= 0:
            os.close(fd)


def _restore_state_file(directory_fd: int, name: str, content: bytes | None) -> None:
    if content is None:
        try:
            os.unlink(name, dir_fd=directory_fd)
        except FileNotFoundError:
            pass
    else:
        _write_state_file(directory_fd, name, content)


def write_publication_metadata(root: Path, title: bytes, body: bytes) -> None:
    """Atomically replace the two publication files in pinned Task State."""
    with contract_state_lock(root) as directory_fd:
        previous = {
            name: _read_state_file(directory_fd, name)
            for name in ("pr-title.txt", "pr-body.md")
        }
        try:
            _write_state_file(directory_fd, "pr-title.txt", title)
            _write_state_file(directory_fd, "pr-body.md", body)
            _assert_state_dir_binding(root, directory_fd)
        except Exception:
            for name, content in previous.items():
                _restore_state_file(directory_fd, name, content)
            raise


def write_verification_receipt(root: Path, content: bytes) -> None:
    """Replace the verification receipt through pinned Task State."""
    with contract_state_lock(root) as directory_fd:
        _write_state_file(directory_fd, "verification.json", content)
        _assert_state_dir_binding(root, directory_fd)


def validate_issue_number(value: str) -> int:
    if not ISSUE_RE.fullmatch(value):
        raise ContractError("Issue number must be an exact positive decimal integer")
    return int(value)


def repository_identity(root: Path) -> str:
    remote = lifecycle.git("remote", "get-url", "origin", cwd=root, check=False)
    if not remote:
        raise ContractError("cannot resolve repository identity: origin is missing")
    remote = remote.removesuffix(".git")
    if remote.startswith("git@"):
        parts = remote.removeprefix("git@").split(":", 1)
        if len(parts) != 2 or parts[0] != "github.com":
            raise ContractError("origin must use the GitHub host")
        path = parts[1]
        identity = path
    else:
        parsed = urlparse(remote)
        if parsed.scheme != "https" or parsed.hostname != "github.com" or parsed.username:
            raise ContractError("origin must be a standard GitHub HTTPS URL")
        identity = parsed.path.lstrip("/")
    if any(ord(char) < 32 or ord(char) == 127 for char in identity):
        raise ContractError("repository identity contains control characters")
    if identity.count("/") != 1 or any(not re.fullmatch(r"[A-Za-z0-9_.-]+", part or "") for part in identity.split("/")):
        raise ContractError(f"cannot resolve owner/name from origin: {remote}")
    return identity


def fetch_issue(root: Path, issue: str, runner=None) -> tuple[str, dict]:
    number = validate_issue_number(issue)
    identity = repository_identity(root)
    env = os.environ.copy()
    for key in ("GH_REPO", "GH_HOST", "GH_ENTERPRISE_TOKEN"):
        env.pop(key, None)
    result = (runner or subprocess.run)(
        ["gh", "api", "--hostname", "github.com", f"repos/{identity}/issues/{number}"],
        cwd=root,
        text=True,
        capture_output=True,
        env=env,
    )
    if result.returncode != 0:
        raise ContractError(result.stderr.strip() or "GitHub Issue lookup failed")
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise ContractError("GitHub Issue response is not valid JSON") from exc
    if not isinstance(payload, dict):
        raise ContractError("GitHub Issue response is not an object")
    if payload.get("number") != number:
        raise ContractError("GitHub response Issue number mismatch")
    if payload.get("pull_request") is not None or payload.get("isPullRequest") is True:
        raise ContractError("pull requests cannot be Task sources")
    title, body = payload.get("title"), payload.get("body")
    if not isinstance(title, str) or not title.strip():
        raise ContractError("Issue title is missing or empty")
    if not isinstance(body, str) or not body.strip():
        raise ContractError("Issue body is missing or empty")
    repo = payload.get("repository")
    if isinstance(repo, dict) and repo.get("full_name") not in (None, identity):
        raise ContractError("Issue repository identity mismatch")
    repository_url = payload.get("repository_url")
    issue_url = payload.get("html_url")
    if (
        not isinstance(repository_url, str)
        or repository_url.casefold() != f"https://api.github.com/repos/{identity}".casefold()
        or not isinstance(issue_url, str)
        or issue_url.casefold() != f"https://github.com/{identity}/issues/{number}".casefold()
    ):
        raise ContractError("Issue URL/repository identity mismatch")
    if payload.get("state") != "open":
        raise ContractError("Issue must be open")
    if len(title) > 256 or any(ord(char) < 32 or ord(char) == 127 for char in title):
        raise ContractError("Issue title contains control characters")
    if len(body.encode("utf-8")) > 1024 * 1024:
        raise ContractError("Issue body exceeds the supported snapshot size")
    return identity, payload


def _digest(payload: dict) -> str:
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
    return hashlib.sha256(encoded).hexdigest()


def authoritative_payload(payload: dict, number: int, identity: str) -> dict:
    result = {
        "number": number,
        "url": payload.get("html_url", payload.get("url")),
        "title": payload["title"],
        "body": payload["body"],
        "state": payload["state"],
        "repository": identity,
    }
    labels = payload.get("labels", [])
    assignees = payload.get("assignees", [])
    milestone = payload.get("milestone")
    if not isinstance(labels, list) or not all(isinstance(item, dict) and isinstance(item.get("name"), str) for item in labels):
        raise ContractError("Issue labels are malformed")
    if not isinstance(assignees, list) or not all(isinstance(item, dict) and isinstance(item.get("login"), str) for item in assignees):
        raise ContractError("Issue assignees are malformed")
    if milestone is not None and (
        not isinstance(milestone, dict)
        or not isinstance(milestone.get("number"), int)
        or not isinstance(milestone.get("title"), str)
    ):
        raise ContractError("Issue milestone is malformed")
    result["labels"] = [item["name"] for item in labels]
    result["assignees"] = [item["login"] for item in assignees]
    result["milestone"] = (
        None if milestone is None else {"number": milestone["number"], "title": milestone["title"]}
    )
    return result


def _canonical_state(text: str, issue: int, digest: str) -> str:
    values = {
        "Purpose": f"Authoritative source: {SNAPSHOT}#title (Issue #{issue}); body: {SNAPSHOT}#body",
        "Scope": f"- Authoritative source: {SNAPSHOT}#body (Issue #{issue})",
        "Prohibited changes": "- Changes outside the authoritative Issue scope; source: repository policy",
        "Dependencies": f"- Authoritative source: {SNAPSHOT}#assignees, #labels, #milestone",
        "Acceptance criteria": f"- Satisfy the authoritative Issue #{issue} body in {SNAPSHOT}#body",
        "Test plan": f"- Verify authoritative Issue #{issue} fields from {SNAPSHOT}; run `just project::check`",
        "Stop conditions": "- Source snapshot, repository identity, or pristine-state preconditions become invalid",
        "Coordination surfaces": "- None recorded; no coordination data is inferred",
        "External resources": f"- Authoritative source: {SNAPSHOT}#url",
    }
    for heading, value in values.items():
        pattern = rf"(?ms)(^## {re.escape(heading)}\n\n).*?(?=^## |\Z)"
        text, count = re.subn(pattern, rf"\g<1>{value}\n\n", text, count=1)
        if count != 1:
            raise ContractError(f"Task State missing required section: {heading}")
    text = re.sub(r"(?m)^- Unverified:.*$", "- Unverified: none; canonical Issue contract", text)
    text += f"\n<!-- canonical-contract sha256={digest} issue={issue} -->\n"
    return text


def _placeholder_state(root: Path, state_text: str | None = None) -> str:
    template = root / ".automation" / "templates" / "task-state.md"
    if not template.is_file():
        raise ContractError(f"missing Task State template: {template}")
    state = lifecycle.state_path(root)
    if state_text is None:
        state_text = state.read_text(encoding="utf-8")
    def identity(label: str) -> str | None:
        match = re.search(rf"(?m)^- {re.escape(label)}: (.+)$", state_text or "")
        return match.group(1).strip() if match else None
    task = identity("Task ID")
    branch = identity("Branch")
    worktree = identity("Worktree")
    base = identity("Base branch")
    revision = identity("Base revision")
    values = {"@@TASK_ID@@": task, "@@BRANCH@@": branch, "@@WORKTREE@@": worktree,
              "@@BASE_BRANCH@@": base, "@@BASE_REVISION@@": revision}
    if any(value is None for value in values.values()):
        raise ContractError("Task State identity is incomplete")
    text = template.read_text(encoding="utf-8")
    for marker, value in values.items():
        text = text.replace(marker, value or "")
    return text


def _pristine(root: Path, task: str) -> lifecycle.WorktreeRecord:
    record = lifecycle.require_local_task(root, task)
    if lifecycle.state_status(lifecycle.state_path(root)) != "initialized":
        raise ContractError("contract hydration requires initialized Task State")
    units = lifecycle.read_work_units(record, task)
    if units["units"]:
        raise ContractError("contract hydration requires no Work Units")
    if lifecycle.git("status", "--porcelain", cwd=root):
        raise ContractError("contract hydration requires a clean tracked/product worktree")
    base = lifecycle.extract_identity_value(lifecycle.state_path(root), "Base revision")
    if not base or lifecycle.git("rev-parse", "HEAD", cwd=root) != base:
        raise ContractError("contract hydration requires HEAD equal to Base revision")
    return record


def _hydrate_task_contract_locked(root: Path, task: str, issue: str, payload: dict, identity: str, directory_fd: int) -> dict:
    number = validate_issue_number(issue)
    if task != issue:
        raise ContractError("Task ID must exactly match its authoritative Issue number")
    record = _pristine(root, task)
    _assert_state_dir_binding(root, directory_fd)
    state_path = lifecycle.state_path(root)
    state_bytes = _read_state_file(directory_fd, "task.md")
    if state_bytes is None:
        raise ContractError("Task State is missing")
    existing_state = state_bytes.decode("utf-8")
    placeholder = _placeholder_state(root, existing_state)
    if existing_state != placeholder and "canonical-contract sha256=" not in existing_state:
        raise ContractError("Task State is not the exact canonical pristine placeholder")
    old = None
    snapshot = root / SNAPSHOT
    contract_path = root / CONTRACT
    snapshot_bytes = _read_state_file(directory_fd, "issue.json")
    contract_bytes = _read_state_file(directory_fd, "contract.json")
    if contract_bytes is not None and snapshot_bytes is None:
        raise ContractError("canonical contract metadata exists without its snapshot")
    payload = authoritative_payload(payload, number, identity)
    if snapshot_bytes is not None:
        try:
            old = json.loads(snapshot_bytes.decode("utf-8"))
        except json.JSONDecodeError as exc:
            raise ContractError("existing Issue snapshot is malformed") from exc
        if old.get("sha256") != _digest(old.get("payload", {})):
            raise ContractError("existing Issue snapshot integrity check failed")
        try:
            if contract_bytes is None:
                raise ContractError("existing canonical contract metadata is missing")
            metadata = json.loads(contract_bytes.decode("utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise ContractError("existing canonical contract metadata is malformed") from exc
        if metadata.get("sha256") != old.get("sha256") or metadata.get("issue") != old.get("issue") or metadata.get("repository") != old.get("repository"):
            raise ContractError("existing canonical contract metadata mismatch")
        if old.get("issue") != number or old.get("repository") != identity:
            raise ContractError("existing Task Contract source conflict")
        if old.get("sha256") != _digest(payload):
            raise ContractError("existing Task Contract source content conflict")
        payload = old["payload"]
    digest = _digest(payload)
    value = {"schema_version": 1, "issue": number, "repository": identity, "sha256": digest, "payload": payload}
    new_state = _canonical_state(placeholder, number, digest)
    if existing_state != placeholder and existing_state != new_state:
        raise ContractError("existing Task State is not the exact canonical hydrated state")
    values = {
        "issue.json": (json.dumps(value, sort_keys=True) + "\n").encode(),
        "contract.json": (json.dumps({"schema_version": 1, "issue": number, "repository": identity, "snapshot": SNAPSHOT, "sha256": digest}, sort_keys=True) + "\n").encode(),
        "task.md": new_state.encode(),
    }
    before = {name: _read_state_file(directory_fd, name) for name in values}
    try:
        _assert_state_dir_binding(root, directory_fd)
        for name, content in values.items():
            _write_state_file(directory_fd, name, content)
        _assert_state_dir_binding(root, directory_fd)
    except Exception:
        for name, content in before.items():
            _restore_state_file(directory_fd, name, content)
        raise
    return value


def hydrate_task_contract(root: Path, task: str, issue: str, payload: dict, identity: str) -> dict:
    validate_issue_number(issue)
    lifecycle.require_local_task(root, task)
    with contract_state_lock(root) as directory_fd:
        return _hydrate_task_contract_locked(root, task, issue, payload, identity, directory_fd)


def recover_task_from_issue(worktree: Path, issue: str, *, runner=None) -> dict:
    root = lifecycle.repo_root(worktree)
    record = lifecycle.current_worktree(root)
    if record.path != worktree.resolve():
        raise ContractError("recovery must run in the existing Task worktree")
    task = lifecycle.extract_identity_value(lifecycle.state_path(worktree), "Task ID")
    if not task:
        raise ContractError("Task State is missing Task ID")
    if task != issue:
        raise ContractError("Task ID must exactly match its authoritative Issue number")
    identity, payload = fetch_issue(worktree, issue, runner)
    return hydrate_task_contract(worktree, task, issue, payload, identity)


def validate_contract(root: Path, task: str, *, require_pristine: bool = False) -> dict:
    if require_pristine:
        record = _pristine(root, task)
    else:
        record = lifecycle.require_local_task(root, task)
    state = lifecycle.state_path(root).read_text(encoding="utf-8")
    _validate_task_identity_exact(record, task, state)
    if "canonical-contract sha256=" not in state:
        raise ContractError("Task State has no canonical contract marker")
    path = root / SNAPSHOT
    if not path.is_file():
        raise ContractError("canonical Issue snapshot is missing")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ContractError("canonical Issue snapshot is malformed") from exc
    if set(data) != {"schema_version", "issue", "repository", "sha256", "payload"} or data.get("schema_version") != 1:
        raise ContractError("canonical Issue snapshot has an invalid schema")
    issue = data.get("issue")
    repository = data.get("repository")
    payload = data.get("payload")
    if isinstance(issue, bool) or not isinstance(issue, int) or issue < 1 or str(issue) != task:
        raise ContractError("canonical Issue snapshot Task identity mismatch")
    if not isinstance(repository, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ContractError("canonical Issue snapshot repository is malformed")
    if not isinstance(payload, dict) or set(payload) != {
        "number", "url", "title", "body", "state", "repository", "labels", "assignees", "milestone"
    }:
        raise ContractError("canonical Issue payload has an invalid schema")
    if (
        isinstance(payload.get("number"), bool)
        or payload.get("number") != issue
        or payload.get("repository") != repository
        or payload.get("state") != "open"
        or not isinstance(payload.get("url"), str)
        or payload["url"].casefold() != f"https://github.com/{repository}/issues/{issue}".casefold()
        or not isinstance(payload.get("title"), str)
        or not payload["title"].strip()
        or not isinstance(payload.get("body"), str)
        or not payload["body"].strip()
        or not isinstance(payload.get("labels"), list)
        or not all(isinstance(item, str) for item in payload["labels"])
        or not isinstance(payload.get("assignees"), list)
        or not all(isinstance(item, str) for item in payload["assignees"])
        or (
            payload.get("milestone") is not None
            and (
                not isinstance(payload["milestone"], dict)
                or set(payload["milestone"]) != {"number", "title"}
                or isinstance(payload["milestone"].get("number"), bool)
                or not isinstance(payload["milestone"].get("number"), int)
                or not isinstance(payload["milestone"].get("title"), str)
            )
        )
    ):
        raise ContractError("canonical Issue payload identity or content is malformed")
    if data.get("sha256") != _digest(payload):
        raise ContractError("canonical Issue snapshot integrity check failed")
    try:
        metadata = json.loads((root / CONTRACT).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContractError("canonical contract metadata is malformed") from exc
    if metadata != {"schema_version": 1, "issue": data["issue"], "repository": data["repository"], "snapshot": SNAPSHOT, "sha256": data["sha256"]}:
        raise ContractError("canonical contract metadata mismatch")
    markers = re.findall(r"canonical-contract sha256=([0-9a-f]{64}) issue=([0-9]+)", state)
    if len(markers) != 1 or markers[0][0] != data["sha256"] or int(markers[0][1]) != data["issue"]:
        raise ContractError("Task State canonical contract marker mismatch")
    expected = _canonical_state(_placeholder_state(root), data["issue"], data["sha256"])
    for heading in ("Purpose", "Scope", "Prohibited changes", "Dependencies", "Acceptance criteria", "Test plan", "Stop conditions", "Coordination surfaces", "External resources"):
        if len(re.findall(rf"(?m)^## {re.escape(heading)}$", state)) != 1:
            raise ContractError(f"canonical Task State section is missing or duplicated: {heading}")
        pattern = rf"(?ms)^## {re.escape(heading)}\n\n(.*?)(?=^## |\Z)"
        actual = re.search(pattern, state)
        wanted = re.search(pattern, expected)
        if not actual or not wanted or actual.group(1).strip() != wanted.group(1).strip():
            raise ContractError(f"canonical Task State section is tampered: {heading}")
    return {"status": "READY", "task": task, "worktree": str(record.path), "issue": data["issue"], "repository": data["repository"], "sha256": data["sha256"]}


def _validate_task_identity_exact(record: lifecycle.WorktreeRecord, task: str, state: str) -> None:
    expected = {
        "Task ID": task,
        "Branch": record.branch,
        "Worktree": str(record.path),
    }
    for label, value in expected.items():
        if value is None or len(re.findall(rf"(?m)^- {re.escape(label)}: .+$", state)) != 1:
            raise ContractError(f"Task State identity is missing or duplicated: {label}")
        if not re.search(rf"(?m)^- {re.escape(label)}: {re.escape(value)}$", state):
            raise ContractError(f"Task State identity mismatch: {label}")


def _resolve_contract_target(root: Path, task: str | None) -> tuple[Path, str]:
    current = lifecycle.current_worktree(root)
    main = lifecycle.main_worktree(root)
    if task is None:
        task = lifecycle.extract_identity_value(lifecycle.state_path(root), "Task ID")
    if not task:
        raise ContractError("canonical Task Contract is missing")
    record = lifecycle.worktree_for_task(root, task)
    if current.path not in {main.path, record.path}:
        raise ContractError("contract check cannot inspect a sibling Task worktree")
    return record.path, task


def _validate_authoritative_issue(
    root: Path, task: str, repository: str, digest: str, runner=None
) -> None:
    identity, payload = fetch_issue(root, task, runner)
    if identity != repository:
        raise ContractError("live Issue repository identity mismatch")
    current = authoritative_payload(payload, int(task), identity)
    if _digest(current) != digest:
        raise ContractError("canonical Issue snapshot no longer matches its authoritative Issue")


def check_contract(root: Path, task: str | None = None, *, runner=None) -> dict:
    target, task = _resolve_contract_target(root, task)
    result = validate_contract(target, task, require_pristine=True)
    if repository_identity(target) != result["repository"]:
        raise ContractError("live repository identity mismatch")
    _validate_authoritative_issue(
        target, task, result["repository"], result["sha256"], runner
    )
    result["mode"] = "initial"
    return result


def check_resume_contract(root: Path, task: str | None = None, *, runner=None) -> dict:
    target, task = _resolve_contract_target(root, task)
    result = validate_contract(target, task)
    try:
        status = lifecycle.state_status(lifecycle.state_path(target))
    except lifecycle.LifecycleError as exc:
        raise ContractError(str(exc)) from exc
    if status not in RESUMABLE_STATES:
        raise ContractError(f"Task State status is not resumable: {status}")
    live_repository = repository_identity(target)
    if live_repository != result["repository"]:
        raise ContractError("live repository identity mismatch")
    _validate_authoritative_issue(
        target, task, result["repository"], result["sha256"], runner
    )
    result["mode"] = "resume"
    result["taskStatus"] = status
    return result
