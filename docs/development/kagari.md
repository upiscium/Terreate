# KAGARI development workflow

Terreate has no embedded Agent Core lifecycle or Task Orchestrator. Ordinary
build and test commands remain `just project::configure`,
`just project::build`, `just project::test`, and `just project::check`.

**Use KAGARI v0.1.0 as the preferred external workflow for ordinary feature
work**, subject to a scoped, explicitly approved TaskSpec. KAGARI is not a
runtime dependency of Terreate and does not belong in every project devShell.
Its currently verified profile is `GA_BOUNDED_PROFILE_QUALIFIED`: a private,
independent, no-remote Git Work Unit cloned from an exact committed revision.
Its native component is still `TEST_ONLY_UNQUALIFIED`; general production
activation and external publication attestation are not established. KAGARI
does not edit the attached Project directly, push its branch, open PRs or merge.

## Host setup and read-only preflight

Install/manage KAGARI and OpenCode **outside** the Terreate project (for
example with Home Manager). Do not assume the `kagari-v010-ga` executable is
already on the ordinary shell PATH: the existing Nix-based installation on
the development host uses an external `ga-v0.1.0-gcroot`, not an executable
exported by Terreate's devShell. Supply `KAGARI_BIN` as an absolute trusted
binary path if your setup uses another package location.

From the ordinary, attached Terreate Git checkout (a real `.git` directory,
**not** a linked `.worktrees/*` checkout), run this block in the same shell
that will later prepare the Task:

```sh
set -eu
PROJECT_ROOT="$(pwd -P)"
if [ ! -d "$PROJECT_ROOT/.git" ] || [ -L "$PROJECT_ROOT/.git" ]; then
  printf '%s\n' "KAGARI requires an ordinary Git root, not a linked worktree" >&2
  exit 1
fi

if [ -z "${KAGARI_BIN:-}" ]; then
  if command -v kagari-v010-ga >/dev/null 2>&1; then
    KAGARI_BIN="$(command -v kagari-v010-ga)"
  elif [ -L "$HOME/.local/state/kagari-v010-ga-stable/ga-v0.1.0-gcroot" ]; then
    KAGARI_PACKAGE="$(readlink -f "$HOME/.local/state/kagari-v010-ga-stable/ga-v0.1.0-gcroot")"
    KAGARI_BIN="$KAGARI_PACKAGE/bin/kagari-v010-ga"
  else
    printf '%s\n' "KAGARI is not installed/on PATH; install it externally or set KAGARI_BIN" >&2
    exit 1
  fi
fi
KAGARI_BIN="$(readlink -f "$KAGARI_BIN")"
if [ ! -x "$KAGARI_BIN" ]; then
  printf '%s\n' "KAGARI_BIN is not an executable file" >&2
  exit 1
fi

OPENCODE_LINK="$(command -v opencode || true)"
if [ -z "$OPENCODE_LINK" ]; then
  printf '%s\n' "Install OpenCode externally before preparing a KAGARI Task" >&2
  exit 1
fi
OPENCODE_BIN="$(readlink -f "$OPENCODE_LINK")"
if [ ! -f "$OPENCODE_BIN" ] || [ ! -x "$OPENCODE_BIN" ]; then
  printf '%s\n' "Cannot resolve a canonical, executable OpenCode binary" >&2
  exit 1
fi

"$KAGARI_BIN" doctor --project-root "$PROJECT_ROOT" --state-home "$HOME/.local/state"
```

The expected attached-project result identifies `INSTALLED_GA_CANDIDATE`
and `GA_BOUNDED_PROFILE_QUALIFIED`. If it reports a missing/blocked
attachment, stop and resolve the installation state with the KAGARI
installation/repair procedure; do not silently treat `doctor` failure as a
successful Task grant. The stored GC-root convention above is specific to
the current Nix-based installation, not a portable universal KAGARI path.

## TaskSpec and Task approval

A KAGARI TaskSpec v1 is an explicitly reviewed JSON file containing exactly
`schema_version`, `goal`, `base_revision`, `allowed_paths`, `tests`,
`acceptance`, `subagent_count` (at least two), and `planning`
(`auto` or `specified`). The `base_revision` must match the attached
Project's exact **committed** Git HEAD. Every allowed path must be explicit.

Protected TaskSpec locations include `.git/**`, `.opencode/**`,
`AGENTS.md`, `Justfile`, `justfile`, `just/**`, `opencode.json`,
`opencode.jsonc`, `flake.nix`, and `flake.lock`. Such infrastructure
work requires separate maintenance/review authorization rather than a
broadened KAGARI Work Unit.

KAGARI executes `tests[].argv` on the host as literal executable/argument
arrays, **not as shell expressions**. For example,
`{"argv":["/absolute/path/to/just","project::check"],"cwd":"."}`
is valid when that exact executable is available to the KAGARI host.
Terreate's Nix devShell provides Just even if the ordinary shell does not;
resolve the pinned executable before writing an approved TaskSpec:

```sh
nix develop --no-update-lock-file --command sh -c 'command -v just'
```

For a separately approved numeric Issue and reviewed TaskSpec, set the
variables in the **same shell** as the host preflight above:

```sh
: "${TASK_SPEC:?Set TASK_SPEC to an absolute reviewed TaskSpec v1 JSON path}"
: "${ISSUE_NUMBER:?Set ISSUE_NUMBER to the approved numeric GitHub Issue}"
case "$TASK_SPEC" in
  /*) ;;
  *) printf '%s\n' "TASK_SPEC must be absolute" >&2; exit 1 ;;
esac
case "$ISSUE_NUMBER" in
  ''|*[!0-9]*) printf '%s\n' "ISSUE_NUMBER must be numeric" >&2; exit 1 ;;
esac
test -f "$TASK_SPEC" || { printf '%s\n' "TaskSpec file not found" >&2; exit 1; }

"$KAGARI_BIN" prepare-task \
  --project-root "$PROJECT_ROOT" --state-home "$HOME/.local/state" \
  --task-spec "$TASK_SPEC" --opencode "$OPENCODE_BIN" \
  --repository upiscium/Terreate --task "$ISSUE_NUMBER"
```

Inspect the exact returned job ID with `show-task --job-id <JOB_ID>`.
`approve-task` then requires a **local foreground TTY** and an exact per-job
consent phrase; separate native Human Asks must also be granted where
indicated. Never simulate or preauthorize consent in an automated session.
Only then may `run-task --job-id <JOB_ID>` execute. Preserve result evidence
and any Product patch; independently review and explicitly authorize
integration into Terreate. A private Work Unit success is **not** permission
to publish a PR, push, merge or modify the original Project.

## Existing Task #270

The legacy Agent Core #270 worktree contains an independently backed-up
five-file, unpublished Logical Device/Queue correction. It is not part of
the Agent Core removal PR #375. Do not delete the dirty worktree or import
it into an unrelated KAGARI Task; review its exact source manifest and
integration plan separately. Legacy Agent Core Task State is not migrated
implicitly.
