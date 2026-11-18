<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Control commands

ddbsh exposes a readline based interface. The UP-ARROW and DOWN-ARROW keys allow you to view previous commands, and pick a previous command to reexecute. (On how a batch of statements is assembled and submitted, see [isql-mode input](README.md#isql-mode-input).)

Commands are terminated with the ';' character.

## CONNECT

**Purpose:** Connect to a different region or endpoint

**Syntax:** CONNECT *region* [WITH ENDPOINT endpoint] ;

```
$ ./ddbsh
ddbsh - version 0.2
us-east-1> connect us-west-1;
CONNECT
us-west-1>
```

The prompt shows the region name, and the additional (*) if you are connected to a non-standard endpoint.

## SHOW VERSION

**Purpose:** The SHOW VERSION command displays the version of DynamoDB Shell.

``` SQL
us-east-1> show version;
ddbsh - version 0.5.1
us-east-1>
```

*Note:* The SHOW VERSION command was added in DynamoDB Shell version 0.5.1.

## SHOW CONNECTION

**Purpose:** The SHOW CONNECTION command reports the running ddbsh version and the
current backend binding — the region, the endpoint (or `(default)` when no
endpoint override is set), and which transport the engine is talking through.

**Syntax:** SHOW CONNECTION ;

``` SQL
us-east-1> show connection;
ddbsh - version 0.11.18
Region: us-east-1
Endpoint: (default)
Backend: aws-sdk-cpp (direct DynamoDB)
us-east-1>
```

The `Backend` line names the active transport. The native binary reports
`aws-sdk-cpp (direct DynamoDB)`; the browser/WebAssembly builds report their
respective transports (the HTTP fetch bridge to `ddbsh-web-proxy`, or the in-tab
ExtendDB submit transport). The `Endpoint` line shows the overridden endpoint when
one is set (the same condition that adds the `(*)` suffix to the prompt).

*Note:* The SHOW CONNECTION command was added in DynamoDB Shell version 0.11.10.

## QUIT

**Purpose:** You have had a good day, and you want to do something else.

**Syntax:** QUIT

