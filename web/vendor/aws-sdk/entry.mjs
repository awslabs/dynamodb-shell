// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// entry.mjs — the esbuild entry point for the vendored AWS SDK for JavaScript v3
// bundle (P9, D-P9-5 / D-P9-02-8). devtools/vendor-aws-sdk-js bundles this into
// aws-sdk-dynamodb.mjs, the only SDK file the browser edition loads.
//
// It exports the DynamoDB client plus exactly the command classes for the
// operations the ddbsh-wire codec emits. web/sdk-bridge.mjs keeps the matching
// OPERATIONS allowlist; sdk-bridge.test.mjs checks the two agree. Adding an
// operation to the codec means adding its command here, rebuilding the bundle,
// and adding it to OPERATIONS.

export {
    DynamoDBClient,
    CreateBackupCommand,
    CreateTableCommand,
    DeleteBackupCommand,
    DeleteItemCommand,
    DeleteTableCommand,
    DescribeBackupCommand,
    DescribeContinuousBackupsCommand,
    DescribeLimitsCommand,
    DescribeTableCommand,
    DescribeTableReplicaAutoScalingCommand,
    DescribeTimeToLiveCommand,
    GetItemCommand,
    ListBackupsCommand,
    ListTablesCommand,
    PutItemCommand,
    QueryCommand,
    RestoreTableFromBackupCommand,
    RestoreTableToPointInTimeCommand,
    ScanCommand,
    SearchVectorsCommand,
    TransactGetItemsCommand,
    TransactWriteItemsCommand,
    UpdateContinuousBackupsCommand,
    UpdateItemCommand,
    UpdateTableCommand,
    UpdateTimeToLiveCommand,
} from "@aws-sdk/client-dynamodb";
