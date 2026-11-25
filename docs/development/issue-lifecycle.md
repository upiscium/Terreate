# Issue lifecycle

A wanted feature and a task that can be implemented now are not the same thing.

```text
Feature Request / Proposal
        ↓ graduation
Architecture / Contract, where one is needed
        ↓
Implementation child Issue
        ↓
Task / PR lifecycle
```

## Parents and children

- A **parent** Issue coordinates a wide area (a roadmap, a quality gate). It is not
  itself a giant implementation task and is closed only when its own completion criteria
  hold, not when its children close.
- A **child** Issue has independent acceptance criteria and is small enough to review.
- A broad Feature Request becomes a parent and is split into child Proposals with
  distinct responsibilities.

## Graduation

A Proposal graduates to an implementation Issue when its prerequisite contracts are
mature and a concrete use case exists. Graduation criteria (is it ready to implement?)
are different from acceptance criteria (is the implementation done?). An optional
abstraction with no concrete use case stays a Proposal, or is closed as not planned.

## Category semantics

Module, Adapter, Integration and Extension mean what the
[architecture](../architecture/index.md) says. A new Issue names its category and stays
inside that category's dependency rules.

## Completion

An Issue is closed with a comment that states what was verified and what was not. A
passing test, a review or a CI result is not a substitute for that statement, and an
environment that could not run a test is reported as skipped, never as passed.
