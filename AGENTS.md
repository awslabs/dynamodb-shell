<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# AGENTS.md — agent onboarding for the ddbsh reimplementation

This file orients an AI (or human) contributor working in the **code repo**
(`dynamodb-shell`). It is the fast path to building, testing, and changing the
reimplementation correctly. Read it before you touch anything. The authoritative
behavioral reference is `ddbsh-architecture.md` in the orchestration repo; the
decision record and phase workflow live in that repo's `discussions/`.

## What this repo is

A SQL-like interactive CLI for Amazon DynamoDB, delivered as:

- **An engine library** (`libddbsh`, CMake target `ddbsh`): lexer, parser,
  command model, expression engine, SQL→API planner, execution behind an
  injected client, and output/EXPLAIN formatting. **SDK-free and terminal-free**
  public API under `include/ddbsh/`.
- **A thin CLI** (CMake target `ddbsh-cli`, installed as the executable
  **`ddbsh`**): argument parsing, prompt, line editing + history, config-file
  loading, signal handling, and wiring a concrete AWS SDK client into the
  library. No lexing/parsing/execution logic lives here.

The original C++11 `ddbsh` (awslabs upstream) is retained under `oracle/` as the
**fidelity oracle** — the byte-for-byte golden-output reference. It is never
edited; its binary is exposed as **`ddbsh-old`** for manual comparison.

## Repository layout

```
include/ddbsh/     public library headers (SDK-free, terminal-free)
src/               engine library implementation
  parser/          flex lexer.l + bison grammar.yy (reentrant; PIMPL'd)
  expressions/     symbol table, attribute serialization
  planner/         key-schema resolution, SELECT GetItem/Query/Scan planner
  execute/         per-command executors
  render/          output + EXPLAIN formatting
cli/               the ddbsh CLI (readline + AWS SDK client seam)
tests/             native GoogleTest unit tests + boundary_check.sh
  golden/          golden-parity CTest project (drives the shipping binary)
examples/          embed_demo.cpp — standalone library-embedding sample
oracle/            pristine original C++11 ddbsh (the fidelity oracle)
devtools/          run-tests (the ONLY sanctioned test entry point)
```

## Build directories (one clear story)

Three build trees are produced by `devtools/run-tests`, plus optional preset
builds for local development. Keep them distinct — do not conflate them:

| Directory              | Produced by            | Contains                                        |
|------------------------|------------------------|-------------------------------------------------|
| `build/`               | run-tests (oracle)     | oracle `ddbsh-old` (v0.8.x) + `ddbshtest`       |
| `build-engine/`        | run-tests (new engine) | the shipping `ddbsh` CLI (new C++23 engine)     |
| `build-golden/`        | run-tests (golden)     | configure-only golden-parity CTest project      |
| `build-golden-full/`   | run-tests --full-engine| golden project with every family on the engine  |
| `build-preset/linux-*` | CMakePresets           | local dev builds (debug/release/sanitize)       |

All are git-ignored (`build*`). Preset builds live under `build-preset/` (not
`build/`) specifically so `build/` is unambiguously the oracle.

## Canonical commands

These are the only allowed build/test/lint commands (see
`.kiro/steering/coding-standards.md` in the orchestration repo):

```sh
# configure + build (library, CLI, native tests)
cmake -S . -B build-preset/linux-debug   # or use a CMakePreset
cmake --build build-preset/linux-debug

# with a CMakePreset (gcc14 + the prebuilt AWS SDK)
cmake --preset linux-release && cmake --build --preset linux-release

# format / lint (repo .clang-format / .clang-tidy are the source of truth)
clang-format -i <files>
clang-tidy <files>
```

Build with warnings-as-errors (`-Wall -Wextra -Werror`); the generated
flex/bison translation units are the only `-Wno-error` exceptions.

> **clang-tidy toolchain caveat (`TODO(cleanup)`):** in the current dev
> environment the only C++23 toolchain is gcc14, and clang-tidy cannot parse
> gcc14's libstdc++ C++23 headers (it emits tens of thousands of system-header
> warnings plus spurious parse errors). The enforced quality gate is therefore
> the gcc14 `-Wall -Wextra -Werror` build (clean). A CI follow-up should add a
> clang job with a matched libc++/libstdc++ (or a `gcc -fanalyzer` pass) so
> clang-tidy can run cleanly; this is an environment limitation, not a code
> defect.

## Testing — MANDATORY protocol

- **Always run tests via `devtools/run-tests`** from the **orchestration repo**.
  Never invoke `ctest`, the test binary, or a suite runner directly for a phase
  gate. Since the Step-2 exit gate (2h-1) the script builds ONLY the new engine
  and runs every non-global-tables golden family against it via the golden
  harness (the gate exit propagates); the original C++11 `oracle/` tree is
  retained as the golden/`.exp`/`ddbsh-wait` source but is no longer built or
  run. Artifacts land in `discussions/` named by HEAD short hash.
- **Full-engine mode:** `devtools/run-tests --full-engine` registers the same
  family set as the default gate but forces the exit code to 0, so it is a
  MEASUREMENT (read the parity figure from the printed counts / artifact) rather
  than a gate. Since 2h-1 it differs from the default only in exit semantics.
- **Backend is real DynamoDB (us-east-1)** via the standard `~/.aws` chain. There
  is no DynamoDB Local / no endpoint override. `create/insert/update/delete/drop`
  hit real tables and cost real money — use a dedicated test account/region. The
  target (endpoint + access key id) is echoed and must be verified.
- **No test is skipped.** A missing credential is an infrastructure failure, not
  a skip.
- **Native unit tests** (GoogleTest, offline, zero cost) are the fastest signal
  while iterating; they include the EXPLAIN-via-`FakeDDBClient` fidelity tests
  and the `engine_boundary_check`. They are built by the preset builds and run
  under ctest in those build dirs, but a **phase gate** still requires the full
  `devtools/run-tests` artifact at HEAD.

## Fidelity discipline (the regression contract)

- The original `oracle/tests/` golden `.exp` files define behavior. The
  reimplementation must reproduce the same stdout for the same commands.
- Preserve exactly: the deterministic expression-alias scheme
  (`#a<prefix><n>` / `:v<prefix><n>`), the SQL→API decision matrix
  (GetItem/Query/Scan), item rendering (alpha-sorted attrs, quote-if-needed,
  `TRUE`/`FALSE`/`NULL`, top-level `: ` vs nested `:` spacing), status tokens,
  and error strings. See `ddbsh-architecture.md` §8–§9 and §14.
- Any intentional divergence requires a human-approved decision recorded in a
  `discussions/` file, with the golden expectation updated and annotated.

## Library/CLI boundary (phase-exit blocker if violated)

- The engine library must have **no** terminal/I/O/process dependency: no
  `readline`, `iostream`, `cout`/`cerr`, `printf`, no `exit`/`abort`/`assert` on
  user/service/data paths, and no `Aws::`/`aws/` includes. Errors propagate as
  `Result<T> = std::expected<T, ddbsh::Error>`.
- `tests/boundary_check.sh` enforces this and runs as a CTest
  (`engine_boundary_check`). Run it before every handoff.
- SDK translation lives only in `cli/aws_ddb_client.cpp`, behind the
  `ddbsh::IDDBClient` seam.

## Interactive input: the isql `go` model

The CLI assembles input Sybase-isql style: it buffers physical lines until a
**terminator line**, then hands the whole batch to the engine.

- Default terminator is the word `go` on a line by itself (case-insensitive).
- A bare `;` does **not** execute interactively — it only ends a statement
  inside the batch. `go`, a blank line (with `-t` and no argument), or EOF
  flushes.
- `-c`/`-f` always flush at EOF, which is why the golden `.exp` command strings
  (`;`-terminated, EOF-flushed) produce byte-identical output.

This is a deliberate, documented CLI-only divergence (tenets 2/8); the engine
still parses `;`-terminated statements.

## Known gotchas

- **EXPLAIN of a SELECT is NOT backend-free.** The planner resolves the key
  schema via `DescribeTable`, so `EXPLAIN SELECT` against an unseeded table
  errors "Table not found". The deterministic, backend-free EXPLAIN signal is
  the native `FakeDDBClient` tests, not the shipping binary against a bare
  account.
- **Two binaries, two names.** The new shipping CLI builds as `ddbsh`
  (`build-engine/ddbsh`); the oracle builds as `ddbsh-old` (`build/ddbsh-old`),
  so the shipping-binary name `ddbsh` is unambiguous and the two installs do not
  collide (D-A). The library file is `libddbsh`; no collision with the engine
  executable.
- **The oracle (`oracle/`) is pristine.** Never modify its C++ source. The
  `ddbsh` -> `ddbsh-old` rename is a CMake target + `install()` change (build
  glue), the one allowed oracle edit class.
- **AWS SDK toolchain.** The prebuilt SDK is gcc14; the oracle and engine must
  link with the matching toolchain (`CC=gcc14-gcc CXX=gcc14-g++`), as
  `devtools/run-tests` and the CMakePresets do.

## Git workflow

- Work on `dev/<slug>` branches. **Never commit to `main`. Never push.** The
  human merges and pushes.
- Commit early and often; imperative first line ≤72 chars.
- Every source file carries the Apache-2.0 header (see `oracle/LICENSE` /
  repo `LICENSE`).

## Versioning

Single project version in `CMakeLists.txt`. The developer bumps the **patch**
as part of the Developer → Reviewer handoff (every merge is a unique version).
Minor/major are human-directed; the principal reviewer tags the merge commit
`v<MAJOR>.<MINOR>.<PATCH>`. The three pinned version assertions live in
`tests/walking_path_test.cpp` and `tests/reentrancy_test.cpp` — update them with
every bump.

## Agent workflow (orchestration repo)

Work flows architect → developer → reviewer → principal-reviewer → human,
driven by `discussions/next-step.md` in the orchestration repo
(`dynamodb-shell-steering`). Each agent appends to the active `discussions/`
file and completes its handoff checklist before setting the next step. Read the
role steering under `.kiro/steering/` there before acting.
