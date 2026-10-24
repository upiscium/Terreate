# Terreate — Contributor instructions

Terreate is a C++23/Vulkan-Hpp **library**. The caller owns the application loop, frame scheduling, resource lifetimes and synchronization. Do not introduce a monolithic Engine or hidden execution control.

## Development

- Prefer GCC/g++ for the real build, CMake/Ninja/Just for build orchestration, and the Nix devShell for reproducibility. `just project::check` is the project-owned build/test/format/lint gate.
- Keep Vulkan lifetimes, borrowed handles, queue selection, configuration defaults and effective state explicit and inspectable. Favor move-only RAII values and avoid unnecessary shared ownership.
- **Use the externally installed KAGARI v0.1.0 isolated Work Unit workflow by default for ordinary feature implementation and tests.** Each run needs an exact, explicitly approved TaskSpec tied to a committed Git revision, a separate local foreground Human Task consent, and any native Human Asks. KAGARI works in a private independent clone and cannot directly modify, commit or push the attached Terreate Project. If unavailable, report the limitation; do not silently switch to unrestricted direct edits.
- KAGARI TaskSpec v1 excludes protected paths and path segments such as `.git/**`, `.opencode/**`, `AGENTS.md`, `Justfile`, `justfile`, `just/**`, `opencode.json`, `opencode.jsonc`, `flake.nix`, and `flake.lock`. Work affecting repository instructions, Nix or build-tool infrastructure requires an explicitly authorized, separately reviewed maintenance path, not a Work Unit allowlist or permission bypass.
- KAGARI-first is a workflow preference, **not** an automatic permission boundary for ordinary OpenCode sessions outside KAGARI. Do not infer authority to edit, execute shell commands, commit, push, merge or delete work from these instructions or an executor response. Preserve dirty work and require the relevant human approval before publication.
- Terreate does not depend on embedded Agent Core v3, a repository-owned Task Orchestrator, or a GUI/game engine. Do not recreate them as an implicit part of routine development.

See [KAGARI workflow](docs/development/kagari.md) for the host executable preflight, exact TaskSpec and consent requirements. Application-owned execution control and native Vulkan escape hatches remain primary design requirements.
