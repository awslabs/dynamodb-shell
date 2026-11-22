<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Running ddbsh

## Getting started

Running ddbsh is as simple as executing the ddbsh command. ddbsh is written in C++ and uses the DynamoDB API through the AWS DynamoDB SDK. Assuming that you built it as above, and completed the install, then you should be able to run it with just this.

```
$ ddbsh
ddbsh - version 0.2
us-east-1> _
```

When you connect to ddbsh, you are automatically connected to an AWS region. In the illustration above, the default region was configured in the AWS configuration file ~/.aws/config

```
$ more ~/.aws/config
[default]
region=us-east-1
output=text
```

For more information visit [AWS configuration and credentials files](https://docs.aws.amazon.com/sdkref/latest/guide/file-format.html).

## Environment Variables

ddbsh respects the default AWS environment variables, see [How to set environment variables](https://docs.aws.amazon.com/sdkref/latest/guide/environment-variables.html).

### DDBSH\_ENDPOINT\_OVERRIDE

Used to specify an alternate endpoint, such as when using ExtendDB.


## The three configurations

ddbsh runs in three configurations, formed by two choices: **which form of
ddbsh** (the native binary or the browser edition) and **which backend** (real
Amazon DynamoDB or a self-hosted [ExtendDB](https://github.com/awslabs/extenddb)
server). ExtendDB is a local, self-hosted, DynamoDB-compatible server; it is the
recommended way to experiment with ddbsh without touching a real AWS account or
incurring cost.

| # | ddbsh form | Backend | How it connects |
|---|---|---|---|
| [1](01-native-aws.md) | native binary | AWS DynamoDB | default AWS credential chain, standard regional endpoint |
| [2](02-native-extenddb.md) | native binary | ExtendDB on a host | `DDBSH_ENDPOINT_OVERRIDE` + an ExtendDB-issued key in a dedicated `~/.aws` profile + a CA bundle for the self-signed cert |
| [3](03-browser-aws.md) | browser (static page, WebAssembly) | AWS DynamoDB | the tab calls DynamoDB directly; the bundled AWS SDK for JavaScript signs with short-term credentials you paste into the page |

Each configuration has its own page (linked from the table above). The browser
edition against ExtendDB is not available; use the native binary for ExtendDB.
To publish the browser edition for others, see [HOSTING](../HOSTING.md).
Developer details for the browser edition are in
[`web/README.md`](../../web/README.md).


## Cross Account Access

In DynamoDB, a user can access resources (tables, indices) created and owned by another user using IAM policies. This is called a "Role Based Policy". You can find information about how to do this in the DynamoDB documentation [here](https://docs.aws.amazon.com/prescriptive-guidance/latest/patterns/configure-cross-account-access-to-amazon-dynamodb.html).

The steps are described in brief below. Two accounts are involved here, one called the "ACTOR", and the other called "OWNER". The "OWNER" is the one who owns the table or index. The ACTOR is another account that is permitted access by the OWNER.

1. As the OWNER, create a table
2. As the OWNER, create a role with an AWS account as the trusted entity. Specify the account id for the ACTOR here. For the purpose of testing, I used the policy AmazonDynamoDBFullAccess. This is available in the web interface and a bit harder with the CLI.
3. Find the ARN for the newly created policy.
4. As the ACTOR, permit the user to Assume the role based on its ARN.
5. Configure the ~/.aws/credentials file

If you get this configured right, you can set the environment variable AWS_DEFAULT_PROFILE and specify the ARN in that profile section.

In my test setup, and for the purposes of running tests, I have a profile called cross-acct in my ~/.aws/credentials file, and it looks like this.

```
[default]
aws_access_key_id=<access-key-id-here>
aws_secret_access_key=<secret-key-here>

[cross-acct]
role_arn=arn:aws:iam::OWNER:role/<role-name>
source_profile=default
```

where OWNER is the account ID of the owner account, and ACTOR is the account ID of the actor account.

When using DynamoDB Shell, you have to set AWS_DEFAULT_PROFILE like this.

```
$ export AWS_DEFAULT_PROFILE=cross-acct
$ ddbsh
```

Note that if you put the configuration of the profile in the config file, the format is slightly different. You must prefix the profile name with the word "profile".

The same cross-acct setting in the config file would look like this.

```
[profile cross-acct]
role_arn=arn:aws:iam::OWNER:role/<role-name>
source_profile=default
```

