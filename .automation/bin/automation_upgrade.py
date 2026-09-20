#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tomllib
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath

if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
import git_private_state as private_state


class UpgradeError(RuntimeError):
    pass


RECEIPT_NAME = "automation-maintenance.json"
CONSUMED_RECEIPT_NAME = "automation-maintenance.consumed.json"
SOURCE_RECOVERY_PROOF_NAME = "source-recovery-proof.json"
TASK_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
RECEIPT_FIELDS = {
    "schema_version",
    "status",
    "task_id",
    "branch",
    "worktree",
    "source",
    "source_revision",
    "current_version",
    "upstream_version",
    "changed_paths",
    "authority_head",
    "authority_nonce",
    "path_fingerprints",
}
FULL_OID_RE = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})")
_GIT_EXECUTABLE: Path | None = None


def task_state_dir(repo: Path) -> Path:
    try:
        root = repo.resolve()
        state = root / ".task-state"
        metadata = state.lstat()
        resolved = state.resolve()
    except OSError as exc:
        raise UpgradeError("Task State directory is unavailable") from exc
    if (
        not stat.S_ISDIR(metadata.st_mode)
        or stat.S_ISLNK(metadata.st_mode)
        or (resolved != root and root not in resolved.parents)
    ):
        raise UpgradeError("Task State directory is unsafe")
    return resolved


def git_head(repo: Path) -> str:
    result = run(["git", "rev-parse", "HEAD"], cwd=repo, check=False)
    return result.stdout.strip() if result.returncode == 0 else ""


def git_object_format(repo: Path) -> str:
    value = run(["git", "rev-parse", "--show-object-format"], cwd=repo).stdout.strip()
    if value not in {"sha1", "sha256"}:
        raise UpgradeError("cannot determine the Git object format")
    return value


def _oid_pattern(repo: Path) -> re.Pattern[str]:
    return re.compile(rf"[0-9a-f]{{{40 if git_object_format(repo) == 'sha1' else 64}}}")


def validate_commit_oid(repo: Path, value: object, *, field: str = "Git revision") -> str:
    if not isinstance(value, str) or _oid_pattern(repo).fullmatch(value) is None:
        raise UpgradeError(f"{field} must be a full lowercase commit ID for this repository")
    resolved = run(["git", "rev-parse", "--verify", f"{value}^{{commit}}"], cwd=repo, check=False)
    if resolved.returncode != 0 or resolved.stdout.strip() != value:
        raise UpgradeError(f"{field} is not an exact commit in this repository object database")
    return value


def source_revision(source: Path) -> str | None:
    value = git_head(source)
    return value or None


def receipt_path(repo: Path) -> Path:
    return task_state_dir(repo) / RECEIPT_NAME


def consumed_receipt_path(repo: Path) -> Path:
    return task_state_dir(repo) / CONSUMED_RECEIPT_NAME


def common_git_dir(repo: Path) -> Path:
    value = Path(run(["git", "rev-parse", "--git-common-dir"], cwd=repo).stdout.strip())
    return value.resolve() if value.is_absolute() else (repo / value).resolve()


def worktree_admin_dir(repo: Path) -> Path:
    try:
        raw = run(["git", "rev-parse", "--absolute-git-dir"], cwd=repo).stdout.strip()
        candidate = Path(raw)
        if not raw or not candidate.is_absolute():
            raise UpgradeError("Git returned an invalid absolute worktree administrative directory")
        admin = candidate.resolve()
        if not admin.is_dir():
            raise UpgradeError("worktree administrative directory does not exist")
        return admin
    except (OSError, ValueError, RuntimeError) as exc:
        raise UpgradeError("cannot resolve worktree administrative directory") from exc


def _authority_locations(repo: Path) -> tuple[Path, Path | None]:
    admin = worktree_admin_dir(repo)
    new_parent = admin / private_state.NAMESPACE / "automation-maintenance"
    if new_parent.exists() or new_parent.is_symlink():
        try:
            private_state.safe_directory(admin / private_state.NAMESPACE)
            private_state.safe_directory(new_parent)
        except private_state.GitPrivateStateError as exc:
            raise UpgradeError(str(exc)) from exc
    try:
        resolved_parent = new_parent.resolve()
    except (OSError, ValueError, RuntimeError) as exc:
        raise UpgradeError("cannot resolve authority directory") from exc
    if resolved_parent != admin and admin not in resolved_parent.parents:
        raise UpgradeError("authority directory escapes the worktree administrative directory")
    common = common_git_dir(repo)
    key = hashlib.sha256(str(repo.resolve()).encode("utf-8")).hexdigest()
    migrated = common / private_state.NAMESPACE / "automation-maintenance" / f"{key}.json"
    if os.path.lexists(migrated):
        try:
            private_state.safe_directory(common / private_state.NAMESPACE)
            private_state.safe_directory(migrated.parent)
            private_state.read_bytes(migrated, "migrated maintenance authority")
        except private_state.GitPrivateStateError as exc:
            raise UpgradeError("missing or invalid successful-upgrade authority record") from exc
        legacy = migrated
    else:
        old_admin = _safe_old_admin_authority(repo)
        if old_admin is not None and os.path.lexists(old_admin[0]):
            legacy = old_admin[0]
        else:
            legacy_location = _safe_legacy_location(repo)
            legacy = legacy_location[0] if legacy_location is not None else None
    return new_parent / "authority.json", legacy


def _safe_old_admin_authority(repo: Path) -> tuple[Path, Path] | None:
    try:
        admin = worktree_admin_dir(repo)
        root = admin / "opencode"
        if root.is_symlink() or (root.exists() and not root.is_dir()):
            return None
        parent = root / "automation-maintenance"
        if parent.is_symlink() or (parent.exists() and not parent.is_dir()):
            return None
        resolved = parent.resolve()
        if resolved != admin and admin not in resolved.parents:
            return None
        return parent / "authority.json", admin
    except (OSError, ValueError, RuntimeError, UpgradeError):
        return None


def _safe_legacy_location(repo: Path) -> tuple[Path, Path] | None:
    """Return the legacy record and its containment guard only when safe."""
    try:
        common = common_git_dir(repo)
        if not common.is_dir():
            return None
        parent = common / "opencode" / "automation-maintenance"
        resolved_parent = parent.resolve()
        if resolved_parent != common and common not in resolved_parent.parents:
            return None
        key = hashlib.sha256(str(repo.resolve()).encode("utf-8")).hexdigest()
        return parent / f"{key}.json", common
    except (OSError, ValueError, RuntimeError, UpgradeError, private_state.GitPrivateStateError):
        return None


def authority_path(repo: Path) -> Path:
    return _authority_locations(repo)[0]


def source_recovery_proof_path(repo: Path) -> Path:
    """Return the proof path, using the same containment guard as authority."""
    authority = authority_path(repo)
    return authority.parent / SOURCE_RECOVERY_PROOF_NAME


def canonical_json(value: dict) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def receipt_digest(receipt: dict) -> str:
    return hashlib.sha256(canonical_json(receipt)).hexdigest()


def write_authority(repo: Path, receipt: dict, *, lock_held: bool = False) -> None:
    if not lock_held:
        try:
            private_state.prepare(
                repo,
                admin=True,
                common_dir=common_git_dir(repo),
                admin_dir=worktree_admin_dir(repo),
            )
        except private_state.GitPrivateStateError as exc:
            raise UpgradeError(str(exc)) from exc
    _write_authority_at(
        authority_path(repo),
        {
            "schema_version": 1,
            "task_id": receipt["task_id"],
            "branch": receipt["branch"],
            "worktree": receipt["worktree"],
            "authority_nonce": receipt["authority_nonce"],
            "receipt_sha256": receipt_digest(receipt),
        },
        admin=worktree_admin_dir(repo),
        lock_held=lock_held,
    )


def _write_authority_at(
    path: Path,
    value: dict,
    *,
    admin: Path | None = None,
    lock_held: bool = False,
) -> None:
    try:
        if admin is not None:
            parent_before = path.parent.resolve()
            if parent_before != admin and admin not in parent_before.parents:
                raise UpgradeError("authority directory escapes the worktree administrative directory")
        private_state.ensure_parent(path)
        if admin is not None:
            parent_after = path.parent.resolve()
            if parent_after != admin and admin not in parent_after.parents:
                raise UpgradeError("authority directory escapes the worktree administrative directory")
        private_state.exclusive_write_bytes(
            path,
            json.dumps(value, indent=2, sort_keys=True).encode("utf-8") + b"\n",
            _lock_held=lock_held,
        )
    except (OSError, ValueError, RuntimeError, private_state.GitPrivateStateError) as exc:
        raise UpgradeError(f"cannot publish authority record: {path}") from exc


def _rollback_authority_guard(repo: Path, authority: Path) -> Path:
    old_admin = _safe_old_admin_authority(repo)
    if old_admin is not None and authority == old_admin[0]:
        return old_admin[1]
    legacy = _safe_legacy_location(repo)
    if legacy is not None and authority == legacy[0]:
        return legacy[1]
    common = common_git_dir(repo)
    if authority.parent == common / private_state.NAMESPACE / "automation-maintenance":
        return common
    if authority == authority_path(repo):
        return worktree_admin_dir(repo)
    raise UpgradeError("cannot safely resolve authority location for rollback")


def validate_authority(repo: Path, receipt: dict) -> Path:
    new_path, legacy_path = _authority_locations(repo)
    path = new_path
    if not os.path.lexists(path):
        if legacy_path is None or not os.path.lexists(legacy_path):
            raise UpgradeError("missing or invalid successful-upgrade authority record")
        path = legacy_path
    try:
        private_state.validate_record(path, "successful-upgrade authority record")
        authority = json.loads(
            private_state.read_bytes(path, "successful-upgrade authority record").decode("utf-8")
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, private_state.GitPrivateStateError) as exc:
        raise UpgradeError("missing or invalid successful-upgrade authority record") from exc
    expected = {
        "schema_version": 1,
        "task_id": receipt["task_id"],
        "branch": receipt["branch"],
        "worktree": receipt["worktree"],
        "authority_nonce": receipt["authority_nonce"],
        "receipt_sha256": receipt_digest(receipt),
    }
    if authority != expected:
        raise UpgradeError("automation receipt does not match successful-upgrade authority")
    return path


def _read_json_record(path: Path, error: str) -> dict:
    _, value = _read_json_record_bytes(path, error)
    return value


def _read_json_record_bytes(path: Path, error: str) -> tuple[bytes, dict]:
    try:
        if private_state.NAMESPACE in path.parts or "opencode" in path.parts:
            raw = private_state.read_bytes(path, error)
        else:
            metadata = path.lstat()
            if not stat.S_ISREG(metadata.st_mode) or path.is_symlink():
                raise UpgradeError(error)
            raw = path.read_bytes()
        value = json.loads(raw.decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, private_state.GitPrivateStateError) as exc:
        raise UpgradeError(error) from exc
    if not isinstance(value, dict):
        raise UpgradeError(error)
    return raw, value


def _bridge_authority_path(repo: Path) -> Path:
    return authority_path(repo)


def _bridge_authority(receipt: dict, proof_digest: str) -> dict:
    return {
        "schema_version": 2,
        "kind": "source-recovery-bridge",
        "task_id": receipt["task_id"],
        "branch": receipt["branch"],
        "worktree": receipt["worktree"],
        "authority_nonce": receipt["authority_nonce"],
        "receipt_sha256": receipt_digest(receipt),
        "proof_sha256": proof_digest,
    }


def _validate_no_authority(repo: Path) -> None:
    new_path, legacy_path = _authority_locations(repo)
    if os.path.lexists(new_path) or (legacy_path is not None and os.path.lexists(legacy_path)):
        raise UpgradeError("cannot recover with an existing authority record")


def authority_exists(repo: Path) -> bool:
    new_path, legacy_path = _authority_locations(repo)
    return os.path.lexists(new_path) or (legacy_path is not None and os.path.lexists(legacy_path))


def issue_pair(repo: Path, receipt: dict, *, lock_held: bool = False) -> None:
    destination = receipt_path(repo)
    published: tuple[int, int] | None = None
    try:
        published = exclusive_json_write(destination, receipt)
        if lock_held:
            write_authority(repo, receipt, lock_held=True)
        else:
            write_authority(repo, receipt)
    except (OSError, UpgradeError):
        if published is not None:
            try:
                current = destination.lstat()
                if (current.st_dev, current.st_ino) == published:
                    destination.unlink()
            except OSError as exc:
                raise UpgradeError("receipt publication failed and rollback failed") from exc
        raise


def require_ignored_untracked(repo: Path, paths: tuple[Path, ...]) -> None:
    for path in paths:
        relative = path.relative_to(repo).as_posix()
        tracked = run(
            ["git", "ls-files", "--error-unmatch", "--", relative],
            cwd=repo,
            check=False,
        ).returncode == 0
        if tracked:
            raise UpgradeError(f"required Task State path is tracked: {relative}")
        ignored = run(["git", "check-ignore", "-q", relative], cwd=repo, check=False).returncode == 0
        if not ignored:
            raise UpgradeError(f"required Task State path is not ignored: {relative}")


def atomic_json_write(path: Path, value: dict) -> None:
    atomic_bytes_write(path, (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8"))


def atomic_bytes_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.tmp.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(content)
            stream.flush()
            try:
                os.fsync(stream.fileno())
            except OSError:
                pass
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError as exc:
            raise UpgradeError(f"cannot clean temporary JSON record: {temporary}") from exc


def exclusive_json_write(path: Path, value: dict) -> tuple[int, int]:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.tmp.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(json.dumps(value, indent=2, sort_keys=True).encode("utf-8") + b"\n")
            stream.flush()
            try:
                os.fsync(stream.fileno())
            except OSError:
                pass
        os.link(temporary, path, follow_symlinks=False)
        metadata = path.lstat()
        return metadata.st_dev, metadata.st_ino
    except FileExistsError as exc:
        raise UpgradeError("cannot publish an automation receipt over an existing receipt") from exc
    except OSError as exc:
        raise UpgradeError(f"cannot publish JSON record: {path}") from exc
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError as exc:
            raise UpgradeError(f"cannot clean temporary JSON record: {temporary}") from exc


def exclusive_bytes_write(path: Path, content: bytes) -> tuple[int, int]:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.tmp.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(content)
            stream.flush()
            try:
                os.fsync(stream.fileno())
            except OSError:
                pass
        os.link(temporary, path, follow_symlinks=False)
        metadata = path.lstat()
        return metadata.st_dev, metadata.st_ino
    except FileExistsError as exc:
        raise UpgradeError(f"cannot publish record over an existing record: {path}") from exc
    except OSError as exc:
        raise UpgradeError(f"cannot publish record: {path}") from exc
    finally:
        temporary.unlink(missing_ok=True)


def _validate_admin_record_parent(path: Path, admin: Path) -> None:
    try:
        resolved_admin = admin.resolve()
        parent = path.parent.resolve()
    except (OSError, ValueError, RuntimeError) as exc:
        raise UpgradeError("record path containment cannot be validated") from exc
    if parent != resolved_admin and resolved_admin not in parent.parents:
        raise UpgradeError("record path escapes the worktree administrative directory")


def exclusive_json_write_contained(path: Path, value: dict, *, admin: Path) -> tuple[int, int]:
    _validate_admin_record_parent(path, admin)
    result = exclusive_json_write(path, value)
    _validate_admin_record_parent(path, admin)
    return result


def exclusive_bytes_write_contained(path: Path, content: bytes, *, admin: Path) -> tuple[int, int]:
    _validate_admin_record_parent(path, admin)
    result = exclusive_bytes_write(path, content)
    _validate_admin_record_parent(path, admin)
    return result


def file_fingerprint(repo: Path, raw_path: str) -> dict[str, object]:
    path = repo / Path(*raw_path.split("/"))
    try:
        state = path.lstat()
    except FileNotFoundError:
        return {"state": "absent", "mode": None, "content_sha256": None}
    mode = stat.S_IMODE(state.st_mode)
    if path.is_file() and not path.is_symlink():
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        return {"state": "file", "mode": mode, "content_sha256": digest}
    if path.is_symlink():
        digest = hashlib.sha256(os.readlink(path).encode("utf-8", "surrogateescape")).hexdigest()
        return {"state": "symlink", "mode": mode, "content_sha256": digest}
    if path.is_dir():
        return {"state": "directory", "mode": mode, "content_sha256": None}
    return {"state": "special", "mode": mode, "content_sha256": None}


def parse_task_identity(repo: Path, task: str) -> tuple[str, str, Path]:
    if not TASK_ID_RE.fullmatch(task):
        raise UpgradeError(f"invalid Task ID: {task!r}")
    state_path = task_state_dir(repo) / "task.md"
    if not state_path.is_file():
        raise UpgradeError("operation requires a Task worktree with Task State")
    text = state_path.read_text(encoding="utf-8")
    values: dict[str, str] = {}
    for label in ("Task ID", "Branch", "Worktree"):
        matches = re.findall(rf"(?m)^- {re.escape(label)}: (.+)$", text)
        if len(matches) != 1 or not matches[0].strip():
            raise UpgradeError(f"Task State must contain exactly one {label} identity field")
        values[label] = matches[0].strip()
    if values["Task ID"] != task:
        raise UpgradeError("Task State identity does not match requested Task")
    branch = values["Branch"]
    if not (branch.startswith(f"task/{task}-") or branch.startswith(f"fix/{task}-")):
        raise UpgradeError(f"Task State branch is not the Task branch for {task}")
    worktree = Path(values["Worktree"]).resolve()
    if worktree != repo.resolve():
        raise UpgradeError("Task State worktree does not match the current worktree")
    return task, branch, worktree


def registered_worktrees(repo: Path) -> dict[Path, tuple[str | None, str | None]]:
    output = run(["git", "worktree", "list", "--porcelain"], cwd=repo).stdout
    records: dict[Path, tuple[str | None, str | None]] = {}
    current: dict[str, str] = {}
    for line in output.splitlines() + [""]:
        if not line:
            if current.get("worktree"):
                branch = current.get("branch", "").removeprefix("refs/heads/") or None
                records[Path(current["worktree"]).resolve()] = (branch, current.get("HEAD"))
            current = {}
            continue
        key, _, value = line.partition(" ")
        current[key] = value
    visible_top = Path(run(["git", "rev-parse", "--show-toplevel"], cwd=repo).stdout.strip())
    if visible_top.is_absolute():
        visible_top = visible_top.resolve()
        current_repo = repo.resolve()
        admin = worktree_admin_dir(repo)
        current_branch = run(["git", "branch", "--show-current"], cwd=repo).stdout.strip() or None
        current_head = git_head(repo) or None
        admin_record = records.get(admin)
        if (
            visible_top == current_repo
            and visible_top not in records
            and admin_record is not None
            and admin_record == (current_branch, current_head)
        ):
            records[visible_top] = admin_record
    return records


def require_registered_task(repo: Path, task: str) -> tuple[str, str, Path]:
    _, branch, worktree = parse_task_identity(repo, task)
    registered = registered_worktrees(repo)
    actual = registered.get(worktree)
    current_branch = run(["git", "branch", "--show-current"], cwd=repo).stdout.strip()
    head = git_head(repo)
    branch_oid = run(["git", "rev-parse", f"refs/heads/{branch}"], cwd=repo, check=False).stdout.strip()
    if (
        actual is None
        or current_branch != branch
        or actual[0] != branch
        or not head
        or not branch_oid
        or actual[1] != head
        or branch_oid != head
    ):
        raise UpgradeError("current Task worktree is not registered with the expected branch")
    default = run(["git", "symbolic-ref", "--quiet", "--short", "refs/remotes/origin/HEAD"], cwd=repo, check=False).stdout.strip().removeprefix("origin/")
    if not default:
        raise UpgradeError("cannot resolve default branch")
    if branch == default:
        raise UpgradeError("operation refused on the default branch")
    return task, branch, worktree


@dataclass(frozen=True)
class Action:
    path: str
    action: str
    reason: str


@dataclass(frozen=True)
class Migration:
    from_version: int
    to_version: int
    remove_paths: tuple[Path, ...]
    require_absent_paths: tuple[Path, ...]


@dataclass(frozen=True)
class SavedPath:
    kind: str
    content: bytes | str | None
    mode: int | None


def git_environment(overrides: dict[str, str] | None = None) -> dict[str, str]:
    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("GIT_")
        and not key.startswith("LD_")
        and not key.startswith("DYLD_")
        and key != "EMAIL"
    }
    if overrides:
        environment.update(overrides)
    environment["GIT_OPTIONAL_LOCKS"] = "0"
    return environment


def git_executable() -> Path:
    global _GIT_EXECUTABLE
    if _GIT_EXECUTABLE is not None:
        return _GIT_EXECUTABLE
    candidate = shutil.which("git")
    if not candidate:
        raise UpgradeError("trusted Git executable is unavailable")
    executable = Path(candidate).resolve()
    try:
        metadata = executable.stat()
    except OSError as exc:
        raise UpgradeError("trusted Git executable is unavailable") from exc
    if (
        not stat.S_ISREG(metadata.st_mode)
        or metadata.st_uid != 0
        or stat.S_IMODE(metadata.st_mode) & 0o022
    ):
        raise UpgradeError(f"Git executable is not root-owned and immutable: {executable}")
    _GIT_EXECUTABLE = executable
    return executable


def trusted_git_command(command: list[str]) -> list[str]:
    if not command or command[0] != "git":
        raise UpgradeError("trusted Git command must start with git")
    return [
        str(git_executable()),
        "-c",
        "core.fsmonitor=false",
        "-c",
        "core.hooksPath=/dev/null",
        "-c",
        "core.pager=",
        *command[1:],
    ]


def run(
    command: list[str],
    *,
    cwd: Path,
    check: bool = True,
    env_overrides: dict[str, str] | None = None,
    input_text: str | None = None,
) -> subprocess.CompletedProcess[str]:
    is_git = bool(command and command[0] == "git")
    environment = git_environment(env_overrides) if is_git else None
    executable_command = trusted_git_command(command) if is_git else command
    result = subprocess.run(
        executable_command,
        cwd=cwd,
        text=True,
        input=input_text,
        capture_output=True,
        env=environment,
    )
    if check and result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit {result.returncode}"
        raise UpgradeError(f"{' '.join(command)}: {detail}")
    return result


def root() -> Path:
    installed_root = Path(__file__).resolve().parents[2]
    result = run(["git", "rev-parse", "--show-toplevel"], cwd=installed_root)
    discovered = Path(result.stdout.strip()).resolve()
    if discovered != installed_root:
        raise UpgradeError("installed Agent Core path does not match the Git worktree root")
    return installed_root


def version(repo: Path) -> str:
    path = repo / ".automation" / "VERSION"
    if not path.is_file():
        raise UpgradeError("missing .automation/VERSION")
    return path.read_text(encoding="utf-8").strip()


def upstream(repo: Path) -> dict[str, str]:
    path = repo / ".automation" / "UPSTREAM"
    if not path.is_file():
        raise UpgradeError("missing .automation/UPSTREAM")
    raw = tomllib.loads(path.read_text(encoding="utf-8"))
    required = ("repository", "ref", "component")
    missing = [key for key in required if not isinstance(raw.get(key), str) or not raw[key]]
    if missing:
        raise UpgradeError("invalid UPSTREAM fields: " + ", ".join(missing))
    return {key: raw[key] for key in required}


def context(repo: Path) -> dict:
    return {
        "version": version(repo),
        "adapter": (repo / ".automation" / "ADAPTER").read_text(encoding="utf-8").strip(),
        "upstream": upstream(repo),
    }


def resolve_source(path: Path | None) -> Path:
    if path is None:
        raise UpgradeError("update check requires --source <Templates checkout>; no remote code is fetched automatically")
    source = path.resolve()
    if not (source / "components" / "agent-core" / ".automation" / "VERSION").is_file():
        raise UpgradeError(f"not a Templates source checkout: {source}")
    return source


def git_bytes(command: list[str], *, cwd: Path) -> bytes:
    if not command or command[0] != "git":
        raise UpgradeError("byte helper only accepts Git commands")
    result = subprocess.run(
        trusted_git_command(command),
        cwd=cwd,
        capture_output=True,
        check=False,
        env=git_environment(),
    )
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip() or f"exit {result.returncode}"
        raise UpgradeError(f"{' '.join(command)}: {detail}")
    return result.stdout


def resolve_pinned_source(path: Path) -> tuple[Path, str]:
    source = resolve_source(path)
    top = Path(run(["git", "rev-parse", "--show-toplevel"], cwd=source).stdout.strip()).resolve()
    head = git_head(source)
    if top != source:
        raise UpgradeError("source must be a Git worktree root with a full non-null HEAD")
    validate_commit_oid(source, head, field="source HEAD")
    status = git_bytes(
        ["git", "status", "--porcelain=v1", "-z", "--", "components/agent-core"], cwd=source
    )
    if status:
        raise UpgradeError("source components/agent-core must be clean")
    return source, head


def resolve_clean_source_worktree(path: Path) -> tuple[Path, str]:
    """Validate the whole Templates checkout for source-side recovery."""
    source = resolve_source(path)
    top = Path(run(["git", "rev-parse", "--show-toplevel"], cwd=source).stdout.strip()).resolve()
    head = git_head(source)
    if top != source:
        raise UpgradeError("source must be a Git worktree root with a full non-null HEAD")
    validate_commit_oid(source, head, field="source HEAD")
    if git_bytes(["git", "status", "--porcelain=v1", "-z"], cwd=source):
        raise UpgradeError("Templates source worktree must be clean")
    return source, head


def _source_is_compatible(receipt_source: str, source: Path) -> bool:
    candidate = Path(receipt_source)
    if not candidate.is_absolute() or not candidate.exists() or not candidate.is_dir():
        return False
    try:
        candidate = candidate.resolve()
        top = Path(run(["git", "rev-parse", "--show-toplevel"], cwd=candidate).stdout.strip()).resolve()
        return top == candidate and (
            candidate == source or common_git_dir(candidate) == common_git_dir(source)
        )
    except (OSError, UpgradeError, ValueError, RuntimeError):
        return False


def _validate_source_revision(source: Path, revision: object) -> str:
    return validate_commit_oid(source, revision, field="receipt source_revision")


def _validate_expected_implementation_revision(revision: object) -> str:
    if not isinstance(revision, str) or FULL_OID_RE.fullmatch(revision) is None:
        raise UpgradeError(
            "expected implementation revision must be a full lowercase hexadecimal Git revision"
        )
    return revision


def load_ownership(repo: Path) -> dict[str, str]:
    path = repo / ".automation" / "ownership.toml"
    if not path.is_file():
        return {}
    raw = tomllib.loads(path.read_text(encoding="utf-8"))
    return {str(key): str(value) for key, value in raw.get("paths", {}).items()}


def managed(relative: Path) -> bool:
    text = relative.as_posix()
    if text in {"AGENTS.md", "Justfile", "opencode.json"}:
        return True
    if text.startswith(".opencode/"):
        return True
    if text.startswith(".automation/"):
        return text not in {
            ".automation/ADAPTER",
            ".automation/INIT.fragment.md",
            ".automation/adoption.toml",
        }
    return False


def deletable_managed(relative: Path) -> bool:
    text = relative.as_posix()
    if text == "opencode.json" or text.startswith(".opencode/"):
        return True
    if text.startswith(".automation/"):
        return text not in {
            ".automation/ADAPTER",
            ".automation/INIT.fragment.md",
            ".automation/adoption.toml",
            ".automation/VERSION",
        }
    return False


def version_number(repo: Path) -> int:
    raw = version(repo)
    try:
        parsed = int(raw)
    except ValueError as exc:
        raise UpgradeError(f"invalid Agent Core VERSION: {raw!r}") from exc
    if parsed <= 0 or str(parsed) != raw:
        raise UpgradeError(f"invalid Agent Core VERSION: {raw!r}")
    return parsed


def migration_path(raw: object, *, field: str, managed_delete: bool) -> Path:
    if not isinstance(raw, str) or not raw:
        raise UpgradeError(f"migrations.toml {field} entries must be non-empty strings")
    if "\\" in raw or any(character in raw for character in "*?[]{}"):
        raise UpgradeError(f"migrations.toml {field} path must be exact repository-relative: {raw!r}")
    pure = PurePosixPath(raw)
    if pure.is_absolute() or not pure.parts or any(part in {"", ".", ".."} for part in pure.parts):
        raise UpgradeError(f"migrations.toml {field} path is unsafe: {raw!r}")
    if pure.as_posix() != raw:
        raise UpgradeError(f"migrations.toml {field} path is not normalized: {raw!r}")
    relative = Path(*pure.parts)
    if managed_delete and not deletable_managed(relative):
        raise UpgradeError(f"migrations.toml remove_paths path is not Agent Core managed: {raw!r}")
    return relative


def load_migrations(source_core: Path) -> list[Migration]:
    path = source_core / ".automation" / "migrations.toml"
    if not path.is_file():
        return []
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise UpgradeError(f"invalid migrations.toml: {exc}") from exc
    unknown_top_level = set(raw) - {"schema_version", "migrations"}
    if unknown_top_level:
        raise UpgradeError(f"migrations.toml unknown top-level fields: {sorted(unknown_top_level)}")
    if type(raw.get("schema_version")) is not int or raw["schema_version"] != 1:
        raise UpgradeError("migrations.toml schema_version must be integer 1")
    entries = raw.get("migrations", [])
    if not isinstance(entries, list):
        raise UpgradeError("migrations.toml migrations must be an array of tables")

    migrations: list[Migration] = []
    transitions: set[tuple[int, int]] = set()
    required_fields = {"from_version", "to_version", "remove_paths", "require_absent_paths"}
    for index, entry in enumerate(entries):
        label = f"migrations.toml migrations[{index}]"
        if not isinstance(entry, dict):
            raise UpgradeError(f"{label} must be a table")
        unknown = set(entry) - required_fields
        missing = required_fields - set(entry)
        if unknown:
            raise UpgradeError(f"{label} unknown fields: {sorted(unknown)}")
        if missing:
            raise UpgradeError(f"{label} missing fields: {sorted(missing)}")
        from_version = entry["from_version"]
        to_version = entry["to_version"]
        if type(from_version) is not int or type(to_version) is not int or from_version <= 0 or to_version <= 0:
            raise UpgradeError(f"{label} versions must be positive integers")
        if to_version != from_version + 1:
            raise UpgradeError(f"{label} transition must advance exactly one version")
        transition = (from_version, to_version)
        if transition in transitions:
            raise UpgradeError(f"{label} duplicates transition {from_version} -> {to_version}")
        transitions.add(transition)

        parsed_paths: dict[str, tuple[Path, ...]] = {}
        for field, managed_delete in (("remove_paths", True), ("require_absent_paths", False)):
            values = entry[field]
            if not isinstance(values, list):
                raise UpgradeError(f"{label}.{field} must be a list")
            paths = tuple(
                migration_path(value, field=f"{label}.{field}", managed_delete=managed_delete)
                for value in values
            )
            if len(set(paths)) != len(paths):
                raise UpgradeError(f"{label}.{field} contains duplicate paths")
            parsed_paths[field] = paths
        migrations.append(
            Migration(
                from_version=from_version,
                to_version=to_version,
                remove_paths=parsed_paths["remove_paths"],
                require_absent_paths=parsed_paths["require_absent_paths"],
            )
        )
    return sorted(migrations, key=lambda item: (item.to_version, item.from_version))


def path_present(path: Path) -> bool:
    return os.path.lexists(path)


def symlink_ancestor(repo: Path, relative: Path) -> Path | None:
    current = repo
    for part in relative.parts[:-1]:
        current /= part
        if current.is_symlink():
            return current.relative_to(repo)
    return None


def migration_actions(
    repo: Path,
    migrations: list[Migration],
) -> tuple[list[Action], list[str], set[Path]]:
    actions: list[Action] = []
    blockers: list[str] = []
    deleted: set[Path] = set()
    for migration in migrations:
        transition = f"{migration.from_version} -> {migration.to_version}"
        for relative in migration.require_absent_paths:
            if relative in deleted:
                continue
            ancestor = symlink_ancestor(repo, relative)
            if ancestor is not None:
                blockers.append(
                    f"migration {transition} precondition {relative.as_posix()}: "
                    f"symlink ancestor {ancestor.as_posix()} is unsafe; operator must remove or replace it"
                )
            elif path_present(repo / relative):
                blockers.append(
                    f"migration {transition} precondition {relative.as_posix()}: path must be absent; "
                    "operator must resolve it before upgrading"
                )
        for relative in migration.remove_paths:
            ancestor = symlink_ancestor(repo, relative)
            if ancestor is not None:
                actions.append(
                    Action(
                        relative.as_posix(),
                        "blocked",
                        f"migration {transition} delete has symlink ancestor {ancestor.as_posix()}",
                    )
                )
                continue
            destination = repo / relative
            if relative in deleted:
                continue
            if not path_present(destination):
                actions.append(
                    Action(
                        relative.as_posix(),
                        "noop",
                        f"already absent for Agent Core migration {transition}",
                    )
                )
                continue
            if destination.is_symlink() or destination.is_file():
                actions.append(
                    Action(
                        relative.as_posix(),
                        "delete",
                        f"removed by Agent Core migration {transition}",
                    )
                )
                deleted.add(relative)
            elif destination.is_dir():
                actions.append(Action(relative.as_posix(), "blocked", f"migration {transition} refuses directory deletion"))
            else:
                actions.append(Action(relative.as_posix(), "blocked", f"migration {transition} refuses special-file deletion"))
    return actions, blockers, deleted


def replace_agent_rules(existing: str, upstream_rules: str) -> tuple[str | None, str]:
    begin = "<!-- BEGIN AGENT CORE RULES -->"
    end = "<!-- END AGENT CORE RULES -->"
    begin_count = existing.count(begin)
    end_count = existing.count(end)
    if begin_count != 1 or end_count != 1:
        return None, "managed Agent Core rules block is missing or malformed"
    start = existing.index(begin)
    finish = existing.index(end, start) + len(end)
    replacement = f"{begin}\n{upstream_rules.rstrip()}\n{end}"
    return existing[:start] + replacement + existing[finish:], "replace managed Agent Core rules block"


def merge_just_router(existing: str, upstream: str) -> tuple[str | None, str]:
    module_re = re.compile(r"^(mod\??)\s+([A-Za-z0-9_-]+)\s+(['\"])(.+?)\3\s*$")
    required: dict[str, str] = {}
    for line in upstream.splitlines():
        match = module_re.match(line.strip())
        if match:
            required[match.group(2)] = line.strip()
    lines = existing.splitlines()
    by_name: dict[str, list[int]] = {}
    for index, line in enumerate(lines):
        match = module_re.match(line.strip())
        if match:
            by_name.setdefault(match.group(2), []).append(index)
    for name, expected in required.items():
        indices = by_name.get(name, [])
        if len(indices) > 1:
            return None, f"Just module {name!r} is declared more than once"
        if indices:
            current = lines[indices[0]].strip()
            current_match = module_re.match(current)
            expected_match = module_re.match(expected)
            assert current_match and expected_match
            if current_match.group(4) != expected_match.group(4):
                return None, f"Just module {name!r} points to repository-owned path {current_match.group(4)!r}"
            lines[indices[0]] = expected
        else:
            lines.append(expected)
    suffix = "\n" if existing.endswith("\n") or not existing else ""
    return "\n".join(lines) + suffix, "merge Agent Core router declarations"


def write_topology_blocker(repo: Path, relative: Path, planned_deletes: set[Path]) -> str | None:
    current = repo
    current_relative = Path()
    for part in relative.parts[:-1]:
        current /= part
        current_relative /= part
        if current_relative in planned_deletes:
            return None
        if current.is_symlink():
            return f"symlink ancestor {current_relative.as_posix()} cannot be upgraded safely"
        if path_present(current) and not current.is_dir():
            return f"non-directory ancestor {current_relative.as_posix()} blocks managed path"
    destination = repo / relative
    if relative not in planned_deletes and destination.is_symlink():
        return "destination symlink cannot be upgraded safely"
    return None


def action_for(repo: Path, source_core: Path, relative: Path, ownership: dict[str, str]) -> Action:
    rel = relative.as_posix()
    destination = repo / relative
    source = source_core / relative
    if not destination.exists():
        return Action(rel, "create", "Agent Core-owned path is absent")
    if not destination.is_file() or not source.is_file():
        return Action(rel, "blocked", "non-file collision cannot be upgraded safely")
    if destination.read_bytes() == source.read_bytes():
        return Action(rel, "noop", "already matches upstream")

    if rel == "AGENTS.md":
        existing = destination.read_text(encoding="utf-8")
        has_marker = "<!-- BEGIN AGENT CORE RULES -->" in existing or "<!-- END AGENT CORE RULES -->" in existing
        if has_marker:
            merged, detail = replace_agent_rules(existing, source.read_text(encoding="utf-8"))
            return Action(rel, "merge" if merged is not None else "blocked", detail)
        if ownership.get(rel) == "replace":
            return Action(rel, "replace", "ownership metadata marks generated AGENTS.md as replaceable")
        return Action(rel, "blocked", "AGENTS.md ownership is ambiguous and no managed block exists")

    if rel == "Justfile":
        existing = destination.read_text(encoding="utf-8")
        adopted_router = "# Agent Core module router" in existing
        if adopted_router or ownership.get(rel) != "replace":
            merged, detail = merge_just_router(existing, source.read_text(encoding="utf-8"))
            return Action(rel, "merge" if merged is not None else "blocked", detail)
        return Action(rel, "replace", "ownership metadata marks generated Justfile as replaceable")

    return Action(rel, "replace", "Agent Core-owned path")


def git_object_bytes(repo: Path, revision: str, path: str) -> bytes:
    result = subprocess.run(
        trusted_git_command(["git", "show", f"{revision}:{path}"]),
        cwd=repo,
        capture_output=True,
        check=False,
        env=git_environment(),
    )
    if result.returncode != 0:
        raise UpgradeError(f"cannot read Git object {path}")
    return result.stdout


def materialize_tree(
    repo: Path, revision: str, destination: Path, prefix: str = "", *, surface_only: bool = False
) -> None:
    entries = git_bytes(["git", "ls-tree", "-r", "-z", revision, "--", prefix or "."], cwd=repo).split(b"\0")
    destination.mkdir(parents=True, exist_ok=True)
    prefix_bytes = (prefix.rstrip("/") + "/").encode() if prefix else b""
    for entry in entries:
        if not entry:
            continue
        header, raw_path = entry.split(b"\t", 1)
        fields = header.split()
        if prefix_bytes and not raw_path.startswith(prefix_bytes):
            raise UpgradeError("Git returned an unexpected snapshot path")
        relative_raw = raw_path[len(prefix_bytes):] if prefix_bytes else raw_path
        try:
            relative_text = relative_raw.decode("utf-8")
            relative = PurePosixPath(relative_text)
        except UnicodeDecodeError as exc:
            raise UpgradeError("snapshot contains a non-UTF-8 path") from exc
        if surface_only and not (
            relative_text in {"AGENTS.md", "Justfile", "opencode.json"}
            or relative_text.startswith(".automation/")
            or relative_text.startswith(".opencode/")
        ):
            continue
        if len(fields) != 3 or fields[0] not in {b"100644", b"100755"} or fields[1] != b"blob":
            raise UpgradeError("Agent Core snapshot contains a symlink, submodule, or special Git entry")
        if (
            relative.is_absolute()
            or not relative.parts
            or any(part in {"", ".", ".."} for part in relative.parts)
            or relative.as_posix() != relative_text
        ):
            raise UpgradeError("snapshot contains an unsafe or unnormalized path")
        target = destination.joinpath(*relative.parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(git_object_bytes(repo, revision, raw_path.decode("utf-8")))
        target.chmod(0o755 if fields[0] == b"100755" else 0o644)


def materialize_source_snapshot(source: Path, revision: str, temporary: Path) -> tuple[Path, Path]:
    """Reconstruct only Agent Core from a pinned source revision.

    The returned paths are (synthetic Templates root, synthetic Agent Core
    root).  In particular, callers must not use the live source checkout for
    planning or copying after this point.
    """
    snapshot_root = temporary / "source"
    source_core = snapshot_root / "components" / "agent-core"
    materialize_tree(source, revision, source_core, "components/agent-core")
    return snapshot_root, source_core


def revalidate_source(source: Path, revision: str) -> None:
    current, current_revision = resolve_pinned_source(source)
    if current != source or current_revision != revision:
        raise UpgradeError("source changed during upgrade planning or mutation")


def _anchored_parent(root: Path, relative: Path, *, create: bool) -> tuple[int, str]:
    """Open a managed path's parent without following any component symlink."""
    if not relative.parts or relative.is_absolute() or any(part in {"", ".", ".."} for part in relative.parts):
        raise UpgradeError(f"unsafe managed path: {relative}")
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    root_fd = os.open(root, flags)
    current = root_fd
    try:
        for component in relative.parts[:-1]:
            try:
                next_fd = os.open(component, flags, dir_fd=current)
            except FileNotFoundError:
                if not create:
                    raise
                os.mkdir(component, 0o755, dir_fd=current)
                next_fd = os.open(component, flags, dir_fd=current)
            os.close(current)
            current = next_fd
        return current, relative.name
    except BaseException:
        os.close(current)
        raise


def _anchored_read(parent_fd: int, name: str) -> bytes:
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=parent_fd)
    try:
        metadata = os.fstat(fd)
        if not stat.S_ISREG(metadata.st_mode):
            raise UpgradeError(f"managed path is not a regular file: {name}")
        chunks: list[bytes] = []
        while True:
            chunk = os.read(fd, 65536)
            if not chunk:
                return b"".join(chunks)
            chunks.append(chunk)
    finally:
        os.close(fd)


def _anchored_parent_unchanged(tree: Path, relative: Path, parent_fd: int) -> None:
    check_fd, _ = _anchored_parent(tree, relative, create=False)
    try:
        current = os.fstat(check_fd)
        pinned = os.fstat(parent_fd)
        if (current.st_dev, current.st_ino) != (pinned.st_dev, pinned.st_ino):
            raise UpgradeError(f"managed path parent changed during upgrade: {relative}")
    finally:
        os.close(check_fd)


def _anchored_write(parent_fd: int, name: str, content: bytes, mode: int) -> None:
    try:
        current = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
    except FileNotFoundError:
        current = None
    if current is not None and not stat.S_ISREG(current.st_mode):
        raise UpgradeError(f"managed path became unsafe after planning: {name}")
    temporary = f".{name}.upgrade-{secrets.token_hex(16)}"
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=parent_fd)
    published = False
    try:
        view = memoryview(content)
        while view:
            written = os.write(fd, view)
            if written <= 0:
                raise OSError("short managed-path write")
            view = view[written:]
        os.fchmod(fd, mode)
        os.fsync(fd)
        os.close(fd)
        os.rename(temporary, name, src_dir_fd=parent_fd, dst_dir_fd=parent_fd)
        published = True
        os.fsync(parent_fd)
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
        if not published:
            try:
                os.unlink(temporary, dir_fd=parent_fd)
            except FileNotFoundError:
                pass


def _apply_plan_to_tree_anchored(tree: Path, source_core: Path, plan: dict) -> list[str]:
    actionable = [item for item in plan["actions"] if item["action"] != "noop"]
    phases = {"delete": 0, "create": 1, "replace": 2, "merge": 3}
    ordered = sorted(
        [item for item in actionable if item["path"] != ".automation/VERSION"],
        key=lambda item: (phases.get(item["action"], 99), item["path"]),
    ) + [item for item in actionable if item["path"] == ".automation/VERSION"]
    changed: list[str] = []
    for item in ordered:
        relative = Path(item["path"])
        parent_fd, name = _anchored_parent(tree, relative, create=item["action"] != "delete")
        try:
            if item["action"] == "delete":
                try:
                    metadata = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
                except FileNotFoundError:
                    metadata = None
                if metadata is not None and not (stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode)):
                    raise UpgradeError(f"delete became unsafe after planning: {item['path']}")
                if metadata is not None:
                    os.unlink(name, dir_fd=parent_fd)
                    os.fsync(parent_fd)
            else:
                source = source_core / relative
                if item["action"] == "merge" and item["path"] == "AGENTS.md":
                    current = _anchored_read(parent_fd, name).decode("utf-8")
                    merged, detail = replace_agent_rules(current, source.read_text(encoding="utf-8"))
                    if merged is None:
                        raise UpgradeError(f"AGENTS.md merge became unsafe: {detail}")
                    content = merged.encode("utf-8")
                    mode = stat.S_IMODE(
                        os.stat(name, dir_fd=parent_fd, follow_symlinks=False).st_mode
                    )
                elif item["action"] == "merge" and item["path"] == "Justfile":
                    current = _anchored_read(parent_fd, name).decode("utf-8")
                    merged, detail = merge_just_router(current, source.read_text(encoding="utf-8"))
                    if merged is None:
                        raise UpgradeError(f"Justfile merge became unsafe: {detail}")
                    content = merged.encode("utf-8")
                    mode = stat.S_IMODE(
                        os.stat(name, dir_fd=parent_fd, follow_symlinks=False).st_mode
                    )
                else:
                    content = source.read_bytes()
                    mode = stat.S_IMODE(source.stat().st_mode)
                _anchored_write(parent_fd, name, content, mode)
            _anchored_parent_unchanged(tree, relative, parent_fd)
            changed.append(item["path"])
        finally:
            os.close(parent_fd)
    return sorted(set(changed))


def apply_plan_to_tree(tree: Path, source_core: Path, plan: dict, *, anchored: bool = False) -> list[str]:
    if anchored:
        return _apply_plan_to_tree_anchored(tree, source_core, plan)
    actionable = [item for item in plan["actions"] if item["action"] != "noop"]
    planned_deletes = {Path(item["path"]) for item in actionable if item["action"] == "delete"}
    blockers: list[str] = []
    for item in actionable:
        relative = Path(item["path"])
        if item["action"] == "delete":
            target = tree / relative
            ancestor = symlink_ancestor(tree, relative)
            if ancestor is not None:
                blockers.append(f"{item['path']}: delete has symlink ancestor {ancestor.as_posix()}")
                continue
            if path_present(target) and not (target.is_symlink() or target.is_file()):
                blockers.append(f"{item['path']}: delete target changed to an unsafe type")
        else:
            blocker = write_topology_blocker(tree, relative, planned_deletes)
            if blocker:
                blockers.append(f"{item['path']}: {blocker}")
    if blockers:
        raise UpgradeError("upgrade blocked before mutation:\n- " + "\n- ".join(blockers))
    phases = {"delete": 0, "create": 1, "replace": 2, "merge": 3}
    ordered = sorted(
        [item for item in actionable if item["path"] != ".automation/VERSION"],
        key=lambda item: (phases.get(item["action"], 99), item["path"]),
    ) + [item for item in actionable if item["path"] == ".automation/VERSION"]
    changed: list[str] = []
    for item in ordered:
        relative = Path(item["path"])
        target = tree / relative
        source = source_core / relative
        if item["action"] == "delete":
            ancestor = symlink_ancestor(tree, relative)
            if ancestor is not None:
                raise UpgradeError(
                    f"delete path gained symlink ancestor after planning: {ancestor.as_posix()}"
                )
            if target.is_symlink() or target.is_file():
                target.unlink()
            elif path_present(target):
                raise UpgradeError(f"delete became unsafe after planning: {item['path']}")
        else:
            blocker = write_topology_blocker(tree, relative, set())
            if blocker:
                raise UpgradeError(f"managed path became unsafe after planning: {item['path']}: {blocker}")
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.is_symlink():
                raise UpgradeError(f"destination became a symlink after planning: {item['path']}")
            if item["action"] in {"create", "replace"}:
                shutil.copy2(source, target)
            elif item["action"] == "merge" and item["path"] == "AGENTS.md":
                merged, detail = replace_agent_rules(target.read_text(encoding="utf-8"), source.read_text(encoding="utf-8"))
                if merged is None:
                    raise UpgradeError(f"AGENTS.md merge became unsafe: {detail}")
                target.write_text(merged, encoding="utf-8")
            elif item["action"] == "merge" and item["path"] == "Justfile":
                merged, detail = merge_just_router(target.read_text(encoding="utf-8"), source.read_text(encoding="utf-8"))
                if merged is None:
                    raise UpgradeError(f"Justfile merge became unsafe: {detail}")
                target.write_text(merged, encoding="utf-8")
            else:
                raise UpgradeError(f"unsupported upgrade action: {item}")
        changed.append(item["path"])
    return sorted(set(changed))


def _capture_paths_anchored(tree: Path, paths: list[str]) -> dict[str, SavedPath]:
    saved: dict[str, SavedPath] = {}
    for raw_path in paths:
        relative = Path(*raw_path.split("/"))
        try:
            parent_fd, name = _anchored_parent(tree, relative, create=False)
        except FileNotFoundError:
            saved[raw_path] = SavedPath("absent", None, None)
            continue
        try:
            try:
                metadata = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
            except FileNotFoundError:
                saved[raw_path] = SavedPath("absent", None, None)
                continue
            mode = stat.S_IMODE(metadata.st_mode)
            if stat.S_ISLNK(metadata.st_mode):
                saved[raw_path] = SavedPath("symlink", os.readlink(name, dir_fd=parent_fd), mode)
            elif stat.S_ISREG(metadata.st_mode):
                saved[raw_path] = SavedPath("file", _anchored_read(parent_fd, name), mode)
            else:
                raise UpgradeError(f"cannot snapshot unsafe upgrade destination path: {raw_path}")
            _anchored_parent_unchanged(tree, relative, parent_fd)
        finally:
            os.close(parent_fd)
    return saved


def capture_paths(
    tree: Path,
    paths: list[str],
    *,
    anchored: bool = False,
) -> dict[str, SavedPath]:
    if anchored:
        return _capture_paths_anchored(tree, paths)
    saved: dict[str, SavedPath] = {}
    for raw_path in paths:
        target = tree / Path(*raw_path.split("/"))
        try:
            metadata = target.lstat()
        except FileNotFoundError:
            saved[raw_path] = SavedPath("absent", None, None)
            continue
        mode = stat.S_IMODE(metadata.st_mode)
        if target.is_symlink():
            saved[raw_path] = SavedPath("symlink", os.readlink(target), mode)
        elif target.is_file():
            saved[raw_path] = SavedPath("file", target.read_bytes(), mode)
        else:
            raise UpgradeError(f"cannot snapshot unsafe upgrade destination path: {raw_path}")
    return saved


def _restore_paths_anchored(tree: Path, saved: dict[str, SavedPath]) -> None:
    for raw_path, state in saved.items():
        relative = Path(*raw_path.split("/"))
        try:
            parent_fd, name = _anchored_parent(tree, relative, create=state.kind != "absent")
        except FileNotFoundError:
            if state.kind == "absent":
                continue
            raise
        try:
            try:
                metadata = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
            except FileNotFoundError:
                metadata = None
            if metadata is not None:
                if not (stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode)):
                    raise UpgradeError(f"cannot restore unsafe upgrade destination path: {raw_path}")
                os.unlink(name, dir_fd=parent_fd)
                os.fsync(parent_fd)
            if state.kind == "absent":
                continue
            if state.kind == "file" and isinstance(state.content, bytes) and state.mode is not None:
                _anchored_write(parent_fd, name, state.content, state.mode)
            elif state.kind == "symlink" and isinstance(state.content, str):
                os.symlink(state.content, name, dir_fd=parent_fd)
            else:  # pragma: no cover
                raise UpgradeError(f"invalid saved upgrade destination state: {raw_path}")
            _anchored_parent_unchanged(tree, relative, parent_fd)
        finally:
            os.close(parent_fd)


def restore_paths(tree: Path, saved: dict[str, SavedPath], *, anchored: bool = False) -> None:
    if anchored:
        _restore_paths_anchored(tree, saved)
        return
    for raw_path, state in saved.items():
        target = tree / Path(*raw_path.split("/"))
        if target.is_symlink() or target.is_file():
            target.unlink()
        elif path_present(target):
            raise UpgradeError(f"cannot restore upgrade destination path: {raw_path}")
        if state.kind == "absent":
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if state.kind == "file" and isinstance(state.content, bytes) and state.mode is not None:
            target.write_bytes(state.content)
            target.chmod(state.mode)
        elif state.kind == "symlink" and isinstance(state.content, str):
            target.symlink_to(state.content)
        else:  # pragma: no cover - capture_paths constructs only valid states
            raise UpgradeError(f"invalid saved upgrade destination state: {raw_path}")


def build_plan(repo: Path, source: Path) -> dict:
    local = version(repo)
    remote = version(source / "components" / "agent-core")
    local_number = version_number(repo)
    source_core = source / "components" / "agent-core"
    remote_number = version_number(source_core)
    ownership = load_ownership(repo)
    migrations = load_migrations(source_core)
    selected = [
        migration
        for migration in migrations
        if migration.to_version <= remote_number
    ]
    actions, blockers, deleted = migration_actions(repo, selected)
    if remote_number < local_number:
        blockers.append(
            f"source Agent Core VERSION {remote_number} is older than local VERSION {local_number}; downgrade is forbidden"
        )
    for path in sorted(source_core.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(source_core)
        if managed(relative):
            topology_blocker = write_topology_blocker(repo, relative, deleted)
            if topology_blocker:
                actions.append(Action(relative.as_posix(), "blocked", topology_blocker))
            elif relative in deleted:
                actions.append(Action(relative.as_posix(), "create", "reintroduced after an Agent Core migration"))
            else:
                actions.append(action_for(repo, source_core, relative, ownership))
    blockers.extend(f"{item.path}: {item.reason}" for item in actions if item.action == "blocked")
    changed = [item.path for item in actions if item.action in {"create", "replace", "merge", "delete"}]
    return {
        "currentVersion": local,
        "upstreamVersion": remote,
        "updateAvailable": bool(changed),
        "source": str(source),
        "managedPaths": [".automation/**", ".opencode/**", "AGENTS.md", "Justfile", "opencode.json"],
        "protectedRepositoryPaths": ["just/project/**", "just/local.just", ".github/workflows/**"],
        "actions": [asdict(item) for item in actions],
        "blockers": blockers,
        "canApply": not blockers,
        "readOnly": True,
    }


def require_maintenance(repo: Path) -> tuple[str, str, Path]:
    branch = run(["git", "branch", "--show-current"], cwd=repo).stdout.strip()
    if not branch:
        raise UpgradeError("detached HEAD is not supported")
    state = task_state_dir(repo) / "task.md"
    task_match = re.search(r"(?m)^- Task ID: (.+)$", state.read_text(encoding="utf-8")) if state.is_file() else None
    if not task_match:
        raise UpgradeError("upgrade requires a registered non-default Task branch")
    task_id = task_match.group(1).strip()
    identity = require_registered_task(repo, task_id)
    require_ignored_untracked(
        repo,
        (task_state_dir(repo) / "task.md", receipt_path(repo), consumed_receipt_path(repo)),
    )
    if os.environ.get("AUTOMATION_MAINTENANCE") != "1":
        raise UpgradeError("upgrade requires AUTOMATION_MAINTENANCE=1 in a dedicated Automation Maintenance Task")
    return identity


def check_update(repo: Path, source_path: Path, expected_source_revision: str | None = None) -> dict:
    source, revision = resolve_pinned_source(source_path)
    if expected_source_revision is not None:
        validate_commit_oid(source, expected_source_revision, field="expected source revision")
        if revision != expected_source_revision:
            raise UpgradeError("source HEAD does not match the expected source revision")
    with tempfile.TemporaryDirectory(prefix="automation-update-") as temporary:
        snapshot, _ = materialize_source_snapshot(source, revision, Path(temporary))
        plan = build_plan(repo, snapshot)
        revalidate_source(source, revision)
    plan["source"] = str(source)
    plan["sourceRevision"] = revision
    return plan


def apply(
    repo: Path,
    source_path: Path,
    expected_source_revision: str,
    post_apply_check=None,
    state_lock=None,
) -> dict:
    try:
        private_state.prepare(
            repo, admin=True, common_dir=common_git_dir(repo), admin_dir=worktree_admin_dir(repo)
        )
        with private_state.mutation_lock(repo, admin=True):
            if state_lock is not None:
                with state_lock():
                    return _apply_locked(
                        repo,
                        source_path,
                        expected_source_revision,
                        post_apply_check,
                    )
            return _apply_locked(repo, source_path, expected_source_revision, post_apply_check)
    except private_state.GitPrivateStateError as exc:
        raise UpgradeError(str(exc)) from exc


def _apply_locked(
    repo: Path,
    source_path: Path,
    expected_source_revision: str,
    post_apply_check=None,
) -> dict:
    task_id, branch, worktree = require_maintenance(repo)
    if os.path.lexists(receipt_path(repo)) or authority_exists(repo):
        raise UpgradeError("cannot apply with an existing receipt or authority record")
    source, revision = resolve_pinned_source(source_path)
    validate_commit_oid(source, expected_source_revision, field="expected source revision")
    if revision != expected_source_revision:
        raise UpgradeError("source HEAD does not match the expected source revision")
    with tempfile.TemporaryDirectory(prefix="automation-upgrade-") as temporary:
        snapshot, source_core = materialize_source_snapshot(source, revision, Path(temporary))
        plan = build_plan(repo, snapshot)
        if plan["blockers"]:
            raise UpgradeError("upgrade blocked:\n- " + "\n- ".join(plan["blockers"]))
        # This check is deliberately before any destination mutation, including
        # the no-change return path.
        revalidate_source(source, revision)
        actionable = [item for item in plan["actions"] if item["action"] != "noop"]
        if not actionable:
            return {
                "status": "NO_CHANGES",
                "repositoryRoot": str(repo),
                "sourceCore": str(source / "components" / "agent-core"),
                "sourceRevision": revision,
                "adapter": (repo / ".automation" / "ADAPTER").read_text(encoding="utf-8").strip(),
                "changedPaths": [],
                "commitCreated": False,
                "pushPerformed": False,
                "mergePerformed": False,
                "requiredNextChecks": [],
            }
        expected_paths = sorted({item["path"] for item in actionable})
        saved_paths = capture_paths(repo, expected_paths, anchored=True)
        consumed_path = consumed_receipt_path(repo)
        saved_consumed = capture_paths(
            repo,
            [consumed_path.relative_to(repo).as_posix()],
            anchored=True,
        )
        try:
            changed = apply_plan_to_tree(repo, source_core, plan, anchored=True)
            # A source race after mutation fails closed: no receipt is issued.
            revalidate_source(source, revision)
            changed_paths = sorted(set(changed))
            result = {
                "status": "APPLIED",
                "repositoryRoot": str(repo),
                "sourceCore": str(source / "components" / "agent-core"),
                "sourceRevision": revision,
                "adapter": (repo / ".automation" / "ADAPTER").read_text(encoding="utf-8").strip(),
                "changedPaths": changed_paths,
                "commitCreated": False,
                "pushPerformed": False,
                "mergePerformed": False,
                "requiredNextChecks": [
                    "git diff --check",
                    "just agent::doctor",
                    "just project::check",
                    "repository CI/smoke tests",
                ],
            }
            receipt = {
                "schema_version": 1,
                "status": "active",
                "task_id": task_id,
                "branch": branch,
                "worktree": str(worktree),
                "source": str(source),
                "source_revision": revision,
                "current_version": plan["currentVersion"],
                "upstream_version": plan["upstreamVersion"],
                "changed_paths": changed_paths,
                "authority_head": git_head(repo),
                "authority_nonce": secrets.token_hex(32),
                "path_fingerprints": {path: file_fingerprint(repo, path) for path in changed_paths},
            }
            issue_pair(repo, receipt, lock_held=True)
            try:
                consumed_path.unlink(missing_ok=True)
            except OSError as exc:
                raise UpgradeError("cannot remove stale consumed receipt") from exc
            if post_apply_check is not None:
                post_apply_check()
            return result
        except Exception as failure:
            rollback_errors: list[str] = []
            try:
                restore_paths(repo, saved_paths, anchored=True)
            except Exception as rollback_error:
                rollback_errors.append(f"destination rollback failed: {rollback_error}")
            try:
                for publication in (receipt_path(repo), authority_path(repo)):
                    if os.path.lexists(publication):
                        if publication == authority_path(repo):
                            private_state.unlink(publication, _lock_held=True)
                        else:
                            publication.unlink()
            except Exception as rollback_error:
                rollback_errors.append(f"publication rollback failed: {rollback_error}")
            try:
                restore_paths(repo, saved_consumed, anchored=True)
            except Exception as rollback_error:
                rollback_errors.append(f"stale receipt rollback failed: {rollback_error}")
            if rollback_errors:
                raise UpgradeError(
                    f"upgrade failed and rollback failed: {'; '.join(rollback_errors)}"
                ) from failure
            raise


def pending_paths(repo: Path) -> list[str]:
    paths: set[str] = set()
    for args in (
        ("diff", "--no-ext-diff", "--name-only", "-z"),
        ("diff", "--no-ext-diff", "--cached", "--name-only", "-z"),
        ("ls-files", "--others", "--exclude-standard", "-z"),
    ):
        output = git_bytes(["git", *args], cwd=repo)
        for raw in output.split(b"\0"):
            if not raw:
                continue
            try:
                paths.add(raw.decode("utf-8"))
            except UnicodeDecodeError as exc:
                raise UpgradeError("Git reported a non-UTF-8 pending path") from exc
    return sorted(paths)


def bootstrap_fingerprint(tree: Path, raw_path: str) -> dict[str, object]:
    return file_fingerprint(tree, raw_path)


def reject_bootstrap_path(repo: Path, raw_path: str) -> None:
    path = validate_receipt_path(raw_path)
    relative = Path(*path.split("/"))
    policy_path = repo / ".automation" / "policy.toml"
    policy = tomllib.loads(policy_path.read_text(encoding="utf-8")) if policy_path.is_file() else {}
    secret_patterns = policy.get("paths", {}).get("secret_patterns", [])
    if path == ".task-state" or path.startswith(".task-state/"):
        raise UpgradeError(f"pending path is Task State: {path}")
    if any(token.lower() in path.lower() for token in secret_patterns):
        raise UpgradeError(f"pending path matches a configured secret pattern: {path}")
    if path.startswith("just/project/") or path == "just/local.just":
        raise UpgradeError(f"pending path is repository-owned: {path}")
    if path.startswith(".github/"):
        raise UpgradeError(f"pending path is repository-owned: {path}")
    if path == ".automation/ADAPTER" or path == ".automation/INIT.fragment.md" or path == ".automation/adoption.toml":
        raise UpgradeError(f"pending path is Adapter-owned: {path}")
    if not managed(relative):
        raise UpgradeError(f"pending path is outside Agent Core: {path}")


def bootstrap_receipt(repo: Path, source_path: Path, expected_source_revision: str) -> dict:
    task_id, branch, worktree = require_maintenance(repo)
    authority_head = git_head(repo)
    authority_branch_oid = run(["git", "rev-parse", f"refs/heads/{branch}"], cwd=repo).stdout.strip()
    existing: dict | None = None
    existing_bytes: bytes | None = None
    if os.path.lexists(receipt_path(repo)):
        try:
            existing_bytes = receipt_path(repo).read_bytes()
            existing = validate_receipt_schema(json.loads(existing_bytes.decode("utf-8")))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise UpgradeError("invalid automation upgrade receipt") from exc
        if (existing["task_id"], existing["branch"], existing["worktree"]) != (task_id, branch, str(worktree)):
            raise UpgradeError("automation receipt identity does not match the current Task worktree")
        if authority_exists(repo):
            raise UpgradeError("cannot bootstrap with an existing receipt or authority record")
    elif authority_exists(repo):
        raise UpgradeError("cannot bootstrap with an existing receipt or authority record")
    pending = pending_paths(repo)
    if not pending and existing is None:
        raise UpgradeError("bootstrap requires non-empty pending paths")
    for path in pending:
        reject_bootstrap_path(repo, path)
    source, source_head = resolve_pinned_source(source_path)
    validate_commit_oid(source, expected_source_revision, field="expected source revision")
    if source_head != expected_source_revision:
        raise UpgradeError("source HEAD does not match the expected source revision")
    source_head = expected_source_revision
    if existing is not None:
        if str(source) != existing["source"] or source_head != existing["source_revision"]:
            raise UpgradeError("receipt source or revision is stale")
        pending = receipt_paths(repo, existing)
    state_dir = task_state_dir(repo)
    require_ignored_untracked(repo, (state_dir / "automation-bootstrap.tmp",))
    with tempfile.TemporaryDirectory(prefix="automation-bootstrap-", dir=state_dir) as temporary:
        baseline = Path(temporary) / "baseline"
        source_snapshot = Path(temporary) / "source" / "components" / "agent-core"
        materialize_tree(repo, authority_head, baseline, surface_only=True)
        materialize_source_snapshot(source, source_head, Path(temporary))
        before = {
            p.relative_to(baseline).as_posix(): bootstrap_fingerprint(baseline, p.relative_to(baseline).as_posix())
            for p in baseline.rglob("*") if p.is_file()
        }
        plan = build_plan(baseline, source_snapshot.parent.parent)
        if plan["blockers"]:
            raise UpgradeError("upgrade blocked:\n- " + "\n- ".join(plan["blockers"]))
        selected = [
            migration for migration in load_migrations(source_snapshot)
            if migration.to_version <= version_number(source_snapshot)
        ]
        _, migration_blockers, _ = migration_actions(repo, selected)
        if migration_blockers:
            raise UpgradeError("upgrade blocked:\n- " + "\n- ".join(migration_blockers))
        apply_plan_to_tree(baseline, source_snapshot, plan)
        returned = set(item["path"] for item in plan["actions"] if item["action"] != "noop")
        expected = sorted(
            path for path in returned
            if before.get(path, {"state": "absent", "mode": None, "content_sha256": None})
            != bootstrap_fingerprint(baseline, path)
        )
        if expected != pending:
            raise UpgradeError("pending paths do not exactly match the reconstructed upgrade")
        if not expected:
            revalidate_source(source, source_head)
            return {"status": "NO_CHANGES", "changedPaths": [], "authorityIssued": False}
        expected_fingerprints = {path: bootstrap_fingerprint(baseline, path) for path in expected}
    revalidate_source(source, source_head)
    current_identity = require_registered_task(repo, task_id)
    current_registration = registered_worktrees(repo).get(worktree)
    if (
        current_identity != (task_id, branch, worktree)
        or current_registration != (branch, authority_head)
        or git_head(repo) != authority_head
        or run(["git", "rev-parse", f"refs/heads/{branch}"], cwd=repo).stdout.strip() != authority_branch_oid
    ):
        raise UpgradeError("Task identity or HEAD changed during receipt reconstruction")
    if pending_paths(repo) != pending:
        raise UpgradeError("pending paths changed during receipt reconstruction")
    current_fingerprints = {path: file_fingerprint(repo, path) for path in expected}
    if current_fingerprints != expected_fingerprints:
        raise UpgradeError("pending Agent Core content does not match the reconstructed upgrade")
    receipt = {
        "schema_version": 1, "status": "active", "task_id": task_id, "branch": branch,
        "worktree": str(worktree), "source": str(source.resolve()), "source_revision": source_head,
        "current_version": plan["currentVersion"], "upstream_version": plan["upstreamVersion"],
        "changed_paths": expected, "authority_head": authority_head, "authority_nonce": secrets.token_hex(32),
        "path_fingerprints": current_fingerprints,
    }
    revalidate_source(source, source_head)
    current_identity = require_registered_task(repo, task_id)
    current_registration = registered_worktrees(repo).get(worktree)
    if (
        current_identity != (task_id, branch, worktree)
        or current_registration != (branch, authority_head)
        or git_head(repo) != authority_head
        or run(["git", "rev-parse", f"refs/heads/{branch}"], cwd=repo).stdout.strip() != authority_branch_oid
    ):
        raise UpgradeError("Task identity or HEAD changed before authority publication")
    if pending_paths(repo) != pending:
        raise UpgradeError("pending paths changed before authority publication")
    current_fingerprints = {path: file_fingerprint(repo, path) for path in expected}
    if current_fingerprints != expected_fingerprints:
        raise UpgradeError("pending Agent Core content changed before authority publication")
    if existing is not None:
        receipt["authority_nonce"] = existing["authority_nonce"]
        receipt["path_fingerprints"] = current_fingerprints
        if receipt != existing:
            raise UpgradeError("existing receipt does not match the reconstructed upgrade")
        if receipt_path(repo).read_bytes() != existing_bytes or authority_exists(repo):
            raise UpgradeError("receipt or authority changed during authority recovery")
        _write_authority_at(authority_path(repo), {
            "schema_version": 1,
            "task_id": receipt["task_id"],
            "branch": receipt["branch"],
            "worktree": receipt["worktree"],
            "authority_nonce": receipt["authority_nonce"],
            "receipt_sha256": receipt_digest(receipt),
        }, admin=worktree_admin_dir(repo))
        try:
            consumed_receipt_path(repo).unlink(missing_ok=True)
        except OSError as exc:
            raise UpgradeError("cannot remove stale consumed receipt") from exc
        return {"status": "AUTHORITY_RECOVERED", "changedPaths": expected, "authorityIssued": True}
    revalidate_source(source, source_head)
    issue_pair(repo, receipt)
    try:
        consumed_receipt_path(repo).unlink(missing_ok=True)
    except OSError as exc:
        raise UpgradeError("cannot remove stale consumed receipt") from exc
    return {"status": "RECEIPT_BOOTSTRAPPED", "changedPaths": expected, "authorityIssued": True}


def _standard_authority_only(repo: Path, receipt: dict) -> Path:
    new_path, legacy_path = _authority_locations(repo)
    locations = [path for path in (new_path, legacy_path) if path is not None and os.path.lexists(path)]
    if len(locations) != 1:
        raise UpgradeError("missing or ambiguous successful-upgrade authority record")
    if os.path.lexists(source_recovery_proof_path(repo)):
        raise UpgradeError("cannot rebind source-recovery provenance")
    authority = validate_authority(repo, receipt)
    if authority != locations[0]:
        raise UpgradeError("missing or invalid successful-upgrade authority record")
    return authority


def _reconstruct_pending(repo: Path, source: Path, revision: str, authority_head: str) -> tuple[list[str], dict[str, object], tuple[str, str]]:
    """Reconstruct an upgrade solely from immutable trees, never from a live source tree."""
    with tempfile.TemporaryDirectory(prefix="automation-rebind-") as temporary:
        baseline = Path(temporary) / "baseline"
        snapshot, source_core = materialize_source_snapshot(source, revision, Path(temporary))
        materialize_tree(repo, authority_head, baseline, surface_only=True)
        before = {
            path.relative_to(baseline).as_posix(): bootstrap_fingerprint(baseline, path.relative_to(baseline).as_posix())
            for path in baseline.rglob("*") if path.is_file()
        }
        plan = build_plan(baseline, snapshot)
        if plan["blockers"]:
            raise UpgradeError("reconstruction blocked:\n- " + "\n- ".join(plan["blockers"]))
        selected = [migration for migration in load_migrations(source_core)
                    if migration.to_version <= version_number(source_core)]
        _, migration_blockers, _ = migration_actions(repo, selected)
        if migration_blockers:
            raise UpgradeError("reconstruction blocked:\n- " + "\n- ".join(migration_blockers))
        returned = {item["path"] for item in plan["actions"] if item["action"] != "noop"}
        apply_plan_to_tree(baseline, source_core, plan)
        paths = sorted(path for path in returned if before.get(path, {"state": "absent", "mode": None, "content_sha256": None})
                       != bootstrap_fingerprint(baseline, path))
        return paths, {path: bootstrap_fingerprint(baseline, path) for path in paths}, (plan["currentVersion"], plan["upstreamVersion"])


def _rename_record_at(source: str, destination: str, *, source_dir_fd: int, destination_dir_fd: int) -> None:
    """Narrow test seam for the dirfd-relative atomic publication boundary."""
    os.rename(
        source,
        destination,
        src_dir_fd=source_dir_fd,
        dst_dir_fd=destination_dir_fd,
    )


def _replace_standard_pair_locked(repo: Path, old_receipt_raw: bytes, old_authority_raw: bytes,
                                  receipt: dict, authority_path_value: Path,
                                  receipt_identity: tuple[int, int],
                                  authority_identity: tuple[int, int]) -> None:
    active = receipt_path(repo)
    admin = _rollback_authority_guard(repo, authority_path_value)
    _validate_admin_record_parent(authority_path_value, admin)
    if not hasattr(os, "O_DIRECTORY") or not hasattr(os, "O_NOFOLLOW") \
             or any(operation not in getattr(os, "supports_dir_fd", set())
                    for operation in (os.open, os.rename, os.unlink)):
        raise UpgradeError("secure dirfd-relative record replacement is unavailable")
    originals = {active: (old_receipt_raw, receipt_identity),
                 authority_path_value: (old_authority_raw, authority_identity)}
    new_instances: dict[Path, tuple[bytes, tuple[int, int]]] = {}
    handles: dict[Path, tuple[int, str]] = {}

    def open_parent(path: Path) -> tuple[int, str]:
        parent = path.parent
        expected = os.stat(parent, follow_symlinks=False)
        if not stat.S_ISDIR(expected.st_mode):
            raise UpgradeError("record parent is not a directory")
        if not parent.is_absolute():
            raise UpgradeError("record parent is not absolute")
        flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
        fd = os.open(os.sep, flags)
        try:
            for component in parent.parts[1:]:
                next_fd = os.open(component, flags, dir_fd=fd)
                os.close(fd)
                fd = next_fd
        except BaseException:
            os.close(fd)
            raise
        opened = os.fstat(fd)
        if (opened.st_dev, opened.st_ino) != (expected.st_dev, expected.st_ino):
            os.close(fd)
            raise UpgradeError("record parent changed during secure open")
        return fd, path.name

    def read_at(handle: tuple[int, str]) -> tuple[bytes, tuple[int, int]]:
        fd, name = handle
        record_fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=fd)
        try:
            metadata = os.fstat(record_fd)
            if not stat.S_ISREG(metadata.st_mode):
                raise UpgradeError("record is not a regular file")
            chunks: list[bytes] = []
            while True:
                chunk = os.read(record_fd, 65536)
                if not chunk:
                    break
                chunks.append(chunk)
            return b"".join(chunks), (metadata.st_dev, metadata.st_ino)
        finally:
            os.close(record_fd)

    def write_all(fd: int, content: bytes) -> None:
        offset = 0
        while offset < len(content):
            written = os.write(fd, content[offset:])
            if written <= 0:
                raise OSError("short secure record write")
            offset += written

    def parent_unchanged(handle: tuple[int, str], path: Path) -> bool:
        opened = os.fstat(handle[0])
        try:
            current = os.stat(path.parent, follow_symlinks=False)
        except OSError:
            return False
        return stat.S_ISDIR(current.st_mode) and (current.st_dev, current.st_ino) == (opened.st_dev, opened.st_ino)

    def replace(path: Path, content: bytes) -> None:
        expected_bytes, expected_identity = originals[path]
        handle = handles[path]
        if read_at(handle) != (expected_bytes, expected_identity):
            raise UpgradeError("receipt or authority changed before provenance replacement")
        fd, name = handle
        temporary = f".{name}.replace-{secrets.token_hex(16)}"
        temporary_fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=fd)
        published = False
        try:
            try:
                write_all(temporary_fd, content)
                os.fsync(temporary_fd)
            finally:
                os.close(temporary_fd)
            _rename_record_at(temporary, name, source_dir_fd=fd, destination_dir_fd=fd)
            published = True
        finally:
            if not published:
                try:
                    os.unlink(temporary, dir_fd=fd)
                except FileNotFoundError:
                    pass
        created = read_at(handle)
        if created[0] != content:
            raise UpgradeError("new record failed postvalidation")
        new_instances[path] = created

    try:
        for path, (old_bytes, old_identity) in originals.items():
            handles[path] = open_parent(path)
            if read_at(handles[path]) != (old_bytes, old_identity):
                raise UpgradeError("receipt or authority changed before provenance replacement")
        receipt_bytes = (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode("utf-8")
        replace(active, receipt_bytes)
        authority_bytes = (json.dumps({
            "schema_version": 1, "task_id": receipt["task_id"], "branch": receipt["branch"],
            "worktree": receipt["worktree"], "authority_nonce": receipt["authority_nonce"],
            "receipt_sha256": receipt_digest(receipt),
        }, indent=2, sort_keys=True) + "\n").encode("utf-8")
        replace(authority_path_value, authority_bytes)
        try:
            receipt_record = validate_receipt_schema(json.loads(receipt_bytes.decode("utf-8")))
            authority_record = json.loads(authority_bytes.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:  # pragma: no cover - generated above
            raise UpgradeError("rebound receipt or authority is not valid JSON") from exc
        expected_authority = {
            "schema_version": 1,
            "task_id": receipt["task_id"],
            "branch": receipt["branch"],
            "worktree": receipt["worktree"],
            "authority_nonce": receipt["authority_nonce"],
            "receipt_sha256": receipt_digest(receipt),
        }
        if (
            read_at(handles[active]) != new_instances[active]
            or read_at(handles[authority_path_value]) != new_instances[authority_path_value]
            or receipt_record != receipt
            or authority_record != expected_authority
            # These are deliberately the last checks before success. Semantic
            # validation above reads only through retained directory FDs.
            or not parent_unchanged(handles[active], active)
            or not parent_unchanged(handles[authority_path_value], authority_path_value)
        ):
            raise UpgradeError("rebound receipt or authority failed post-publication validation")
    except (OSError, UpgradeError) as failure:
        rollback_errors: list[str] = []
        for path, (old_bytes, old_identity) in originals.items():
            try:
                if path in new_instances:
                    new_bytes, new_identity = new_instances[path]
                    if read_at(handles[path]) != (new_bytes, new_identity):
                        raise UpgradeError("unknown concurrent record preserved")
                    replace_old = f".{path.name}.rollback-{secrets.token_hex(16)}"
                    replace_old_fd = os.open(replace_old, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=handles[path][0])
                    rollback_published = False
                    try:
                        try:
                            write_all(replace_old_fd, old_bytes)
                            os.fsync(replace_old_fd)
                        finally:
                            os.close(replace_old_fd)
                        _rename_record_at(
                            replace_old,
                            handles[path][1],
                            source_dir_fd=handles[path][0],
                            destination_dir_fd=handles[path][0],
                        )
                        rollback_published = True
                    finally:
                        if not rollback_published:
                            try:
                                os.unlink(replace_old, dir_fd=handles[path][0])
                            except FileNotFoundError:
                                pass
                elif read_at(handles[path]) != (old_bytes, old_identity):
                    raise UpgradeError("unknown concurrent record preserved")
            except (OSError, UpgradeError) as rollback:
                rollback_errors.append(f"{path}: {rollback}")
        if rollback_errors:
            raise UpgradeError("provenance replacement failed and rollback failed: " + "; ".join(rollback_errors)) from failure
        raise
    finally:
        for fd, _ in handles.values():
            try:
                os.close(fd)
            except OSError:
                pass


def _replace_standard_pair(repo: Path, old_receipt_raw: bytes, old_authority_raw: bytes,
                           receipt: dict, authority_path_value: Path,
                           receipt_identity: tuple[int, int], authority_identity: tuple[int, int]) -> None:
    """Replace a common/admin record pair under ordered namespace fences."""
    try:
        with private_state.mutation_lock(repo, admin=True):
            _replace_standard_pair_locked(
                repo,
                old_receipt_raw,
                old_authority_raw,
                receipt,
                authority_path_value,
                receipt_identity,
                authority_identity,
            )
    except private_state.GitPrivateStateError as exc:
        raise UpgradeError(str(exc)) from exc


def _rebind_maintenance_provenance(repo: Path, source_path: Path, expected_source_revision: str,
                                   *, require_source_head: bool,
                                   expected_implementation_revision: str | None = None) -> dict:
    """Replace only a valid standard receipt/authority pair with new provenance."""
    task_id, branch, worktree = require_maintenance(repo)
    source, current_source_revision = resolve_pinned_source(source_path)
    validate_commit_oid(source, expected_source_revision, field="expected source revision")
    if require_source_head and current_source_revision != expected_source_revision:
        raise UpgradeError("source HEAD does not match the expected source revision")
    active = receipt_path(repo)
    raw_receipt, old = _read_json_record_bytes(active, "invalid automation upgrade receipt")
    old = validate_receipt_schema(old)
    old_paths_from_receipt = receipt_paths(repo, old)
    fingerprints = old.get("path_fingerprints")
    if not isinstance(fingerprints, dict) or set(fingerprints) != set(old_paths_from_receipt):
        raise UpgradeError("automation receipt fingerprints do not match its paths")
    if any(file_fingerprint(repo, path) != fingerprints[path] for path in old_paths_from_receipt):
        raise UpgradeError("automation receipt path content/state fingerprint changed")
    if old["source_revision"] is None or not _source_is_compatible(old["source"], source):
        raise UpgradeError("receipt source is not compatible with the current Templates source")
    old_revision = _validate_source_revision(source, old["source_revision"])
    authority = _standard_authority_only(repo, old)
    old_authority_raw = private_state.read_bytes(authority, "maintenance authority")
    receipt_identity = (active.lstat().st_dev, active.lstat().st_ino)
    authority_identity = (authority.lstat().st_dev, authority.lstat().st_ino)
    authority_head = validate_commit_oid(repo, old["authority_head"], field="receipt authority HEAD")
    if os.path.lexists(consumed_receipt_path(repo)):
        raise UpgradeError("cannot rebind a consumed automation receipt")
    if (old["task_id"], old["branch"], old["worktree"]) != (task_id, branch, str(worktree)):
        raise UpgradeError("automation receipt identity does not match the current Task worktree")
    if git_head(repo) != authority_head or pending_paths(repo) != old["changed_paths"]:
        raise UpgradeError("maintenance worktree changed since provenance was issued")
    authority_branch_oid = run(["git", "rev-parse", f"refs/heads/{branch}"], cwd=repo).stdout.strip()
    authority_registration = registered_worktrees(repo).get(worktree)
    if authority_branch_oid != authority_head or authority_registration != (branch, authority_head):
        raise UpgradeError("Task branch registration does not match the receipt authority")
    old_paths, old_fingerprints, old_versions = _reconstruct_pending(repo, source, old_revision, authority_head)
    if old_paths != old["changed_paths"] or old_fingerprints != old["path_fingerprints"]:
        raise UpgradeError("receipt does not validate against its immutable source reconstruction")
    if old_versions != (old["current_version"], old["upstream_version"]):
        raise UpgradeError("receipt versions do not validate against its immutable source reconstruction")
    expected_paths, expected_fingerprints, expected_versions = _reconstruct_pending(repo, source, expected_source_revision, authority_head)
    if expected_paths != old_paths or any(file_fingerprint(repo, path) != expected_fingerprints[path] for path in expected_paths):
        raise UpgradeError("expected provenance does not produce the existing pending content")
    candidate = dict(old)
    candidate.update({"source": str(source), "source_revision": expected_source_revision,
                      "current_version": expected_versions[0], "upstream_version": expected_versions[1],
                      "authority_nonce": secrets.token_hex(32), "path_fingerprints": expected_fingerprints})
    # Revalidate all protected state immediately before publication.
    if require_source_head:
        current_source, current_revision = resolve_pinned_source(source)
    else:
        current_source, current_revision = resolve_clean_source_worktree(source)
        if expected_implementation_revision is None or current_revision != expected_implementation_revision:
            raise UpgradeError("source implementation changed before provenance replacement")
    if active.read_bytes() != raw_receipt or private_state.read_bytes(authority, "maintenance authority") != old_authority_raw or pending_paths(repo) != expected_paths \
            or git_head(repo) != authority_head or current_source != source \
            or (require_source_head and current_revision != expected_source_revision) \
            or (active.lstat().st_dev, active.lstat().st_ino) != receipt_identity \
            or (authority.lstat().st_dev, authority.lstat().st_ino) != authority_identity \
            or run(["git", "rev-parse", f"refs/heads/{branch}"], cwd=repo).stdout.strip() != authority_branch_oid \
            or registered_worktrees(repo).get(worktree) != authority_registration \
            or validate_commit_oid(source, expected_source_revision, field="expected source revision") != expected_source_revision:
        raise UpgradeError("receipt, authority, source, or pending content changed before provenance replacement")
    if any(file_fingerprint(repo, path) != expected_fingerprints[path] for path in expected_paths):
        raise UpgradeError("pending Agent Core content changed before provenance replacement")
    if old["source_revision"] == expected_source_revision and old_versions == expected_versions:
        return {"status": "PROVENANCE_ALREADY_BOUND", "changedPaths": expected_paths, "sourceRevision": expected_source_revision}
    _replace_standard_pair(repo, raw_receipt, old_authority_raw, candidate, authority,
                           receipt_identity, authority_identity)
    return {"status": "PROVENANCE_REBOUND", "changedPaths": expected_paths, "sourceRevision": expected_source_revision}


def rebind_maintenance_provenance(repo: Path, source_path: Path, expected_source_revision: str) -> dict:
    return _rebind_maintenance_provenance(repo, source_path, expected_source_revision, require_source_head=True)


def rebind_maintenance_provenance_from_source(
    target_repo: Path,
    templates_source_root: Path,
    expected_source_revision: str,
    *,
    expected_implementation_revision: str,
) -> dict:
    """Source-side bridge: validate the live implementation and retain historical source objects."""
    source, implementation_revision = resolve_clean_source_worktree(templates_source_root)
    validate_commit_oid(source, expected_implementation_revision, field="expected implementation revision")
    if implementation_revision != expected_implementation_revision:
        raise UpgradeError("source HEAD does not match the expected implementation revision")
    validate_commit_oid(source, expected_source_revision, field="expected source revision")
    return _rebind_maintenance_provenance(
        target_repo, source, expected_source_revision, require_source_head=False,
        expected_implementation_revision=expected_implementation_revision,
    )


PROOF_FIELDS = {
    "schema_version", "kind", "task_id", "branch", "worktree", "authority_head",
    "authority_nonce", "receipt_sha256", "receipt_bytes_sha256", "changed_paths_sha256",
    "path_fingerprints_sha256", "implementation_source", "implementation_revision",
    "receipt_source", "receipt_source_revision",
}


def _recovery_proof(receipt: dict, *, implementation_source: Path, implementation_revision: str,
                    raw_receipt: bytes, paths: list[str], fingerprints: dict) -> dict:
    return {
        "schema_version": 1,
        "kind": "source-recovery-proof",
        "task_id": receipt["task_id"],
        "branch": receipt["branch"],
        "worktree": receipt["worktree"],
        "authority_head": receipt["authority_head"],
        "authority_nonce": receipt["authority_nonce"],
        "receipt_sha256": receipt_digest(receipt),
        "receipt_bytes_sha256": hashlib.sha256(raw_receipt).hexdigest(),
        "changed_paths_sha256": hashlib.sha256(canonical_json({"changed_paths": paths})).hexdigest(),
        "path_fingerprints_sha256": hashlib.sha256(canonical_json(fingerprints)).hexdigest(),
        "implementation_source": str(implementation_source),
        "implementation_revision": implementation_revision,
        "receipt_source": receipt["source"],
        "receipt_source_revision": receipt["source_revision"],
    }


def _validate_recovery_proof(proof: object) -> dict:
    if not isinstance(proof, dict) or set(proof) != PROOF_FIELDS or proof.get("schema_version") != 1 \
            or proof.get("kind") != "source-recovery-proof":
        raise UpgradeError("missing or invalid source-recovery proof")
    for key in ("task_id", "branch", "worktree", "authority_head", "authority_nonce",
                "receipt_sha256", "receipt_bytes_sha256", "changed_paths_sha256",
                "path_fingerprints_sha256", "implementation_source", "implementation_revision",
                "receipt_source", "receipt_source_revision"):
        if not isinstance(proof[key], str) or not proof[key]:
            raise UpgradeError("missing or invalid source-recovery proof")
    if FULL_OID_RE.fullmatch(proof["implementation_revision"]) is None:
        raise UpgradeError("invalid source-recovery implementation revision")
    for key in ("receipt_sha256", "receipt_bytes_sha256", "changed_paths_sha256", "path_fingerprints_sha256"):
        if not re.fullmatch(r"[0-9a-f]{64}", proof[key]):
            raise UpgradeError("invalid source-recovery proof digest")
    return proof


def _validate_recovery_records(repo: Path, receipt: dict, raw_receipt: bytes, proof: dict) -> Path:
    expected = _recovery_proof(
        receipt,
        implementation_source=Path(proof["implementation_source"]),
        implementation_revision=proof["implementation_revision"],
        raw_receipt=raw_receipt,
        paths=receipt["changed_paths"],
        fingerprints=receipt["path_fingerprints"],
    )
    if proof != expected:
        raise UpgradeError("source-recovery proof does not match the reconstructed receipt")
    authority = _read_json_record(_bridge_authority_path(repo), "missing or invalid source-recovery authority")
    if authority != _bridge_authority(receipt, hashlib.sha256(canonical_json(proof)).hexdigest()):
        raise UpgradeError("source-recovery authority does not match its proof")
    return _bridge_authority_path(repo)


def recover_maintenance_authority_from_source(
    target_repo: Path,
    templates_source_root: Path,
    *,
    expected_implementation_revision: str,
) -> dict:
    """Reconstruct and publish the source-recovery proof/bridge without touching target files."""
    expected_implementation_revision = _validate_expected_implementation_revision(
        expected_implementation_revision
    )
    task_id, branch, worktree = require_maintenance(target_repo)
    try:
        private_state.prepare(
            target_repo,
            admin=True,
            common_dir=common_git_dir(target_repo),
            admin_dir=worktree_admin_dir(target_repo),
            _allow_incomplete_recovery=True,
        )
    except private_state.GitPrivateStateError as exc:
        raise UpgradeError(str(exc)) from exc
    source, implementation_revision = resolve_clean_source_worktree(templates_source_root)
    if implementation_revision != expected_implementation_revision:
        raise UpgradeError("source HEAD does not match the expected implementation revision")
    active = receipt_path(target_repo)
    if not active.is_file():
        raise UpgradeError("no active automation upgrade receipt")
    try:
        raw_receipt = active.read_bytes()
        receipt = validate_receipt_schema(json.loads(raw_receipt.decode("utf-8")))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise UpgradeError("invalid automation upgrade receipt") from exc
    receipt_scope = receipt_paths(target_repo, receipt)
    if receipt["source_revision"] is None or not _source_is_compatible(receipt["source"], source):
        raise UpgradeError("receipt source is not compatible with the current Templates source")
    receipt_revision = _validate_source_revision(source, receipt["source_revision"])
    if (receipt["task_id"], receipt["branch"], receipt["worktree"]) != (task_id, branch, str(worktree)):
        raise UpgradeError("automation receipt identity does not match the current Task worktree")
    _validate_no_authority(target_repo)
    authority_head = receipt["authority_head"]
    authority_branch_oid = run(["git", "rev-parse", f"refs/heads/{branch}"], cwd=target_repo).stdout.strip()
    authority_registration = registered_worktrees(target_repo).get(worktree)
    pending_before = pending_paths(target_repo)
    for pending in pending_before:
        reject_bootstrap_path(target_repo, pending)
    with tempfile.TemporaryDirectory(prefix="automation-source-recovery-", dir=task_state_dir(target_repo)) as temporary:
        baseline = Path(temporary) / "baseline"
        snapshot, source_core = materialize_source_snapshot(source, receipt_revision, Path(temporary))
        materialize_tree(target_repo, authority_head, baseline, surface_only=True)
        before = {
            path.relative_to(baseline).as_posix(): bootstrap_fingerprint(baseline, path.relative_to(baseline).as_posix())
            for path in baseline.rglob("*") if path.is_file()
        }
        plan = build_plan(baseline, snapshot)
        if plan["blockers"]:
            raise UpgradeError("reconstruction blocked:\n- " + "\n- ".join(plan["blockers"]))
        selected = [migration for migration in load_migrations(source_core)
                    if migration.to_version <= version_number(source_core)]
        _, migration_blockers, _ = migration_actions(target_repo, selected)
        if migration_blockers:
            raise UpgradeError("reconstruction blocked:\n- " + "\n- ".join(migration_blockers))
        returned = {item["path"] for item in plan["actions"] if item["action"] != "noop"}
        apply_plan_to_tree(baseline, source_core, plan)
        expected = sorted(path for path in returned
                          if before.get(path, {"state": "absent", "mode": None, "content_sha256": None})
                          != bootstrap_fingerprint(baseline, path))
        expected_fingerprints = {path: bootstrap_fingerprint(baseline, path) for path in expected}
        candidate_versions = (plan["currentVersion"], plan["upstreamVersion"])
    if pending_before != expected or expected != receipt_scope \
            or any(file_fingerprint(target_repo, p) != expected_fingerprints[p] for p in expected):
        raise UpgradeError("pending target paths do not match the reconstructed upgrade")
    if git_head(target_repo) != authority_head or authority_branch_oid != authority_head \
            or require_registered_task(target_repo, task_id) != (task_id, branch, worktree) \
            or registered_worktrees(target_repo).get(worktree) != authority_registration:
        raise UpgradeError("Task identity or HEAD changed during source recovery")
    actual_fingerprints = {p: file_fingerprint(target_repo, p) for p in expected}
    candidate = {
        "schema_version": 1, "status": "active", "task_id": task_id, "branch": branch,
        "worktree": str(worktree), "source": receipt["source"], "source_revision": receipt_revision,
        "current_version": candidate_versions[0], "upstream_version": candidate_versions[1],
        "changed_paths": receipt_scope, "authority_head": authority_head,
        "authority_nonce": receipt["authority_nonce"], "path_fingerprints": actual_fingerprints,
    }
    if active.read_bytes() != raw_receipt or candidate != receipt:
        raise UpgradeError("receipt or target changed during source recovery")
    proof = _recovery_proof(receipt, implementation_source=source,
                            implementation_revision=expected_implementation_revision,
                            raw_receipt=raw_receipt, paths=expected, fingerprints=actual_fingerprints)
    proof_path = source_recovery_proof_path(target_repo)
    created_proof: tuple[int, int] | None = None
    try:
        proof_admin = worktree_admin_dir(target_repo)
        _validate_admin_record_parent(proof_path, proof_admin)
        if os.path.lexists(proof_path):
            existing = _validate_recovery_proof(_read_json_record(proof_path, "missing or invalid source-recovery proof"))
            if existing != proof:
                raise UpgradeError("existing source-recovery proof does not match reconstruction")
        else:
            private_state.ensure_parent(proof_path)
            created_proof = private_state.exclusive_write_bytes(
                proof_path,
                json.dumps(proof, indent=2, sort_keys=True).encode("utf-8") + b"\n",
            )
        current_source, current_implementation_revision = resolve_clean_source_worktree(source)
        if (current_source != source
                or current_implementation_revision != expected_implementation_revision):
            raise UpgradeError("source changed before source-recovery publication")
        if active.read_bytes() != raw_receipt or authority_exists(target_repo):
            raise UpgradeError("receipt or authority changed before source-recovery bridge publication")
        if (git_head(target_repo) != authority_head
                or run(["git", "rev-parse", f"refs/heads/{branch}"], cwd=target_repo).stdout.strip() != authority_branch_oid
                or require_registered_task(target_repo, task_id) != (task_id, branch, worktree)
                or registered_worktrees(target_repo).get(worktree) != authority_registration
                or pending_paths(target_repo) != pending_before
                or receipt_paths(target_repo, receipt) != receipt_scope
                or any(file_fingerprint(target_repo, p) != expected_fingerprints[p] for p in expected)):
            raise UpgradeError("target changed before source-recovery bridge publication")
        if _validate_recovery_proof(_read_json_record(proof_path, "missing or invalid source-recovery proof")) != proof:
            raise UpgradeError("source-recovery proof changed before bridge publication")
        _write_authority_at(_bridge_authority_path(target_repo), _bridge_authority(receipt, hashlib.sha256(canonical_json(proof)).hexdigest()),
                            admin=worktree_admin_dir(target_repo))
    except (OSError, UpgradeError, private_state.GitPrivateStateError):
        if created_proof is not None:
            try:
                private_state.unlink(proof_path, expected_identity=created_proof)
            except (OSError, private_state.GitPrivateStateError) as exc:
                raise UpgradeError("source-recovery publication failed and proof rollback failed") from exc
        raise
    return {"status": "AUTHORITY_RECOVERED", "changedPaths": expected,
            "implementationRevision": expected_implementation_revision, "receiptSourceRevision": receipt_revision}


def validate_receipt_path(raw: object) -> str:
    if not isinstance(raw, str) or not raw or "\\" in raw:
        raise UpgradeError("receipt contains an unsafe path")
    pure = PurePosixPath(raw)
    if pure.is_absolute() or any(part in {"", ".", ".."} for part in pure.parts) or pure.as_posix() != raw:
        raise UpgradeError(f"receipt contains an unnormalized path: {raw!r}")
    return raw


def receipt_paths(repo: Path, receipt: dict) -> list[str]:
    raw = receipt.get("changed_paths")
    if not isinstance(raw, list) or not raw:
        raise UpgradeError("automation receipt has no changed paths")
    paths = [validate_receipt_path(item) for item in raw]
    if paths != sorted(paths) or len(paths) != len(set(paths)):
        raise UpgradeError("automation receipt paths must be sorted and unique")
    secret_file = repo / ".automation" / "policy.toml"
    policy = tomllib.loads(secret_file.read_text(encoding="utf-8")) if secret_file.is_file() else {}
    secrets = policy.get("paths", {}).get("secret_patterns", [])
    for path in paths:
        relative = Path(*path.split("/"))
        if path == ".task-state" or path.startswith(".task-state/") or not managed(relative):
            raise UpgradeError(f"receipt path is not Agent Core managed: {path}")
        if any(token.lower() in path.lower() for token in secrets):
            raise UpgradeError(f"receipt path matches a configured secret pattern: {path}")
    return paths


def validate_receipt_schema(receipt: object) -> dict:
    if not isinstance(receipt, dict) or set(receipt) != RECEIPT_FIELDS:
        raise UpgradeError("automation upgrade receipt has an invalid schema")
    if receipt.get("schema_version") != 1 or receipt.get("status") != "active":
        raise UpgradeError("unsupported automation upgrade receipt")
    required_strings = (
        "task_id",
        "branch",
        "worktree",
        "source",
        "current_version",
        "upstream_version",
        "authority_head",
        "authority_nonce",
    )
    if any(not isinstance(receipt.get(field), str) or not receipt[field] for field in required_strings):
        raise UpgradeError("automation upgrade receipt has invalid provenance fields")
    revision = receipt.get("source_revision")
    if revision is not None and (not isinstance(revision, str) or not revision):
        raise UpgradeError("automation upgrade receipt has an invalid source revision")
    if not Path(receipt["source"]).is_absolute():
        raise UpgradeError("automation upgrade receipt source must be absolute")
    if FULL_OID_RE.fullmatch(receipt["authority_head"]) is None:
        raise UpgradeError("automation upgrade receipt has an invalid authority HEAD")
    if not re.fullmatch(r"[0-9a-f]{64}", receipt["authority_nonce"]):
        raise UpgradeError("automation upgrade receipt has an invalid authority nonce")
    return receipt


def staged_fingerprint(
    repo: Path,
    path: str,
    *,
    env_overrides: dict[str, str],
) -> dict[str, object]:
    entry = run(
        ["git", "ls-files", "--stage", "--", path],
        cwd=repo,
        env_overrides=env_overrides,
    ).stdout.strip()
    if not entry:
        return {"state": "absent", "mode": None, "content_sha256": None}
    fields = entry.split(maxsplit=3)
    if len(fields) != 4 or fields[2] != "0":
        raise UpgradeError(f"staged path has an unsupported index entry: {path}")
    mode = fields[0]
    result = subprocess.run(
        trusted_git_command(["git", "show", f":{path}"]),
        cwd=repo,
        capture_output=True,
        check=False,
        env=git_environment(env_overrides),
    )
    if result.returncode != 0:
        raise UpgradeError(f"cannot read staged content for {path}")
    if mode not in {"100644", "100755"}:
        raise UpgradeError(f"staged path has an unsupported mode: {path}")
    return {
        "state": "file",
        "mode": 0o755 if mode == "100755" else 0o644,
        "content_sha256": hashlib.sha256(result.stdout).hexdigest(),
    }


def normalized_git_fingerprint(fingerprint: object) -> dict[str, object]:
    if not isinstance(fingerprint, dict):
        raise UpgradeError("automation receipt contains an invalid path fingerprint")
    state = fingerprint.get("state")
    if state == "absent":
        return {"state": "absent", "mode": None, "content_sha256": None}
    if state != "file" or not isinstance(fingerprint.get("mode"), int):
        raise UpgradeError("automation receipt authorizes an unsupported path state")
    digest = fingerprint.get("content_sha256")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise UpgradeError("automation receipt contains an invalid content fingerprint")
    return {
        "state": "file",
        "mode": 0o755 if fingerprint["mode"] & 0o111 else 0o644,
        "content_sha256": digest,
    }


def _commit_mode_locked(
    repo: Path,
    task: str,
    message: str,
    mode: str,
    *,
    source_root: Path | None = None,
    expected_implementation_revision: str | None = None,
) -> dict[str, object]:
    _, branch, worktree = require_registered_task(repo, task)
    active = receipt_path(repo)
    state_dir = task_state_dir(repo)
    private_index = state_dir / "automation-maintenance.index"
    require_ignored_untracked(repo, (active, consumed_receipt_path(repo), private_index))
    if not active.is_file():
        raise UpgradeError("no active successful automation upgrade receipt")
    try:
        receipt = json.loads(active.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise UpgradeError("invalid automation upgrade receipt") from exc
    receipt = validate_receipt_schema(receipt)
    if (receipt.get("task_id"), receipt.get("branch"), receipt.get("worktree")) != (task, branch, str(worktree)):
        raise UpgradeError("automation receipt identity does not match the current Task worktree")
    if receipt.get("authority_head") != git_head(repo):
        raise UpgradeError("automation receipt authority HEAD is stale")
    paths = receipt_paths(repo, receipt)
    fingerprints = receipt.get("path_fingerprints")
    if not isinstance(fingerprints, dict) or set(fingerprints) != set(paths):
        raise UpgradeError("automation receipt fingerprints do not match its paths")
    if any(file_fingerprint(repo, path) != fingerprints[path] for path in paths):
        raise UpgradeError("automation receipt path content/state fingerprint changed")
    if pending_paths(repo) != paths:
        raise UpgradeError("pending paths do not exactly match the automation receipt")
    proof_path = source_recovery_proof_path(repo)
    proof: dict | None = None
    proof_raw: bytes | None = None
    if mode == "standard":
        authority = validate_authority(repo, receipt)
    elif mode == "source_recovery":
        if expected_implementation_revision is None:
            raise UpgradeError("source-recovery commit requires an expected implementation revision")
        expected_implementation_revision = _validate_expected_implementation_revision(
            expected_implementation_revision
        )
        proof_admin = worktree_admin_dir(repo)
        _validate_admin_record_parent(proof_path, proof_admin)
        try:
            proof_raw, proof_identity = private_state.read_bytes_identity(
                proof_path, "source-recovery proof"
            )
            proof_record = json.loads(proof_raw.decode("utf-8"))
        except (private_state.GitPrivateStateError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise UpgradeError("missing or invalid source-recovery proof") from exc
        proof = _validate_recovery_proof(proof_record)
        source, current_revision = resolve_clean_source_worktree(Path(proof["implementation_source"]))
        if (proof["implementation_source"] != str(source)
                or proof["implementation_revision"] != expected_implementation_revision
                or current_revision != expected_implementation_revision
                or (source_root is not None and source_root != source)):
            raise UpgradeError("source-recovery proof implementation is stale")
        if not _source_is_compatible(receipt["source"], source):
            raise UpgradeError("source-recovery receipt source is incompatible")
        _validate_source_revision(source, receipt["source_revision"])
        authority = _validate_recovery_records(repo, receipt, active.read_bytes(), proof)
        if private_state.read_bytes(proof_path, "source-recovery proof") != proof_raw:
            raise UpgradeError("source-recovery proof changed before commit mutation")
    else:  # pragma: no cover
        raise UpgradeError("unsupported commit mode")

    consumed = consumed_receipt_path(repo)
    consumed_receipt = dict(receipt)
    consumed_receipt["status"] = "consumed"
    consumed_receipt["commit_sha"] = None
    private_index = state_dir / "automation-maintenance.index"
    committed = False
    commit_sha = ""
    active_moved = False
    authority_removed = False
    proof_removed = False
    consumed_instances: set[tuple[int, int]] = set()
    try:
        if os.path.lexists(consumed):
            existing = consumed.lstat()
            if not stat.S_ISREG(existing.st_mode) or stat.S_ISLNK(existing.st_mode):
                raise UpgradeError("existing consumed receipt has an unsafe type")
        os.replace(active, consumed)
        active_moved = True
        moved_metadata = consumed.lstat()
        consumed_instances.add((moved_metadata.st_dev, moved_metadata.st_ino))
        atomic_json_write(consumed, consumed_receipt)
        consumed_metadata = consumed.lstat()
        consumed_instances.add((consumed_metadata.st_dev, consumed_metadata.st_ino))
        authority_raw, authority_identity = private_state.read_bytes_identity(
            authority, "maintenance authority"
        )
        expected_authority = (
            _bridge_authority(receipt, hashlib.sha256(canonical_json(proof)).hexdigest())
            if mode == "source_recovery" and proof is not None
            else {
                "schema_version": 1, "task_id": receipt["task_id"], "branch": receipt["branch"],
                "worktree": receipt["worktree"], "authority_nonce": receipt["authority_nonce"],
                "receipt_sha256": receipt_digest(receipt),
            }
        )
        if json.loads(authority_raw.decode("utf-8")) != expected_authority:
            raise UpgradeError("authority changed before commit mutation")
        private_state.unlink(
            authority,
            expected_identity=authority_identity,
            expected_content=authority_raw,
            _lock_held=True,
        )
        authority_removed = True
        if mode == "source_recovery" and proof is not None:
            current_proof, current_proof_identity = private_state.read_bytes_identity(
                proof_path, "source-recovery proof"
            )
            if current_proof != proof_raw or current_proof_identity != proof_identity:
                raise UpgradeError("source-recovery proof changed before removal")
            private_state.unlink(
                proof_path,
                expected_identity=proof_identity,
                expected_content=proof_raw,
                _lock_held=True,
            )
            proof_removed = True
        task_state_dir(repo)
        private_index.unlink(missing_ok=True)
        index_environment = {"GIT_INDEX_FILE": str(private_index)}
        run(["git", "read-tree", "HEAD"], cwd=repo, env_overrides=index_environment)
        run(["git", "add", "--", *paths], cwd=repo, env_overrides=index_environment)
        run(
            ["git", "diff", "--no-ext-diff", "--cached", "--check"],
            cwd=repo,
            env_overrides=index_environment,
        )
        staged = sorted(
            run(
                ["git", "diff", "--no-ext-diff", "--cached", "--name-only"],
                cwd=repo,
                env_overrides=index_environment,
            ).stdout.splitlines()
        )
        if staged != paths:
            raise UpgradeError("staged paths do not exactly match the automation receipt")
        if any(file_fingerprint(repo, path) != fingerprints[path] for path in paths):
            raise UpgradeError("automation receipt path changed while staging")
        for path in paths:
            if staged_fingerprint(
                repo,
                path,
                env_overrides=index_environment,
            ) != normalized_git_fingerprint(fingerprints[path]):
                raise UpgradeError(f"staged content does not match the automation receipt: {path}")
        commit_message = message.strip() or f"task: {task}"
        if task not in commit_message:
            commit_message = f"{commit_message}\n\nTask: {task}"
        tree = run(
            ["git", "write-tree"],
            cwd=repo,
            env_overrides=index_environment,
        ).stdout.strip()
        expected_head = receipt["authority_head"]
        commit_sha = run(
            ["git", "commit-tree", tree, "-p", expected_head],
            cwd=repo,
            input_text=commit_message + "\n",
        ).stdout.strip()
        run(
            ["git", "update-ref", f"refs/heads/{branch}", commit_sha, expected_head],
            cwd=repo,
        )
        committed = True
        consumed_receipt["commit_sha"] = commit_sha
        atomic_json_write(consumed, consumed_receipt)
        run(["git", "reset", "-q", commit_sha, "--", *paths], cwd=repo)
    except (UpgradeError, OSError, private_state.GitPrivateStateError) as failure:
        if committed:
            raise UpgradeError(
                f"commit {commit_sha} was published but finalization failed"
            ) from failure
        rollback_errors: list[str] = []
        try:
            if proof_removed and proof is not None:
                if proof_raw is None:
                    raise UpgradeError("source-recovery proof bytes were not captured")
                private_state.exclusive_write_bytes(
                    proof_path, proof_raw, _lock_held=True
                )
        except (OSError, UpgradeError, private_state.GitPrivateStateError) as rollback_error:
            rollback_errors.append(f"proof restore failed: {rollback_error}")
        try:
            if authority_removed:
                restored = _bridge_authority(receipt, hashlib.sha256(canonical_json(proof)).hexdigest()) if mode == "source_recovery" and proof is not None else {
                    "schema_version": 1, "task_id": receipt["task_id"], "branch": receipt["branch"],
                    "worktree": receipt["worktree"], "authority_nonce": receipt["authority_nonce"],
                    "receipt_sha256": receipt_digest(receipt),
                }
                _write_authority_at(
                    authority,
                    restored,
                    admin=_rollback_authority_guard(repo, authority),
                    lock_held=True,
                )
        except (OSError, UpgradeError) as rollback_error:
            rollback_errors.append(f"authority restore failed: {rollback_error}")
        try:
            if consumed_instances:
                current = consumed.lstat()
                if (current.st_dev, current.st_ino) in consumed_instances:
                    consumed.unlink()
                else:
                    raise UpgradeError("consumed receipt changed during rollback")
        except OSError as rollback_error:
            rollback_errors.append(f"consumed receipt cleanup failed: {rollback_error}")
        except UpgradeError as rollback_error:
            rollback_errors.append(f"consumed receipt cleanup failed: {rollback_error}")
        try:
            if active_moved:
                if os.path.lexists(active):
                    raise UpgradeError("active receipt appeared during rollback")
                exclusive_json_write(active, receipt)
        except (OSError, UpgradeError) as rollback_error:
            rollback_errors.append(f"active receipt restore failed: {rollback_error}")
        if rollback_errors:
            raise UpgradeError(
                f"commit failed: {failure}; rollback failed: {'; '.join(rollback_errors)}"
            ) from failure
        if isinstance(failure, OSError):
            raise UpgradeError("commit failed during filesystem operation") from failure
        raise
    finally:
        try:
            task_state_dir(repo)
            private_index.unlink(missing_ok=True)
        except OSError as cleanup_error:
            raise UpgradeError("private index cleanup failed") from cleanup_error
    result: dict[str, str | bool | list[str]] = {
        "status": "COMMITTED", "commit_sha": commit_sha,
    }
    if mode == "source_recovery":
        result.update({
            "commitCreated": True,
            "changedPaths": paths,
            "pushPerformed": False,
            "mergePerformed": False,
        })
    return result


def _commit_mode(
    repo: Path,
    task: str,
    message: str,
    mode: str,
    *,
    source_root: Path | None = None,
    expected_implementation_revision: str | None = None,
) -> dict[str, object]:
    try:
        private_state.prepare(
            repo,
            admin=True,
            common_dir=common_git_dir(repo),
            admin_dir=worktree_admin_dir(repo),
        )
        with private_state.mutation_lock(repo, admin=True):
            return _commit_mode_locked(
                repo,
                task,
                message,
                mode,
                source_root=source_root,
                expected_implementation_revision=expected_implementation_revision,
            )
    except private_state.GitPrivateStateError as exc:
        raise UpgradeError(str(exc)) from exc


def commit(repo: Path, task: str, message: str) -> dict[str, object]:
    return _commit_mode(repo, task, message, "standard")


def commit_recovered_maintenance(
    target_repo: Path,
    templates_source_root: Path,
    task: str,
    message: str,
    *,
    expected_implementation_revision: str,
) -> dict[str, object]:
    """Commit only a proof-bound source recovery; no network or merge operation is performed."""
    # The source argument is deliberately checked before the receipt is opened, and
    # is not a target mutation/apply hook.
    expected_implementation_revision = _validate_expected_implementation_revision(
        expected_implementation_revision
    )
    source, source_revision = resolve_clean_source_worktree(templates_source_root)
    if source_revision != expected_implementation_revision:
        raise UpgradeError("source HEAD does not match the expected implementation revision")
    return _commit_mode(
        target_repo,
        task,
        message,
        "source_recovery",
        source_root=source,
        expected_implementation_revision=expected_implementation_revision,
    )


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Agent Core version/update/upgrade contract")
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("version")
    check = sub.add_parser("check-update")
    check.add_argument("--source", type=Path, required=True)
    check.add_argument("--expected-source-revision")
    upgrade = sub.add_parser("upgrade")
    upgrade.add_argument("--source", type=Path, required=True)
    upgrade.add_argument("--expected-source-revision", required=True)
    bootstrap = sub.add_parser("bootstrap-receipt")
    bootstrap.add_argument("--source", type=Path, required=True)
    bootstrap.add_argument("--expected-source-revision", required=True)
    rebind = sub.add_parser("rebind-maintenance-provenance")
    rebind.add_argument("--source", type=Path, required=True)
    rebind.add_argument("--expected-source-revision", required=True)
    commit_parser = sub.add_parser("commit")
    commit_parser.add_argument("task")
    commit_parser.add_argument("message", nargs="?", default="")
    return p


def main() -> int:
    args = parser().parse_args()
    try:
        repo = root()
        if args.command == "version":
            result = context(repo)
        elif args.command == "check-update":
            result = check_update(repo, args.source, args.expected_source_revision)
        elif args.command == "upgrade":
            result = apply(repo, args.source, args.expected_source_revision)
        elif args.command == "bootstrap-receipt":
            result = bootstrap_receipt(repo, args.source, args.expected_source_revision)
        elif args.command == "rebind-maintenance-provenance":
            result = rebind_maintenance_provenance(repo, args.source, args.expected_source_revision)
        elif args.command == "commit":
            result = commit(repo, args.task, args.message)
        else:  # pragma: no cover
            raise UpgradeError(f"unsupported command: {args.command}")
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except UpgradeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
