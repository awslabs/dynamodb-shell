// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * table_exists.hpp
 *
 * helper to check if a table exists.
 *
 * Amrith - 2022-08-12
 */

#ifndef __TABLE_EXISTS_HPP_DEFINED__
#define __TABLE_EXISTS_HPP_DEFINED__

#include <string>
#include <aws/dynamodb/DynamoDBClient.h>

bool table_exists (std::string name, Aws::DynamoDB::DynamoDBClient * pClient);

// Block until the table reaches the desired control-plane state. Both return
// true on success, or false (with the reason logged) on a terminal error or if
// the overall deadline is exceeded -- they never hang indefinitely.
bool wait_for_table_active(std::string name, Aws::DynamoDB::DynamoDBClient * pClient);
bool wait_for_table_gone(std::string name, Aws::DynamoDB::DynamoDBClient * pClient);

#endif
