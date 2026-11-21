<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Building ddbsh

This is the single build guide for ddbsh: building the C++23 library and shell
from source, installing a pre-built release, and (if you need it) building the
AWS SDK for C++ from source.

## Build from source

Prerequisites: a C++23 compiler (GCC ≥ 12 / Clang ≥ 16 / MSVC 19.3x), CMake
≥ 3.21, flex and bison (reentrant scanner + `lalr1.cc` parser), GNU readline,
zlib, and the AWS SDK for C++ (`dynamodb`, `iam`, `sts` components).

```sh
cmake -S . -B build
cmake --build build
```

This produces, from one build: the engine library (`ddbsh`, SDK-free), the
optional AWS-backed client component (`ddbsh-aws`, alias `ddbsh::aws` — the
ready-made `AwsDDBClient` plus `AwsApiGuard`), the CLI (CMake target
`ddbsh-cli`, installed as the executable `ddbsh`), and the native unit tests
(`ddbsh-tests`). The AWS component (and the CLI, which needs it) is built when
`DDBSH_BUILD_AWS` is ON (the default); set it OFF for a pure SDK-free
engine-only build/install that requires no AWS SDK at all. If the AWS SDK is
installed to a non-default prefix, point CMake at it:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/aws-sdk-cpp
```

Install the library (headers + lib), the AWS component, and the CLI to a prefix:

```sh
cmake --install build --prefix /path/to/prefix
```

The install ships a `find_package(ddbsh)` CMake package (under
`lib/cmake/ddbsh`) exporting `ddbsh::ddbsh` and — when the AWS component was
built — `ddbsh::aws`. A consumer links either target with no hand-written
include/lib wiring; see [Embedding the library](EMBEDDING.md).

### macOS (Homebrew)

Install the toolchain and dependencies with Homebrew:

```sh
brew install cmake bison flex readline zlib
```

Homebrew's `bison` and `flex` are **keg-only**: they are not symlinked onto your
`PATH` because macOS ships its own (ancient) copies in `/usr/bin`. The system
`bison` is version 2.3, which is too old for the C++ `lalr1.cc` parser skeleton
this project uses, so you must make the Homebrew versions take precedence.

Prepend them to your `PATH` for the current shell (and add to `~/.zshrc` to make
it permanent):

```sh
# Apple Silicon (Homebrew prefix /opt/homebrew)
export PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:$PATH"

# Intel (Homebrew prefix /usr/local)
export PATH="/usr/local/opt/bison/bin:/usr/local/opt/flex/bin:$PATH"
```

Verify you are getting the Homebrew tools (bison ≥ 3.8, not 2.3):

```sh
which bison && bison --version
which flex  && flex  --version
```

Alternatively, point CMake directly at the Homebrew binaries instead of changing
your `PATH` (this also stops CMake from autodetecting `/usr/bin/bison`):

```sh
cmake -S . -B build \
  -DBISON_EXECUTABLE="$(brew --prefix bison)/bin/bison" \
  -DFLEX_EXECUTABLE="$(brew --prefix flex)/bin/flex"
cmake --build build
```

If you installed the AWS SDK for C++ via Homebrew or to a custom prefix, add it
to `CMAKE_PREFIX_PATH` as shown above (you can combine the flags).

#### Avoiding AWS SDK version conflicts

The vector-search commands require the AWS SDK for C++ **≥ 1.11.916** (the first
release exposing the `SearchVectors` API). A common macOS pitfall is having more
than one SDK installed at once — for example a Homebrew `aws-sdk-cpp` under
`/opt/homebrew` and a manual build installed into `/usr/local`. When
`find_package(AWSSDK)` resolves one version but the compiler's default include
path (`/usr/local/include`) pulls `aws/core/VersionConfig.h` from another, the
build fails with:

```
error: 'AWS_SDK_VERSION_PATCH' macro redefined [-Werror,-Wmacro-redefined]
```

Fix it by ensuring exactly one SDK is discoverable, and point CMake at it:

```sh
# list every AWS SDK VersionConfig.h and its version
for f in $(find /usr/local /opt/homebrew $HOME -path '*aws/core/VersionConfig.h' 2>/dev/null); do
  echo "$f"; grep AWS_SDK_VERSION_PATCH "$f"; done

# remove the unwanted install (e.g. an older Homebrew SDK), then reconfigure clean
# (the CMake cache pins the previously found SDK, so delete the build dir)
rm -rf build
cmake -S . -B build -DCMAKE_PREFIX_PATH=/prefix/of/sdk-1.11.916
cmake --build build
```

`CMAKE_PREFIX_PATH` is searched ahead of system defaults, so both the SDK CMake
package and its headers resolve to the same install. Do not silence
`-Wmacro-redefined` to work around this — the clashing version macros mean you
would be compiling against one SDK's headers while linking another's libraries.


## Build the browser edition

The browser edition (see [configuration 3](configurations/03-browser-aws.md))
compiles the engine to WebAssembly with Emscripten. It needs no AWS SDK for C++.

- **emsdk 3.1.61** (the version CI pins), installed per
  [`web/README.md`](../web/README.md); its scripts need Python 3.10 or newer.
- **Node.js 20 or newer** for the Node-side tests and gates (the bundled AWS SDK
  for JavaScript requires it). emsdk ships one; the devtools scripts use it.

```sh
emcmake cmake --preset wasm-release
cmake --build build-preset/wasm-release
```

The build stages the complete static bundle in
`build-preset/wasm-release/web/`: `index.html`, the JavaScript modules,
`ddbsh.js`/`ddbsh.wasm`, and `vendor/aws-sdk/` (the pinned SDK bundle). Serve
that directory with `devtools/serve-web` (loopback only).

The AWS SDK for JavaScript bundle (`web/vendor/aws-sdk/aws-sdk-dynamodb.mjs`) is
committed, built from exact version pins and a committed lockfile.
`devtools/vendor-aws-sdk-js` rebuilds it (needs npm registry access);
`devtools/vendor-aws-sdk-js --check` rebuilds it in a temporary directory and
fails if the result differs from the committed files, which CI runs. To bump the
SDK, change the pin in `web/vendor/aws-sdk/package.json`, run
`devtools/vendor-aws-sdk-js --lock`, and commit every changed file under
`web/vendor/aws-sdk/` plus `web/index.html` (its CSP host list is generated).

## Install a release

Instead of building from source, you can install a pre-built binary.

### Installing on Mac via Homebrew

On a macintosh, you can install DynamoDB shell from homebrew with the following two commands.

``` bash
brew tap aws/tap
brew install aws-ddbsh
```

You need to create the AWS credentials and configuration files. For instructions [read this](https://docs.aws.amazon.com/cli/latest/userguide/cli-configure-files.html).

### Installing on ubuntu

Binaries for ubuntu-latest are built by github. To find the latest binaries navigate to [Actions](https://github.com/awslabs/dynamodb-shell/actions) and find the latest run of the "Build DynamoDB Shell" action. When you click on that link, you fill find (at the very bottom) a section of Artifacts. There you will find a file with binaries that you can run on an ubuntu machine.

``` bash
$ unzip binaries-linux.zip
Archive:  binaries-linux.zip
  inflating: ddbsh
  inflating: ddbshtest
$
```

You need to create the AWS credentials and configuration files. For instructions [read this](https://docs.aws.amazon.com/cli/latest/userguide/cli-configure-files.html).


## Building the AWS SDK for C++ from source

The `ddbsh::aws` component and the CLI need the AWS SDK for C++ (`dynamodb`,
`iam`, `sts` components, version ≥ 1.11.916 for vector search). Prefer a
distribution or Homebrew package where available; build it from source only when
you need a specific version or none is packaged.

### Building the SDK

The following has been found to work reliably.

> **AWS SDK version.** ddbsh requires the AWS SDK for C++ `dynamodb`, `iam`, and
> `sts` components. Use a recent release (the project is developed against
> **1.11.916** or newer).
>
> **Generator-agnostic commands.** The steps below use CMake's build/install
> drivers (`cmake --build` / `cmake --install`) instead of `make` /
> `make install`, so they work whether CMake generated Makefiles **or** Ninja.
> On a box whose default generator is Ninja, no `Makefile` is produced and
> `make` fails with "No targets specified and no makefile found."
>
> **Install prefix.** Pick a prefix and keep it consistent with where ddbsh's
> `find_package(AWSSDK)` later resolves (its `CMAKE_PREFIX_PATH`). `sudo` is
> needed only for a root-owned prefix such as `/usr/local`; a user-owned prefix
> like `$HOME/.local/aws-sdk-cpp` needs no `sudo`. Verify the cache took the
> prefix before installing: `cmake -LA . | grep CMAKE_INSTALL_PREFIX` — the AWS
> SDK's top-level CMake has been observed to retain a stale
> `CMAKE_INSTALL_PREFIX=/usr/local` from the first configure; re-run
> `cmake . -DCMAKE_INSTALL_PREFIX=<prefix>` to make it stick.

```
% git clone https://github.com/aws/aws-sdk-cpp.git

% mkdir aws-sdk-build

% cd aws-sdk-cpp

% git submodule update --init --recursive

% cd ../aws-sdk-build

% cmake ../aws-sdk-cpp -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/usr/local/ \
        -DCMAKE_INSTALL_PREFIX=/usr/local/ \
        -DBUILD_ONLY="dynamodb;iam;sts" \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_TESTING=OFF \
        -DFORCE_SHARED_CRT=OFF

% cmake --build . --parallel $(nproc)

% sudo cmake --install .

```

### Rebuilding the SDK

```
% cd <aws-sdk-dir>

% git pull --recurse-submodules=yes

% cd ../aws-sdk-build

% cmake ../aws-sdk-cpp -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/usr/local/ \
        -DCMAKE_INSTALL_PREFIX=/usr/local/ \
        -DBUILD_ONLY="dynamodb;iam;sts" \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_TESTING=OFF \
        -DFORCE_SHARED_CRT=OFF

% cmake --build . --parallel $(nproc)

% sudo cmake --install .

```

Once the SDK is installed, build ddbsh as shown in
[Build from source](#build-from-source) above, pointing `CMAKE_PREFIX_PATH` at
the SDK install prefix if it is not on the default search path.
