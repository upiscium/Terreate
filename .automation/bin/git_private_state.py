"""Owned, fail-closed storage for Agent Core's Git-private runtime state."""
from __future__ import annotations

from contextlib import contextmanager, nullcontext
from dataclasses import dataclass
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat
import subprocess


NAMESPACE = "agent-core"
LEGACY_NAMESPACE = "opencode"
TASK_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
PR_RE = re.compile(r"[1-9][0-9]*")
HASH_RE = re.compile(r"[0-9a-f]{64}")
OID_RE = re.compile(r"[0-9a-fA-F]{40}|[0-9a-fA-F]{64}")
REPOSITORY_RE = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
SHARED_DIRS = ("cleanup", "integration", "discard-pristine", "automation-maintenance")
FIXED_AUTHORITY_FILES = {
    "authority.json",
    "source-recovery-proof.json",
    "publication-recovery.json",
}
LOCK_FILES = {"cleanup.lock", "migration.lock"}
TEMP_RE = re.compile(r"\.(?:migrate|record)\.[0-9]+\.[0-9a-f]{16}")
_GIT_EXECUTABLE = "git"


class GitPrivateStateError(RuntimeError):
    pass


@dataclass(frozen=True)
class Topology:
    common: Path
    admin: Path

    @property
    def admin_is_common(self) -> bool:
        return self.admin == self.common


@dataclass(frozen=True)
class MigrationFile:
    source: Path
    target: Path
    identity: tuple[int, int]
    mode: int
    content: bytes


def _git_path(root: Path, argument: str) -> Path:
    environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    environment.update({
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_OPTIONAL_LOCKS": "0",
        "GIT_TERMINAL_PROMPT": "0",
    })
    result = subprocess.run(
        [_GIT_EXECUTABLE, "-c", "core.fsmonitor=false", "-c", "core.hooksPath=/dev/null",
         "-c", "core.pager=", "rev-parse", argument],
        cwd=root, text=True, capture_output=True, check=False, env=environment,
    )
    raw = result.stdout.strip()
    if result.returncode != 0 or not raw:
        raise GitPrivateStateError(f"cannot resolve Git private-state location: {argument}")
    candidate = Path(raw)
    try:
        resolved = candidate.resolve() if candidate.is_absolute() else (root / candidate).resolve()
        metadata = resolved.lstat()
    except (OSError, RuntimeError, ValueError) as exc:
        raise GitPrivateStateError(f"cannot resolve Git private-state location: {argument}") from exc
    if not stat.S_ISDIR(metadata.st_mode):
        raise GitPrivateStateError(f"Git private-state location is not a directory: {resolved}")
    return resolved


def topology(root: Path) -> Topology:
    return Topology(_git_path(root, "--git-common-dir"), _git_path(root, "--absolute-git-dir"))


def common_git_dir(root: Path) -> Path:
    return topology(root).common


def admin_git_dir(root: Path) -> Path:
    return topology(root).admin


def common_state(root: Path) -> Path:
    return common_git_dir(root) / NAMESPACE


def admin_maintenance(root: Path) -> Path:
    return admin_git_dir(root) / NAMESPACE / "automation-maintenance"


def publication_recovery_receipt(root: Path) -> Path:
    """Return the protected receipt path in this worktree's Git admin dir."""
    return admin_maintenance(root) / "publication-recovery.json"


def cleanup_receipt(root: Path, task: str) -> Path:
    if TASK_RE.fullmatch(task) is None:
        raise GitPrivateStateError("invalid Task ID")
    return common_state(root) / "cleanup" / f"{task}.json"


def discard_receipt(root: Path, task: str) -> Path:
    if TASK_RE.fullmatch(task) is None:
        raise GitPrivateStateError("invalid Task ID")
    return common_state(root) / "discard-pristine" / f"{task}.json"


def integration_checkpoint(root: Path, pr: str) -> Path:
    if PR_RE.fullmatch(pr) is None:
        raise GitPrivateStateError("invalid pull request number")
    return common_state(root) / "integration" / f"pr-{pr}.head"


def _lstat(path: Path, what: str) -> os.stat_result:
    try:
        return path.lstat()
    except OSError as exc:
        raise GitPrivateStateError(f"cannot inspect {what}: {path}") from exc


def _require_dir(path: Path, what: str = "private-state directory") -> os.stat_result:
    metadata = _lstat(path, what)
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise GitPrivateStateError(f"unsafe {what}: {path}")
    return metadata


def _require_owned_mode(path: Path, metadata: os.stat_result, mode: int, what: str,
                        *, exact: bool = False) -> None:
    if metadata.st_uid != os.geteuid():
        raise GitPrivateStateError(f"unsafe {what} ownership: {path}")
    actual = stat.S_IMODE(metadata.st_mode)
    unsafe = actual != mode if exact else (actual & mode) != mode
    unsafe = unsafe or bool(actual & 0o022)
    if unsafe:
        raise GitPrivateStateError(f"unsafe {what} mode: {path}")


def _require_legacy_dir(path: Path, what: str) -> os.stat_result:
    metadata = _require_dir(path, what)
    _require_owned_mode(path, metadata, 0o700, what)
    return metadata


def _require_legacy_record(path: Path, what: str) -> os.stat_result:
    metadata = _require_regular(path, what)
    _require_owned_mode(path, metadata, 0o400, what)
    if metadata.st_mode & 0o111:
        raise GitPrivateStateError(f"unsafe {what} mode: {path}")
    return metadata


def _require_canonical_dir(path: Path, what: str = "canonical private-state directory") -> os.stat_result:
    metadata = _require_dir(path, what)
    _require_owned_mode(path, metadata, 0o700, what, exact=True)
    return metadata


def _require_canonical_file(path: Path, what: str) -> os.stat_result:
    metadata = _require_regular(path, what)
    _require_owned_mode(path, metadata, 0o600, what, exact=True)
    return metadata


def _require_regular(path: Path, what: str) -> os.stat_result:
    metadata = _lstat(path, what)
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise GitPrivateStateError(f"unsafe {what}: {path}")
    return metadata


def safe_directory(path: Path) -> None:
    if NAMESPACE in path.parts:
        _require_canonical_dir(path)
    else:
        _require_dir(path)


def _open_dir(path: Path) -> int:
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise GitPrivateStateError(f"cannot open private-state directory: {path}") from exc
    if not stat.S_ISDIR(os.fstat(descriptor).st_mode):
        os.close(descriptor)
        raise GitPrivateStateError(f"unsafe private-state directory: {path}")
    return descriptor


def _require_git_admin_descriptor(
    descriptor: int, path: Path, what: str = "Git administrative directory"
) -> os.stat_result:
    metadata = os.fstat(descriptor)
    if not stat.S_ISDIR(metadata.st_mode):
        raise GitPrivateStateError(f"unsafe {what}: {path}")
    if metadata.st_uid != os.geteuid():
        raise GitPrivateStateError(f"unsafe {what} ownership: {path}")
    if stat.S_IMODE(metadata.st_mode) & 0o022:
        raise GitPrivateStateError(f"unsafe {what} mode: {path}")
    return metadata


def _require_canonical_dir_descriptor(
    descriptor: int, path: Path, what: str = "canonical private-state directory"
) -> os.stat_result:
    metadata = os.fstat(descriptor)
    if not stat.S_ISDIR(metadata.st_mode):
        raise GitPrivateStateError(f"unsafe {what}: {path}")
    if metadata.st_uid != os.geteuid():
        raise GitPrivateStateError(f"unsafe {what} ownership: {path}")
    if stat.S_IMODE(metadata.st_mode) != 0o700:
        raise GitPrivateStateError(f"unsafe {what} mode: {path}")
    return metadata


def _require_canonical_file_descriptor(
    descriptor: int, path: Path, what: str
) -> os.stat_result:
    metadata = os.fstat(descriptor)
    if not stat.S_ISREG(metadata.st_mode):
        raise GitPrivateStateError(f"unsafe {what}: {path}")
    if metadata.st_uid != os.geteuid():
        raise GitPrivateStateError(f"unsafe {what} ownership: {path}")
    if stat.S_IMODE(metadata.st_mode) != 0o600:
        raise GitPrivateStateError(f"unsafe {what} mode: {path}")
    return metadata


def _namespace_marker(path: Path) -> tuple[int, str]:
    indexes = [
        index for index, part in enumerate(path.parts)
        if part in {NAMESPACE, LEGACY_NAMESPACE}
    ]
    if not indexes:
        raise GitPrivateStateError(
            f"path is outside a recognized private-state namespace: {path}"
        )
    marker = indexes[-1]
    return marker, path.parts[marker]


def _open_anchored_parent(path: Path) -> int:
    """Open and validate the descriptors that anchor a private-state parent."""
    parts = path.parts
    marker, namespace = _namespace_marker(path)
    canonical = namespace == NAMESPACE
    boundary = Path(*parts[:marker])
    descriptor = _open_dir(boundary)
    try:
        if canonical:
            _require_git_admin_descriptor(descriptor, boundary)
        flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
        current = boundary
        for component in parts[marker:-1]:
            child = os.open(component, flags, dir_fd=descriptor)
            current /= component
            try:
                child_meta = os.fstat(child)
                if not stat.S_ISDIR(child_meta.st_mode):
                    raise GitPrivateStateError(
                        f"unsafe private-state directory component: {current}"
                    )
                if canonical:
                    _require_canonical_dir_descriptor(child, current)
            except (GitPrivateStateError, OSError):
                os.close(child)
                raise
            os.close(descriptor)
            descriptor = child
        return descriptor
    except GitPrivateStateError:
        os.close(descriptor)
        raise
    except OSError as exc:
        os.close(descriptor)
        raise GitPrivateStateError(f"cannot traverse private-state directory: {path.parent}") from exc


def _ensure_child_directory(parent: Path, name: str) -> Path:
    descriptor = _open_dir(parent)
    try:
        created = False
        try:
            os.mkdir(name, 0o700, dir_fd=descriptor)
            os.chmod(name, 0o700, dir_fd=descriptor, follow_symlinks=False)
            os.fsync(descriptor)
            created = True
        except FileExistsError:
            pass
        flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
        child = os.open(name, flags, dir_fd=descriptor)
        try:
            metadata = os.fstat(child)
            if not stat.S_ISDIR(metadata.st_mode):
                raise GitPrivateStateError(f"unsafe private-state directory: {parent / name}")
            if not created and (
                metadata.st_uid != os.geteuid()
                or stat.S_IMODE(metadata.st_mode) != 0o700
            ):
                raise GitPrivateStateError(f"unsafe private-state directory: {parent / name}")
        finally:
            os.close(child)
    except OSError as exc:
        raise GitPrivateStateError(f"cannot create private-state directory: {parent / name}") from exc
    finally:
        os.close(descriptor)
    return parent / name


def _ensure_namespace(base: Path, subdirectories: tuple[str, ...]) -> Path:
    descriptor = _open_dir(base)
    current = base
    try:
        _require_git_admin_descriptor(descriptor, base)
        flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
        for name in (NAMESPACE, *subdirectories):
            created = False
            try:
                os.mkdir(name, 0o700, dir_fd=descriptor)
                os.chmod(name, 0o700, dir_fd=descriptor, follow_symlinks=False)
                os.fsync(descriptor)
                created = True
            except FileExistsError:
                pass
            child = os.open(name, flags, dir_fd=descriptor)
            child_meta = os.fstat(child)
            if not stat.S_ISDIR(child_meta.st_mode):
                os.close(child)
                raise GitPrivateStateError(f"unsafe private-state directory: {current / name}")
            if child_meta.st_uid != os.geteuid() or stat.S_IMODE(child_meta.st_mode) != 0o700:
                os.close(child)
                raise GitPrivateStateError(f"unsafe canonical private-state directory: {current / name}")
            os.close(descriptor)
            descriptor = child
            current /= name
        return current
    except GitPrivateStateError:
        raise
    except OSError as exc:
        raise GitPrivateStateError(f"cannot create private-state namespace below: {base}") from exc
    finally:
        os.close(descriptor)


def read_bytes_identity(
    path: Path, what: str = "private-state record"
) -> tuple[bytes, tuple[int, int]]:
    flags = (os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) |
             getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NONBLOCK", 0))
    parent_fd = _open_anchored_parent(path)
    try:
        descriptor = os.open(path.name, flags, dir_fd=parent_fd)
        try:
            metadata = os.fstat(descriptor)
            if _namespace_marker(path)[1] == NAMESPACE:
                metadata = _require_canonical_file_descriptor(descriptor, path, what)
            elif not stat.S_ISREG(metadata.st_mode):
                raise GitPrivateStateError(f"unsafe {what}: {path}")
            chunks: list[bytes] = []
            while True:
                chunk = os.read(descriptor, 1024 * 1024)
                if not chunk:
                    break
                chunks.append(chunk)
            after = os.fstat(descriptor)
            if (metadata.st_dev, metadata.st_ino, metadata.st_size) != (
                after.st_dev, after.st_ino, after.st_size
            ):
                raise GitPrivateStateError(f"{what} changed while reading: {path}")
            return b"".join(chunks), (metadata.st_dev, metadata.st_ino)
        finally:
            os.close(descriptor)
    except GitPrivateStateError:
        raise
    except OSError as exc:
        raise GitPrivateStateError(f"cannot read {what}: {path}") from exc
    finally:
        os.close(parent_fd)


def read_bytes(path: Path, what: str = "private-state record") -> bytes:
    return read_bytes_identity(path, what)[0]


def validate_record(path: Path, what: str = "private-state record") -> None:
    """Require the ownership and mode contract for a persisted state record."""
    if NAMESPACE in path.parts:
        _require_canonical_file(path, what)
    else:
        _require_legacy_record(path, what)


def _valid_shared_file(directory: str, name: str, *, allow_fixed: bool) -> bool:
    if directory in {"cleanup", "discard-pristine"}:
        return name.endswith(".json") and TASK_RE.fullmatch(name[:-5]) is not None
    if directory == "integration":
        return re.fullmatch(r"pr-[1-9][0-9]*\.head", name) is not None
    if directory == "automation-maintenance":
        return (
            re.fullmatch(r"[0-9a-f]{64}\.json", name) is not None
            or (allow_fixed and name in FIXED_AUTHORITY_FILES)
        )
    return False


def _recover_canonical_temps(layout: Topology, *, include_admin: bool = True) -> None:
    """Remove only our exact, owned publication artifacts while fenced."""
    directories = [layout.common / NAMESPACE]
    if include_admin and not layout.admin_is_common:
        directories.append(layout.admin / NAMESPACE)
    for namespace in directories:
        if _namespace_kind(namespace, foreign_regular=False) != "directory":
            continue
        subdirs = [namespace / name for name in SHARED_DIRS]
        for directory in subdirs:
            if not directory.exists():
                continue
            _require_canonical_dir(directory)
            for item in directory.iterdir():
                if item.name.startswith("."):
                    if TEMP_RE.fullmatch(item.name) is None:
                        raise GitPrivateStateError(f"unknown canonical temporary: {item}")
                    metadata = _require_canonical_file(item, "canonical temporary")
                    if metadata.st_nlink < 1 or metadata.st_nlink > 2:
                        raise GitPrivateStateError(f"unsafe canonical temporary link count: {item}")
                    parent_fd = _open_anchored_parent(item)
                    try:
                        os.unlink(item.name, dir_fd=parent_fd)
                        os.fsync(parent_fd)
                    finally:
                        os.close(parent_fd)


def _legacy_json(content: bytes, path: Path) -> dict:
    try:
        value = json.loads(content.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise GitPrivateStateError(f"invalid legacy private-state record: {path}") from exc
    if not isinstance(value, dict):
        raise GitPrivateStateError(f"invalid legacy private-state record: {path}")
    return value


def _valid_oid(value: object) -> bool:
    return isinstance(value, str) and OID_RE.fullmatch(value) is not None


def _valid_digest(value: object) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _valid_task_id(value: object) -> bool:
    return isinstance(value, str) and TASK_RE.fullmatch(value) is not None


def _valid_nonempty(value: object) -> bool:
    return isinstance(value, str) and bool(value)


def _valid_absolute_path(value: object) -> bool:
    return _valid_nonempty(value) and Path(value).is_absolute()


def _valid_repository(value: object) -> bool:
    return isinstance(value, str) and REPOSITORY_RE.fullmatch(value) is not None


def _valid_branch_worktree(branch: object, task: object, worktree: object) -> bool:
    if not (_valid_task_id(task) and isinstance(branch, str) and isinstance(worktree, str)):
        return False
    if not (branch.startswith(f"task/{task}-") or branch.startswith(f"fix/{task}-")):
        return False
    path = Path(worktree)
    return (path.is_absolute() and path.parent.name == ".worktrees" and
            path.name == branch.split("/", 1)[1])


def _valid_task_branch(branch: object, task: object) -> bool:
    return (
        _valid_task_id(task)
        and isinstance(branch, str)
        and (branch.startswith(f"task/{task}-") or branch.startswith(f"fix/{task}-"))
    )


def _valid_authority_fields(value: dict) -> bool:
    return (
        _valid_task_id(value.get("task_id"))
        and _valid_task_branch(value.get("branch"), value.get("task_id"))
        and _valid_absolute_path(value.get("worktree"))
        and _valid_digest(value.get("authority_nonce"))
        and _valid_digest(value.get("receipt_sha256"))
    )


def _valid_cleanup_evidence(value: dict, status: str, local_head: str) -> bool:
    if status == "merged":
        return (set(value) == {"repository", "pr", "published_head", "upstream"}
                and _valid_repository(value.get("repository"))
                and isinstance(value.get("pr"), int) and not isinstance(value.get("pr"), bool)
                and value["pr"] > 0 and _valid_oid(value.get("published_head"))
                and value.get("published_head").casefold() == local_head.casefold()
                and value.get("upstream") in {"live", "deleted"})
    return (set(value) == {"repository", "upstream", "base_revision"}
            and _valid_repository(value.get("repository"))
            and value.get("upstream") == "cancelled-safe"
            and _valid_oid(value.get("base_revision")))


def _validate_authority_filename(path: Path, content: bytes) -> None:
    if path.parent.name != "automation-maintenance" or path.name in FIXED_AUTHORITY_FILES:
        return
    value = _legacy_json(content, path)
    expected = hashlib.sha256(str(Path(value["worktree"]).resolve()).encode()).hexdigest()
    if path.name != expected + ".json":
        raise GitPrivateStateError(f"invalid private-state authority filename: {path}")


def _validate_authority_topology(path: Path, content: bytes, layout: Topology) -> Path:
    """Bind authority ownership to a worktree registered in this topology."""
    if path.parent.name != "automation-maintenance":
        raise GitPrivateStateError(f"authority has an invalid parent: {path}")
    value = _legacy_json(content, path)
    worktree = Path(value["worktree"])
    dot_git = worktree / ".git"
    try:
        metadata = dot_git.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            raise ValueError
        if stat.S_ISDIR(metadata.st_mode):
            admin = dot_git.resolve(strict=True)
        elif stat.S_ISREG(metadata.st_mode):
            marker = dot_git.read_text(encoding="utf-8").strip()
            if not marker.startswith("gitdir: "):
                raise ValueError
            target = Path(marker[8:])
            admin = (target if target.is_absolute() else worktree / target).resolve(strict=True)
        else:
            raise ValueError
    except (OSError, UnicodeDecodeError, ValueError, KeyError) as exc:
        raise GitPrivateStateError(f"authority worktree is not registered: {path}") from exc
    common = layout.common.resolve()
    allowed = admin == common or admin == layout.admin.resolve()
    if not allowed and admin.parent.name == "worktrees" and admin.parent.parent == common:
        try:
            registered_gitdir = (admin / "gitdir").read_text(encoding="utf-8").strip()
            allowed = Path(registered_gitdir).resolve(strict=True) == dot_git.resolve(strict=True)
        except (OSError, UnicodeDecodeError):
            allowed = False
    if not allowed:
        raise GitPrivateStateError(f"authority worktree is outside Git topology: {path}")
    try:
        head = (admin / "HEAD").read_text(encoding="utf-8").strip()
    except (OSError, UnicodeDecodeError) as exc:
        raise GitPrivateStateError(f"authority worktree registration is unreadable: {path}") from exc
    if head != f"ref: refs/heads/{value['branch']}":
        raise GitPrivateStateError(f"authority branch does not match worktree registration: {path}")
    return admin


def _validate_publication_recovery_topology(
    path: Path, value: dict, layout: Topology
) -> Path:
    """Bind a publication recovery receipt to its exact worktree and HEAD."""
    admin = _validate_authority_topology(path, json.dumps(value).encode(), layout)
    worktree = Path(value["worktree"])
    try:
        result = subprocess.run(
            [_GIT_EXECUTABLE, "-C", str(worktree), "rev-parse", "--verify", "HEAD"],
            text=True,
            capture_output=True,
            check=False,
            env={
                key: value
                for key, value in os.environ.items()
                if not key.startswith("GIT_")
            } | {
                "GIT_CONFIG_NOSYSTEM": "1",
                "GIT_CONFIG_GLOBAL": os.devnull,
                "GIT_OPTIONAL_LOCKS": "0",
                "GIT_TERMINAL_PROMPT": "0",
            },
        )
        actual_head = result.stdout.strip()
    except OSError as exc:
        raise GitPrivateStateError(
            f"publication recovery worktree HEAD is unreadable: {path}"
        ) from exc
    if result.returncode != 0 or not _valid_oid(actual_head):
        raise GitPrivateStateError(f"publication recovery worktree HEAD is unreadable: {path}")
    if value["head"] != actual_head:
        raise GitPrivateStateError(f"publication recovery HEAD does not match worktree: {path}")
    return admin


def _validate_legacy_content(path: Path, content: bytes) -> None:
    directory = path.parent.name
    name = path.name
    if directory == "integration":
        try:
            text = content.decode("ascii", errors="strict")
        except UnicodeDecodeError as exc:
            raise GitPrivateStateError(f"invalid legacy integration checkpoint: {path}") from exc
        if (not text.endswith("\n") or not _valid_oid(text[:-1]) or
                text[:-1] != text[:-1].lower()):
            raise GitPrivateStateError(f"invalid legacy integration checkpoint: {path}")
        return
    value = _legacy_json(content, path)
    if directory == "cleanup":
        required = {"schema_version", "task", "status", "worktree", "branch", "local_head", "evidence"}
        valid = (
            set(value) == required
            and value.get("schema_version") == 1
            and value.get("task") == name[:-5]
            and value.get("status") in {"merged", "cancelled"}
            and _valid_branch_worktree(value.get("branch"), value.get("task"), value.get("worktree"))
            and _valid_oid(value.get("local_head"))
            and isinstance(value.get("evidence"), dict)
            and _valid_cleanup_evidence(value["evidence"], value["status"], value["local_head"])
        )
    elif directory == "discard-pristine":
        required = {
            "schema_version", "operation", "task", "status", "worktree", "branch",
            "base_branch", "base_revision", "local_head", "repository",
        }
        valid = (
            set(value) == required
            and value.get("schema_version") == 1
            and value.get("operation") == "discard-pristine"
            and value.get("task") == name[:-5]
            and value.get("status") == "initialized"
            and _valid_branch_worktree(value.get("branch"), value.get("task"), value.get("worktree"))
            and _valid_nonempty(value.get("base_branch"))
            and _valid_repository(value.get("repository"))
            and _valid_oid(value.get("base_revision"))
            and _valid_oid(value.get("local_head"))
            and value.get("local_head").casefold() == value.get("base_revision").casefold()
        )
    elif name == "publication-recovery.json":
        required = {
            "schema_version", "kind", "repository", "task_id", "worktree", "branch",
            "head", "base_branch", "base_revision", "pr_number",
            "blocked_state_sha256", "publication_ready_state_sha256",
            "draft_pr_created_state_sha256", "work_units_sha256", "verification_sha256",
            "contract_sha256", "issue_sha256",
        }
        valid = (
            set(value) == required
            and value.get("schema_version") == 1
            and value.get("kind") == "blocked-publication-recovery"
            and _valid_repository(value.get("repository"))
            and _valid_task_id(value.get("task_id"))
            and _valid_absolute_path(value.get("worktree"))
            and _valid_task_branch(value.get("branch"), value.get("task_id"))
            and _valid_oid(value.get("head"))
            and _valid_nonempty(value.get("base_branch"))
            and _valid_oid(value.get("base_revision"))
            and isinstance(value.get("pr_number"), int)
            and not isinstance(value.get("pr_number"), bool)
            and value.get("pr_number") > 0
            and all(_valid_digest(value.get(field)) for field in (
                "blocked_state_sha256", "publication_ready_state_sha256",
                "draft_pr_created_state_sha256", "work_units_sha256",
                "verification_sha256", "contract_sha256",
            ))
            and (value.get("issue_sha256") is None or _valid_digest(value.get("issue_sha256")))
        )
    elif name == "source-recovery-proof.json":
        required = {
            "schema_version", "kind", "task_id", "branch", "worktree", "authority_head",
            "authority_nonce", "receipt_sha256", "receipt_bytes_sha256", "changed_paths_sha256",
            "path_fingerprints_sha256", "implementation_source", "implementation_revision",
            "receipt_source", "receipt_source_revision",
        }
        valid = (
            set(value) == required
            and value.get("schema_version") == 1
            and value.get("kind") == "source-recovery-proof"
            and _valid_authority_fields(value)
            and _valid_oid(value.get("authority_head"))
            and all(_valid_digest(value.get(field)) for field in (
                "receipt_bytes_sha256", "changed_paths_sha256", "path_fingerprints_sha256",
            ))
            and _valid_absolute_path(value.get("implementation_source"))
            and _valid_oid(value.get("implementation_revision"))
            and _valid_absolute_path(value.get("receipt_source"))
            and _valid_oid(value.get("receipt_source_revision"))
        )
    else:
        standard = {"schema_version", "task_id", "branch", "worktree", "authority_nonce", "receipt_sha256"}
        bridge = standard | {"kind", "proof_sha256"}
        valid = (
            (set(value) == standard and value.get("schema_version") == 1
             and _valid_authority_fields(value))
            or (set(value) == bridge and value.get("schema_version") == 2
                and value.get("kind") == "source-recovery-bridge"
                and _valid_authority_fields(value)
                and _valid_digest(value.get("proof_sha256")))
        )
    if not valid:
        raise GitPrivateStateError(f"invalid legacy private-state record: {path}")


def _namespace_kind(path: Path, *, foreign_regular: bool) -> str:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return "absent"
    except OSError as exc:
        raise GitPrivateStateError(f"cannot inspect private-state namespace: {path}") from exc
    if stat.S_ISREG(metadata.st_mode) and foreign_regular:
        return "foreign"
    if stat.S_ISDIR(metadata.st_mode) and not stat.S_ISLNK(metadata.st_mode):
        return "directory"
    raise GitPrivateStateError(f"unsafe private-state namespace: {path}")


def _scan_shared_legacy(layout: Topology) -> tuple[list[tuple[Path, Path]], Path | None]:
    root = layout.common / LEGACY_NAMESPACE
    kind = _namespace_kind(root, foreign_regular=True)
    if kind != "directory":
        return [], None
    pairs: list[tuple[Path, Path]] = []
    _require_legacy_dir(root, "legacy private-state directory")
    legacy_lock: Path | None = None
    for entry in root.iterdir():
        if entry.name == "cleanup.lock":
            metadata = _require_regular(entry, "legacy cleanup lock")
            _require_owned_mode(entry, metadata, 0o600, "legacy cleanup lock")
            if metadata.st_mode & 0o111:
                raise GitPrivateStateError(f"unsafe legacy cleanup lock mode: {entry}")
            legacy_lock = entry
            continue
        if entry.name not in SHARED_DIRS:
            raise GitPrivateStateError(f"unknown legacy private-state entry: {entry}")
        _require_legacy_dir(entry, "legacy private-state directory")
        for child in entry.iterdir():
            allow_fixed = entry.name == "automation-maintenance"
            if not _valid_shared_file(entry.name, child.name, allow_fixed=allow_fixed):
                raise GitPrivateStateError(f"unknown legacy private-state entry: {child}")
            _require_legacy_record(child, "legacy private-state record")
            if entry.name == "automation-maintenance":
                content = read_bytes(child)
                _validate_legacy_content(child, content)
                _validate_authority_filename(child, content)
                authority_value = _legacy_json(content, child)
                authority_admin = (
                    _validate_publication_recovery_topology(child, authority_value, layout)
                    if child.name == "publication-recovery.json"
                    else _validate_authority_topology(child, content, layout)
                )
            if child.name in FIXED_AUTHORITY_FILES:
                if authority_admin not in {layout.common, layout.admin}:
                    raise GitPrivateStateError(
                        f"fixed authority belongs to a different worktree admin: {child}"
                    )
                pairs.append((child, authority_admin / NAMESPACE / entry.name / child.name))
            else:
                pairs.append((child, layout.common / NAMESPACE / entry.name / child.name))
    _validate_legacy_relations(
        root, (layout.common / NAMESPACE, layout.admin / NAMESPACE)
    )
    return pairs, legacy_lock


def _scan_admin_legacy(layout: Topology) -> list[tuple[Path, Path]]:
    if layout.admin_is_common:
        return []
    root = layout.admin / LEGACY_NAMESPACE
    kind = _namespace_kind(root, foreign_regular=True)
    if kind != "directory":
        return []
    entries = list(root.iterdir())
    if any(entry.name != "automation-maintenance" for entry in entries):
        unknown = next(entry for entry in entries if entry.name != "automation-maintenance")
        raise GitPrivateStateError(f"unknown legacy private-state entry: {unknown}")
    if not entries:
        return []
    maintenance = entries[0]
    _require_legacy_dir(maintenance, "legacy maintenance authority directory")
    pairs: list[tuple[Path, Path]] = []
    for child in maintenance.iterdir():
        if child.name not in FIXED_AUTHORITY_FILES:
            raise GitPrivateStateError(f"unknown legacy private-state entry: {child}")
        _require_legacy_record(child, "legacy private-state record")
        content = read_bytes(child)
        _validate_legacy_content(child, content)
        _validate_authority_filename(child, content)
        authority_value = _legacy_json(content, child)
        owner = (
            _validate_publication_recovery_topology(child, authority_value, layout)
            if child.name == "publication-recovery.json"
            else _validate_authority_topology(child, content, layout)
        )
        if owner != layout.admin:
            raise GitPrivateStateError(f"fixed authority is outside its worktree admin: {child}")
        pairs.append((child, layout.admin / NAMESPACE / "automation-maintenance" / child.name))
    _validate_legacy_relations(
        root, (layout.common / NAMESPACE, layout.admin / NAMESPACE)
    )
    return pairs


def _validate_legacy_relations(root: Path, canonical_roots: tuple[Path, Path]) -> None:
    maintenance = root / "automation-maintenance"
    if not maintenance.is_dir():
        return
    proof = maintenance / "source-recovery-proof.json"
    authority = maintenance / "authority.json"
    if authority.exists() and not proof.exists():
        authority_value = _legacy_json(
            read_bytes(authority, "legacy maintenance authority"), authority
        )
        if authority_value.get("kind") == "source-recovery-bridge":
            canonical_proofs = [
                candidate / "automation-maintenance" / "source-recovery-proof.json"
                for candidate in canonical_roots
            ]
            canonical_proof = next((candidate for candidate in canonical_proofs
                                    if candidate.is_file() and _recovery_pair_matches(
                                        authority_value,
                                        _legacy_json(read_bytes(candidate), candidate),
                                    )), None)
            if canonical_proof is None:
                raise GitPrivateStateError(
                    f"legacy source-recovery bridge lacks its proof: {root}"
                )
            proof = canonical_proof
        else:
            return
    if proof.exists() and not authority.exists():
        canonical_authorities = [
            candidate / "automation-maintenance" / "authority.json"
            for candidate in canonical_roots
        ]
        proof_value = _legacy_json(read_bytes(proof, "legacy source-recovery proof"), proof)
        canonical_authority = next((candidate for candidate in canonical_authorities
                                    if candidate.is_file() and _recovery_pair_matches(
                                        _legacy_json(read_bytes(candidate), candidate),
                                        proof_value,
                                    )), None)
        if canonical_authority is None:
            raise GitPrivateStateError(
                f"legacy source-recovery proof lacks its bridge: {root}"
            )
        authority = canonical_authority
    if not proof.exists() or not authority.exists():
        return
    proof_value = _legacy_json(read_bytes(proof, "legacy source-recovery proof"), proof)
    authority_value = _legacy_json(read_bytes(authority, "legacy maintenance authority"), authority)
    if not _recovery_pair_matches(authority_value, proof_value):
        raise GitPrivateStateError(f"conflicting legacy authority and proof identity: {root}")


def _recovery_pair_matches(authority: dict, proof: dict) -> bool:
    if authority.get("kind") != "source-recovery-bridge":
        return False
    if any(proof.get(field) != authority.get(field) for field in (
        "task_id", "branch", "worktree", "authority_nonce", "receipt_sha256"
    )):
        return False
    digest = hashlib.sha256(
        json.dumps(proof, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    return authority.get("proof_sha256") == digest


def _validate_canonical(
    layout: Topology, *, allow_incomplete_recovery: bool = False
) -> None:
    shared = layout.common / NAMESPACE
    kind = _namespace_kind(shared, foreign_regular=False)
    if kind == "directory":
        _require_canonical_dir(shared)
        for entry in shared.iterdir():
            if entry.name in LOCK_FILES:
                _require_canonical_file(entry, "canonical private-state lock")
                continue
            if entry.name not in SHARED_DIRS:
                raise GitPrivateStateError(f"unknown canonical private-state entry: {entry}")
            _require_canonical_dir(entry)
            for child in entry.iterdir():
                if child.name.startswith("."):
                    if TEMP_RE.fullmatch(child.name) is None:
                        raise GitPrivateStateError(f"unknown canonical private-state entry: {child}")
                    temporary = _require_canonical_file(child, "canonical temporary")
                    if temporary.st_nlink < 1 or temporary.st_nlink > 2:
                        raise GitPrivateStateError(f"unsafe canonical temporary link count: {child}")
                    continue
                # Fixed files in common are the main worktree's admin records.
                if not _valid_shared_file(entry.name, child.name, allow_fixed=True):
                    raise GitPrivateStateError(f"unknown canonical private-state entry: {child}")
                _require_canonical_file(child, "canonical private-state record")
                content = read_bytes(child, "canonical private-state record")
                _validate_legacy_content(child, content)
                _validate_authority_filename(child, content)
                if entry.name == "automation-maintenance":
                    authority_value = _legacy_json(content, child)
                    authority_admin = (
                        _validate_publication_recovery_topology(child, authority_value, layout)
                        if child.name == "publication-recovery.json"
                        else _validate_authority_topology(child, content, layout)
                    )
                    if child.name in FIXED_AUTHORITY_FILES and authority_admin != layout.common:
                        raise GitPrivateStateError(
                            f"fixed authority is outside its worktree admin: {child}"
                        )
    if not layout.admin_is_common:
        admin_root = layout.admin / NAMESPACE
        kind = _namespace_kind(admin_root, foreign_regular=False)
        if kind == "directory":
            _require_canonical_dir(admin_root)
            entries = list(admin_root.iterdir())
            if any(entry.name not in {"automation-maintenance", "migration.lock"} for entry in entries):
                unknown = next(
                    entry for entry in entries
                    if entry.name not in {"automation-maintenance", "migration.lock"}
                )
                raise GitPrivateStateError(f"unknown canonical private-state entry: {unknown}")
            lock = admin_root / "migration.lock"
            if lock.exists():
                _require_canonical_file(lock, "canonical private-state lock")
            maintenance = admin_root / "automation-maintenance"
            if maintenance.exists():
                _require_canonical_dir(maintenance)
                for child in maintenance.iterdir():
                    if child.name.startswith("."):
                        if TEMP_RE.fullmatch(child.name) is None:
                            raise GitPrivateStateError(f"unknown canonical private-state entry: {child}")
                        temporary = _require_canonical_file(child, "canonical temporary")
                        if temporary.st_nlink < 1 or temporary.st_nlink > 2:
                            raise GitPrivateStateError(f"unsafe canonical temporary link count: {child}")
                        continue
                    if child.name not in FIXED_AUTHORITY_FILES:
                        raise GitPrivateStateError(f"unknown canonical private-state entry: {child}")
                    _require_canonical_file(child, "canonical private-state record")
                    content = read_bytes(child, "canonical private-state record")
                    _validate_legacy_content(child, content)
                    _validate_authority_filename(child, content)
                    authority_value = _legacy_json(content, child)
                    owner = (
                        _validate_publication_recovery_topology(child, authority_value, layout)
                        if child.name == "publication-recovery.json"
                        else _validate_authority_topology(child, content, layout)
                    )
                    if owner != layout.admin:
                        raise GitPrivateStateError(
                            f"fixed authority is outside its worktree admin: {child}"
                        )

    # Canonical maintenance authority is fixed to the administrative topology;
    # a bridge, when present, must name the exact canonical proof bytes.
    for namespace in {layout.common / NAMESPACE, layout.admin / NAMESPACE}:
        proof = namespace / "automation-maintenance" / "source-recovery-proof.json"
        bridge = namespace / "automation-maintenance" / "authority.json"
        proof_exists = proof.is_file()
        bridge_exists = bridge.is_file()
        if proof_exists and not bridge_exists:
            legacy_bridges = {
                layout.common / LEGACY_NAMESPACE / "automation-maintenance/authority.json",
                layout.admin / LEGACY_NAMESPACE / "automation-maintenance/authority.json",
            }
            proof_value = _legacy_json(read_bytes(proof), proof)
            if not allow_incomplete_recovery and not any(candidate.is_file() and _recovery_pair_matches(
                _legacy_json(read_bytes(candidate), candidate), proof_value
            ) for candidate in legacy_bridges):
                raise GitPrivateStateError(f"source-recovery proof lacks bridge authority: {namespace}")
        if bridge_exists:
            bridge_value = _legacy_json(read_bytes(bridge), bridge)
            is_bridge = bridge_value.get("kind") == "source-recovery-bridge"
            legacy_proofs = {
                layout.common / LEGACY_NAMESPACE / "automation-maintenance/source-recovery-proof.json",
                layout.admin / LEGACY_NAMESPACE / "automation-maintenance/source-recovery-proof.json",
            }
            if is_bridge and not proof_exists and not any(
                candidate.is_file() and _recovery_pair_matches(
                    bridge_value, _legacy_json(read_bytes(candidate), candidate)
                ) for candidate in legacy_proofs
            ):
                raise GitPrivateStateError(f"incomplete source-recovery authority pair: {namespace}")
            if not is_bridge and proof_exists:
                raise GitPrivateStateError(f"incomplete source-recovery authority pair: {namespace}")
        if proof_exists and bridge_exists:
            proof_value = _legacy_json(read_bytes(proof), proof)
            for field in ("task_id", "branch", "worktree", "authority_nonce", "receipt_sha256"):
                if proof_value.get(field) != bridge_value.get(field):
                    raise GitPrivateStateError(f"conflicting authority and proof identity: {namespace}")
            digest = hashlib.sha256(
                json.dumps(proof_value, sort_keys=True, separators=(",", ":")).encode("utf-8")
            ).hexdigest()
            if bridge_value.get("proof_sha256") != digest:
                raise GitPrivateStateError(f"source-recovery proof digest mismatch: {namespace}")


def _inspect_pairs(pairs: list[tuple[Path, Path]]) -> list[MigrationFile]:
    inspected: list[MigrationFile] = []
    for source, target in pairs:
        metadata = _require_legacy_record(source, "legacy private-state record")
        content = read_bytes(source, "legacy private-state record")
        _validate_legacy_content(source, content)
        _validate_authority_filename(source, content)
        try:
            target.lstat()
        except FileNotFoundError:
            pass
        except OSError as exc:
            raise GitPrivateStateError(f"cannot inspect canonical private-state record: {target}") from exc
        else:
            if read_bytes(target, "canonical private-state record") != content:
                raise GitPrivateStateError(f"conflicting private-state records: {source} and {target}")
        inspected.append(MigrationFile(
            source, target, (metadata.st_dev, metadata.st_ino),
            stat.S_IMODE(metadata.st_mode), content,
        ))
    return inspected


def _exclusive_publish(path: Path, content: bytes, mode: int) -> tuple[int, int]:
    parent_fd = _open_anchored_parent(path)
    temporary = f".migrate.{os.getpid()}.{secrets.token_hex(8)}"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(temporary, flags, 0o600, dir_fd=parent_fd)
        try:
            view = memoryview(content)
            while view:
                written = os.write(descriptor, view)
                if written <= 0:
                    raise OSError("short private-state write")
                view = view[written:]
            os.fchmod(descriptor, mode)
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        try:
            os.link(temporary, path.name, src_dir_fd=parent_fd, dst_dir_fd=parent_fd,
                    follow_symlinks=False)
            os.fsync(parent_fd)
        except FileExistsError:
            _require_canonical_file(path, "canonical private-state record")
            if read_bytes(path, "canonical private-state record") != content:
                raise GitPrivateStateError(f"conflicting canonical private-state record: {path}")
        published = os.stat(path.name, dir_fd=parent_fd, follow_symlinks=False)
        if (
            not stat.S_ISREG(published.st_mode)
            or published.st_uid != os.geteuid()
            or stat.S_IMODE(published.st_mode) != 0o600
        ):
            raise GitPrivateStateError(f"unsafe canonical private-state record: {path}")
        return published.st_dev, published.st_ino
    except GitPrivateStateError:
        raise
    except OSError as exc:
        raise GitPrivateStateError(f"cannot publish private-state record: {path}") from exc
    finally:
        try:
            os.unlink(temporary, dir_fd=parent_fd)
        except FileNotFoundError:
            pass
        finally:
            os.close(parent_fd)


def _exclusive_publish_direct(path: Path, content: bytes, mode: int) -> tuple[int, int]:
    """Restore an absent legacy record without a visible temporary artifact."""
    parent_fd = _open_anchored_parent(path)
    descriptor: int | None = None
    try:
        descriptor = os.open(
            path.name,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
            mode,
            dir_fd=parent_fd,
        )
        os.fchmod(descriptor, mode)
        view = memoryview(content)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise OSError("short private-state write")
            view = view[written:]
        os.fsync(descriptor)
        metadata = os.fstat(descriptor)
        os.fsync(parent_fd)
        return metadata.st_dev, metadata.st_ino
    except OSError as exc:
        raise GitPrivateStateError(f"cannot restore legacy private-state record: {path}") from exc
    finally:
        if descriptor is not None:
            os.close(descriptor)
        os.close(parent_fd)


def ensure_parent(path: Path) -> None:
    parts = path.parts
    indexes = [index for index, part in enumerate(parts) if part == NAMESPACE]
    if not indexes:
        # Legacy restoration may use only an already-existing validated parent.
        descriptor = _open_anchored_parent(path)
        os.close(descriptor)
        return
    marker = indexes[-1]
    boundary = Path(*parts[:marker])
    _ensure_namespace(boundary, tuple(parts[marker + 1:-1]))


def _state_namespace(path: Path) -> Path:
    """Return the namespace containing a canonical state path."""
    indexes = [i for i, part in enumerate(path.parts) if part == NAMESPACE]
    if not indexes:
        raise GitPrivateStateError(f"path is outside canonical private-state namespace: {path}")
    return Path(*path.parts[: indexes[-1] + 1])


def _legacy_equivalent(path: Path) -> tuple[Path, Path]:
    """Return the state root and canonical equivalent of one legacy path."""
    indexes = [i for i, part in enumerate(path.parts) if part == LEGACY_NAMESPACE]
    if not indexes:
        raise GitPrivateStateError(f"path is outside legacy private-state namespace: {path}")
    marker = indexes[-1]
    state_root = Path(*path.parts[:marker])
    relative = Path(*path.parts[marker + 1:])
    return state_root, state_root / NAMESPACE / relative


def exclusive_write_bytes(
    path: Path, content: bytes, mode: int = 0o600, *, _lock_held: bool = False
) -> tuple[int, int]:
    """Durably publish a new state record without replacing any existing object."""
    if NAMESPACE not in path.parts:
        state_root, canonical = _legacy_equivalent(path)
        _ensure_namespace(state_root, ())
        with _file_lock(state_root / NAMESPACE / "migration.lock", create=True, exact=True):
            if os.path.lexists(path):
                raise GitPrivateStateError(f"legacy private-state record already exists: {path}")
            target = canonical if not (state_root / LEGACY_NAMESPACE).exists() else path
            ensure_parent(target)
            if target == canonical:
                return _exclusive_publish(target, content, 0o600)
            return _exclusive_publish_direct(target, content, mode)
    ensure_parent(path)
    namespace = _state_namespace(path)
    if _lock_held:
        return _exclusive_publish(path, content, 0o600)
    with _file_lock(namespace / "migration.lock", create=True, exact=True):
        _recover_canonical_temps(Topology(namespace.parent, namespace.parent))
        return _exclusive_publish(path, content, 0o600)


def _identity_unlink(item: MigrationFile) -> None:
    if read_bytes(item.source, "legacy private-state record") != item.content:
        raise GitPrivateStateError(f"legacy private-state record changed: {item.source}")
    metadata = _require_legacy_record(item.source, "legacy private-state record")
    if (metadata.st_dev, metadata.st_ino) != item.identity:
        raise GitPrivateStateError(f"legacy private-state identity changed: {item.source}")
    parent_fd = _open_anchored_parent(item.source)
    try:
        current = os.stat(item.source.name, dir_fd=parent_fd, follow_symlinks=False)
        if (current.st_dev, current.st_ino) != item.identity or not stat.S_ISREG(current.st_mode):
            raise GitPrivateStateError(f"legacy private-state identity changed: {item.source}")
        os.unlink(item.source.name, dir_fd=parent_fd)
        os.fsync(parent_fd)
    except GitPrivateStateError:
        raise
    except OSError as exc:
        raise GitPrivateStateError(f"cannot remove legacy private-state record: {item.source}") from exc
    finally:
        os.close(parent_fd)


def _unlink_identity_path(path: Path, identity: tuple[int, int], what: str) -> None:
    parent_fd = _open_anchored_parent(path)
    try:
        metadata = os.stat(path.name, dir_fd=parent_fd, follow_symlinks=False)
        if ((metadata.st_dev, metadata.st_ino) != identity or
                not stat.S_ISREG(metadata.st_mode)):
            raise GitPrivateStateError(f"{what} identity changed: {path}")
        os.unlink(path.name, dir_fd=parent_fd)
        os.fsync(parent_fd)
    except GitPrivateStateError:
        raise
    except OSError as exc:
        raise GitPrivateStateError(f"cannot remove {what}: {path}") from exc
    finally:
        os.close(parent_fd)


def _handoff_cleanup_lock(
    legacy: Path, canonical: Path, expected_identity: tuple[int, int]
) -> tuple[int, int]:
    legacy_meta = _require_regular(legacy, "legacy cleanup lock")
    _require_owned_mode(legacy, legacy_meta, 0o600, "legacy cleanup lock")
    if (legacy_meta.st_dev, legacy_meta.st_ino) != expected_identity:
        raise GitPrivateStateError(f"legacy cleanup lock identity changed: {legacy}")
    legacy_parent_fd = _open_anchored_parent(legacy)
    canonical_parent_fd = _open_anchored_parent(canonical)
    try:
        legacy_fd = os.open(
            legacy.name,
            os.O_RDWR | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0),
            dir_fd=legacy_parent_fd,
        )
        try:
            opened = os.fstat(legacy_fd)
            if (opened.st_dev, opened.st_ino) != expected_identity:
                raise GitPrivateStateError(f"legacy cleanup lock identity changed: {legacy}")
            os.fchmod(legacy_fd, 0o600)
            os.fsync(legacy_fd)
        finally:
            os.close(legacy_fd)
    finally:
        os.close(legacy_parent_fd)
    try:
        try:
            canonical_meta = os.stat(
                canonical.name, dir_fd=canonical_parent_fd, follow_symlinks=False
            )
        except FileNotFoundError:
            canonical_meta = None
        if canonical_meta is not None:
            if (
                not stat.S_ISREG(canonical_meta.st_mode)
                or canonical_meta.st_uid != os.geteuid()
                or stat.S_IMODE(canonical_meta.st_mode) != 0o600
            ):
                raise GitPrivateStateError(f"unsafe canonical cleanup lock: {canonical}")
            if (canonical_meta.st_dev, canonical_meta.st_ino) != (
                expected_identity
            ):
                raise GitPrivateStateError(
                    "BLOCKED: legacy and canonical cleanup locks use different inodes; "
                    "automatic cutover cannot prove a single cleanup fence"
                )
        else:
            try:
                source_fd = _open_anchored_parent(legacy)
                try:
                    os.link(
                        legacy.name,
                        canonical.name,
                        src_dir_fd=source_fd,
                        dst_dir_fd=canonical_parent_fd,
                        follow_symlinks=False,
                    )
                finally:
                    os.close(source_fd)
                os.fsync(canonical_parent_fd)
                published = os.stat(
                    canonical.name, dir_fd=canonical_parent_fd, follow_symlinks=False
                )
                if (published.st_dev, published.st_ino) != expected_identity:
                    raise GitPrivateStateError(
                        f"legacy cleanup lock identity changed during handoff: {legacy}"
                    )
            except OSError as exc:
                raise GitPrivateStateError(
                    f"cannot establish cleanup lock handoff: {canonical}"
                ) from exc
    finally:
        os.close(canonical_parent_fd)
    return expected_identity


def _preflight_cleanup_lock_handoff(legacy: Path | None, canonical: Path) -> None:
    if legacy is None:
        return
    legacy_meta = _require_regular(legacy, "legacy cleanup lock")
    _require_owned_mode(legacy, legacy_meta, 0o600, "legacy cleanup lock")
    try:
        canonical_meta = canonical.lstat()
    except FileNotFoundError:
        return
    _require_canonical_file(canonical, "canonical cleanup lock")
    if (canonical_meta.st_dev, canonical_meta.st_ino) != (
        legacy_meta.st_dev, legacy_meta.st_ino
    ):
        raise GitPrivateStateError(
            "BLOCKED: legacy and canonical cleanup locks use different inodes; "
            "automatic cutover cannot prove a single cleanup fence"
        )


def _remove_known_empty_legacy_directories(layout: Topology) -> None:
    roots = [layout.common / LEGACY_NAMESPACE]
    if not layout.admin_is_common:
        roots.append(layout.admin / LEGACY_NAMESPACE)
    for root in roots:
        if _namespace_kind(root, foreign_regular=True) != "directory":
            continue
        for name in SHARED_DIRS:
            child = root / name
            try:
                _require_legacy_dir(child, "legacy private-state directory")
            except GitPrivateStateError:
                if child.exists() or child.is_symlink():
                    raise
                continue
            if not any(child.iterdir()):
                child.rmdir()
                descriptor = _open_dir(root)
                try:
                    os.fsync(descriptor)
                finally:
                    os.close(descriptor)
        if not any(root.iterdir()):
            root.rmdir()
            descriptor = _open_dir(root.parent)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)


@contextmanager
def _file_lock(path: Path, *, create: bool, nonblocking: bool = False, exact: bool = False):
    flags = os.O_RDWR | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    parent_fd = _open_anchored_parent(path)
    descriptor: int | None = None
    try:
        if create:
            try:
                descriptor = os.open(path.name, flags | os.O_CREAT | os.O_EXCL, 0o600, dir_fd=parent_fd)
                os.fchmod(descriptor, 0o600)
                os.fsync(parent_fd)
            except FileExistsError:
                descriptor = os.open(path.name, flags, dir_fd=parent_fd)
        else:
            descriptor = os.open(path.name, flags, dir_fd=parent_fd)
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise GitPrivateStateError(f"unsafe private-state lock: {path}")
        if metadata.st_uid != os.geteuid() or (exact and stat.S_IMODE(metadata.st_mode) != 0o600) or (
                not exact and (stat.S_IMODE(metadata.st_mode) & 0o022 or metadata.st_mode & 0o111 or
                               not (metadata.st_mode & 0o600))):
            raise GitPrivateStateError(f"unsafe private-state lock mode or ownership: {path}")
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | (fcntl.LOCK_NB if nonblocking else 0))
        except BlockingIOError as exc:
            raise GitPrivateStateError(f"BLOCKED: private-state lock is contended: {path}") from exc
    except GitPrivateStateError:
        if descriptor is not None:
            os.close(descriptor)
        os.close(parent_fd)
        raise
    except OSError as exc:
        if descriptor is not None:
            os.close(descriptor)
        os.close(parent_fd)
        raise GitPrivateStateError(f"cannot acquire private-state lock: {path}") from exc
    try:
        locked = os.fstat(descriptor)
        yield (locked.st_dev, locked.st_ino)
    finally:
        if descriptor is not None:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_UN)
            finally:
                os.close(descriptor)
        os.close(parent_fd)


def prepare(
    root: Path,
    *,
    admin: bool = False,
    common_dir: Path | None = None,
    admin_dir: Path | None = None,
    _legacy_lock_identity: tuple[int, int] | None = None,
    _allow_incomplete_recovery: bool = False,
) -> None:
    """Validate and migrate known legacy state before a state mutation."""
    layout = (
        Topology(common_dir.resolve(), (admin_dir or common_dir).resolve())
        if common_dir is not None
        else topology(root)
    )
    _validate_canonical(layout, allow_incomplete_recovery=_allow_incomplete_recovery)
    shared_pairs, legacy_lock = _scan_shared_legacy(layout)
    pairs = shared_pairs + (_scan_admin_legacy(layout) if admin else [])
    _inspect_pairs(pairs)  # preflight before creating even the migration lock
    if legacy_lock is not None and _legacy_lock_identity is None:
        with _file_lock(legacy_lock, create=False, nonblocking=True) as lock_identity:
            prepare(
                root,
                admin=admin,
                common_dir=layout.common,
                admin_dir=layout.admin,
                _legacy_lock_identity=lock_identity,
                _allow_incomplete_recovery=_allow_incomplete_recovery,
            )
        return

    _preflight_cleanup_lock_handoff(
        legacy_lock, layout.common / NAMESPACE / "cleanup.lock"
    )
    _ensure_namespace(layout.common, ())
    admin_lock_context = nullcontext()
    if admin and not layout.admin_is_common:
        _ensure_namespace(layout.admin, ())
        admin_lock_context = _file_lock(
            layout.admin / NAMESPACE / "migration.lock", create=True, exact=True
        )
    with _file_lock(
        layout.common / NAMESPACE / "migration.lock", create=True, exact=True
    ), admin_lock_context:
        # Repeat all classification under the migration lock.
        _recover_canonical_temps(layout, include_admin=admin)
        _validate_canonical(layout, allow_incomplete_recovery=_allow_incomplete_recovery)
        shared_pairs, _ = _scan_shared_legacy(layout)
        pairs = shared_pairs + (_scan_admin_legacy(layout) if admin else [])
        inspected = _inspect_pairs(pairs)
        for name in SHARED_DIRS:
            _ensure_namespace(layout.common, (name,))
        if admin:
            _ensure_namespace(layout.admin, ("automation-maintenance",))
        legacy_lock_identity = None
        if legacy_lock is not None:
            if _legacy_lock_identity is None:
                raise GitPrivateStateError("legacy cleanup lock is not held for handoff")
            legacy_lock_identity = _handoff_cleanup_lock(
                legacy_lock,
                layout.common / NAMESPACE / "cleanup.lock",
                _legacy_lock_identity,
            )
        for item in inspected:
            try:
                item.target.lstat()
            except FileNotFoundError:
                _exclusive_publish(item.target, item.content, 0o600)
        _validate_canonical(layout, allow_incomplete_recovery=_allow_incomplete_recovery)
        # No source is removed until every destination is durable and equivalent.
        for item in inspected:
            _require_canonical_file(item.target, "canonical private-state record")
            if read_bytes(item.target, "canonical private-state record") != item.content:
                raise GitPrivateStateError(f"canonical private-state record changed: {item.target}")
        for item in inspected:
            metadata = _require_legacy_record(item.source, "legacy private-state record")
            if (metadata.st_dev, metadata.st_ino) != item.identity or read_bytes(
                item.source, "legacy private-state record"
            ) != item.content:
                raise GitPrivateStateError(f"legacy private-state record changed: {item.source}")
        for item in inspected:
            _identity_unlink(item)
        if legacy_lock is not None and legacy_lock_identity is not None:
            _unlink_identity_path(legacy_lock, legacy_lock_identity, "legacy cleanup lock")
        _remove_known_empty_legacy_directories(layout)


def write_bytes(path: Path, content: bytes) -> None:
    """Atomically and durably replace one prepared canonical regular file."""
    namespace = _state_namespace(path)
    ensure_parent(path)
    with _file_lock(namespace / "migration.lock", create=True, exact=True):
        _recover_canonical_temps(Topology(namespace.parent, namespace.parent))
        _write_bytes_locked(path, content)


@contextmanager
def mutation_lock(root: Path, *, admin: bool = False):
    """Fence a caller-managed canonical transaction in global lock order."""
    layout = topology(root)
    _ensure_namespace(layout.common, ())
    admin_context = nullcontext()
    if admin and not layout.admin_is_common:
        _ensure_namespace(layout.admin, ())
        admin_context = _file_lock(
            layout.admin / NAMESPACE / "migration.lock", create=True, exact=True
        )
    with _file_lock(
        layout.common / NAMESPACE / "migration.lock", create=True, exact=True
    ), admin_context:
        _recover_canonical_temps(layout, include_admin=admin)
        yield


def _write_bytes_locked(path: Path, content: bytes) -> None:
    parent_fd = _open_anchored_parent(path)
    temporary = f".record.{os.getpid()}.{secrets.token_hex(8)}"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    try:
        try:
            current = os.stat(path.name, dir_fd=parent_fd, follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            if not stat.S_ISREG(current.st_mode):
                raise GitPrivateStateError(f"unsafe canonical private-state record: {path}")
        descriptor = os.open(temporary, flags, 0o600, dir_fd=parent_fd)
        try:
            os.fchmod(descriptor, 0o600)
            view = memoryview(content)
            while view:
                written = os.write(descriptor, view)
                if written <= 0:
                    raise OSError("short private-state write")
                view = view[written:]
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        os.replace(temporary, path.name, src_dir_fd=parent_fd, dst_dir_fd=parent_fd)
        os.fsync(parent_fd)
    except GitPrivateStateError:
        raise
    except OSError as exc:
        raise GitPrivateStateError(f"cannot write private-state record: {path}") from exc
    finally:
        try:
            os.unlink(temporary, dir_fd=parent_fd)
        except FileNotFoundError:
            pass
        finally:
            os.close(parent_fd)


def unlink(
    path: Path,
    *,
    expected_identity: tuple[int, int] | None = None,
    expected_content: bytes | None = None,
    _lock_held: bool = False,
) -> None:
    """Remove only a canonical regular file through its verified parent."""
    if NAMESPACE not in path.parts:
        # A pre-cutover consumer may need to consume or roll back a validated
        # historical authority before the upgraded implementation is committed.
        state_root, canonical = _legacy_equivalent(path)
        _ensure_namespace(state_root, ())
        with _file_lock(state_root / NAMESPACE / "migration.lock", create=True, exact=True):
            legacy_exists = os.path.lexists(path)
            canonical_exists = os.path.lexists(canonical)
            if legacy_exists and canonical_exists:
                if expected_content is None:
                    raise GitPrivateStateError(
                        f"legacy and canonical private-state records both exist: {path}"
                    )
                if read_bytes(path) != expected_content or read_bytes(canonical) != expected_content:
                    raise GitPrivateStateError(f"private-state record changed during migration: {path}")
                _unlink_locked(path, expected_identity=expected_identity)
                _unlink_locked(canonical)
                return
            if legacy_exists:
                _unlink_locked(path, expected_identity=expected_identity)
                return
            if not canonical_exists or expected_content is None:
                raise GitPrivateStateError(f"private-state record changed during migration: {path}")
            if read_bytes(canonical, "canonical private-state record") != expected_content:
                raise GitPrivateStateError(f"private-state record changed during migration: {path}")
            _unlink_locked(canonical)
        return
    namespace = _state_namespace(path)
    if _lock_held:
        _unlink_locked(path, expected_identity=expected_identity)
        return
    with _file_lock(namespace / "migration.lock", create=True, exact=True):
        _recover_canonical_temps(Topology(namespace.parent, namespace.parent))
        _unlink_locked(path, expected_identity=expected_identity)


def _unlink_locked(path: Path, *, expected_identity: tuple[int, int] | None = None) -> None:
    parent_fd = _open_anchored_parent(path)
    try:
        try:
            metadata = os.stat(path.name, dir_fd=parent_fd, follow_symlinks=False)
        except FileNotFoundError:
            return
        if not stat.S_ISREG(metadata.st_mode):
            raise GitPrivateStateError(f"unsafe canonical private-state record: {path}")
        if expected_identity is not None and (metadata.st_dev, metadata.st_ino) != expected_identity:
            raise GitPrivateStateError(f"private-state record identity changed: {path}")
        os.unlink(path.name, dir_fd=parent_fd)
        os.fsync(parent_fd)
    except GitPrivateStateError:
        raise
    except OSError as exc:
        raise GitPrivateStateError(f"cannot remove private-state record: {path}") from exc
    finally:
        os.close(parent_fd)


@contextmanager
def cleanup_lock(root: Path):
    """Cut over legacy cleanup locking, then acquire only the canonical lock."""
    layout = topology(root)
    _validate_canonical(layout)
    _, legacy = _scan_shared_legacy(layout)
    # Lock old consumers out while migrating cleanup/discard evidence.
    legacy_context = (_file_lock(legacy, create=False, nonblocking=True)
                      if legacy is not None else nullcontext())
    with legacy_context as legacy_identity:
        prepare(
            root,
            common_dir=layout.common,
            admin_dir=layout.admin,
            _legacy_lock_identity=legacy_identity,
        )
        if legacy is not None:
            # The open legacy descriptor now names the same inode as the
            # canonical path and remains held for the whole caller operation.
            yield
        else:
            with _file_lock(
                layout.common / NAMESPACE / "cleanup.lock", create=True, exact=True
            ):
                yield
