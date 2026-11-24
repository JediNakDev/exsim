# Project instructions

## Read only what the task needs

- Start with `README.md`, then `CONTEXT.md`, then the relevant source and tests.
- Keep docs focused on topics that need more depth than the README.
- Match the README's short, direct writing style. Do not add dated reports or long task histories.
- Open review findings live in GitHub issues. A finding is fixed only when the code changed and a check proves it.

## Keep it simple

The project must stay simple, straightforward, and lightweight. This outranks cleverness and completeness.

- Make the smallest change that solves the problem. Prefer deleting code to adding it.
- One way to do each thing. Before adding a type, helper, or file, look for one that already exists.
- No code without a production caller. Remove it instead of keeping it for later. Test-only helpers live under `test/`.
- Inline logic that is used once. Extract a function only when it has a second caller or names a real boundary.
- No new dependency, layer, config option, or abstraction unless the task cannot be done without it.
- A file lives in the folder that matches what it does, and its name says what it contains.
- Defaults stay small. Large capacity is an explicit config choice, not something a default constructor allocates.
- When hand-rolling something the standard library or a common library already offers, write the reason in one or two lines (a comment or `docs/dsa.md`).

## What to prioritise

Judge every change against these themes, in this order. `CONTEXT.md` has the detail.

1. Modern C++: type safety, clear ownership, RAII.
2. Linux and networking: BSD sockets, epoll, TCP semantics, binary serialization, stated trade-offs.
3. Distributed systems: clear process boundaries and message framing between them.
4. Data structures and algorithms: performance decided at design time.
5. Profiling and benchmarking: numbers from the current production path.
6. Testing: unit and integration tests with every change.
7. README: understandable and buildable in 5 to 10 minutes.

## User constraints

- Product: empty exchange processes, one per exchange, started from a config file. External trading engines supply all orders and liquidity.
- No web frontend, built-in trader strategies, or PostgreSQL. Keep live exchange state in RAM, with config files and optional logs on disk.
- Language: C++23 for production builds, tests, and editor tooling.
- Management: run `exsim_exchanged --config PATH` directly and stop it with a signal. Runtime commands go over its owner-only Unix admin socket. No HTTP management API (user decision 2026-11-23). Trading engines connect directly to each exchange's protocol endpoints.
- Active verification: unit and integration tests on macOS and local OrbStack Ubuntu. Fuzz and end-to-end suites are deferred.
- Do not edit `README.md` unless the user asks for a specific change.
- Simulation, matching, exchange protocols, accounting, and simulated time belong in C++.
- Never include a co-author attribution in commits. Avoid em dashes in responses, comments, and new prose.

## Delegates

- Pick the model that fits the task and say which one and why (user decision 2026-11-15; this replaces the earlier GPT-only rule).
- Independent verification of review findings goes to GPT-6.1 Sol with high reasoning.
- Do not use local Ollama while the user is working on this machine.
- Do not poll running delegates. Use T3 completion notifications and follow the tool's `next` instruction.

## Coordination

- Preserve untracked `.claude/`, `output/`, and unfinished implementation files from interrupted work.
- Do not alter another delegate's owned files. Coordinate public API/spec changes before resuming dependent implementations.
- Record task IDs, checkout paths, commits, checks, and next actions in ignored `output/` task records. Keep long logs in files; return concise findings and exact log paths.
- Use harness-created isolated worktrees for independent implementation. Preserve existing interrupted worktrees and resume their actual child threads when continuing that work.
- Review fixes merge into the shared fix branch, not `main`. One issue per branch; rebase before merging.
- Never claim that a design, declaration, partial branch, or successful launch is a working integrated feature.
