<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Testing

Run the test suite via `devtools/run-tests` from the orchestration repo. Since
the Step-2 exit gate, every non-global-tables golden family runs against the
new engine (the shipping `ddbsh` binary driven through the external harness in
`tests/golden/`); the gate exit propagates. The original C++11 engine was
removed from the tree during the phase-2i oracle retirement (preserved in git
history under the `oracle-archive-v0.8.1` tag); its golden contract was
relocated to a single command-family tree at `tests/golden/` and remains the
regression source. The
native GoogleTest unit tests (offline, `FakeDDBClient`)
build with the project (`ddbsh-tests`) and run alongside as the deterministic
backend-free fidelity signal.

## Running the native unit tests directly

Once you complete a build you can run the native unit tests directly with
CTest:

```
% ctest --test-dir .
Running tests...
Test project /Users/amrithie/source/github/dynamodb-shell/build
        Start   1: ddbsh-basic-cli
[...]
```

