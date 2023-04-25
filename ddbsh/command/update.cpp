// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * update.cpp
 *
 * command handler for update
 *
 * Amrith - 2022-08-15
 */

#include "aws-includes.hpp"
#include "ddbsh.hpp"
#include "update.hpp"
#include "logging.hpp"
#include "get_key_schema.hpp"
#include "select_helper.hpp"
#include "strip_newlines.hpp"
#include "allocation.hpp"

using namespace ddbsh;

int CUpdateCommand::run()
{
    std::string op = m_update ? "UPDATE" : "UPSERT";
    logdebug("[%s, %d] In %s. table = %s, operation = %s\n",
             __FILENAME__, __LINE__, __FUNCTION__, m_table_name.c_str(), op.c_str());

    std::string table_pk, table_rk;

    if (get_key_schema(m_table_name, &table_pk, &table_rk) != 0)
        return -1;

    // can_update_item() checks to see whether a unique item is
    // identified in the where clause. This would be that a full
    // primary key is available. If yes, directly update the item.
    // Since it is acceptable to provide a table or index name, the
    // check for direct update goes only against the table. Therefore
    // we use the table pk and rk here.
    if (m_where && m_where->can_update_item(table_pk, table_rk))
    {
        logdebug("[%s, %d] will attempt do_updateitem()\n", __FILENAME__, __LINE__);
        return do_updateitem(table_pk, table_rk);
    }
    else if (m_index_name.length() == 0)
    {
        // the user did not provide an index. Therefore the query or
        // scan go against the base table.
        if (m_where && m_where->can_query_table(table_pk, table_rk))
        {
            // if a primary key is not identified in the where clause it
            // means that either (a) the partition key is not available,
            // or (b) the table has a range key which isn't listed. In
            // either event, we first check to see whether or not we can
            // query the table. for that, we need a primary key, and some
            logdebug("[%s, %d] will attempt do_query()\n", __FILENAME__, __LINE__);
            return do_query(table_pk, table_rk);
        }
        else
        {
            // if you can't even do a query, then there's no partition key
            // and we're relegated to a scan.
            logdebug("[%s, %d] will attempt do_scan()\n", __FILENAME__, __LINE__);
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
            // pass do_query the table pk and rk because it uses those
            // to make the update request. Internally it invokes
            // select helper's setup() method with table and index
            // name so the query will be using the index if one is
            // provided. Same for scan below.
            logdebug("[%s, %d] will attempt do_query() against index\n", __FILENAME__, __LINE__);
            return do_query(table_pk, table_rk);
        }
        else
        {
            // no option but scan the index. See comment above with
            // query for why this is table_pk and rk.
            logdebug("[%s, %d] will attempt do_scan() against index\n", __FILENAME__, __LINE__);
            return do_scan(table_pk, table_rk);
        }
    }
}

CUpdateCommand::~CUpdateCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
    delete m_set;
    delete m_remove;
    delete m_where;
    delete m_rate_limit;
}

int CUpdateCommand::do_scan(std::string pk, std::string rk)
{
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    CSymbolTable st;
    std::string pkname, rkname;

    logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    Aws::DynamoDB::Model::UpdateItemRequest * uir = make_update_request(pk, rk, &st);

    // make and execute a scan against the table.
    CSelectHelper helper;

    if (helper.setup(m_table_name, m_index_name, m_where, m_rate_limit ? true : false))
        return -1;

    Aws::DynamoDB::Model::ScanRequest * request = helper.scan_request() ;

    int retval = 0;

    do
    {
        if (explaining())
        {
            printf("Scan(%s)\n", explain_string(request->SerializePayload()).c_str());
        }

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
                retval = do_update(uir, key);

                if (retval)
                    break;
            }
        }
        else
        {
            logerror("[%s, %d] %s failed. %s\n", __FILENAME__, __LINE__, m_update ? "UPDATE" : "UPSERT",
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
        printf("%s (%d read, %d modified, %d ccf)\n", m_update ? "UPDATE" : "UPSERT", nread(), nmodified(), nccf());

    delete uir;
    delete request;
    return retval;
}

int CUpdateCommand::do_query(std::string pk, std::string rk)
{
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    CSymbolTable st;
    std::string pkname, rkname;

    logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    Aws::DynamoDB::Model::UpdateItemRequest * uir = make_update_request(pk, rk, &st);

    // make and execute a query against the table.
    CSelectHelper helper;
    if (helper.setup(m_table_name, m_index_name, m_where, m_rate_limit ? true : false))
        return -1;

    Aws::DynamoDB::Model::QueryRequest * request = helper.query_request();

    int retval = 0;

    do
    {
        if (explaining())
        {
            printf("Query(%s)\n", explain_string(request->SerializePayload()).c_str());
        }

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
                retval = do_update(uir, key);

                if (retval)
                    break;
            }
        }
        else
        {
            logerror("[%s, %d] %s failed. %s\n", __FILENAME__, __LINE__, m_update ? "UPDATE" : "UPSERT",
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
        printf("%s (%d read, %d modified, %d ccf)\n", m_update ? "UPDATE" : "UPSERT", nread(), nmodified(), nccf());

    delete uir;
    delete request;
    return retval;
}

int CUpdateCommand::do_updateitem(std::string pk, std::string rk)
{
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    CSymbolTable st;
    std::string pkname, rkname;

    int retval = 0;

    logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    Aws::DynamoDB::Model::UpdateItemRequest * uir = make_update_request(pk, rk, &st);

    retval = do_update(uir, m_where->get_gud_key(pk, rk));

    if (!retval && !explaining())
        printf("%s (%d read, %d modified, %d ccf)\n", m_update ? "UPDATE" : "UPSERT", nread(), nmodified(), nccf());

    delete uir;
    return retval;
}

// make an update request. This method will populate the symbol table
// with all the elements required in the set or remove clauses.
Aws::DynamoDB::Model::UpdateItemRequest * CUpdateCommand::make_update_request(std::string pk, std::string rk, CSymbolTable * st)
{
    Aws::DynamoDB::Model::UpdateItemRequest * uir = NEW Aws::DynamoDB::Model::UpdateItemRequest;

    logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    uir->SetTableName(m_table_name);
    // don't call SetKey() that is up to the caller of this method.

    assert(m_set || m_remove);

    if (m_set)
    {
        std::string set = "SET ";

        for (int i = 0; i < m_set->size(); i ++)
        {
            CUpdateSetElement p = (*m_set)[i];
            logdebug("[%s, %d] p = %s\n", __FILENAME__, __LINE__, p.serialize(NULL).c_str());
            if (i != 0)
                set += ", ";

            set += p.serialize(st);
        }

        logdebug("[%s, %d] setting update expression to %s\n", __FILENAME__, __LINE__, set.c_str());
        uir->SetUpdateExpression(set);
    }
    else if (m_remove)
    {
        std::string rmv = "REMOVE ";
        for (int i = 0; i < m_remove->size(); i++)
        {
            Aws::String p = (*m_remove)[i];
            if (i != 0)
                rmv += ", ";

            rmv += st->new_attribute(p);
        }

        logdebug("[%s, %d] setting remove expression to %s\n", __FILENAME__, __LINE__, rmv.c_str());
        uir->SetUpdateExpression(rmv);
    }

    std::string cond;

    if (m_update)
    {
        cond = "attribute_exists(" + st->new_attribute(pk) + ")";
    }

    if (m_where)
    {
        std::string qf = m_where->update_delete_condition_check(pk, rk, st);
        if (!qf.empty())
        {
            if (!cond.empty())
                cond += " AND ";

            cond += qf;
        }

        logdebug("[%s, %d] setting condition expression to %s\n", __FILENAME__, __LINE__, strip_newlines(cond).c_str());
    }

    if (!cond.empty())
        uir->SetConditionExpression(cond);

    // the caller had better have setup mappings in the symbol table for the PK and optional RK
    if (st->has_names())
        uir->SetExpressionAttributeNames(st->get_names());

    if (st->has_values())
        uir->SetExpressionAttributeValues(st->get_values());

    if (m_rate_limit)
        uir->SetReturnConsumedCapacity(Aws::DynamoDB::Model::ReturnConsumedCapacity::TOTAL);

    logdebug("[%s, %d] complete request is %s\n", __FILENAME__, __LINE__, strip_newlines(uir->SerializePayload()).c_str());
    return uir;
}

int CUpdateCommand::do_update(Aws::DynamoDB::Model::UpdateItemRequest * uir,
                              Aws::Map< Aws::String, Aws::DynamoDB::Model::AttributeValue> key)
{
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    uir->SetKey(key);
    logdebug("[%s, %d] complete update request = %s\n", __FILENAME__, __LINE__, strip_newlines(uir->SerializePayload()).c_str());

    if (explaining())
    {
        printf("UpdateItem(%s)\n", explain_string(uir->SerializePayload()).c_str());
    }
    else
    {
        if (m_rate_limit)
            m_rate_limit->writelimit();

        const Aws::DynamoDB::Model::UpdateItemOutcome& result = p_dynamoDBClient->UpdateItem(*uir);

        if(!result.IsSuccess())
        {
            if (result.GetError().GetExceptionName() != "ConditionalCheckFailedException")
            {
                logerror("[%s, %d] %s failed. %s. %s. %s\n", __FILENAME__, __LINE__, m_update ? "UPDATE" : "UPSERT",
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

Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * CUpdateCommand::txwrite()
{
    logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    std::string pk, rk;
    if (get_key_schema(m_table_name, &pk, &rk) != 0)
        return NULL;

    if (!m_where || !m_where->can_update_item(pk, rk))
        return NULL;

    CSymbolTable st;
    Aws::DynamoDB::Model::UpdateItemRequest * uir = make_update_request(pk, rk, &st);

    Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * tx = NEW Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem>;
    Aws::DynamoDB::Model::TransactWriteItem twi;
    Aws::DynamoDB::Model::Update update;

    update.SetKey(m_where->get_gud_key(pk, rk));
    update.SetUpdateExpression(uir->GetUpdateExpression());
    update.SetTableName(uir->GetTableName());

    if (uir->GetConditionExpression().length())
        update.SetConditionExpression(uir->GetConditionExpression());

    if (uir->GetExpressionAttributeNames().size())
        update.SetExpressionAttributeNames(uir->GetExpressionAttributeNames());

    if (uir->GetExpressionAttributeValues().size())
        update.SetExpressionAttributeValues(uir->GetExpressionAttributeValues());

    delete uir;
    twi.SetUpdate(update);
    tx->push_back(twi);
    return tx;
}

