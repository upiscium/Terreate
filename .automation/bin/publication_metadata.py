#!/usr/bin/env python3
"""Prepare and validate evidence-backed pull request metadata."""

from __future__ import annotations

import json
import re
from pathlib import Path

import task_contract
import task_lifecycle


class PublicationMetadataError(RuntimeError):
    pass


PLACEHOLDER_PATTERNS = (
    r"@@[A-Z0-9_]+@@",
    r"\bTBD\b",
    r"Describe the implemented Task outcome\.",
    r"Task-specific criteria copied from `?\.task-state/task\.md`?",
    r"Define Task-specific acceptance criteria",
)
NOT_RUN_RE = re.compile(r"(?im)^.*(?:NOT[ _-]?RUN|not run).*$")


def _sections(text: str) -> dict[str, str]:
    matches = list(re.finditer(r"(?m)^## (.+?)\s*$", text))
    return {
        match.group(1): text[match.end() : matches[index + 1].start() if index + 1 < len(matches) else len(text)].strip()
        for index, match in enumerate(matches)
    }


def _content_lines(value: str) -> list[str]:
    ignored = {"none yet.", "none yet", "none recorded", "none recorded yet."}
    return [line for line in value.splitlines() if line.strip() and line.strip().lower().lstrip("- ") not in ignored]


def _identity(text: str, name: str) -> str:
    match = re.search(rf"(?m)^- {re.escape(name)}: (.+)$", text)
    if match is None or not match.group(1).strip():
        raise PublicationMetadataError(f"Task State is missing {name}")
    return match.group(1).strip()


def _current_state_value(text: str, name: str) -> str:
    match = re.search(rf"(?m)^- {re.escape(name)}: (.+)$", text)
    if match is None or not match.group(1).strip():
        raise PublicationMetadataError(f"Task State is missing Current state {name}")
    return match.group(1).strip()


def _is_none_value(value: str) -> bool:
    return value.casefold().rstrip(".") in {"none", "none recorded"}


def _requirements(lines: list[str]) -> list[str]:
    requirements = []
    for line in lines:
        value = re.sub(r"^-\s*\[[ xX]\]\s*", "", line.strip())
        value = value.removeprefix("- ").strip()
        if value:
            requirements.append(f"- Requirement: {value}")
    return requirements


def _read_json(path: Path) -> dict | None:
    if not path.exists():
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PublicationMetadataError(f"invalid persisted evidence: {path}") from exc
    if not isinstance(value, dict):
        raise PublicationMetadataError(f"invalid persisted evidence: {path}")
    return value


def verification_evidence(root: Path, task: str, head: str) -> dict:
    receipt = _read_json(root / ".task-state" / "verification.json")
    if receipt is None:
        raise PublicationMetadataError("missing persisted project verification evidence")
    if set(receipt) != {"schema_version", "task_id", "head", "clean_tracked_worktree", "worktree_stable", "project_check"} or receipt["schema_version"] != 1:
        raise PublicationMetadataError("invalid project verification evidence schema")
    check = receipt.get("project_check")
    if receipt.get("task_id") != task:
        raise PublicationMetadataError("project verification evidence belongs to another Task")
    if receipt.get("head") != head or not isinstance(check, dict):
        raise PublicationMetadataError("project verification evidence is stale")
    if check.get("command") != ["just", "project::check"] or check.get("returncode") != 0:
        raise PublicationMetadataError("project::check has no persisted PASS evidence")
    if receipt.get("clean_tracked_worktree") is not True or receipt.get("worktree_stable") is not True:
        raise PublicationMetadataError("project verification is not bound to a clean stable worktree")
    if not isinstance(check.get("executed_at"), str) or not check["executed_at"]:
        raise PublicationMetadataError("project verification evidence has no execution time")
    return receipt


def completed_reviews(root: Path, task: str) -> list[str]:
    value = _read_json(root / ".task-state" / "work-units.json")
    if value is None:
        return []
    if value.get("task_id") != task or not isinstance(value.get("units"), dict):
        raise PublicationMetadataError("Work Unit evidence does not match the Task")
    effective: dict[str, tuple[int, str, dict]] = {}
    for identifier, unit in value["units"].items():
        if not isinstance(unit, dict) or unit.get("requested_role") not in {"reviewer", "security-reviewer"}:
            continue
        sequence = (
            task_lifecycle.canonical_work_unit_sequence(task, identifier)
            if isinstance(identifier, str)
            else None
        )
        if sequence is None:
            raise PublicationMetadataError(
                f"review Work Unit has no canonical sequence: {identifier}"
            )
        role = unit["requested_role"]
        if role not in effective or sequence > effective[role][0]:
            effective[role] = (sequence, identifier, unit)

    reviews = []
    for role in ("reviewer", "security-reviewer"):
        if role not in effective:
            continue
        _, identifier, unit = effective[role]
        if unit.get("state") != "completed":
            raise PublicationMetadataError(
                f"required {role} Work Unit is not completed: {identifier}"
            )
        transitions = unit.get("transitions")
        digest = transitions[-1].get("evidence_sha256") if isinstance(transitions, list) and transitions else None
        if isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{64}", digest):
            reviews.append(f"- `{identifier}` — `{role}` — completed — evidence `{digest}`")
    return reviews


def canonical_metadata(root: Path, task: str, *, head: str, changed_paths: list[str]) -> tuple[str, str]:
    state_path = root / ".task-state" / "task.md"
    try:
        text = state_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise PublicationMetadataError(f"missing Task State: {state_path}") from exc
    if _identity(text, "Task ID") != task:
        raise PublicationMetadataError("Task State identity does not match requested Task")
    sections = _sections(text)
    purpose = _content_lines(sections.get("Purpose", ""))
    criteria = _content_lines(sections.get("Acceptance criteria", ""))
    if not purpose or not criteria:
        raise PublicationMetadataError("Task purpose and acceptance criteria must be resolved")
    requirements = _requirements(criteria)
    if not requirements:
        raise PublicationMetadataError("Task acceptance criteria contain no authoritative requirements")
    blockers = _current_state_value(sections.get("Current state", ""), "Blockers")
    unverified = _current_state_value(sections.get("Current state", ""), "Unverified")
    risks = []
    if not _is_none_value(blockers):
        risks.append(f"- Blockers: {blockers}")
    if not _is_none_value(unverified):
        risks.append(f"- Unverified: {unverified}")
    if not risks:
        risks = ["- None recorded."]
    summary = purpose[0].lstrip("- ").strip()
    verification = verification_evidence(root, task, head)
    title = f"{task}: {summary}"
    changes = [f"- `{path.replace('`', '')}`" for path in changed_paths] or ["- No tracked changes recorded."]
    reviews = completed_reviews(root, task)
    if not any("— `reviewer` — completed" in review for review in reviews):
        raise PublicationMetadataError(
            "publication requires a completed reviewer Work Unit"
        )
    followups = _content_lines(sections.get("Follow-up Task candidates", "")) or ["- None recorded."]
    body = "\n".join(
        [
            "## Summary", "", f"Task: {task}", "", *purpose,
            "", "## Changed paths", "", *changes,
            "", "## Acceptance criteria", "",
            "The following are authoritative Task requirements; completion evidence is reported separately under Validation and Reviews.",
            "", *requirements,
            "", "## Validation", "",
            f"- `just project::check`: PASS at `{head}` (persisted executed evidence)",
            "", "## Reviews", "", *(reviews or ["- None recorded."]),
            "", "## Risks and unverified areas", "", *risks,
            "", "## Follow-up Tasks", "", *followups, "",
        ]
    )
    validate_metadata(title, body, receipt=verification)
    return title, body


def validate_metadata(title: str, body: str, *, receipt: dict | None = None) -> None:
    if not title.strip() or not body.strip():
        raise PublicationMetadataError("pull request title and body must be non-empty")
    combined = title + "\n" + body
    for pattern in PLACEHOLDER_PATTERNS:
        if re.search(pattern, combined, re.IGNORECASE):
            raise PublicationMetadataError("unresolved pull request publication placeholder")
    not_run = NOT_RUN_RE.findall(body)
    if not_run:
        if receipt and receipt.get("project_check", {}).get("returncode") == 0:
            raise PublicationMetadataError("pull request metadata contradicts persisted PASS evidence with NOT RUN")
        raise PublicationMetadataError("unresolved NOT RUN publication metadata")
    required = ("## Summary", "## Acceptance criteria", "## Validation", "## Risks and unverified areas", "## Follow-up Tasks")
    if any(heading not in body for heading in required):
        raise PublicationMetadataError("pull request body is missing required sections")


def canonical_pr_body_matches(canonical: str, live: str | None) -> bool:
    """Compare PR bodies while tolerating only one transport-level terminal LF."""

    if not isinstance(live, str) or canonical.endswith("\r\n") or live.endswith(
        "\r\n"
    ):
        return False

    def without_transport_lf(body: str) -> str:
        if body.endswith("\n") and not body.endswith("\n\n"):
            return body[:-1]
        return body

    return without_transport_lf(canonical) == without_transport_lf(live)


def write_metadata(root: Path, title: str, body: str) -> None:
    try:
        task_contract.write_publication_metadata(
            root,
            (title + "\n").encode(),
            body.encode(),
        )
    except task_contract.ContractError as exc:
        raise PublicationMetadataError(str(exc)) from exc


def read_and_validate_metadata(root: Path, *, receipt: dict | None = None) -> tuple[str, str]:
    title_path = root / ".task-state" / "pr-title.txt"
    body_path = root / ".task-state" / "pr-body.md"
    try:
        title = title_path.read_text(encoding="utf-8")
        if title.endswith("\n"):
            title = title[:-1]
        body = body_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise PublicationMetadataError("run agent::pr-prepare before publication") from exc
    validate_metadata(title, body, receipt=receipt)
    return title, body
