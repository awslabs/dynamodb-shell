// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * transact-write.hpp
 *
 * Definition of the transact write command.
 *
 * Amrith Kumar. 2022-12-10
 */

#ifndef __TRANSACT_WRITE_HPP_DEFINED__
#define __TRANSACT_WRITE_HPP_DEFINED__

#include "aws-includes.hpp"
#include "command.hpp"
#include "transact.hpp"
#include "logging.hpp"

namespace ddbsh
{
    class CTransactWriteCommand : public CTransactCommand
    {
    public:
        CTransactWriteCommand(CCommand * pCommand) {
            logdebug("[%s, %d] Pushing command %p to tx-read-command\n", __FILENAME__, __LINE__, pCommand);
            append(pCommand);
        };

        ~CTransactWriteCommand() {
        };

        virtual void append(CCommand * pCommand) {
            logdebug("[%s, %d] Pushing command %p to tx-write-command\n", __FILENAME__, __LINE__, pCommand);
            Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * txitem = pCommand->txwrite();

            if (txitem && txitem->size())
            {
                for (auto f: *txitem)
                    m_items.push_back(f);
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
            logdebug("[%s, %d] tx-write-command commit() invoked.\n", __FILENAME__, __LINE__);
            m_committed = true;
        };

        virtual void rollback() {
            logdebug("[%s, %d] tx-write-command commit() invoked.\n", __FILENAME__, __LINE__);
            m_abort = true;
        };

        virtual int run() {
            int retval = 0;

            if (m_committed && !m_abort) {
                Aws::DynamoDB::Model::TransactWriteItemsRequest request;

                request.SetTransactItems(m_items);
                if (explaining()) {
                    printf("TransactWriteItems(%s)\n", explain_string(request.SerializePayload()).c_str());
                } else {
                    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
                    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

                    const Aws::DynamoDB::Model::TransactWriteItemsOutcome & result = p_dynamoDBClient->TransactWriteItems(request);
                    if (result.IsSuccess()) {
                        printf("COMMIT\n");
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
        };

    private:
        Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> m_items;
    };
}
#endif
