// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * serialize_attribute.cpp
 *
 * strip newline characters from the output of SerializeAttribute
 *
 * Amrith - 2022-10-31
 *
 */

#include <algorithm>
#include <string>
#include <aws/dynamodb/model/AttributeValue.h>

std::string serialize_attribute(Aws::DynamoDB::Model::AttributeValue v)
{
        std::string value = v.SerializeAttribute();
        value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
        return value;
};

