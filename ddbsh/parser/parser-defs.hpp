// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
#ifndef __PARSER_H_DEFINED__
#define __PARSER_H_DEFINED__

#include "aws-includes.hpp"
#include "logical_expression.hpp"
#include "logical_comparison.hpp"
#include "logical_and.hpp"
#include "logical_or.hpp"
#include "logical_attribute_exists.hpp"
#include "logical_attribute_type.hpp"
#include "logical_begins_with.hpp"
#include "logical_between.hpp"
#include "logical_comparison.hpp"
#include "logical_contains.hpp"
#include "logical_in.hpp"
#include "ratelimit.hpp"
#include "where.hpp"
#include "symbol_table.hpp"
#include "update_set_element.hpp"

typedef struct
{
    Aws::DynamoDB::Model::BillingMode mode;
    Aws::DynamoDB::Model::ProvisionedThroughput throughput;
} billing_mode_and_throughput_t;

#endif
