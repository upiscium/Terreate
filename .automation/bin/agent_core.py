#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import quote

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import task_lifecycle as lifecycle
import publication_metadata as publication
import task_contract
import git_private_state as private_state

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover
    tomllib = None

TASK_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
SAFE_CHECK_CONCLUSIONS = {"SUCCESS", "SKIPPED", "NEUTRAL"}


class AutomationError(RuntimeError):
    pass


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    check: bool = True,
    remove_env: tuple[str, ...] = (),
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    for name in [key for key in environment if key.startswith("GIT_")]:
        environment.pop(name, None)
    for name in remove_env:
        environment.pop(name, None)
    result = subprocess.run(
        command, cwd=cwd, text=True, capture_output=True, env=environment
    )
    if check and result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit {result.returncode}"
        raise AutomationError(f"{' '.join(command)}: {detail}")
    return result


def git(*args: str, cwd: Path | None = None, check: bool = True) -> str:
    return run(["git", *args], cwd=cwd, check=check).stdout.strip()


def gh(*args: str, cwd: Path | None = None) -> str:
    return run(["gh", *args], cwd=cwd, remove_env=("GH_REPO",)).stdout.strip()


def repo_root(cwd: Path | None = None) -> Path:
    return Path(git("rev-parse", "--show-toplevel", cwd=cwd)).resolve()


def common_git_dir(root: Path) -> Path:
    value = Path(git("rev-parse", "--git-common-dir", cwd=root))
    return value if value.is_absolute() else (root / value).resolve()


def current_branch(root: Path) -> str:
    branch = git("branch", "--show-current", cwd=root)
    if not branch:
        raise AutomationError("detached HEAD is not supported")
    return branch


def default_branch(root: Path) -> str:
    symbolic = git("symbolic-ref", "--quiet", "--short", "refs/remotes/origin/HEAD", cwd=root, check=False)
    if symbolic.startswith("origin/"):
        return symbolic.removeprefix("origin/")
    try:
        data = json.loads(gh("repo", "view", "--json", "defaultBranchRef", cwd=root))
        name = data.get("defaultBranchRef", {}).get("name")
        if name:
            return name
    except (AutomationError, json.JSONDecodeError):
        pass
    raise AutomationError("cannot resolve default branch; configure origin/HEAD or GitHub CLI access")


def validate_task(task: str) -> None:
    if not TASK_RE.fullmatch(task):
        raise AutomationError(f"invalid Task ID: {task!r}")


def validate_slug(slug: str) -> None:
    if not SLUG_RE.fullmatch(slug):
        raise AutomationError(f"invalid Task slug: {slug!r}")


def ensure_task_branch(root: Path, task: str) -> str:
    validate_task(task)
    branch = current_branch(root)
    if not (branch.startswith(f"task/{task}-") or branch.startswith(f"fix/{task}-")):
        raise AutomationError(f"current branch {branch!r} is not the Task branch for {task}")
    if branch == default_branch(root):
        raise AutomationError("Task operation refused on the default branch")
    return branch


def policy(root: Path) -> dict:
    path = root / ".automation" / "policy.toml"
    if tomllib is None:
        raise AutomationError("Python 3.11+ is required to parse policy.toml")
    try:
        return tomllib.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise AutomationError(f"missing policy: {path}") from exc


def matches_protected(path: str, patterns: list[str]) -> bool:
    for raw in patterns:
        if raw.endswith("/**") and (path == raw[:-3] or path.startswith(raw[:-2])):
            return True
        if path == raw:
            return True
    return False


def pending_paths(root: Path) -> list[str]:
    paths: set[str] = set()
    for args in (
        ("diff", "--name-only"),
        ("diff", "--cached", "--name-only"),
        ("ls-files", "--others", "--exclude-standard"),
    ):
        output = git(*args, cwd=root)
        paths.update(line for line in output.splitlines() if line)
    return sorted(paths)


def reject_unsafe_paths(root: Path, paths: list[str]) -> None:
    cfg = policy(root)
    protected = cfg.get("paths", {}).get("automation_core", [])
    secret_names = cfg.get("paths", {}).get("secret_patterns", [])
    bad_core = [path for path in paths if matches_protected(path, protected)]
    if bad_core:
        raise AutomationError("ordinary Task modifies Automation Core: " + ", ".join(bad_core))
    lowered = [(path, path.lower()) for path in paths]
    bad_secret = [path for path, low in lowered if any(token.lower() in low for token in secret_names)]
    if bad_secret:
        raise AutomationError("potential secret file in Task changes: " + ", ".join(bad_secret))
    if any(path == ".task-state" or path.startswith(".task-state/") for path in paths):
        raise AutomationError(".task-state must never be committed")


def ensure_task_state_excluded(root: Path) -> None:
    exclude = common_git_dir(root) / "info" / "exclude"
    exclude.parent.mkdir(parents=True, exist_ok=True)
    lines = exclude.read_text(encoding="utf-8").splitlines() if exclude.exists() else []
    if "/.task-state/" not in lines:
        with exclude.open("a", encoding="utf-8") as handle:
            if lines and lines[-1] != "":
                handle.write("\n")
            handle.write("/.task-state/\n")


def task_state_path(root: Path) -> Path:
    return root / ".task-state" / "task.md"


def write_task_state(worktree: Path, task: str, branch: str, base: str, base_revision: str) -> None:
    template = worktree / ".automation" / "templates" / "task-state.md"
    state_dir = worktree / ".task-state"
    state_dir.mkdir(parents=True, exist_ok=True)
    text = template.read_text(encoding="utf-8")
    replacements = {
        "@@TASK_ID@@": task,
        "@@BRANCH@@": branch,
        "@@WORKTREE@@": str(worktree),
        "@@BASE_BRANCH@@": base,
        "@@BASE_REVISION@@": base_revision,
    }
    for key, value in replacements.items():
        text = text.replace(key, value)
    (state_dir / "task.md").write_text(text, encoding="utf-8")


def task_start(root: Path, task: str, slug: str) -> None:
    validate_task(task)
    validate_slug(slug)
    base = default_branch(root)
    base_ref = f"refs/remotes/origin/{base}"
    base_revision = git("rev-parse", "--verify", base_ref, cwd=root, check=False) or git("rev-parse", base, cwd=root)
    branch = f"task/{task}-{slug}"
    worktree = root / ".worktrees" / f"{task}-{slug}"
    if worktree.exists():
        raise AutomationError(f"worktree path already exists: {worktree}")
    if git("show-ref", "--verify", "--quiet", f"refs/heads/{branch}", cwd=root, check=False) == "":
        result = run(["git", "show-ref", "--verify", "--quiet", f"refs/heads/{branch}"], cwd=root, check=False)
        if result.returncode == 0:
            raise AutomationError(f"branch already exists: {branch}")
    worktree.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "worktree", "add", "-b", branch, str(worktree), base_revision], cwd=root)
    ensure_task_state_excluded(worktree)
    write_task_state(worktree, task, branch, base, base_revision)
    print(json.dumps({"task": task, "branch": branch, "worktree": str(worktree), "base": base, "baseRevision": base_revision}))


def context(root: Path) -> dict:
    branch = current_branch(root)
    state = task_state_path(root)
    return {
        "repositoryRoot": str(root),
        "worktree": str(root),
        "branch": branch,
        "defaultBranch": default_branch(root),
        "taskState": str(state) if state.exists() else None,
    }


def doctor(root: Path) -> None:
    missing = [tool for tool in ("git", "gh", "just", "python3") if shutil.which(tool) is None]
    if missing:
        raise AutomationError("missing required tools: " + ", ".join(missing))
    if not (root / ".automation" / "policy.toml").is_file():
        raise AutomationError("missing .automation/policy.toml")
    if not (root / "just" / "project" / "mod.just").is_file():
        raise AutomationError("missing Project Adapter: just/project/mod.just")
    ensure_task_state_excluded(root)
    print("Agent Core doctor: PASS")


def status(root: Path, task: str) -> None:
    branch = ensure_task_branch(root, task)
    print(json.dumps({"task": task, "branch": branch, "status": git("status", "--short", cwd=root).splitlines()}))


def verify(root: Path, task: str) -> None:
    ensure_task_branch(root, task)
    head = git("rev-parse", "HEAD", cwd=root)
    status_before = git("status", "--porcelain", "--untracked-files=all", cwd=root)
    result = run(["just", "project::check"], cwd=root, check=False)
    status_after = git("status", "--porcelain", "--untracked-files=all", cwd=root)
    receipt = {
        "schema_version": 1,
        "task_id": task,
        "head": head,
        "clean_tracked_worktree": not status_before and not status_after,
        "worktree_stable": status_before == status_after,
        "project_check": {
            "command": ["just", "project::check"],
            "returncode": result.returncode,
            "executed_at": datetime.now(timezone.utc).isoformat(),
        },
    }
    try:
        task_contract.write_verification_receipt(
            root, (json.dumps(receipt, sort_keys=True) + "\n").encode()
        )
    except task_contract.ContractError as exc:
        raise AutomationError(str(exc)) from exc
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit {result.returncode}"
        raise AutomationError(f"just project::check: {detail}")
    if status_before != status_after:
        raise AutomationError("project::check changed tracked or untracked product files")
    print("Project verification: PASS")


def commit_task(root: Path, task: str, message: str) -> None:
    ensure_task_branch(root, task)
    paths = pending_paths(root)
    if not paths:
        raise AutomationError("no Task changes to commit")
    reject_unsafe_paths(root, paths)
    run(["git", "add", "--", *paths], cwd=root)
    run(["git", "diff", "--cached", "--check"], cwd=root)
    staged = git("diff", "--cached", "--name-only", cwd=root).splitlines()
    reject_unsafe_paths(root, staged)
    commit_message = message.strip() or f"task: {task}"
    if task not in commit_message:
        commit_message = f"{commit_message}\n\nTask: {task}"
    run(["git", "commit", "-m", commit_message], cwd=root)
    print(git("rev-parse", "HEAD", cwd=root))


def push_task(root: Path, task: str) -> None:
    branch = ensure_task_branch(root, task)
    run(["git", "push", "--set-upstream", "origin", f"HEAD:refs/heads/{branch}"], cwd=root)
    print(f"pushed origin/{branch}")


def canonical_repository(root: Path) -> str:
    path = root / ".task-state" / "contract.json"
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AutomationError(f"invalid canonical Task Contract metadata: {path}") from exc
    repository = value.get("repository") if isinstance(value, dict) else None
    if not isinstance(repository, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise AutomationError("canonical Task Contract repository is invalid")
    try:
        live = task_contract.repository_identity(root)
    except task_contract.ContractError as exc:
        raise AutomationError(str(exc)) from exc
    if live.casefold() != repository.casefold():
        raise AutomationError("live origin repository does not match the canonical Task Contract")
    return repository


def pr_for_branch(root: Path, branch: str, repository: str | None = None) -> dict | None:
    command = ["gh", "pr", "view", branch]
    if repository is not None:
        command.extend(["--repo", repository])
    command.extend(["--json", "number,title,body,headRefName,baseRefName,isDraft,isCrossRepository,state,headRefOid"])
    result = run(command, cwd=root, check=False)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit {result.returncode}"
        if "no pull requests found" in detail.lower():
            return None
        raise AutomationError(f"cannot resolve pull request for {branch}: {detail}")
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise AutomationError("invalid pull request data returned by GitHub") from exc
    return value


def remote_branch_head(root: Path, branch: str, repository: str) -> str:
    value = gh(
        "api",
        "--hostname",
        "github.com",
        f"repos/{repository}/git/ref/heads/{quote(branch, safe='')}",
        "--jq",
        ".object.sha",
        cwd=root,
    )
    if not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", value):
        raise AutomationError("remote Task branch HEAD is not a full immutable revision")
    return value


def _publication_context(root: Path, task: str) -> tuple[str, dict, str]:
    branch = ensure_task_branch(root, task)
    head = git("rev-parse", "HEAD", cwd=root)
    try:
        record = lifecycle.require_local_task(root, task)
        lifecycle.require_resolved_contract(record, task)
        status = lifecycle.state_status(lifecycle.state_path(root))
    except lifecycle.LifecycleError as exc:
        raise AutomationError(str(exc)) from exc
    repository = canonical_repository(root)
    return branch, {"record": record, "status": status, "repository": repository}, head


def pr_prepare(root: Path, task: str) -> None:
    branch, context, head = _publication_context(root, task)
    if context["status"] not in {"publication-ready", "draft-pr-created"}:
        raise AutomationError(f"publication metadata preparation requires publication-ready or draft-pr-created; found {context['status']}")
    state = lifecycle.state_path(root).read_text(encoding="utf-8")
    base_revision = re.search(r"(?m)^- Base revision: ([0-9a-fA-F]{40,64})$", state)
    if base_revision is None:
        raise AutomationError("Task State has no valid Base revision")
    paths = git("diff", "--name-only", f"{base_revision.group(1)}...{head}", cwd=root).splitlines()
    try:
        title, body = publication.canonical_metadata(root, task, head=head, changed_paths=paths)
        publication.write_metadata(root, title, body)
    except publication.PublicationMetadataError as exc:
        raise AutomationError(str(exc)) from exc
    print(json.dumps({"task": task, "branch": branch, "title": title, "body": str(root / '.task-state' / 'pr-body.md')}))


def _validated_local_metadata(root: Path, task: str, head: str) -> tuple[str, Path, str]:
    try:
        receipt = publication.verification_evidence(root, task, head)
        title, body = publication.read_and_validate_metadata(root, receipt=receipt)
        expected_title, expected_body = publication.canonical_metadata(
            root,
            task,
            head=head,
            changed_paths=git("diff", "--name-only", f"{_base_revision(root)}...{head}", cwd=root).splitlines(),
        )
    except publication.PublicationMetadataError as exc:
        raise AutomationError(str(exc)) from exc
    if title != expected_title or not publication.canonical_pr_body_matches(
        expected_body, body
    ):
        raise AutomationError("pull request metadata is stale; run agent::pr-prepare")
    return title, root / ".task-state" / "pr-body.md", body


def _base_revision(root: Path) -> str:
    text = lifecycle.state_path(root).read_text(encoding="utf-8")
    match = re.search(r"(?m)^- Base revision: ([0-9a-fA-F]{40,64})$", text)
    if match is None:
        raise AutomationError("Task State has no valid Base revision")
    return match.group(1)


def _validate_live_pr(pr: dict, *, branch: str, base: str, head: str, title: str, body: str, draft: bool) -> None:
    expected = {
        "headRefName": branch, "baseRefName": base, "headRefOid": head,
        "title": title, "isDraft": draft, "isCrossRepository": False, "state": "OPEN",
    }
    mismatches = [name for name, value in expected.items() if pr.get(name) != value]
    if not publication.canonical_pr_body_matches(body, pr.get("body")):
        mismatches.append("body")
    if mismatches:
        raise AutomationError("live pull request metadata is stale or inconsistent: " + ", ".join(mismatches))


def _validated_pr_number(pr: dict) -> int:
    number = pr.get("number")
    if not isinstance(number, int) or isinstance(number, bool) or number < 1:
        raise AutomationError("pull request identity is invalid")
    return number


def _validate_edit_target(pr: dict, *, branch: str, base: str, head: str) -> None:
    expected = {
        "headRefName": branch, "baseRefName": base, "headRefOid": head,
        "isDraft": True, "isCrossRepository": False, "state": "OPEN",
    }
    mismatches = [name for name, value in expected.items() if pr.get(name) != value]
    if (
        mismatches
        or not isinstance(pr.get("number"), int)
        or isinstance(pr.get("number"), bool)
    ):
        raise AutomationError("pull request repair target identity is invalid: " + ", ".join(mismatches or ["number"]))


def pr_create(root: Path, task: str) -> dict:
    verify(root, task)
    branch, context, head = _publication_context(root, task)
    if context["status"] != "publication-ready":
        raise AutomationError(f"pr-create requires publication-ready; found {context['status']}")
    repository = context["repository"]
    base = default_branch(root)
    title, body, body_text = _validated_local_metadata(root, task, head)
    existing = pr_for_branch(root, branch, repository)
    if existing is not None:
        if not isinstance(existing.get("number"), int) or isinstance(existing.get("number"), bool):
            raise AutomationError("pull request reconciliation target has an invalid number")
        _validate_live_pr(
            existing,
            branch=branch,
            base=base,
            head=head,
            title=title,
            body=body_text,
            draft=True,
        )
        if canonical_repository(root).casefold() != repository.casefold():
            raise AutomationError("repository identity changed during pull request reconciliation")
        confirmed = pr_for_branch(root, branch, repository)
        if not confirmed or confirmed.get("number") != existing["number"]:
            raise AutomationError("pull request identity changed during guarded reconciliation")
        _validate_live_pr(
            confirmed,
            branch=branch,
            base=base,
            head=head,
            title=title,
            body=body_text,
            draft=True,
        )
        try:
            lifecycle.mark_task_publication_state(context["record"], task, "publication-ready", "draft-pr-created")
        except lifecycle.LifecycleError as exc:
            raise AutomationError(str(exc)) from exc
        print(json.dumps(confirmed))
        return confirmed
    if remote_branch_head(root, branch, repository) != head:
        raise AutomationError("remote Task branch does not match the exact publication HEAD")
    gh("pr", "create", "--repo", repository, "--draft", "--base", base, "--head", branch, "--title", title, "--body-file", str(body), cwd=root)
    if canonical_repository(root).casefold() != repository.casefold():
        raise AutomationError("repository identity changed during pull request creation")
    pr = pr_for_branch(root, branch, repository)
    if not pr:
        raise AutomationError("created pull request cannot be re-read")
    _validate_live_pr(pr, branch=branch, base=base, head=head, title=title, body=body_text, draft=True)
    try:
        lifecycle.mark_task_publication_state(context["record"], task, "publication-ready", "draft-pr-created")
    except lifecycle.LifecycleError as exc:
        raise AutomationError(str(exc)) from exc
    print(json.dumps(pr))
    return pr


def pr_edit(root: Path, task: str, expected_pr_number: int | None = None) -> None:
    verify(root, task)
    branch, context, head = _publication_context(root, task)
    if context["status"] not in {"publication-ready", "draft-pr-created"}:
        raise AutomationError(f"pr-edit requires publication-ready or draft-pr-created; found {context['status']}")
    repository = context["repository"]
    if expected_pr_number is not None and (
        not isinstance(expected_pr_number, int)
        or isinstance(expected_pr_number, bool)
        or expected_pr_number < 1
    ):
        raise AutomationError("expected pull request number is invalid")
    pr = pr_for_branch(root, branch, repository)
    if not pr:
        raise AutomationError(f"no pull request for {branch}")
    if expected_pr_number is not None:
        actual_pr_number = pr.get("number")
        if (
            not isinstance(actual_pr_number, int)
            or isinstance(actual_pr_number, bool)
            or actual_pr_number != expected_pr_number
        ):
            raise AutomationError("pull request repair target identity changed before mutation")
    _validate_edit_target(pr, branch=branch, base=default_branch(root), head=head)
    title, body, body_text = _validated_local_metadata(root, task, head)
    gh("pr", "edit", str(pr["number"]), "--repo", repository, "--title", title, "--body-file", str(body), cwd=root)
    if canonical_repository(root).casefold() != repository.casefold():
        raise AutomationError("repository identity changed during pull request repair")
    updated = pr_for_branch(root, branch, repository)
    if not updated or updated.get("number") != pr.get("number"):
        raise AutomationError("pull request identity changed during guarded repair")
    _validate_live_pr(updated, branch=branch, base=default_branch(root), head=head, title=title, body=body_text, draft=True)
    if context["status"] == "publication-ready":
        try:
            lifecycle.mark_task_publication_state(context["record"], task, "publication-ready", "draft-pr-created")
        except lifecycle.LifecycleError as exc:
            raise AutomationError(str(exc)) from exc
    print(f"updated PR #{pr['number']}")


def pr_ready(root: Path, task: str, expected_pr_number: int | None = None) -> None:
    if (
        expected_pr_number is not None
        and (
            not isinstance(expected_pr_number, int)
            or isinstance(expected_pr_number, bool)
            or expected_pr_number < 1
        )
    ):
        raise AutomationError("expected pull request number is invalid")
    verify(root, task)
    branch, context, head = _publication_context(root, task)
    if context["status"] != "draft-pr-created":
        raise AutomationError(f"pr-ready requires draft-pr-created; found {context['status']}")
    title, _, body = _validated_local_metadata(root, task, head)
    repository = context["repository"]
    pr = pr_for_branch(root, branch, repository)
    if not pr:
        raise AutomationError(f"no pull request for {branch}")
    number = pr["number"]
    base = None
    if expected_pr_number is not None:
        number = _validated_pr_number(pr)
        if number != expected_pr_number:
            raise AutomationError("pull request identity changed before mutation")
        base = default_branch(root)

    def require_guarded_context() -> None:
        verify(root, task)
        guarded_branch, guarded_context, guarded_head = _publication_context(root, task)
        if (
            guarded_branch != branch
            or guarded_head != head
            or guarded_context["repository"].casefold() != repository.casefold()
            or guarded_context["status"] != context["status"]
            or guarded_context["record"] != context["record"]
        ):
            raise AutomationError("Task publication context changed during guarded readiness")
        guarded_base = default_branch(root)
        if guarded_base != base:
            raise AutomationError("repository default branch changed during guarded readiness")
        guarded_title, _, guarded_body = _validated_local_metadata(root, task, guarded_head)
        if guarded_title != title or not publication.canonical_pr_body_matches(body, guarded_body):
            raise AutomationError("local publication metadata changed during guarded readiness")

    if pr.get("isDraft"):
        _validate_live_pr(pr, branch=branch, base=base or default_branch(root), head=head, title=title, body=body, draft=True)
        if expected_pr_number is not None:
            require_guarded_context()
            before_ready = pr_for_branch(root, branch, repository)
            if not before_ready or _validated_pr_number(before_ready) != number:
                raise AutomationError("pull request identity changed before mutation")
            _validate_live_pr(before_ready, branch=branch, base=base, head=head, title=title, body=body, draft=True)
        gh("pr", "ready", str(number), "--repo", repository, cwd=root)
        if canonical_repository(root).casefold() != repository.casefold():
            raise AutomationError("repository identity changed while marking pull request ready")
        ready = pr_for_branch(root, branch, repository)
        if not ready or ready.get("number") != number:
            raise AutomationError("pull request identity changed while marking ready")
        if expected_pr_number is not None:
            _validated_pr_number(ready)
        _validate_live_pr(ready, branch=branch, base=base or default_branch(root), head=head, title=title, body=body, draft=False)
    else:
        # Reconcile an earlier successful GitHub readiness write whose local
        # lifecycle transition was interrupted.
        _validate_live_pr(pr, branch=branch, base=base or default_branch(root), head=head, title=title, body=body, draft=False)
    if expected_pr_number is not None:
        require_guarded_context()
        before_transition = pr_for_branch(root, branch, repository)
        if not before_transition or _validated_pr_number(before_transition) != number:
            raise AutomationError("pull request identity changed before lifecycle transition")
        _validate_live_pr(before_transition, branch=branch, base=base, head=head, title=title, body=body, draft=False)
    try:
        lifecycle.mark_task_publication_state(context["record"], task, "draft-pr-created", "integration-pending")
    except lifecycle.LifecycleError as exc:
        raise AutomationError(str(exc)) from exc
    print(f"PR #{number} marked ready")


def cleanup(root: Path, task: str) -> None:
    validate_task(task)
    branch = current_branch(root)
    if task not in branch:
        raise AutomationError("cleanup must run from the Task worktree")
    pr = pr_for_branch(root, branch)
    if not pr or pr.get("state") != "MERGED":
        raise AutomationError("cleanup refused until the Task PR is merged")
    print("cleanup must be executed by the Main worktree in the lifecycle extension (#8)")


def pr_details(root: Path, pr: str) -> dict:
    data = json.loads(gh("pr", "view", pr, "--json", "number,title,body,baseRefName,headRefName,headRefOid,isDraft,isCrossRepository,mergeCommit,mergeable,statusCheckRollup,state", cwd=root))
    return data


def validate_integration(root: Path, pr: str) -> dict:
    data = pr_details(root, pr)
    if data["baseRefName"] != default_branch(root):
        raise AutomationError("PR base is not the repository default branch")
    if data["isDraft"]:
        raise AutomationError("Draft PR cannot be merged")
    if data.get("mergeable") != "MERGEABLE":
        raise AutomationError(f"PR is not mergeable: {data.get('mergeable')}")
    failures = []
    for check in data.get("statusCheckRollup") or []:
        conclusion = check.get("conclusion")
        status = check.get("status")
        if status and status != "COMPLETED":
            failures.append(check.get("name") or check.get("context") or "pending check")
        elif conclusion and conclusion not in SAFE_CHECK_CONCLUSIONS:
            failures.append(check.get("name") or check.get("context") or conclusion)
    if failures:
        raise AutomationError("required checks are not successful: " + ", ".join(failures))
    return data


def integration_checkpoint(root: Path, pr: str) -> Path:
    try:
        return private_state.integration_checkpoint(root, pr)
    except private_state.GitPrivateStateError as exc:
        raise AutomationError(str(exc)) from exc


def integrate_check(root: Path, pr: str) -> None:
    data = validate_integration(root, pr)
    try:
        private_state.prepare(root)
        checkpoint = integration_checkpoint(root, pr)
        private_state.write_bytes(checkpoint, (data["headRefOid"] + "\n").encode("utf-8"))
    except private_state.GitPrivateStateError as exc:
        raise AutomationError(str(exc)) from exc
    print(json.dumps({"pr": data["number"], "head": data["headRefOid"], "status": "verified"}))


def integrate_merge(root: Path, pr: str) -> None:
    checkpoint = integration_checkpoint(root, pr)
    if not checkpoint.exists() or checkpoint.is_symlink() or not checkpoint.is_file():
        raise AutomationError("run integrate::check before merge")
    try:
        expected = private_state.read_bytes(checkpoint, "integration checkpoint").decode("utf-8").strip()
    except private_state.GitPrivateStateError as exc:
        raise AutomationError(str(exc)) from exc
    except UnicodeDecodeError as exc:
        raise AutomationError("integration checkpoint is invalid") from exc
    data = validate_integration(root, pr)
    if data["headRefOid"] != expected:
        raise AutomationError(f"PR head moved after integration check: expected {expected}, got {data['headRefOid']}")
    gh("pr", "merge", pr, "--squash", "--match-head-commit", expected, cwd=root)
    print(f"merged PR #{pr} at {expected}")


def validate_pr_number(pr: str) -> int:
    if not re.fullmatch(r"[1-9][0-9]*", pr):
        raise AutomationError(f"invalid pull request number: {pr!r}")
    return int(pr)


def prs_for_branch(root: Path, branch: str) -> list[dict]:
    try:
        value = json.loads(
            gh(
                "pr",
                "list",
                "--state",
                "all",
                "--head",
                branch,
                "--limit",
                "100",
                "--json",
                "number,headRefName,baseRefName",
                cwd=root,
            )
        )
    except json.JSONDecodeError as exc:
        raise AutomationError("invalid pull request list returned by GitHub") from exc
    if not isinstance(value, list):
        raise AutomationError("invalid pull request list returned by GitHub")
    return [item for item in value if item.get("headRefName") == branch]


def merged_pr_evidence(root: Path, task: str, pr: str) -> tuple[lifecycle.WorktreeRecord, dict]:
    requested = validate_pr_number(pr)
    try:
        lifecycle.require_main_worktree(root)
        record = lifecycle.worktree_for_task(root, task)
        lifecycle.assert_task_identity(record, task)
        status = lifecycle.state_status(lifecycle.state_path(record.path))
    except lifecycle.LifecycleError as exc:
        raise AutomationError(str(exc)) from exc
    if status not in {"integration-pending", "merged"}:
        raise AutomationError(
            "post-merge finalization requires Task status integration-pending or merged; "
            f"found {status}"
        )
    branch = record.branch
    if branch is None:
        raise AutomationError("registered Task worktree is detached")
    data = pr_details(root, pr)
    base = default_branch(root)
    if data.get("number") != requested:
        raise AutomationError("GitHub returned a different pull request")
    if data.get("state") != "MERGED":
        raise AutomationError("pull request is not merged")
    if data.get("headRefName") != branch:
        raise AutomationError("pull request head does not match the registered Task branch")
    if data.get("baseRefName") != base:
        raise AutomationError("pull request base is not the repository default branch")
    if data.get("isCrossRepository"):
        raise AutomationError("cross-repository pull requests cannot finalize a local Task")
    merge_oid = (data.get("mergeCommit") or {}).get("oid")
    if not isinstance(merge_oid, str) or not re.fullmatch(r"[0-9a-fA-F]{40,64}", merge_oid):
        raise AutomationError("merged pull request has no valid merge commit identity")
    matches = prs_for_branch(root, branch)
    if len(matches) != 1 or matches[0].get("number") != requested:
        raise AutomationError("Task pull request identity is missing or ambiguous")
    return record, data


def merge_commit_is_ancestor(root: Path, merge_oid: str, revision: str) -> bool:
    if not re.fullmatch(r"[0-9a-fA-F]{40,64}", merge_oid):
        return False
    return run(
        ["git", "merge-base", "--is-ancestor", merge_oid, revision],
        cwd=root,
        check=False,
    ).returncode == 0


def integrate_finalize(root: Path, task: str, pr: str) -> None:
    validate_task(task)
    record, evidence = merged_pr_evidence(root, task, pr)
    merge_oid = evidence["mergeCommit"]["oid"]
    try:
        synchronized = lifecycle.synchronize_default_branch(root)
    except lifecycle.LifecycleError as exc:
        raise AutomationError(str(exc)) from exc
    revision = synchronized["revision"]
    if not merge_commit_is_ancestor(root, merge_oid, revision):
        raise AutomationError(
            "GitHub merge commit is not present in the synchronized default branch"
        )

    # Re-read GitHub evidence after the fetch/update boundary before granting
    # terminal-state authority.
    current_record, current = merged_pr_evidence(root, task, pr)
    def fingerprint(value: dict) -> tuple:
        return (
            value.get("number"),
            value.get("state"),
            value.get("headRefName"),
            value.get("baseRefName"),
            (value.get("mergeCommit") or {}).get("oid"),
            value.get("isCrossRepository"),
        )
    if current_record != record or fingerprint(current) != fingerprint(evidence):
        raise AutomationError("pull request or Task identity changed during finalization")
    if not merge_commit_is_ancestor(root, merge_oid, revision):
        raise AutomationError("merge identity changed during finalization")
    try:
        lifecycle.require_synchronized_default_branch_revision(
            root, synchronized["branch"], revision
        )
        outcome = lifecycle.mark_task_merged_from_integration(record, task)
    except lifecycle.LifecycleError as exc:
        raise AutomationError(str(exc)) from exc
    print(
        json.dumps(
            {
                "task": task,
                "pr": evidence["number"],
                "branch": synchronized["branch"],
                "revision": revision,
                "mergeCommit": merge_oid,
                "status": outcome,
            }
        )
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    scope = parser.add_subparsers(dest="scope", required=True)
    agent = scope.add_parser("agent")
    agent_cmd = agent.add_subparsers(dest="command", required=True)
    for name in ("doctor", "context"):
        agent_cmd.add_parser(name)
    start = agent_cmd.add_parser("task-start"); start.add_argument("task"); start.add_argument("slug")
    for name in ("status", "verify", "push", "pr-prepare", "pr-create", "pr-edit", "pr-ready", "cleanup"):
        p = agent_cmd.add_parser(name); p.add_argument("task")
    commit = agent_cmd.add_parser("commit"); commit.add_argument("task"); commit.add_argument("message", nargs="?", default="")
    integrate = scope.add_parser("integrate")
    integrate_cmd = integrate.add_subparsers(dest="command", required=True)
    for name in ("check", "merge"):
        p = integrate_cmd.add_parser(name); p.add_argument("pr")
    finalize = integrate_cmd.add_parser("finalize")
    finalize.add_argument("task")
    finalize.add_argument("pr")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        root = repo_root()
        if args.scope == "agent":
            actions = {
                "doctor": lambda: doctor(root),
                "context": lambda: print(json.dumps(context(root), indent=2)),
                "task-start": lambda: task_start(root, args.task, args.slug),
                "status": lambda: status(root, args.task),
                "verify": lambda: verify(root, args.task),
                "commit": lambda: commit_task(root, args.task, args.message),
                "push": lambda: push_task(root, args.task),
                "pr-prepare": lambda: pr_prepare(root, args.task),
                "pr-create": lambda: pr_create(root, args.task),
                "pr-edit": lambda: pr_edit(root, args.task),
                "pr-ready": lambda: pr_ready(root, args.task),
                "cleanup": lambda: cleanup(root, args.task),
            }
        else:
            actions = {
                "check": lambda: integrate_check(root, args.pr),
                "merge": lambda: integrate_merge(root, args.pr),
                "finalize": lambda: integrate_finalize(root, args.task, args.pr),
            }
        actions[args.command]()
        return 0
    except AutomationError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
