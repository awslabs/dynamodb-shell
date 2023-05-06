// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * show_create_table.cpp
 *
 * command handler for show create table
 *
 * Amrith - 2023-05-05
 */

#include "aws-includes.hpp"
#include "ddbsh.hpp"
#include "describe_helper.hpp"
#include "logging.hpp"
#include "parser-defs.hpp"
#include "show_create_table.hpp"
#include "strip_newlines.hpp"
#include "table_exists.hpp"

using namespace ddbsh;

std::string CShowCreateTableCommand::get_key_description(const std::vector<Aws::DynamoDB::Model::KeySchemaElement> schema)
{
    std::string rv = "";
    for (int ix = 0; ix < schema.size(); ix ++)
    {
        if (ix != 0)
            rv += " ";

        rv += ("\"" +  schema[ix].GetAttributeName() + "\" " +
                Aws::DynamoDB::Model::KeyTypeMapper::GetNameForKeyType(schema[ix].GetKeyType()));

        if (ix != schema.size() -1)
            rv += ",";
    }

    return rv;
}

std::string CShowCreateTableCommand::billing_mode_provisioned(Aws::DynamoDB::Model::ProvisionedThroughputDescription ptd)
{
    std::string rv;
    rv = "BILLING MODE PROVISIONED ( " + std::to_string(ptd.GetReadCapacityUnits()) + " RCU, " +
        std::to_string(ptd.GetWriteCapacityUnits()) + " WCU ) ";

    return rv;
}

std::string CShowCreateTableCommand::billing_mode_on_demand()
{
    return "BILLING MODE ON DEMAND ";
}

std::string CShowCreateTableCommand::show_gsis(const Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndexDescription> gsis,
                                                 bool table_on_demand)
{
    std::string rv = "";

    if (gsis.size() > 0)
    {
        rv += "GSI (";

        for (int ix = 0; ix < gsis.size(); ix ++)
        {
            if (ix != 0)
                rv += ", ";

            rv += "\"" + gsis[ix].GetIndexName() + "\" ON ( " + get_key_description(gsis[ix].GetKeySchema()) + " ) ";

            const Aws::DynamoDB::Model::Projection projection = gsis[ix].GetProjection();
            switch(projection.GetProjectionType())
            {
            case Aws::DynamoDB::Model::ProjectionType::ALL:
                rv += "PROJECTING ALL";
                break;

            case Aws::DynamoDB::Model::ProjectionType::KEYS_ONLY:
                rv += "PROJECTING KEYS ONLY";
                break;

            case Aws::DynamoDB::Model::ProjectionType::INCLUDE: {
                const Aws::Vector<Aws::String> pa = projection.GetNonKeyAttributes();
                rv += "PROJECTING INCLUDE (";
                for (int iy = 0; iy < pa.size(); iy ++)
                {
                    if (ix != 0)
                        rv += ", ";
                    rv += pa[ix];
                }

                rv += ")";
            }
            break;

            default:
                logerror("Unknown projection in %s.%s.\n", m_table_name.c_str(), gsis[ix].GetIndexName().c_str());
                throw;
            }

            rv += " ";

            if (!table_on_demand)
                rv += billing_mode_provisioned(gsis[ix].GetProvisionedThroughput());
            else
                rv += billing_mode_on_demand();
        }

        rv += ") "; // matches GSI\n(\n
    }

    return rv;
}

std::string CShowCreateTableCommand::show_lsis(const Aws::Vector<Aws::DynamoDB::Model::LocalSecondaryIndexDescription> lsis)
{
    std::string rv;

    if (lsis.size() > 0)
    {
        rv += "LSI (";

        for (int ix = 0; ix < lsis.size(); ix ++)
        {
            if (ix != 0)
                rv += ", ";

            rv += "\"" + lsis[ix].GetIndexName() + "\" ON ( " + get_key_description(lsis[ix].GetKeySchema()) + " ) ";

            const Aws::DynamoDB::Model::Projection projection = lsis[ix].GetProjection();
            switch(projection.GetProjectionType())
            {
            case Aws::DynamoDB::Model::ProjectionType::ALL:
                rv += "PROJECTING ALL";
                break;

            case Aws::DynamoDB::Model::ProjectionType::KEYS_ONLY:
                rv += "PROJECTING KEYS ONLY";
                break;

            case Aws::DynamoDB::Model::ProjectionType::INCLUDE: {
                const Aws::Vector<Aws::String> pa = projection.GetNonKeyAttributes();
                rv += "PROJECTING INCLUDE (";
                for (int iy = 0; iy < pa.size(); iy ++)
                {
                    if (ix != 0)
                        rv += ", ";
                    rv += pa[ix];
                }

                rv += ")";
            }
            break;

            default:
                logerror("Unknown projection in %s.%s.\n", m_table_name.c_str(), lsis[ix].GetIndexName().c_str());
                throw;
            }

            rv += " ";
        }

        rv += ") "; // matches GSI\n(\n
    }

    return rv;
}

std::string CShowCreateTableCommand::stream_specification(const Aws::DynamoDB::Model::StreamSpecification stream)
{
    std::string rv = "";
    if (!stream.GetStreamEnabled())
    {
        rv = "STREAM DISABLED ";
    }
    else
    {
        switch (stream.GetStreamViewType())
        {
        case Aws::DynamoDB::Model::StreamViewType::NEW_IMAGE:
            rv = "STREAM ( NEW IMAGE ) ";
            break;

        case Aws::DynamoDB::Model::StreamViewType::OLD_IMAGE:
            rv = "STREAM ( OLD IMAGE ) ";
            break;

        case Aws::DynamoDB::Model::StreamViewType::KEYS_ONLY:
            rv = "STREAM ( KEYS ONLY ) ";
            break;

        case Aws::DynamoDB::Model::StreamViewType::NEW_AND_OLD_IMAGES:
            rv = "STREAM ( BOTH IMAGES ) ";
            break;

        default:
            logerror("Unknown stream projection type.");
            throw;
        }
    }

    return rv;
}

int CShowCreateTableCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    std::string ctc;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    if (!table_exists(m_table_name, p_dynamoDBClient))
    {
        logerror("The table \"%s\" does not exist.\n", m_table_name.c_str());
        return 1;
    }

    // there is a table, should be able to describe something.
    CDescribeHelper dth(m_table_name, p_dynamoDBClient, explaining());

    if (!dth.has_dtr())
        return 1;

    const Aws::DynamoDB::Model::TableDescription& td = dth.GetDTR().GetTable();

    if (td.GetTableStatus() != Aws::DynamoDB::Model::TableStatus::ACTIVE)
    {
        logerror("The table \"%s\" is not ACTIVE.\n", m_table_name.c_str());
        return 1;
    }

    ctc = "CREATE TABLE ";
    if (m_not_exists)
        ctc += "IF NOT EXISTS ";

    if (m_nowait)
        ctc += "NOWAIT ";

    ctc += ("\"" + td.GetTableName() + "\" (");

    const std::vector<Aws::DynamoDB::Model::AttributeDefinition> defn = td.GetAttributeDefinitions();

    for (int ix = 0; ix < defn.size(); ix ++)
    {
        if (ix != 0)
            ctc += " ";

        ctc += "\"" + defn[ix].GetAttributeName() + "\"";

        switch(defn[ix].GetAttributeType())
        {
        case Aws::DynamoDB::Model::ScalarAttributeType::S:
            ctc += " string";
            break;

        case Aws::DynamoDB::Model::ScalarAttributeType::N:
            ctc += " number";
            break;

        case Aws::DynamoDB::Model::ScalarAttributeType::B:
            ctc += " binary";
            break;

        default:
            logerror("The table has an invalid data type.\n");
            return 1;
        }

        if (ix != defn.size() - 1)
            ctc += ",";
    }

    ctc += ") PRIMARY KEY (";

    const std::vector<Aws::DynamoDB::Model::KeySchemaElement> schema = td.GetKeySchema();

    ctc += get_key_description(schema);

    ctc += ") ";

    bool table_on_demand = false;
    // billing mode
    if (td.BillingModeSummaryHasBeenSet())
    {
        Aws::DynamoDB::Model::BillingModeSummary bs = td.GetBillingModeSummary();
        if (bs.GetBillingMode() == Aws::DynamoDB::Model::BillingMode::PROVISIONED)
        {
            ctc += billing_mode_provisioned(td.GetProvisionedThroughput());
        }
        else if (bs.GetBillingMode() == Aws::DynamoDB::Model::BillingMode::PAY_PER_REQUEST)
        {
            ctc += billing_mode_on_demand();
            table_on_demand = true;
        }
        else
        {
            logerror ("Unknown Billing mode %d.\n", bs.GetBillingMode());
            return 1;
        }
    }
    else if (td.ProvisionedThroughputHasBeenSet()) {
        ctc += billing_mode_provisioned(td.GetProvisionedThroughput());
    }

    try
    {
        ctc += show_gsis(td.GetGlobalSecondaryIndexes(), table_on_demand);
        ctc += show_lsis(td.GetLocalSecondaryIndexes());

        if (td.StreamSpecificationHasBeenSet())
            ctc += stream_specification(td.GetStreamSpecification());

        switch(td.GetTableClassSummary().GetTableClass())
        {
        case Aws::DynamoDB::Model::TableClass::STANDARD:
            ctc += "TABLE CLASS STANDARD ";
            break;

        case Aws::DynamoDB::Model::TableClass::STANDARD_INFREQUENT_ACCESS:
            ctc += "TABLE CLASS STANDARD INFREQUENT ACCESS ";
            break;

        default:
            logerror("Unknown table class.\n");
            throw;
        }

        if (td.GetDeletionProtectionEnabled())
            ctc += "DELETION PROTECTION ENABLED";
        else
            ctc += "DELETION PROTECTION DISABLED";
    }
    catch (...)
    {
        return -1;
    }

    printf("%s\n", ctc.c_str());
    return 0;
};
