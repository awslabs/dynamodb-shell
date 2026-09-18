// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * get_key_schema.hpp
 *
 * utility function to get PK and RK for a table.
 *
 * Amrith - 2022-11-06
 */

#ifndef __GET_KEY_SCHEMA_HPP_DEFINED__
#define __GET_KEY_SCHEMA_HPP_DEFINED__

#include "ddbsh.hpp"
#include "logging.hpp"
#include "describe_helper.hpp"

int get_key_schema(std::string table,
                   std::string *pk,
                   std::string *rk);

int get_key_schema(std::string table,
                   std::string index,
                   std::string *pk,
                   std::string *rk);

#endif

