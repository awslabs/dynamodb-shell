<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Continuous integration

This note is for maintainers. It covers the GitHub workflows, the AWS accounts
they use, and the publish of the project's site, <https://www.ddbsh.org/>.
[TESTING](TESTING.md) describes the suites themselves.

## Contents

- [What runs when](#what-runs-when)
- [AWS accounts and roles](#aws-accounts-and-roles)
- [GitHub environments](#github-environments)
- [The golden suite in CI](#the-golden-suite-in-ci)
- [The site publish](#the-site-publish)
- [Releasing a version](#releasing-a-version)
- [Updating the pins](#updating-the-pins)
- [Rules for workflow changes](#rules-for-workflow-changes)

## What runs when

| Workflow | When | Jobs | AWS |
|---|---|---|---|
| `test.yml` | every pull request, every push to `main`, on demand | `aws-sdk` builds and caches the pinned AWS SDK for C++; `native` runs the native unit tests; `sanitize` runs them under ASan, UBSan and LSan; `format` checks clang-format 15; `real-dynamodb` runs the golden suite (or, for a pull request from a fork, `real-dynamodb-fork-notice`) | `real-dynamodb` only, environment `ci` |
| `build.yml` | every pull request, a created release, on demand | `build-linux` builds the release binary; `build-wasm` builds, packages and checks the browser bundle | none |
| `web-bundle.yml` | called by `build.yml` and `publish-site.yml` | builds the browser bundle, runs the offline web gates and uploads the artifact `ddbsh-web-bundle` | none |
| `publish-site.yml` | every tag `vMAJOR.MINOR.PATCH`, on demand on a tag | `bundle` (the reusable workflow), then `publish` | `publish` only, environment `site` |
| `homebrew.yml` | a created release, on demand | updates the `aws/homebrew-tap` formula | none |

The real-DynamoDB job needs the `native` job, so a pull request that fails
offline never takes a golden-suite slot.

## AWS accounts and roles

GitHub signs in to AWS through OIDC; no AWS credential is stored on GitHub.
Each role below trusts only GitHub's OIDC provider, with audience
`sts.amazonaws.com` and the exact subject
`repo:awslabs/dynamodb-shell:environment:<environment>`, except the cross
role, which trusts only the CI main role.

| Account | Used for | Role | What the role may do |
|---|---|---|---|
| 291751120739 | CI main: the whole golden suite, in us-east-1, plus us-east-2 and us-west-1 for the global-tables family | `ddbsh-ci-tests` (environment `ci`; sessions up to 2 hours) | DynamoDB on this account's resources in any region; account-level DynamoDB `List*` and `Describe*`; reads of replica auto scaling; assume `ddbsh-ci-cross`. It may never put or delete a table's resource policy. |
| 457765142880 | CI cross: the cross-account family | `ddbsh-ci-cross`, assumed from `ddbsh-ci-tests` | DynamoDB on `ddbsh_test_table_1` in us-east-1 only; `ListTables`, `DescribeLimits`, `DescribeEndpoints`. No resource policies. |
| 321345175056 | the site (stack `ddbsh-web-site`) | `ddbsh-web-site-publisher` (environment `site`) | only the template's deployer policy: list the bucket, put and delete its objects, invalidate the distribution. It cannot read the bucket's settings or an invalidation's status. |

The CI accounts hold no tables but the suite's. Local test runs use other
accounts, so a local run and a CI run never delete each other's tables.

## GitHub environments

| Environment | Protection | Variables (no secrets) |
|---|---|---|
| `ci` | none, so pull requests, `main` and manual runs can use it | `CI_ROLE_ARN` (`arn:aws:iam::291751120739:role/ddbsh-ci-tests`), `CI_CROSS_ROLE_ARN` (`arn:aws:iam::457765142880:role/ddbsh-ci-cross`), `CI_REGION` (`us-east-1`) |
| `site` | deployments only from tags matching `v[0-9]*`; no reviewers and no wait timer | `AWS_ROLE_ARN` (the publisher role), `SITE_BUCKET`, `SITE_DISTRIBUTION_ID`, `SITE_REGION` |

The workflows also fix the accounts themselves: the credentials step accepts
only 291751120739 in `test.yml` and only 321345175056 in `publish-site.yml`,
and the cross-account profile names the cross role in 457765142880. A changed
variable therefore cannot point a run at another account; that takes a
reviewed change to the workflow.

## The golden suite in CI

The `real-dynamodb` job:

1. Builds `ddbsh` and configures the golden suite with
   `devtools/run-tests --build-only`, before any credential exists on the
   runner.
2. Signs in as `ddbsh-ci-tests` with `aws-actions/configure-aws-credentials`,
   which writes the session to the `[default]` profile in `~/.aws`
   (`aws-profile: default`) and exports no `AWS_*` variable.
3. Adds a `cross-acct` profile: `role_arn` is the cross role and
   `source_profile` is `default`. ddbsh performs the AssumeRole into the cross
   account itself, the path the cross-account tests exist to exercise.
4. Runs `devtools/check-ci-target`, which fails the job unless:
   - no credential, profile or endpoint override is in the environment;
   - the AWS CLI resolves `[default]` to the CI main role and `cross-acct` to
     the cross role;
   - ddbsh itself, run as the cross-account family runs it, names the cross
     role in DynamoDB's access-denied message for a table the role may not
     read.
5. Runs the golden suite with `devtools/run-tests` (90 minutes at most).
6. Always runs the `test-cleanup` family afterwards (`ctest -R '^test-cleanup'`),
   which drops the suite's fixed tables in both accounts after a pass, a
   failure or a cancellation.
7. Uploads `discussions/test-*` as the artifact `real-dynamodb-<run>-<attempt>`
   (kept 14 days) and adds the run's summary to the job summary.

**Why a profile and never environment variables.** ddbsh takes credentials
from `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY` and `AWS_SESSION_TOKEN` before
it reads any profile. If the session were in the environment, which is
`configure-aws-credentials`' default, ddbsh would ignore
`AWS_DEFAULT_PROFILE=cross-acct`, and the whole cross-account family would run
in the main account with every test passing. The target check fails the job
when any of those variables is set.

**One run at a time.** The suite's table names are fixed, so the job is in the
concurrency group `real-dynamodb`. A running job is never cancelled. GitHub
keeps at most one waiting job per group, so a newer waiting job replaces an
older one; the replaced job shows as cancelled but never touched AWS. Re-run
it if you need its result.

**Leftover tables.** The cleanup step drops the tables that most families
create. The composite-GSI, vector, explain, show and global-tables families
drop their own tables at their end, so a run that stops in the middle of one of
them can leave its tables behind. A leftover usually makes one later run fail,
and that run removes it. When no run is in progress, any table in the two CI
accounts is a leftover; a maintainer with access to the account can remove it:

```sh
aws dynamodb list-tables --region us-east-1
aws dynamodb delete-table --table-name <table> --region us-east-1
```

`gttest`, the global-tables family's table, lives in us-east-2 with a replica in
us-west-1. Remove the replica first, wait until it is gone from the table's
`Replicas`, then delete the table:

```sh
aws dynamodb update-table --table-name gttest --region us-east-2 --replica-updates '[{"Delete":{"RegionName":"us-west-1"}}]'
aws dynamodb describe-table --table-name gttest --region us-east-2 --query "Table.Replicas"
aws dynamodb delete-table --table-name gttest --region us-east-2
```

**Pull requests from forks.** GitHub gives a pull request from a fork no OIDC
token, so the golden suite cannot run for it. The job "Golden suite (real
DynamoDB): fork pull request" fails with that explanation instead of skipping
silently. To run the suite, a maintainer pushes the branch to
`awslabs/dynamodb-shell` and opens a pull request from it.

## The site publish

`publish-site.yml` runs when a tag such as `v0.12.2` is pushed. The `bundle`
job builds the bundle from the tagged commit with `web-bundle.yml`, the same
build pull requests run. The `publish` job, in the `site` environment:

1. **Guards**, before any AWS call. The run must be on a tag
   `vMAJOR.MINOR.PATCH`; the tag's commit must be on `main`; the tag must be the
   newest such tag on `main`, so an older tag cannot replace a newer release;
   and the environment's variables must be well formed.
2. **Checks the downloaded bundle**: `SHA256SUMS`, the folder name
   `ddbsh-web-<version>` (which comes from the CMake project version, so it
   must match the tag), and the folder's files against `web/bundle-files.txt`.
3. **Signs in** as `ddbsh-web-site-publisher`, again as a profile, for 15
   minutes.
4. **Uploads** with the tag's own `web/hosting/aws/deploy.sh`, which also
   invalidates the distribution.
5. **Checks the live site anonymously**:
   `devtools/check-web-hosting --anonymous https://www.ddbsh.org/ --bucket-domain <bucket>.s3.<region>.amazonaws.com --wait 900`.
   It waits up to 15 minutes for the site to serve the new files, then checks
   every file's status, content type, `Cache-Control`, security headers and
   sha256, the HTTP redirect, and that the bucket refuses anonymous reads. The
   publisher role cannot read the invalidation, so serving the right bytes is
   the test. The log is kept as the artifact `site-publish-<run>-<attempt>`.

Two `publish` runs never overlap (concurrency group `site-publish`).

**When a publish fails.**
- A failure before step 4 leaves the site unchanged.
- A failure in step 4 or 5 can leave the site with a mix of old and new files,
  or with files that were not checked. Use **Re-run failed jobs**: the upload
  sends what changed and the check runs again. GitHub allows re-runs for 30
  days; after that, run the workflow by hand (**Run workflow**) on the tag.
- Running the workflow on a branch fails: the `site` environment accepts only
  tags.

**Rolling back** is a fix forward: revert the change on `main`, then tag the
next patch version. The workflow has no way to publish an older version. The
by-hand procedure in [HOSTING](HOSTING.md#publish-a-new-version) still works
for anyone with the deployer policy.

## Releasing a version

1. Merge the pull request on GitHub.
2. Tag the merge commit `vMAJOR.MINOR.PATCH` and push the tag. This runs the
   site publish.
3. When the publish is green, create the GitHub Release from the tag. Creating
   a Release runs `build.yml` and `homebrew.yml`, which updates the
   `aws/homebrew-tap` formula, so create it deliberately and last.

## Updating the pins

- **Actions** are pinned by full commit SHA, with the release in a comment
  (`uses: actions/checkout@<sha> # v7.0.1`). To update one, look up the
  release's commit (for an annotated tag, the commit it points to), replace
  the SHA and the comment in every workflow that uses the action, and run
  [actionlint](https://github.com/rhysd/actionlint) on the workflows.
- **The AWS SDK for C++** is pinned in `test.yml` by `AWS_SDK_CPP_VERSION` and
  `AWS_SDK_CPP_COMMIT`; change both together. The `aws-sdk` job refuses a tag
  whose commit differs. The cache key contains the version, the runner image
  and the compiler version, so a change to any of them builds the SDK once
  more; raise the key's `-r1` suffix when the build recipe changes.
- **Emscripten** is pinned to 3.1.61 in `web-bundle.yml`, and the offline web
  gates run on Node 22.
- **clang-format** is 15 (15.0.7, the `clang-format-15` package of Ubuntu
  24.04). The `format` job runs on `ubuntu-24.04` so the version stays fixed.
  Format with the same major version locally:
  `git ls-files -z '*.cpp' '*.hpp' '*.h' '*.cc' '*.c' | xargs -0 clang-format-15 --dry-run --Werror`.

## Rules for workflow changes

- Every workflow starts with `permissions: {}`, and each job asks only for
  what it needs: `contents: read`, plus `id-token: write` for the two jobs
  that sign in to AWS.
- `actions/checkout` runs with `persist-credentials: false`.
- Pull requests trigger workflows through `pull_request`, never
  `pull_request_target`, which would run a fork's code with this repository's
  token, environments and AWS roles.
- No `${{ }}` expression goes inside a `run:` script. Event data and variables
  reach a script through `env:`, quoted.
- Credentials reach a runner only as a profile (`aws-profile`), never as
  environment variables.
- Adding or removing a programming language in the repository also needs the
  repository's CodeQL language list updated in its settings.
