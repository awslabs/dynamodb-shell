<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Configuration 3 — browser edition against AWS DynamoDB

The browser edition is a static web page. It runs the same ddbsh engine as the
native binary, compiled to WebAssembly, and sends each request from the browser
tab straight to Amazon DynamoDB. A pinned copy of the AWS SDK for JavaScript v3
(bundled with the page) signs each request with SigV4 and handles retries and
endpoint resolution. There is no server-side component: any static web server
can host the page.

The engine still builds every request body and parses every response itself,
so statements map to the same DynamoDB calls, and print the same output, as in
the native binary. The whole golden test suite runs through this path
(`devtools/run-web-golden`, see [TESTING](../TESTING.md)).

## Short-term credentials

The page asks you to paste **short-term** AWS credentials: an access key id that
starts with `ASIA`, a secret access key, and a session token. Get them with the
AWS CLI and paste the command's output as-is:

```sh
# IAM user keys (add --serial-number/--token-code for MFA):
aws sts get-session-token --duration-seconds 3600

# A role (recommended: a role scoped to the tables you intend to use):
aws sts assume-role --role-arn <scoped-role> --role-session-name ddbsh-web

# An IAM Identity Center (SSO) profile:
aws sso login --profile <p> && aws configure export-credentials --profile <p> --format env
```

The page reads the JSON these commands print, the `--format process` JSON, and
`AWS_ACCESS_KEY_ID=…` style environment lines (bash `export`, PowerShell
`$Env:`, and cmd `set` forms).

A role scoped to the tables you plan to use limits what a leaked session token
can do. A minimal policy for data-plane statements (`SELECT`, `INSERT`,
`UPDATE`, `DELETE`, transactions, `DESCRIBE`) on one table looks like this; DDL
(`CREATE`/`DROP`/`ALTER TABLE`, backups, PITR) needs the matching additional
actions:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": [
        "dynamodb:DescribeTable", "dynamodb:DescribeTimeToLive",
        "dynamodb:DescribeContinuousBackups",
        "dynamodb:GetItem", "dynamodb:Query", "dynamodb:Scan",
        "dynamodb:PutItem", "dynamodb:UpdateItem", "dynamodb:DeleteItem",
        "dynamodb:ConditionCheckItem"
      ],
      "Resource": [
        "arn:aws:dynamodb:us-east-1:<account>:table/<table>",
        "arn:aws:dynamodb:us-east-1:<account>:table/<table>/index/*"
      ]
    },
    { "Effect": "Allow", "Action": "dynamodb:ListTables", "Resource": "*" }
  ]
}
```

Credentials are kept in the tab's memory only. They are never written to
`localStorage`, `sessionStorage`, cookies, IndexedDB, or the URL, and never
logged. The credentials text box is cleared as soon as you press **Use
credentials**. Reloading the page forgets them. The panel shows the masked key
id and the expiry time; when DynamoDB reports the token expired or invalid, the
panel opens again so you can paste new credentials.

## Serving the page

Build the bundle and serve it on this machine (loopback only):

```sh
emcmake cmake --preset wasm-release && cmake --build build-preset/wasm-release
devtools/serve-web 8080         # = python3 -m http.server --bind 127.0.0.1 8080
```

Then open <http://localhost:8080/>. To use it from another machine, tunnel the
port rather than binding a network interface:

```sh
ssh -L 8080:localhost:8080 <build-host>     # then open http://localhost:8080/
```

`http://localhost` counts as a secure context; plain HTTP on a LAN address does
not, and the page refuses credentials there.

To host the page (S3 + CloudFront, GitHub Pages, any static host), publish the
contents of `build-preset/wasm-release/web/` except the Node files
(`ddbsh-node.mjs`, `live-smoke.mjs`, `*.test.mjs`). The page holds no secrets and
needs no sign-in gate. Hosting checklist:

- **HTTPS.** The page accepts credentials only in a secure context.
- **CSP response header.** `index.html` carries its Content-Security-Policy as a
  `<meta>` tag, which cannot set `frame-ancestors`. Send the same policy as a
  response header and add `frame-ancestors 'none'`, so the page cannot be framed
  (clickjacking) while it holds credentials.
- **`application/wasm`.** Serve `.wasm` files with this MIME type (set
  `Content-Type` on upload to S3). Without it the browser falls back to a slower,
  non-streaming WebAssembly load.

No cross-origin-isolation headers are needed.

## What the page refuses, and why

| Refused | Why |
|---|---|
| Long-term `AKIA…` keys, or any set without a session token | A long-term key pasted into a web page is a standing credential at risk; short-term credentials expire. |
| Credentials outside a secure context (`https://` or `http://localhost`) | A page served over plain HTTP can be modified in transit to read what you paste. |
| `CONNECT … WITH ENDPOINT` to anything but AWS DynamoDB | The request carries your session token. Only `https://dynamodb.<region>.amazonaws.com`, `https://dynamodb-fips.<region>.amazonaws.com`, `https://dynamodb.<region>.api.aws` and `https://dynamodb-fips.<region>.api.aws` are allowed, for the regions of the `aws` and `aws-us-gov` partitions. |

The page's Content-Security-Policy enforces the same host list for every network
connection (`connect-src`), and allows scripts only from the page's own origin.

## CONNECT in the browser

`CONNECT <region>` re-targets the session at that region's DynamoDB endpoint, as
in the native binary. `CONNECT <region> WITH ENDPOINT "<url>"` accepts only the
four endpoint forms above, with no path, query or port, and with the same region
in the host name as the `CONNECT` names (SigV4 signs for that region).

When the page refuses a `CONNECT`, the statement still prints `CONNECT` (as the
native binary does when a reconnect fails), followed by a
`browser: CONNECT rejected: <reason>` line. Every later statement fails with
`wire transport: CONNECT rejected: <reason>` until a valid `CONNECT`, so the
session never keeps talking to the previous target by mistake.

## SHOW CONNECTION

```
us-east-1> show connection;
ddbsh - version 0.11.19
Region: us-east-1
Endpoint: (default)
Backend: wire/aws-sdk-js 3.1174.0 -> https://dynamodb.us-east-1.amazonaws.com (default); credentials ASIA…WXYZ expires 2026-11-20T23:03:00.000Z
```

The `Backend` line names the bundled SDK version, the target, and the masked
access key id with its expiry (or `credentials none`). It never shows the secret
or the session token.

## Limitations

- AWS DynamoDB only. The browser edition does not connect to ExtendDB; use the
  native binary for that ([configuration 2](02-native-extenddb.md)).
- No profile, SSO or `~/.aws` resolution in the page; paste credentials from the
  CLI commands above.
- Reloading the page forgets the credentials.
- Service errors carry no request id (the native binary prints one).

Developer details (layout, rebuilding the SDK bundle, the gates) are in
[`web/README.md`](../../web/README.md). For the overview of all configurations,
see the [configurations index](README.md).
