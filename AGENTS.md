# Terreate — Contributor instructions

Terreate is a C++23/Vulkan-Hpp **library**. The caller owns the application loop, frame scheduling, resource lifetimes and synchronization. Do not introduce a monolithic Engine or hidden execution control.

## Development

- Prefer GCC/g++ for the real build, CMake/Ninja/Just for build orchestration, and Nix devShell for reproducibility. `just project::check` is the project-owned build/test/format/lint gate.
- Keep Vulkan lifetime, borrowed handles, queue selection, configuration defaults and effective state explicit and inspectable. Favor move-only RAII values and avoid unnecessary shared ownership.
- Feature work can be delegated through the externally installed KAGARI v0.1.0 *isolated Work Unit* profile when the user approves an exact TaskSpec and its native human consent prompts. KAGARI cannot directly edit or publish the attached project and must not be treated as a background daemon or a game engine.
- Do not infer authorization to change source, commit, push, merge or delete history from an executor response. Preserve user-owned dirty work, and request approval before publication.
- KAGARI v0.1.0 does not allow changes to `AGENTS.md`, `Justfile`, `opencode.json` or `.opencode/**` through an ordinary TaskSpec. Such project-infrastructure changes require a separately reviewed migration/maintenance change, not permission bypass.
- Repository code does not need Agent Core v3, its local Task Orchestrator or any vendor-specific editor agent configuration.

See [KAGARI workflow](docs/development/kagari.md). Application-owned execution control and native Vulkan escape hatches are primary design requirements.
