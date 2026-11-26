# Releases

How a Terreate release is prepared, verified and cut. The first release under this process is `0.1.0`:
a release candidate (`v0.1.0-rc.1`, more candidates if needed), then `v0.1.0` (GA).

## Versioning

- `MAJOR.MINOR.PATCH`. Before `1.0` a **minor** version may change the public API; a patch version only
  fixes bugs. The installed CMake package is compatible only within the same `MAJOR.MINOR`
  (`SameMinorVersion`).
- The CMake project version is `MAJOR.MINOR.PATCH` (CMake has no pre-release part). A release candidate is
  a git tag and a GitHub pre-release: `v0.1.0-rc.1`, `v0.1.0-rc.2`, ... The release is the tag `v0.1.0`.
- One repository, one coordinated version for every component (there is no per-component version).

## Phases

1. **Preparation.** The scope is decided, the open Issues are triaged (in scope, or planned for the next
   release), the license and the release files exist, the gate passes on `main`.
2. **Release candidate.** A feature freeze: from the tag of `v0.1.0-rc.1` to `v0.1.0` only bug fixes,
   documentation and packaging changes are merged. A fix is a normal pull request; after a change that
   touches the code, the next candidate (`-rc.N+1`) is cut. Features for the next release wait in open
   Issues or on branches and are not merged to `main` until the release is out, or are kept off the
   release by being merged after the tag.
3. **Verification.** The checks in [release verification](release-verification.md) are run on real
   hardware and the results are recorded on the release Issue.
4. **GA.** When the exit criteria below hold, the release candidate's tag is re-cut as `v0.1.0` from the
   same commit (no code change between the last candidate and GA).

## Exit criteria (candidate to GA)

- The gate passes on the commit to be released, and its output is attached to the release Issue.
- The real-hardware verification is done and every failure is either fixed (and a new candidate cut) or
  recorded as a known limit in the changelog.
- No open bug Issue labelled for the release.
- `CHANGELOG.md` is final and the release notes say what is supported and what is not.
- The license is in the repository.

## Cutting a candidate or the release

1. `main` is at the intended commit and `just project::check` passes (the gate, not `check-fast`).
2. Update `CHANGELOG.md` (the section for the version, with the date).
3. Tag it annotated: `git tag -a v0.1.0-rc.1 -m "Terreate v0.1.0-rc.1"`; push the tag.
4. Create the GitHub release from the tag: a **pre-release** for a candidate (`gh release create v0.1.0-rc.1
   --prerelease --notes-file <notes>`), a normal release for GA. The notes are the changelog section and
   the list of known limits.
5. Record the commit, the gate output and the verification results on the release Issue.

## Not part of a release

The next release's features (for example `MeshData` and the primitive generators) and the optional
feature families wait. A release does not require closing the roadmap parent Issues: each is closed when
its own completion criteria hold, or re-scoped with a comment saying which release takes the rest.
