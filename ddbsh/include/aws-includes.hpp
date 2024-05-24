// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * aws-includes.hpp
 *
 * A single place to put all the AWS includes.
 *
 * Amrith. 2022-08-15
 */

#ifndef __AWS_INCLUDES_HPP_DEFINED__
#define __AWS_INCLUDES_HPP_DEFINED__

#include <aws/core/Aws.h>
#include <aws/core/utils/Outcome.h>
#include <aws/dynamodb/DynamoDBClient.h>

#include <aws/dynamodb/model/DescribeTableRequest.h>

#include <aws/dynamodb/model/ListTablesRequest.h>
#include <aws/dynamodb/model/ListTablesResult.h>

#include <aws/dynamodb/model/DeleteTableRequest.h>
#include <aws/dynamodb/model/DeleteTableResult.h>

#include <aws/dynamodb/model/ScalarAttributeType.h>
#include <aws/dynamodb/model/AttributeDefinition.h>
#include <aws/dynamodb/model/BillingMode.h>
#include <aws/dynamodb/model/KeySchemaElement.h>
#include <aws/dynamodb/model/ProvisionedThroughput.h>
#include <aws/dynamodb/model/GlobalSecondaryIndex.h>
#include <aws/dynamodb/model/LocalSecondaryIndex.h>
#include <aws/dynamodb/model/StreamSpecification.h>
#include <aws/dynamodb/model/SSESpecification.h>
#include <aws/dynamodb/model/TableClass.h>
#include <aws/dynamodb/model/Tag.h>
#include <aws/dynamodb/model/CreateTableRequest.h>
#include <aws/dynamodb/model/GlobalSecondaryIndexUpdate.h>
#include <aws/dynamodb/model/UpdateGlobalSecondaryIndexAction.h>
#include <aws/dynamodb/model/UpdateTableRequest.h>
#include <aws/dynamodb/model/TableClassSummary.h>
#include <aws/dynamodb/model/DescribeTimeToLiveRequest.h>
#include <aws/dynamodb/model/UpdateTimeToLiveRequest.h>
#include <aws/dynamodb/model/TimeToLiveSpecification.h>
#include <aws/dynamodb/model/DescribeLimitsRequest.h>
#include <aws/dynamodb/model/ListBackupsRequest.h>
#include <aws/dynamodb/model/DescribeBackupRequest.h>
#include <aws/dynamodb/model/CreateBackupRequest.h>
#include <aws/dynamodb/model/DeleteBackupRequest.h>
#include <aws/dynamodb/model/RestoreTableFromBackupRequest.h>
#include <aws/dynamodb/model/ReturnConsumedCapacity.h>
#include <aws/dynamodb/model/ConsumedCapacity.h>

#include <aws/dynamodb/model/GetItemRequest.h>
#include <aws/dynamodb/model/QueryRequest.h>
#include <aws/dynamodb/model/ScanRequest.h>
#include <aws/dynamodb/model/PutItemRequest.h>
#include <aws/dynamodb/model/UpdateItemRequest.h>
#include <aws/dynamodb/model/DeleteItemRequest.h>

#include <aws/dynamodb/model/TransactWriteItemsRequest.h>
#include <aws/dynamodb/model/TransactWriteItem.h>
#include <aws/dynamodb/model/Put.h>
#include <aws/dynamodb/model/Delete.h>
#include <aws/dynamodb/model/Update.h>

#include <aws/dynamodb/model/TransactGetItemsRequest.h>
#include <aws/dynamodb/model/TransactGetItem.h>
#include <aws/dynamodb/model/Get.h>

#include <aws/dynamodb/model/UpdateContinuousBackupsRequest.h>
#include <aws/dynamodb/model/DescribeContinuousBackupsRequest.h>

#include <aws/dynamodb/model/RestoreTableToPointInTimeRequest.h>

#include <aws/dynamodb/model/TransactGetItem.h>
#include <aws/dynamodb/model/Get.h>
#include <aws/dynamodb/model/TransactGetItemsRequest.h>

#include <aws/dynamodb/model/TransactWriteItem.h>
#include <aws/dynamodb/model/ConditionCheck.h>
#include <aws/dynamodb/model/Put.h>
#include <aws/dynamodb/model/Delete.h>
#include <aws/dynamodb/model/Update.h>
#include <aws/dynamodb/model/TransactWriteItemsRequest.h>

#include <aws/dynamodb/model/ReplicationGroupUpdate.h>
#include <aws/dynamodb/model/CreateReplicationGroupMemberAction.h>
#include <aws/dynamodb/model/UpdateReplicationGroupMemberAction.h>
#include <aws/dynamodb/model/DeleteReplicationGroupMemberAction.h>

#include <aws/dynamodb/model/DescribeTableReplicaAutoScalingRequest.h>
#include <aws/dynamodb/model/DescribeTableReplicaAutoScalingResult.h>
#include <aws/dynamodb/model/TableAutoScalingDescription.h>

#include <aws/dynamodb/model/ReturnValue.h>

#include <aws/dynamodb/model/OnDemandThroughput.h>

#endif
