// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * transact-read.hpp
 *
 * Definition of the transact read command.
 *
 * Amrith Kumar. 2022-12-10
 */

#ifndef __TRANSACT_READ_HPP_DEFINED__
#define __TRANSACT_READ_HPP_DEFINED__

#include "aws-includes.hpp"
#include "command.hpp"
#include "transact.hpp"
#include "logging.hpp"
#include "select_helper.hpp"

namespace ddbsh
{
    class CTransactReadCommand : public CTransactCommand
    {
    public:
        CTransactReadCommand(CCommand * pCommand) {
            logdebug("[%s, %d] Pushing command %p to tx-read-command\n", __FILENAME__, __LINE__, pCommand);
            append(pCommand);
        };

        ~CTransactReadCommand() {
        };

        virtual void append(CCommand * pCommand) {
            logdebug("[%s, %d] Pushing command %p to tx-read-command\n", __FILENAME__, __LINE__, pCommand);
            Aws::DynamoDB::Model::TransactGetItem * txitem = pCommand->txget();

            if (txitem)
            {
                m_items.push_back(*txitem);
                delete txitem;
            }
            else
            {
                logdebug("[%s, %d] setting transaction aborted.\n", __FILENAME__, __LINE__);
                this->abort();
            }

            delete pCommand;
        };

        virtual void commit() {
            logdebug("[%s, %d] tx-read-command %s() invoked.\n", __FILENAME__, __LINE__, __FUNCTION__);
            m_committed = true;
        };

        virtual void rollback() {
            logdebug("[%s, %d] tx-read-command %s() invoked.\n", __FILENAME__, __LINE__, __FUNCTION__);
            m_abort = true;
        };

        virtual int run() {
            int retval = 0;

            if (m_committed && !m_abort) {
                Aws::DynamoDB::Model::TransactGetItemsRequest request;

                request.SetTransactItems(m_items);
                if (explaining()) {
                    printf("TransactGetItems(%s)\n", explain_string(request.SerializePayload()).c_str());
                } else {
                    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
                    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

                    const Aws::DynamoDB::Model::TransactGetItemsOutcome & result = p_dynamoDBClient->TransactGetItems(request);
                    if (result.IsSuccess()) {
                        Aws::Vector<Aws::DynamoDB::Model::ItemResponse> responses = result.GetResult().GetResponses();
                        for (auto f : responses) {
                            const Aws::Map< Aws::String, Aws::DynamoDB::Model::AttributeValue > item = f.GetItem();
                            printf("%s\n", CSelectHelper::show_item(item).c_str());
                        }
                    } else {
                        logerror("Transaction failed. %s. %s. %s\n", result.GetError().GetExceptionName().c_str(),
                                 result.GetError().GetRequestId().c_str(),
                                 result.GetError().GetMessage().c_str());
                        retval = -1;
                        printf("ABORT\n");
                    }
                }
            } else {
                printf("ABORT\n");
            }

            return retval;
        }

    private:
        Aws::Vector<Aws::DynamoDB::Model::TransactGetItem> m_items;
    };
}
#endif
