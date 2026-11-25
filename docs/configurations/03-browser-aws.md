<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Configuration 3 — browser edition against AWS DynamoDB

The browser edition is a static web page. It runs the same ddbsh engine as the
native binary, compiled to WebAssembly, and sends each request from the browser
tab straight to Amazon DynamoDB. A pinned copy of the AWS SDK for JavaScript v3
(bundled with the page) signs each request with SigV4 and handles retries and
endpoint resolution. There is no server-side component: a static web host
serves the page ([HOSTING](../HOSTING.md) lists what the host must provide).

The engine still builds every request body and parses every response itself,
so statements map to the same DynamoDB calls, and print the same output, as in
the native binary. The whole golden test suite runs through this path
(`devtools/run-web-golden`, see [TESTING](../TESTING.md)). The
[command reference](../reference/README.md) describes the statements.

## The page

- **The header** shows `ddbsh (web) version <version>` (the engine's version,
  as `show version;` prints it), a **Help ↗** link to this guide, the region,
  `backend: Amazon DynamoDB` and the credentials status. The link opens a new
  tab, so the page keeps your credentials. While a statement or a file runs,
  `running… <N>s` at the right end counts the seconds; screen readers hear
  when it starts and when it finishes. While DynamoDB throttles a request and
  the SDK retries it, the indicator turns orange and reads
  `running… <N>s · throttled, retrying` until that request completes; it also
  pulses, unless your system asks for reduced motion. Other retryable errors
  read `· retrying`. Screen readers hear the first retry of each run.
- **Output appears as it arrives.** Each result row or line is added to the
  transcript as soon as the response that completes it arrives, so a
  `show tables;` over many tables, or a long scan, fills in while it runs.
- **The editor** at the bottom opens at a fifth of the window's height; drag
  its corner to make it taller, up to 40%. Ctrl-Enter (⌘-Enter on a Mac) runs
  everything in it, Enter starts a new line, and the up and down arrows recall
  earlier statements.

## Output formats

Two dropdowns next to **Run** choose how rows are shown:

- **Output**: Default (ddbsh's own output), JSON, CSV, Table (text), or
  Table (HTML). The first four write the same text as the native binary's
  `--output default|json|csv|table`.
- **JSON**: Plain or DynamoDB, as `--json-flavor plain|dynamodb`. It sets how
  JSON writes values, and how the CSV and table cells write maps, lists and
  sets. It is disabled while Output is Default.

**Table (HTML)** is the browser's own format. Each result set is an HTML table
that grows as rows arrive; when a later page brings new attributes, the table
gains columns, and the earlier rows get empty cells in them. Numbers are
right-aligned, and a newline inside a value shows as a line break. A cell
whose row has no such attribute is shaded, so an absent attribute looks
different from an empty string. Status lines, `DESCRIBE`, `EXPLAIN` and errors
stay text between the tables.

The page reads both dropdowns when you run a statement, and does not save
them: a reload starts at Default and Plain. The CLI's batch lines
`set output`, `set json-flavor` and `set terminator` do not exist in the
browser, where they are syntax errors; use the dropdowns instead.

[Output formats](../reference/output-formats.md) describes each format, the
column order, and the edge cases.

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
credentials**. Reloading or leaving the page forgets them, including when the
browser keeps the page in its back/forward cache. The panel shows the masked key
id and the expiry time; when DynamoDB reports the token expired or invalid, the
panel opens again so you can paste new credentials.

## Command history

The page saves the statements you run from the editor (up to 500) in the
browser's `localStorage`, so the up arrow recalls them after a reload.
Statements loaded from a file are not saved, and credentials never are. Saved
statements include any values typed into them: press **Clear history** (in the
toolbar to the right of the editor, below **Clear**) to delete them. It also
empties the history of the page's other open tabs. **Clear** only empties the
transcript.

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

To publish the page for other people, follow [HOSTING](../HOSTING.md). It
covers a private S3 bucket behind CloudFront, deployed with a CloudFormation
template, and what any other host has to provide. In short:

- **Its own origin.** Host the page on a host name of its own, never under a
  path of a blog or another site, and never inside another page's frame.
  Every script on the same origin can read what visitors paste.
- **Only the packaged files.** `cmake --build build-preset/wasm-release
  --target ddbsh-web-package` copies the 11 files the page needs into
  `build-preset/wasm-release/dist/ddbsh-web-<version>/`, with a zip and
  `SHA256SUMS`. Publish that folder, not the build folder (which also holds the
  Node shim, the tests and build files).
- **HTTPS.** The page accepts credentials only in a secure context.
- **`frame-ancestors 'none'` in a response header.** `index.html` carries its
  Content-Security-Policy as a `<meta>` tag, which cannot set
  `frame-ancestors`. Send only that directive as a header; browsers apply it
  together with the `<meta>` policy. The full policy is about 14,000
  characters, too long for header settings such as CloudFront's (at most
  1,783 characters).
- **Content types**, in particular `application/wasm` for `ddbsh.wasm` and
  `text/javascript` for the `.js` and `.mjs` modules.
- **`Cache-Control: no-cache` on every file.** The file names carry no
  version, so with a longer cache time a returning visitor's browser can run
  a new `index.html` with older scripts.

The page holds no secrets and needs no sign-in gate. No cross-origin-isolation
headers are needed.

## What the page refuses, and why

| Refused | Why |
|---|---|
| Long-term `AKIA…` keys, or any set without a session token | A long-term key pasted into a web page is a standing credential at risk; short-term credentials expire. |
| Credentials outside a secure context (`https://` or `http://localhost`) | A page served over plain HTTP can be modified in transit to read what you paste. |
| Credentials when the page is inside a frame | The framing page can overlay or restyle it to capture what you type. The credentials panel is disabled; open the page directly in its own tab. |
| Credentials when another page's script opened the window | A same-origin opener can read everything in the page. The credentials panel is disabled; open the page by typing or pasting its address. |
| `CONNECT … WITH ENDPOINT` to anything but AWS DynamoDB | The request carries your session token. Only `https://dynamodb.<region>.amazonaws.com`, `https://dynamodb-fips.<region>.amazonaws.com`, `https://dynamodb.<region>.api.aws` and `https://dynamodb-fips.<region>.api.aws` are allowed, for the regions of the `aws` and `aws-us-gov` partitions. |

The credentials text box and **Use credentials** stay disabled until the
page's script has checked the secure context, the frame and the opener (rows
two to four above), so a copy of the page whose script cannot run (scripts
blocked, or a file served with the wrong type) never asks for credentials.

The page's Content-Security-Policy (`connect-src`) allows those hosts plus
`search-dynamodb[-fips].<region>.{amazonaws.com,api.aws}`, for the same regions.
The SDK sends vector search (`SearchVectors`, used by `SELECT … NEAREST`) to the
`search-dynamodb` host; `CONNECT … WITH ENDPOINT` does not accept it as an
override. The policy is in the `<meta>` tag of `index.html`, so a host does not
copy it: the host's header carries only `frame-ancestors`. The policy allows
scripts only from the page's own origin and enforces Trusted Types (no script
may pass a string to an HTML or script injection point).

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
ddbsh - version 0.12.6
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
- Reloading or leaving the page forgets the credentials.
- Service errors carry no request id (the native binary prints one).

Developer details (layout, rebuilding the SDK bundle, the gates) are in
[`web/README.md`](../../web/README.md). For the overview of all configurations,
see the [configurations index](README.md).
