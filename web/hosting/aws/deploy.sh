#!/usr/bin/env bash
# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# web/hosting/aws/deploy.sh — upload a packaged ddbsh browser bundle to the
# bucket of a ddbsh-web-hosting.yaml stack and invalidate its distribution.
#
# Usage:
#   web/hosting/aws/deploy.sh <bundle-dir> <bucket> <distribution-id>
#
#   <bundle-dir>       the packaged folder,
#                      build-preset/wasm-release/dist/ddbsh-web-<version>
#   <bucket>           the stack's BucketName output
#   <distribution-id>  the stack's DistributionId output
#
# Credentials come from the usual AWS CLI chain (AWS_PROFILE, ...). The caller
# needs s3:ListBucket on the bucket, s3:PutObject and s3:DeleteObject on its
# objects, and cloudfront:CreateInvalidation on the distribution: exactly what
# the template's optional deployer policy grants.
#
# These are the same commands docs/HOSTING.md lists; each is one line and runs
# unchanged in bash, zsh and PowerShell once DIR, BUCKET and DIST_ID are set.
# One `aws s3 sync` per file type sets the Content-Type browsers require (a
# module script with the wrong type does not load; ddbsh.wasm needs
# application/wasm). --delete removes objects of that type that are no longer
# in the bundle. sync uploads a file when its size differs or the local copy is
# newer than the object; the packaging step writes every file fresh, so upload
# right after packaging. Every file is no-cache: the file names carry no
# version, so a browser must check each file on every load, or a cached app.js
# could run with a newer index.html. An unchanged file costs one 304 reply. A
# visitor who loads the page while these commands run can still get a mix of
# old and new files; once the invalidation has finished, a reload loads one
# version.
#
# TODO(portability): no PowerShell wrapper; on Windows run the commands from
# docs/HOSTING.md directly.

set -euo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <bundle-dir> <bucket> <distribution-id>" >&2
    exit 2
fi
DIR="$1"
BUCKET="$2"
DIST_ID="$3"
if [ ! -f "$DIR/index.html" ] || [ ! -f "$DIR/ddbsh.wasm" ]; then
    echo "error: $DIR does not look like a packaged bundle (no index.html / ddbsh.wasm);" >&2
    echo "  build it with: cmake --build build-preset/wasm-release --target ddbsh-web-package" >&2
    exit 1
fi

aws s3 sync "$DIR" "s3://$BUCKET/" --delete --exclude "*" --include "*.html" --content-type "text/html; charset=utf-8" --cache-control "no-cache"
aws s3 sync "$DIR" "s3://$BUCKET/" --delete --exclude "*" --include "*.js" --include "*.mjs" --content-type "text/javascript" --cache-control "no-cache"
aws s3 sync "$DIR" "s3://$BUCKET/" --delete --exclude "*" --include "*.wasm" --content-type "application/wasm" --cache-control "no-cache"
aws s3 sync "$DIR" "s3://$BUCKET/" --delete --exclude "*" --include "*.css" --content-type "text/css; charset=utf-8" --cache-control "no-cache"
aws s3 sync "$DIR" "s3://$BUCKET/" --delete --exclude "*" --include "*.json" --content-type "application/json" --cache-control "no-cache"
aws s3 sync "$DIR" "s3://$BUCKET/" --delete --exclude "*" --include "*.txt" --content-type "text/plain; charset=utf-8" --cache-control "no-cache"
aws cloudfront create-invalidation --distribution-id "$DIST_ID" --paths "/*"
