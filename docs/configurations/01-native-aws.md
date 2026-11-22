<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Configuration 1 — native ddbsh against AWS DynamoDB

The default. Run `ddbsh` with no endpoint override; it resolves credentials from
the standard AWS chain (environment variables, `~/.aws/credentials`/`config`
profiles, `AWS_PROFILE`, container/instance roles, and `role_arn` +
`source_profile` AssumeRole) and the region from `AWS_REGION`, then
`AWS_DEFAULT_REGION`, then `us-east-1`.

```
$ ddbsh
ddbsh - version 0.12.1
us-east-1>
```

For the full three-configuration overview, see the
[configurations index](README.md).

