# KAGARI development workflow

Terreate keeps no embedded agent lifecycle or Task Orchestrator. Its ordinary
build and test entrypoints remain `just project::configure`,
`just project::build`, `just project::test`, and `just project::check`.

KAGARI v0.1.0 is an **external installed utility**, not a required library
dependency. The current qualified profile is a bounded, isolated Work Unit;
it clones a pinned, committed ordinary Git root into a private no-remote
repository and does not write directly to Terreate, push a branch, create
a pull request or perform merging. General production activation and external
publication are not attested by the utility.

Each Work Unit needs a strict JSON TaskSpec v1 with an exact committed
`base_revision`, only explicitly allowed files (which must not be KAGARI's
protected configuration paths), a `tests` array, measurable acceptance and
two native child agents. A typical host test uses
`{"argv":["just","project::check"],"cwd":"."}`; KAGARI's implementation
and tests must remain inside the permitted isolated clone.

Use the installed binary:

```sh
kagari-v010-ga doctor --project-root "$PWD" --state-home "$HOME/.local/state"
kagari-v010-ga prepare-task --project-root "$PWD" --state-home "$HOME/.local/state" \
  --task-spec /path/to/approved-task-spec.json --opencode /absolute/path/to/opencode \
  --repository upiscium/Terreate --task 347
```

Inspect the returned job ID using `show-task --job-id <ID>`.
`approve-task` requires a **local foreground TTY** and its exact per-job
consent phrase; separate native Human Asks are required where indicated.
Only after these are explicitly granted can `run-task --job-id <ID>` execute.
Never simulate or pre-authorize the foreground consent in automation.
Retrieve the bounded result and Product patch, verify the exact source/base
and apply it through an independently reviewed human-controlled integration
flow. A successful private Work Unit is not a published product change.

The old Task #270 uncommitted Logical Device/Queue work must be brought forward
separately from its exact frozen source manifest. Do not delete that dirty
worktree or regard the pre-existing Draft PR as containing the unpublished
five-file correction. No implicit migration of old Agent Core Task State is
supported.
