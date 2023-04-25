// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * delete.cpp
 *
 * command handler for delete
 *
 * Amrith - 2022-08-15
 */

#include "aws-includes.hpp"
#include "ddbsh.hpp"
#include "delete.hpp"
#include "logging.hpp"
#include "get_key_schema.hpp"
#include "strip_newlines.hpp"
#include "select_helper.hpp"
#include "allocation.hpp"

using namespace ddbsh;

int CDeleteCommand::run()
{
    logdebug("[%s, %d] In %s. table = %s.\n", __FILENAME__, __LINE__, __FUNCTION__, m_table_name.c_str());

    std::string table_pk, table_rk;
    if (get_key_schema(m_table_name, &table_pk, &table_rk) != 0)
        return -1;

    // for a description of the code here, refer to the parallel code
    // in update.cpp
    if (m_where && m_where->can_delete_item(table_pk, table_rk))
    {
        logdebug("[%s, %d] will attempt do_getitem()\n", __FILENAME__, __LINE__);
        return do_deleteitem(table_pk, table_rk);
    }
    else if (m_index_name.length() == 0)
    {
        if (m_where && m_where->can_query_table(table_pk, table_rk))
        {
            logdebug("[%s, %d] will attempt do_query()\n", __FILENAME__, __LINE__);
            return do_query(table_pk, table_rk);
        }
        else
        {
            return do_scan(table_pk, table_rk);
        }
    }
    else
    {
        // the user is attempting an update/upsert targeting the
        // index. Let's see whether we can do a query against that.
        std::string index_pk, index_rk;

        if (get_key_schema(m_table_name, m_index_name, &index_pk, &index_rk) != 0)
            return -1;

        if (m_where && m_where->can_query_index(index_pk, index_rk))
        {
            logdebug("[%s, %d] will attempt do_query() against index\n", __FILENAME__, __LINE__);
            return do_query(table_pk, table_rk);
        }
        else
        {
            logdebug("[%s, %d] will attempt do_scan() against index\n", __FILENAME__, __LINE__);
            return do_scan(table_pk, table_rk);
        }

    }
}

CDeleteCommand::~CDeleteCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
    delete m_where;
    delete m_rate_limit;
}

int CDeleteCommand::do_scan(std::string pk, std::string rk)
{
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    CSymbolTable st;
    std::string pkname, rkname;

    logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    Aws::DynamoDB::Model::DeleteItemRequest * dir = make_delete_request(pk, rk, &st);

    // make and execute a scan against the table.
    CSelectHelper helper;
    helper.setup(m_table_name, m_index_name, m_where, m_rate_limit ? true : false);
    Aws::DynamoDB::Model::ScanRequest * request = helper.scan_request();

    if (explaining())
    {
        printf("Scan(%s)\n", explain_string(request->SerializePayload()).c_str());
    }

    int retval = 0;

    do
    {
        const Aws::DynamoDB::Model::ScanOutcome& sresult = p_dynamoDBClient->Scan(*request);
        if (sresult.IsSuccess())
        {
            const Aws::Vector<Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>> &items = sresult.GetResult().GetItems();

            if (m_rate_limit)
                m_rate_limit->consume_reads(sresult.GetResult().GetConsumedCapacity());

            for (const auto &item : items)
            {
                Aws::Map< Aws::String, Aws::DynamoDB::Model::AttributeValue> key;
                for (const auto &attrib: item)
                    key.emplace(attrib.first, attrib.second);

                this->read();

                retval = do_delete(dir, key);

                if (retval)
                    break;
            }
        }
        else
        {
            logerror("[%s, %d] DELETE failed. %s. %s. %s\n", __FILENAME__, __LINE__,
                     sresult.GetError().GetExceptionName().c_str(),
                     sresult.GetError().GetRequestId().c_str(),
                     sresult.GetError().GetMessage().c_str());
            retval = -1;
            break;
        }

        if (retval)
            break;

        if (sresult.GetResult().GetLastEvaluatedKey().empty())
            break;

        request->SetExclusiveStartKey(sresult.GetResult().GetLastEvaluatedKey());
        if (m_rate_limit)
            m_rate_limit->readlimit();
    } while(1);

    if (!retval && !explaining())
        printf("DELETE (%d read, %d modified, %d ccf)\n", nread(), nmodified(), nccf());

    delete dir;
    delete request;
    return retval;
}

int CDeleteCommand::do_query(std::string pk, std::string rk)
{
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    CSymbolTable st;
    std::string pkname, rkname;

    logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    Aws::DynamoDB::Model::DeleteItemRequest * dir = make_delete_request(pk, rk, &st);

    // make and execute a query against the table.
    CSelectHelper helper;
    helper.setup(m_table_name, m_index_name, m_where, m_rate_limit ? true : false);
    Aws::DynamoDB::Model::QueryRequest * request = helper.query_request();

    int retval = 0;

    if (explaining())
    {
        printf("Query(%s)\n", explain_string(request->SerializePayload()).c_str());
    }
    else
    {
        do
        {
            const Aws::DynamoDB::Model::QueryOutcome& qresult = p_dynamoDBClient->Query(*request);
            if (qresult.IsSuccess())
            {
                const Aws::Vector<Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>> &items =
                    qresult.GetResult().GetItems();

                if (m_rate_limit)
                    m_rate_limit->consume_reads(qresult.GetResult().GetConsumedCapacity());

                for (const auto &item : items)
                {
                    Aws::Map< Aws::String, Aws::DynamoDB::Model::AttributeValue> key;
                    for (const auto &attrib: item)
                        key.emplace(attrib.first, attrib.second);

                    this->read();
                    retval = do_delete(dir, key);

                    if (retval)
                        break;
                }
            }
            else
            {
                logerror("[%s, %d] DELETE failed. %s. %s. %s\n", __FILENAME__, __LINE__,
                         qresult.GetError().GetExceptionName().c_str(),
                         qresult.GetError().GetRequestId().c_str(),
                         qresult.GetError().GetMessage().c_str());
                retval = -1;
                break;
            }

            if (retval)
                break;

            if (qresult.GetResult().GetLastEvaluatedKey().empty())
                break;

            request->SetExclusiveStartKey(qresult.GetResult().GetLastEvaluatedKey());

            if (m_rate_limit)
                m_rate_limit->readlimit();
        } while(1);

        if (!retval && !explaining())
            printf("DELETE (%d read, %d modified, %d ccf)\n", nread(), nmodified(), nccf());
    }

    delete dir;
    delete request;
    return retval;
}

int CDeleteCommand::do_deleteitem(std::string pk, std::string rk)
{
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    CSymbolTable st;
    std::string pkname, rkname;

    logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    Aws::DynamoDB::Model::DeleteItemRequest * dir = make_delete_request(pk, rk, &st);
    int retval = 0;

    retval = do_delete(dir, m_where->get_gud_key(pk, rk));

    if (!retval && !explaining())
        printf("DELETE (%d read, %d modified, %d ccf)\n", nread(), nmodified(), nccf());

    delete dir;
    return retval;
}

// make an delete request. This method will populate the symbol table
// with all the elements required in the set or remove clauses.
Aws::DynamoDB::Model::DeleteItemRequest * CDeleteCommand::make_delete_request(std::string pk, std::string rk, CSymbolTable * st)
{
    Aws::DynamoDB::Model::DeleteItemRequest * dir = NEW Aws::DynamoDB::Model::DeleteItemRequest;

    logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    dir->SetTableName(m_table_name);
    // don't call SetKey() that is up to the caller of this method.

    if (m_where)
    {
        std::string cond = m_where->update_delete_condition_check(pk, rk, st);

        logdebug("[%s, %d] setting condition expression to %s\n", __FILENAME__, __LINE__, strip_newlines(cond).c_str());

        if (!cond.empty())
            dir->SetConditionExpression(cond);
    }

    // the caller had better have setup mappings in the symbol table for the PK and optional RK
    if (st->has_names())
        dir->SetExpressionAttributeNames(st->get_names());

    if (st->has_values())
        dir->SetExpressionAttributeValues(st->get_values());

    if (m_rate_limit)
        dir->SetReturnConsumedCapacity(Aws::DynamoDB::Model::ReturnConsumedCapacity::TOTAL);

    logdebug("[%s, %d] complete request is %s\n", __FILENAME__, __LINE__, explain_string(dir->SerializePayload()).c_str());
    return dir;
}

int CDeleteCommand::do_delete(Aws::DynamoDB::Model::DeleteItemRequest * dir,
                              Aws::Map< Aws::String, Aws::DynamoDB::Model::AttributeValue> key)
{
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    dir->SetKey(key);
    logdebug("[%s, %d] complete delete request = %s\n", __FILENAME__, __LINE__, strip_newlines(dir->SerializePayload()).c_str());

    if (explaining())
    {
        printf("DeleteItem(%s)\n", explain_string(dir->SerializePayload()).c_str());
    }
    else
    {
        if (m_rate_limit)
            m_rate_limit->writelimit();

        const Aws::DynamoDB::Model::DeleteItemOutcome& result = p_dynamoDBClient->DeleteItem(*dir);

        if(!result.IsSuccess())
        {
            if (result.GetError().GetExceptionName() != "ConditionalCheckFailedException")
            {
                logerror("[%s, %d] DELETE failed. %s. %s. %s\n", __FILENAME__, __LINE__,
                         result.GetError().GetExceptionName().c_str(),
                         result.GetError().GetRequestId().c_str(),
                         result.GetError().GetMessage().c_str());
                return -1;
            }
            else
            {
                this->ccf();
            }
        }
        else
        {
            this->modified();

            if(m_rate_limit)
                m_rate_limit->consume_writes(result.GetResult().GetConsumedCapacity());
        }
    }

    return 0;
}

Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * CDeleteCommand::txwrite()
{
    logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    std::string pk, rk;
    if (get_key_schema(m_table_name, &pk, &rk) != 0)
        return NULL;

    if (!m_where || !m_where->can_delete_item(pk, rk))
        return NULL;

    CSymbolTable st;
    Aws::DynamoDB::Model::DeleteItemRequest * dir = make_delete_request(pk, rk, &st);

    Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * tx = NEW Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem>;
    Aws::DynamoDB::Model::TransactWriteItem twi;
    Aws::DynamoDB::Model::Delete del;

    del.SetKey(m_where->get_gud_key(pk, rk));
    del.SetTableName(dir->GetTableName());

    if (dir->GetConditionExpression().length())
        del.SetConditionExpression(dir->GetConditionExpression());

    if(dir->GetExpressionAttributeNames().size())
        del.SetExpressionAttributeNames(dir->GetExpressionAttributeNames());

    if(dir->GetExpressionAttributeValues().size())
        del.SetExpressionAttributeValues(dir->GetExpressionAttributeValues());

    delete dir;
    twi.SetDelete(del);
    tx->push_back(twi);
    return tx;
}
