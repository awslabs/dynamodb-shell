<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Configuration 2 — native ddbsh against a local ExtendDB server

ExtendDB serves the DynamoDB wire protocol over HTTPS with a **self-signed
certificate** and requires its **own issued SigV4 key** (it is not AWS; your real
AWS credentials do not apply). Point ddbsh at it with three things: an endpoint
override, a dedicated `~/.aws` profile holding the ExtendDB-issued key, and a CA
bundle so the TLS handshake trusts the self-signed cert.

### 1. Build and start ExtendDB

Build ExtendDB with a SQLite backend and start it (see the ExtendDB documentation
for the authoritative instructions):

```
cargo build --release --no-default-features --features sqlite
extenddb init --backend sqlite        # prints the admin password once
extenddb serve                        # serves HTTPS, e.g. on 127.0.0.1:18443
```

Create an IAM user, attach a policy, and issue an access key through ExtendDB's
management surface (the SQLite data file and the self-signed TLS cert live under
ExtendDB's data directory, e.g. `~/.extenddb/tls/cert.pem`):

```
extenddb manage ... create-user ddbsh
extenddb manage ... put-user-policy ddbsh --policy '{ "dynamodb:*" on "*" }'
extenddb manage ... create-access-key ddbsh
```

### 2. Store the ExtendDB key in a dedicated AWS profile

Put the **ExtendDB-issued** key in its own profile — never mix it with real AWS
keys:

```
# ~/.aws/credentials
[extenddb]
aws_access_key_id     = <extenddb-issued-access-key-id>
aws_secret_access_key = <extenddb-issued-secret-key>
```

### 3. Point ddbsh at ExtendDB

```
export DDBSH_ENDPOINT_OVERRIDE="https://127.0.0.1:18443"
export AWS_PROFILE="extenddb"
export AWS_REGION="us-east-1"

ddbsh --ca-bundle ~/.extenddb/tls/cert.pem
```

`--ca-bundle` makes the AWS SDK trust ExtendDB's self-signed certificate: ddbsh
wires the resolved path to the SDK client's `caFile`. This mirrors the AWS CLI's
`--ca-bundle`. Without a CA bundle, the TLS handshake fails closed with a
self-signed-certificate error.

ddbsh then behaves exactly as it does against AWS — the `(*)` suffix on the prompt
indicates the non-standard endpoint:

```
ddbsh - version 0.12.4
us-east-1 (*)>
```

### The CA bundle: three sources and their precedence

The AWS SDK for C++ does **not** read a CA bundle from the environment or the
`~/.aws` profile on its own, so ddbsh wires the trust path itself. It accepts the
CA bundle from three sources, in this precedence order (highest first):

1. **`--ca-bundle <file>` CLI flag.**
2. **`AWS_CA_BUNDLE` environment variable.**
3. **`ca_bundle` in the resolved `~/.aws` config profile** (lowest).

Whichever is set first wins; if none is set, the system trust store is used (and
the self-signed ExtendDB cert fails closed). The resolved path is assigned to the
SDK client's `caFile` verbatim — use an **absolute path** (no `~` expansion).
Whatever source supplies it, the trust setting is preserved across a `CONNECT` to
another region/endpoint.

The profile `ca_bundle` is read from the **same profile the credentials resolve
from**: the explicit profile if one is set, else `AWS_PROFILE`, else
`AWS_DEFAULT_PROFILE`, else `default`. Set and verify it with the AWS CLI (it is
written under `[profile <profile_name>]` in `~/.aws/config`, not the credentials
file):

```
# set the profile ca_bundle (writes ~/.aws/config)
aws configure set ca_bundle /path/to/custom-ca-bundle.pem --profile <profile_name>

# read it back to confirm
aws configure get ca_bundle --profile <profile_name>
```

With the profile source, the ExtendDB launch above needs no `--ca-bundle` flag
and no `AWS_CA_BUNDLE`: with `AWS_PROFILE=extenddb` and that profile carrying

```
# ~/.aws/config
[profile extenddb]
ca_bundle = /home/you/.extenddb/tls/cert.pem
```

`ddbsh` trusts ExtendDB's cert purely from the profile key.


For the full three-configuration overview, see the
[configurations index](README.md).
