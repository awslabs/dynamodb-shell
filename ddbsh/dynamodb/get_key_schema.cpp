// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * get_key_schema.cpp
 *
 * utility function to get PK and RK for a table.
 *
 * Amrith - 2022-11-06
 */

#include "ddbsh.hpp"
#include "logging.hpp"
#include "describe_helper.hpp"
#include "get_key_schema.hpp"

using namespace ddbsh;

static int load_key_schema(const Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> &ks, std::string * pk, std::string * rk)
{
    for (const auto &kse: ks)
    {
        if (kse.GetKeyType() ==  Aws::DynamoDB::Model::KeyType::HASH)
            * pk = kse.GetAttributeName();
        else if (kse.GetKeyType() ==  Aws::DynamoDB::Model::KeyType::RANGE)
            * rk = kse.GetAttributeName();
        else
        {
            logerror("[%s, %d] unknown key type %s for attribute %s\n",
                     __FILENAME__, __LINE__,
                     Aws::DynamoDB::Model::KeyTypeMapper::GetNameForKeyType(kse.GetKeyType()).c_str(),
                     kse.GetAttributeName().c_str());
            return -1;
        }
    }

    return 0;
}

int get_key_schema(std::string table,
                   std::string *pk,
                   std::string *rk)
{
    return get_key_schema(table, std::string(""), pk, rk);
}

int get_key_schema(std::string table,
                   std::string index,
                   std::string *pk,
                   std::string *rk) {
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    // first look at the table (table_name) and figure out what its key schema is.
    CDescribeHelper dth(table, p_dynamoDBClient);

    logdebug("[%s, %d] found dth for table %s\n", __FILENAME__, __LINE__, table.c_str());

    if (dth.has_dtr())
    {
        const Aws::DynamoDB::Model::TableDescription& td = dth.GetDTR().GetTable();

        if (index.empty())
        {
            const Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> & ks = td.GetKeySchema();
            if (load_key_schema(ks, pk, rk))
                return -1;
        }
        else
        {
            // this is the index lookup, first search LSIs, then GSIs.
            const Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndexDescription> gsis = td.GetGlobalSecondaryIndexes();
            const Aws::Vector<Aws::DynamoDB::Model::LocalSecondaryIndexDescription> lsis = td.GetLocalSecondaryIndexes();
            for (int ix = 0; ix < lsis.size(); ix ++)
            {
                logdebug("[%s, %d] looking at LSI %s.%s\n", __FILENAME__, __LINE__, table.c_str(), index.c_str());
                if (lsis[ix].GetIndexName() == index)
                {
                    const Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> & ks = lsis[ix].GetKeySchema();
                    if (load_key_schema(ks, pk, rk))
                        return -1;
                }
            }

            for (int ix = 0; ix < gsis.size(); ix ++)
            {
                logdebug("[%s, %d] looking at GSI %s.%s\n", __FILENAME__, __LINE__, table.c_str(), gsis[ix].GetIndexName().c_str());
                if (gsis[ix].GetIndexName() == index)
                {
                    logdebug("[%s, %d] Found index %s.%s\n", __FILENAME__, __LINE__, table.c_str(), index.c_str());
                    const Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> & ks = gsis[ix].GetKeySchema();
                    if (load_key_schema(ks, pk, rk))
                        return -1;
                }
            }
        }
    }
    else
    {
        logdebug("[%s, %d] dtr.has_dtr() returned false\n", __FILENAME__, __LINE__);
        return -1;
    }

    logdebug("[%s, %d] table %s, index %s has pk = %s, rk = %s\n",
             __FILENAME__, __LINE__, table.c_str(), index.empty() ? "NONE" : index.c_str(),
             pk->c_str(), rk->empty() ? "NONE" : rk->c_str());

    return 0;
}


