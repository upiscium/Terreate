# Releases

How a Terreate release is prepared, verified and cut. The first release under this process is `0.1.0`:
a release candidate (`v0.1.0-rc.1`, more candidates if needed), then `v0.1.0` (GA).

Tagging, pushing a tag and creating a GitHub release publish work: each needs the repository owner's explicit
approval (the rule of `AGENTS.md`: nothing is inferred from these instructions).

## Versioning

- `MAJOR.MINOR.PATCH`. Before `1.0` a **minor** version may change the public API; a patch version only
  fixes bugs. The installed CMake package is compatible only within the same `MAJOR.MINOR`
  (`SameMinorVersion`; a test of the installed package checks it).
- The CMake project version is `MAJOR.MINOR.PATCH` (CMake has no pre-release part). A release candidate is a git
  tag and a GitHub pre-release: `v0.1.0-rc.1`, `v0.1.0-rc.2`, ... The release is the tag `v0.1.0`.
- One repository, one coordinated version for every component (there is no per-component version).

## Phases

1. **Preparation.** The scope is decided, the open Issues are triaged (in scope, or planned for the next
   release), the license and the release files exist, the gate passes on `main`. The release Issue tracks it.
2. **Release candidate.** `main` is frozen for features from the commit of `v0.1.0-rc.1` until the `v0.1.0`
   tag: only bug fixes, documentation and packaging changes are merged. Work for the next release stays on
   branches or in open (draft) pull requests and is **not merged to `main`** until the release is out. After a
   fix that changes the code, the next candidate (`-rc.N+1`) is cut.
3. **Verification.** The checks in [release verification](release-verification.md) are run on real hardware
   and the results are recorded on the release Issue.
4. **GA.** When the exit criteria below hold, `v0.1.0` is tagged on the commit of the last candidate, or on a
   later commit that differs from it only in `CHANGELOG.md` and documentation (the gate is run again on it).
   `CHANGELOG.md` is final at `rc.1`; later candidates add their fixes to it, and the GA commit only sets the
   date.

## Exit criteria (candidate to GA)

- The gate passes on the commit to be released, and its output is attached to the release Issue.
- The real-hardware verification is done and every failure is either fixed (and a new candidate cut) or
  recorded as a known limit in the changelog.
- No open Issue labelled `release-blocker`.
- `CHANGELOG.md` is final and the release notes say what is supported and what is not.
- The license is in the repository (`LICENSE`, BSD 3-Clause) and installed with the package. A candidate is not
  published without one.

## Cutting a candidate or the release

1. `main` is at the intended commit and `just project::check` passes (the gate, not `check-fast`).
2. `CHANGELOG.md` has the section for the version (final at `rc.1`; the GA commit sets its date).
3. With the owner's approval, tag it annotated: `git tag -a v0.1.0-rc.1 -m "Terreate v0.1.0-rc.1"`, and push
   the tag.
4. With the owner's approval, create the GitHub release from the tag: a **pre-release** for a candidate
   (`gh release create v0.1.0-rc.1 --prerelease --notes-file <notes>`), a normal release for GA. The notes are
   the changelog section and the list of known limits.
5. Record the commit, the gate output and the verification results on the release Issue.

## Not part of a release

The next release's features (for example `MeshData` and the primitive generators) and the optional feature
families wait. A release does not require closing the roadmap parent Issues: each is closed when its own
completion criteria hold, or re-scoped with a comment saying which release takes the rest.
