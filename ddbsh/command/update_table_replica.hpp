// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * update_table_replica.hpp
 *
 * All commands related to table replicas (Global Tables).
 *
 * Amrith Kumar. 2023-03-06
 */

#ifndef __UPDATE_TABLE_REPLICA_HPP_DEFINED__
#define __UPDATE_TABLE_REPLICA_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CUpdateTableReplica : public CCommand
    {
    public:
        CUpdateTableReplica(std::string table) {
            m_table = table;
        };

        void deleteRegion(std::string region) {
            m_delete = NEW Aws::DynamoDB::Model::DeleteReplicationGroupMemberAction();
            m_delete->SetRegionName(region);
        };

        void addRegion(std::string region, Aws::DynamoDB::Model::TableClass table_class,
                       Aws::DynamoDB::Model::ProvisionedThroughputOverride * table_override,
                       Aws::Vector<Aws::DynamoDB::Model::ReplicaGlobalSecondaryIndex> * gsispec) {
            m_create = NEW Aws::DynamoDB::Model::CreateReplicationGroupMemberAction;
            m_create->SetRegionName(region);

            m_create->SetTableClassOverride(table_class);

            if (table_override) {
                m_create->SetProvisionedThroughputOverride(*table_override);
            }

            if (gsispec) {
                m_create->SetGlobalSecondaryIndexes(*gsispec);
            }
        };

        void updateRegion(std::string region, Aws::DynamoDB::Model::TableClass table_class,
                          Aws::DynamoDB::Model::ProvisionedThroughputOverride * table_override,
                          Aws::Vector<Aws::DynamoDB::Model::ReplicaGlobalSecondaryIndex> * gsispec) {
            m_update = NEW Aws::DynamoDB::Model::UpdateReplicationGroupMemberAction;
            m_update->SetRegionName(region);

            m_update->SetTableClassOverride(table_class);

            if (table_override) {
                m_update->SetProvisionedThroughputOverride(*table_override);
            }

            if (gsispec) {
                m_update->SetGlobalSecondaryIndexes(*gsispec);
            }
        };

        int run() {
            logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
            Aws::DynamoDB::Model::UpdateTableRequest request;
            Aws::DynamoDB::Model::ReplicationGroupUpdate replication_request;

            request.SetTableName(m_table);

            if (m_delete)
                replication_request.SetDelete(*m_delete);

            if (m_update)
                replication_request.SetUpdate(*m_update);

            if (m_create)
                replication_request.SetCreate(*m_create);

            request.AddReplicaUpdates(replication_request);

            if (explaining())
            {
                printf("UpdateTable(%s)\n", explain_string(request.SerializePayload()).c_str());
            }
            else
            {
                Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
                p_dynamoDBClient = CDDBSh::getInstance()->getClient();

                const Aws::DynamoDB::Model::UpdateTableOutcome & result =
                    p_dynamoDBClient->UpdateTable(request);

                if (!result.IsSuccess())
                {
                    logerror("Update Table failed. %s. %s. %s\n",
                             result.GetError().GetExceptionName().c_str(),
                             result.GetError().GetRequestId().c_str(),
                             result.GetError().GetMessage().c_str());
                    return -1;
                }

                printf("ALTER\n");
            }

            return 0;
        };

        virtual ~CUpdateTableReplica() {
            logdebug("[%s, %d] %s\n", __FILENAME__, __LINE__, __FUNCTION__);
            delete m_delete;
            delete m_update;
            delete m_create;
        };

    private:
        std::string m_table;

        Aws::DynamoDB::Model::DeleteReplicationGroupMemberAction * m_delete;
        Aws::DynamoDB::Model::UpdateReplicationGroupMemberAction * m_update;
        Aws::DynamoDB::Model::CreateReplicationGroupMemberAction * m_create;
    };
}
#endif
