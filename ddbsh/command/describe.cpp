// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * describe.cpp
 *
 * command handler for describe
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "describe.hpp"
#include "table_exists.hpp"
#include "logging.hpp"
#include "describe_helper.hpp"
#include "aws-includes.hpp"
#include "strip_newlines.hpp"

using namespace ddbsh;

static void __show_table_basic_info(
    const Aws::DynamoDB::Model::TableDescription& td)
{
    printf("Name: %s (%s)\n", td.GetTableName().c_str(),
           Aws::DynamoDB::Model::TableStatusMapper::\
           GetNameForTableStatus(td.GetTableStatus()).c_str());

    const Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> & ks = td.GetKeySchema();

    std::string key = "";
    for (const auto& kse: ks)
    {
        if (!key.empty())
            key = key + ", ";

        key = key + Aws::DynamoDB::Model::KeyTypeMapper::GetNameForKeyType(kse.GetKeyType()) + " " + kse.GetAttributeName();
    }

    printf("Key: %s\n", key.c_str());

    const Aws::Vector<Aws::DynamoDB::Model::AttributeDefinition> &attribs = td.GetAttributeDefinitions();

    bool first = true;
    printf ("Attributes: " );
    for (const auto &attrib: attribs)
    {
        if (!first)
            printf(", ");
        else
            first = false;

        printf(" %s, %s",
               attrib.GetAttributeName().c_str(),
               Aws::DynamoDB::Model::ScalarAttributeTypeMapper::GetNameForScalarAttributeType(attrib.GetAttributeType()).c_str());
    }

    printf("\n");

    Aws::Utils::DateTime dt = td.GetCreationDateTime();
    printf("Created at: %s\n", dt.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str());
    printf("Table ARN: %s\n", td.GetTableArn().c_str());
    printf("Table ID: %s\n", td.GetTableId().c_str());
    printf("Table size (bytes): %lld\n", td.GetTableSizeBytes());
    printf("Item Count: %lld\n", td.GetItemCount());
    printf("Deletion Protection: %s\n", td.GetDeletionProtectionEnabled() ? "Enabled" : "Disabled");
}

static bool __show_billing_mode(const Aws::DynamoDB::Model::TableDescription& td)
{
    bool table_is_on_demand = false;
    if (td.BillingModeSummaryHasBeenSet())
    {
        Aws::DynamoDB::Model::BillingModeSummary bs = td.GetBillingModeSummary();
        if (bs.GetBillingMode() == Aws::DynamoDB::Model::BillingMode::PROVISIONED)
        {
            Aws::DynamoDB::Model::ProvisionedThroughputDescription ptd =
                td.GetProvisionedThroughput();
            printf("Billing Mode: Provisioned (%lld RCU, %lld WCU)\n",
                   ptd.GetReadCapacityUnits(),
                   ptd.GetWriteCapacityUnits());
        }
        else if (bs.GetBillingMode() == Aws::DynamoDB::Model::BillingMode::PAY_PER_REQUEST)
        {
	    printf("Billing Mode: On Demand\n");
            table_is_on_demand = true;
	    if (td.OnDemandThroughputHasBeenSet())
	    {
		printf("On Demand Throughput: (%lld RCU, %lld WCU)\n",
		       td.GetOnDemandThroughput().MaxReadRequestUnitsHasBeenSet() ?
		       td.GetOnDemandThroughput().GetMaxReadRequestUnits() : -1,
		       td.GetOnDemandThroughput().MaxWriteRequestUnitsHasBeenSet() ?
		       td.GetOnDemandThroughput().GetMaxWriteRequestUnits() : -1 );
	    }
        }
        else
        {
            printf ("Unknown Billing mode %d.\n", bs.GetBillingMode());
        }
    } else if (td.ProvisionedThroughputHasBeenSet()) {
        Aws::DynamoDB::Model::ProvisionedThroughputDescription ptd =
            td.GetProvisionedThroughput();
        printf("Billing Mode: Provisioned (%lld RCU, %lld WCU)\n",
               ptd.GetReadCapacityUnits(),
               ptd.GetWriteCapacityUnits());
    }

    return table_is_on_demand;
}

static void __show_gsi(const Aws::DynamoDB::Model::TableDescription& td, bool table_is_on_demand)
{
    if (td.GlobalSecondaryIndexesHasBeenSet())
    {
        const Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndexDescription> & gsis = td.GetGlobalSecondaryIndexes();
        for (Aws::DynamoDB::Model::GlobalSecondaryIndexDescription  item: gsis)
        {
            printf("GSI %s: ", item.GetIndexName().c_str());
            const Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> &schema = item.GetKeySchema();
            std::string keyschema = "";

            for (Aws::DynamoDB::Model::KeySchemaElement e: schema)
            {
                if (!keyschema.empty())
                    keyschema = keyschema + ", ";

                keyschema = keyschema + Aws::DynamoDB::Model::KeyTypeMapper::GetNameForKeyType(e.GetKeyType()) +
                    " " + e.GetAttributeName();
            }

            printf ("( %s ), ", keyschema.c_str());

            if (!table_is_on_demand)
            {
                Aws::DynamoDB::Model::ProvisionedThroughputDescription ptd = item.GetProvisionedThroughput();

                printf ( " Provisioned (%lld RCU, %lld WCU), ", ptd.GetReadCapacityUnits(), ptd.GetWriteCapacityUnits());
            }
            else
            {
                printf("Billing Mode: On Demand (mirrors table), ");
		if (item.OnDemandThroughputHasBeenSet())
		{
		    printf("Throughput (%lld RCU, %lld WCU), ",
			   item.GetOnDemandThroughput().MaxReadRequestUnitsHasBeenSet() ?
			   item.GetOnDemandThroughput().GetMaxReadRequestUnits() : -1,
			   item.GetOnDemandThroughput().MaxWriteRequestUnitsHasBeenSet() ?
			   item.GetOnDemandThroughput().GetMaxWriteRequestUnits() : -1 );
		}
            }

            if (item.ProjectionHasBeenSet())
            {
                Aws::DynamoDB::Model::Projection proj = item.GetProjection();
                std::string pkeys = "";

                switch (proj.GetProjectionType())
                {
                case Aws::DynamoDB::Model::ProjectionType::INCLUDE:
                {
                    const Aws::Vector<Aws::String> &  projkeys = proj.GetNonKeyAttributes();

                    for (Aws::String pk: projkeys)
                    {
                        if (pkeys.empty())
                            pkeys = pk;
                        else
                            pkeys = pkeys + ", " + pk;
                    }

                    printf( "Projecting (%s) (%s)",
                            Aws::DynamoDB::Model::ProjectionTypeMapper::GetNameForProjectionType(
                                proj.GetProjectionType()).c_str(),
                            pkeys.c_str());
                }
                break;

                default:
                    printf( "Projecting (%s)",
                            Aws::DynamoDB::Model::ProjectionTypeMapper::GetNameForProjectionType(
                                proj.GetProjectionType()).c_str());
                    break;

                }
            }

            printf(", Status: %s", Aws::DynamoDB::Model::IndexStatusMapper::GetNameForIndexStatus(item.GetIndexStatus()).c_str());
            printf(", Backfilling: %s\n", item.GetBackfilling() ? "YES" : "NO");
        }
    }
    else
    {
        printf("GSI: None\n");
    }
}

static void __show_lsi(const Aws::DynamoDB::Model::TableDescription& td)
{
    if (td.LocalSecondaryIndexesHasBeenSet())
    {
        const Aws::Vector<Aws::DynamoDB::Model::LocalSecondaryIndexDescription > &lsis = td.GetLocalSecondaryIndexes();
        for (Aws::DynamoDB::Model::LocalSecondaryIndexDescription  item: lsis)
        {
            printf("LSI %s: ", item.GetIndexName().c_str());
            const Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement>
                &schema = item.GetKeySchema();

            std::string keyschema = "";
            for (Aws::DynamoDB::Model::KeySchemaElement e: schema)
            {
                if (!keyschema.empty())
                    keyschema = keyschema + ", ";

                keyschema = keyschema + Aws::DynamoDB::Model::KeyTypeMapper::GetNameForKeyType(e.GetKeyType()) + " " +
                    e.GetAttributeName();
            }

            printf ("( %s ), ", keyschema.c_str());
        }
        printf("\n");
    }
    else
    {
        printf("LSI: None\n");
    }
}

static void __show_table_class(
    const Aws::DynamoDB::Model::TableDescription& td)
{
    if (td.TableClassSummaryHasBeenSet())
    {
        const Aws::DynamoDB::Model::TableClassSummary tcs = td.GetTableClassSummary();
        printf("Table Class: %s\n", Aws::DynamoDB::Model::TableClassMapper::GetNameForTableClass(tcs.GetTableClass()).c_str());
    }
    else
    {
        printf("Table Class: Has not been set.\n");
    }

}

static void __show_stream(const Aws::DynamoDB::Model::TableDescription& td)
{
    if (td.StreamSpecificationHasBeenSet())
    {
        const Aws::DynamoDB::Model::StreamSpecification stream = td.GetStreamSpecification();
        if (!stream.GetStreamEnabled())
        {
            printf("Stream: Disabled\n");
        }
        else
        {
            printf("Stream: %s\n",
                   Aws::DynamoDB::Model::StreamViewTypeMapper::GetNameForStreamViewType(stream.GetStreamViewType()).c_str());
        }
    }
    else
    {
        printf("Stream: Disabled\n");
    }
}

static void __show_table_sse(
    const Aws::DynamoDB::Model::TableDescription& td)
{
    if (td.SSEDescriptionHasBeenSet())
    {
        const Aws::DynamoDB::Model::SSEDescription &sse = td.GetSSEDescription();

        printf("SSE: (%s)",
            Aws::DynamoDB::Model::SSEStatusMapper::GetNameForSSEStatus(sse.GetStatus()).c_str());
    }
    else
    {
        printf("SSE: Not set\n");
    }
}

static void __show_autoscaling(Aws::DynamoDB::Model::TableAutoScalingDescription asd)
{
    Aws::Vector<Aws::DynamoDB::Model::ReplicaAutoScalingDescription> replicas = asd.GetReplicas();

    for (const auto &repl : replicas)
    {
        printf("Replica Region: %s (Status: %s)\n", repl.GetRegionName().c_str(),
               Aws::DynamoDB::Model::ReplicaStatusMapper::GetNameForReplicaStatus(repl.GetReplicaStatus()).c_str());
    }
}

int CDescribeCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    CDescribeHelper dth(m_table, p_dynamoDBClient, explaining());

    if (dth.has_dtr())
    {
        const Aws::DynamoDB::Model::TableDescription& td = dth.GetDTR().GetTable();
        bool table_is_on_demand;

        if (!explaining())
        {
            __show_table_basic_info(td);
            table_is_on_demand = __show_billing_mode(td);
        }

        Aws::DynamoDB::Model::DescribeContinuousBackupsRequest pitr;
        pitr.SetTableName(td.GetTableName());

        if (explaining())
        {
            printf("DescribeContinuousBackups(%s)\n", explain_string(pitr.SerializePayload()).c_str());
        }
        else
        {
            Aws::DynamoDB::Model::DescribeContinuousBackupsOutcome poutcome = p_dynamoDBClient->DescribeContinuousBackups(pitr);
            if (poutcome.IsSuccess())
            {
                Aws::DynamoDB::Model::DescribeContinuousBackupsResult pitresult =  poutcome.GetResult();
                Aws::DynamoDB::Model::ContinuousBackupsDescription cbdesc = pitresult.GetContinuousBackupsDescription();
                Aws::DynamoDB::Model::PointInTimeRecoveryDescription pitrdesc = cbdesc.GetPointInTimeRecoveryDescription();
                if (pitrdesc.GetPointInTimeRecoveryStatus() == Aws::DynamoDB::Model::PointInTimeRecoveryStatus::ENABLED)
                {
                    Aws::Utils::DateTime earliest, latest;
                    earliest = pitrdesc.GetEarliestRestorableDateTime();
                    latest = pitrdesc.GetLatestRestorableDateTime();
                    printf("PITR is Enabled: [%s to %s]\n",
                           earliest.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str(),
                           latest.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str());
                }
                else if (pitrdesc.GetPointInTimeRecoveryStatus() == Aws::DynamoDB::Model::PointInTimeRecoveryStatus::DISABLED)
                {
                    printf("PITR is Disabled.\n");
                }
            }

            __show_gsi(td, table_is_on_demand);

            __show_lsi(td);

            __show_stream(td);

            __show_table_class(td);

            __show_table_sse(td);
        }

        Aws::DynamoDB::Model::DescribeTableReplicaAutoScalingRequest asr;
        asr.SetTableName(td.GetTableName());

        if (explaining())
        {
            printf("DescribeTableReplicaAutoScaling(%s)\n", explain_string(asr.SerializePayload()).c_str());
        }
        else
        {
            Aws::DynamoDB::Model::DescribeTableReplicaAutoScalingOutcome asro =
                p_dynamoDBClient->DescribeTableReplicaAutoScaling(asr);

            if (asro.IsSuccess())
            {
                Aws::DynamoDB::Model::DescribeTableReplicaAutoScalingResult asrs = asro.GetResult();
                Aws::DynamoDB::Model::TableAutoScalingDescription asd = asrs.GetTableAutoScalingDescription();

                __show_autoscaling(asd);
            }
        }
    }
    else
    {
        return -1;
    }

    return 0;
}

CDescribeCommand::~CDescribeCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}

