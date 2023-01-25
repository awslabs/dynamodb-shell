// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * serialize_attribute.hpp
 *
 * strip newline characters from the output of SerializeAttribute
 *
 * Amrith - 2022-10-31
 *
 */

#ifndef __SERIALIZE_ATTRIBUTE_HPP_DEFINED__
#define __SERIALIZE_ATTRIBUTE_HPP_DEFINED__

#include <algorithm>
#include <string>
#include <aws/dynamodb/model/AttributeValue.h>

std::string serialize_attribute(Aws::DynamoDB::Model::AttributeValue v);

#endif
