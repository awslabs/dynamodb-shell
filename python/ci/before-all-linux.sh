#!/usr/bin/env bash
# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# python/ci/before-all-linux.sh: cibuildwheel's before-all step for the Linux
# wheels (pyproject.toml, [tool.cibuildwheel.linux]). It runs once per job,
# as root, inside the manylinux_2_28 container (AlmaLinux 8).
#
# The grammar needs bison 3.7 or later (%require "3.7") and AlmaLinux 8 ships
# 3.0.4, so this builds GNU bison 3.8.2 from its release tarball, checked
# against a pinned SHA-256, and installs it in /usr/local. flex comes from the
# distribution.

set -euo pipefail

readonly BISON_VERSION="3.8.2"
readonly BISON_SHA256="9bba0214ccf7f1079c5d59210045227bcf619519840ebfa80cd3849cff5a5bf2"
# ftpmirror.gnu.org redirects to a nearby GNU mirror; ftp.gnu.org is the origin.
readonly BISON_URLS=(
    "https://ftpmirror.gnu.org/bison/bison-${BISON_VERSION}.tar.xz"
    "https://ftp.gnu.org/gnu/bison/bison-${BISON_VERSION}.tar.xz"
)

dnf install -y flex m4 xz

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tarball="$work/bison.tar.xz"

fetched=0
for url in "${BISON_URLS[@]}"; do
    if curl --fail --silent --show-error --location --retry 3 --connect-timeout 30 \
        --output "$tarball" "$url"; then
        fetched=1
        break
    fi
    echo "warning: could not download $url" >&2
done
if [ "$fetched" != 1 ]; then
    echo "error: could not download bison ${BISON_VERSION}" >&2
    exit 1
fi
echo "${BISON_SHA256}  $tarball" | sha256sum --check --strict

tar -xJf "$tarball" -C "$work"
(
    cd "$work/bison-${BISON_VERSION}"
    ./configure --prefix=/usr/local --disable-nls --quiet
    make -j"$(nproc)" --quiet
    make install --quiet
)

bison --version | head -n 1
flex --version
