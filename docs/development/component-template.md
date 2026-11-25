# Component documentation template and checklist

Every component (Module, Adapter, Integration or Extension) documents the items below,
in its page under `docs/architecture/`. A new component is not complete without them.

## Page template

```markdown
# <Component> contract

## Responsibility and non-goals
## Public dependencies
## Ownership, borrowing and lifetime
## Execution authority and threading
## Requested, supported and effective configuration
## Errors and diagnostics
## Optional and required dependencies
## Native or backend escape hatch
## Minimal explicit example
## Convenience API and its lower-level equivalent
```

## Checklist for a new component

1. Category (Core, Module, Adapter, Integration, Extension), responsibility and non-goals.
2. Allowed dependencies, checked against the dependency direction.
3. Public and internal boundary: `include/terreate/<name>/` and `src/`.
4. Ownership, borrowing and lifetime; C++ lifetime versus GPU execution lifetime where
   it applies.
5. Execution and threading authority: who calls what, and who waits.
6. Requested, supported and effective state, where the domain has it.
7. Errors (`Result`/`Error`) and diagnostics.
8. Optional versus required dependencies, and what a consumer of the component receives.
9. The native or backend escape hatch.
10. Independent build, test and package behavior (an option, a target, a component in
    `find_package`).

This template and checklist are review conventions; the build does not check them.

A minimal example in the page is compiled and run as a test where practical, so it cannot
drift from the code.
