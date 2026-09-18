// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * select_helper.cpp
 *
 * A simple helper that will build a request for either GetItem, Query, or Scan.
 *
 * Amrith - 2022-11-07
 */

#include <cstring>
#include "aws-includes.hpp"
#include "where.hpp"
#include "select_helper.hpp"
#include "logging.hpp"
#include "ddbsh.hpp"
#include "describe_helper.hpp"
#include "symbol_table.hpp"
#include "get_key_schema.hpp"
#include "strip_newlines.hpp"
#include "allocation.hpp"

using namespace ddbsh;

// is the where clause sufficiently specific that a getitem can be
// performed? that requires fully specified primary key.
bool CSelectHelper::can_getitem()
{
    return (m_index_name.empty() && m_where_clone && m_where_clone->can_getitem(m_pk, m_rk));
}

// select helper is used for both table and index queries. So handle
// indexes specifically.
bool CSelectHelper::can_query()
{
    return (m_where_clone && ((m_index_name.empty() && m_where_clone->can_query_table(m_pk, m_rk)) ||
                              (!m_index_name.empty() && m_where_clone->can_query_index(m_pk, m_rk))));
}

int CSelectHelper::setup(bool consistent, Aws::Vector<Aws::String> * projection, Aws::Vector<Aws::String> * table, CWhere * where,
                         Aws::DynamoDB::Model::ReturnConsumedCapacity returns, bool ratelimit)
{
    std::string ltable, lindex;

    assert(table->size() >= 1);

    ltable = (*table)[0];
    if (table->size() > 1)
        lindex = (*table)[1];

    delete table;

    return setup(consistent, projection, ltable, lindex, where, returns, ratelimit);
}

int CSelectHelper::setup(bool consistent, Aws::Vector<Aws::String> * projection, std::string table, std::string index,
                         CWhere * where, Aws::DynamoDB::Model::ReturnConsumedCapacity returns, bool ratelimit)
{
    logdebug("[%s, %d] In %s.\n", __FILENAME__, __LINE__, __FUNCTION__);

    m_consistent = consistent;
    m_projection = projection;
    m_rate_limited = ratelimit;

    m_table_name = table;
    m_index_name = index;

    m_where_clone = where;
    m_consumed_capacity = returns;

    if (get_key_schema(m_table_name, m_index_name, &m_pk, &m_rk) != 0)
        return -1;

    std::string p = serialize_projection();

    logdebug("[%s, %d] consistent = %s\n", __FILENAME__, __LINE__,  m_consistent ? "True" : "False");
    logdebug("[%s, %d] projection = %s\n", __FILENAME__, __LINE__, p.empty() ? "*" : p.c_str());
    logdebug("[%s, %d] m_pk = %s\n", __FILENAME__, __LINE__, m_pk.c_str());
    logdebug("[%s, %d] m_pk = %s\n", __FILENAME__, __LINE__, m_rk.empty() ? "" : m_rk.c_str());
    logdebug("[%s, %d] table = %s, index = %s\n", __FILENAME__, __LINE__, m_table_name.c_str(),
             m_index_name.empty() ? "NONE" : m_index_name.c_str());
    logdebug("[%s, %d] where = %s\n", __FILENAME__, __LINE__, m_where_clone ? m_where_clone->serialize(NULL).c_str() : "");
    logdebug("[%s, %d] consumed capacity = %s\n", __FILENAME__, __LINE__,
             Aws::DynamoDB::Model::ReturnConsumedCapacityMapper::GetNameForReturnConsumedCapacity(m_consumed_capacity).c_str());

    return 0;
}

int CSelectHelper::setup(std::string table, std::string index, CWhere * where, bool ratelimit)
{
    logdebug("[%s, %d] In %s.\n", __FILENAME__, __LINE__, __FUNCTION__);

    std::string pk, rk;
    if (get_key_schema(table, &pk, &rk) != 0)
        return -1;

    Aws::Vector<Aws::String> * projection = NEW Aws::Vector<Aws::String>;

    assert(!pk.empty());
    projection->push_back(pk);

    if (!rk.empty())
        projection->push_back(rk);

    return setup(false, projection, table, index, where, Aws::DynamoDB::Model::ReturnConsumedCapacity::NONE, ratelimit);
}

Aws::DynamoDB::Model::GetItemRequest * CSelectHelper::getitem_request()
{
    logdebug("[%s, %d] In %s.\n", __FILENAME__, __LINE__, __FUNCTION__);

    assert(this->can_getitem());

    Aws::DynamoDB::Model::GetItemRequest * request = NEW Aws::DynamoDB::Model::GetItemRequest;

    CSymbolTable st;

    request->SetTableName(m_table_name);
    request->SetConsistentRead(m_consistent);

    // if no projection was specified (i.e. select *) then don't send
    // down a projection, the default is all attributes.
    if(!m_projection->empty())
        request->SetProjectionExpression(serialize_projection(&st));

    // it is possible (select *) that there are no names in the symbol table.
    if (st.has_names())
        request->SetExpressionAttributeNames(st.get_names());

    // on a GetItem() request there is no point in looking for rate limits.
    request->SetReturnConsumedCapacity(m_consumed_capacity);

    request->SetKey(m_where_clone->get_gud_key(m_pk, m_rk));

    std::string requeststr =  strip_newlines(request->SerializePayload());
    logdebug("[%s, %d] request = %s\n", __FILENAME__, __LINE__, requeststr.c_str());

    return request;
}

Aws::DynamoDB::Model::QueryRequest * CSelectHelper::query_request()
{
    logdebug("[%s, %d] In %s.\n", __FILENAME__, __LINE__, __FUNCTION__);

    Aws::DynamoDB::Model::QueryRequest * request = NEW Aws::DynamoDB::Model::QueryRequest;

    assert(can_query());

    CSymbolTable st;

    request->SetTableName(m_table_name);
    if (!m_index_name.empty())
        request->SetIndexName(m_index_name);

    request->SetConsistentRead(m_consistent);

    // if no projection was specified (i.e. select *) then don't send
    // down a projection, the default is all attributes.
    if(!m_projection->empty())
        request->SetProjectionExpression(serialize_projection(&st));

    if (m_rate_limited)
        request->SetReturnConsumedCapacity(Aws::DynamoDB::Model::ReturnConsumedCapacity::TOTAL);
    else
        request->SetReturnConsumedCapacity(m_consumed_capacity);

    std::string key_condition = m_where_clone->query_key_condition_expression(m_pk, m_rk, &st);
    logdebug("[%s, %d] setting key condition = %s\n", __FILENAME__, __LINE__, key_condition.c_str());
    request->SetKeyConditionExpression(key_condition);

    std::string filter_expression = m_where_clone->query_filter_expression(m_pk, m_rk, &st);
    if (!filter_expression.empty())
    {
        logdebug("[%s, %d] setting filter expression = %s\n", __FILENAME__, __LINE__, filter_expression.c_str());
        request->SetFilterExpression(filter_expression);
    }

    // there has to be a symbol table
    assert(st.has_names());
    request->SetExpressionAttributeNames(st.get_names());

    // at least a PK check must exist, so there have to be values.
    assert(st.has_values());
    request->SetExpressionAttributeValues(st.get_values());

    // for debugging, set the limit to 1
    // request->SetLimit(1);

    std::string requeststr =  strip_newlines(request->SerializePayload());
    logdebug("[%s, %d] request = %s\n", __FILENAME__, __LINE__, requeststr.c_str());

    return request;
}

Aws::DynamoDB::Model::ScanRequest * CSelectHelper::scan_request()
{
    logdebug("[%s, %d] In %s.\n", __FILENAME__, __LINE__, __FUNCTION__);

    Aws::DynamoDB::Model::ScanRequest * request = NEW Aws::DynamoDB::Model::ScanRequest;

    CSymbolTable st;

    request->SetTableName(m_table_name);
    if (!m_index_name.empty())
        request->SetIndexName(m_index_name);

    request->SetConsistentRead(m_consistent);

    // if no projection was specified (i.e. select *) then don't send
    // down a projection, the default is all attributes.
    if(!m_projection->empty())
        request->SetProjectionExpression(serialize_projection(&st));

    if (m_rate_limited)
        request->SetReturnConsumedCapacity(Aws::DynamoDB::Model::ReturnConsumedCapacity::TOTAL);
    else
        request->SetReturnConsumedCapacity(m_consumed_capacity);

    if (m_where_clone)
    {
        std::string filter_expression = m_where_clone->scan_filter_expression(&st);
        if (!filter_expression.empty())
        {
            logdebug("[%s, %d] setting filter expression = %s\n", __FILENAME__, __LINE__, filter_expression.c_str());
            request->SetFilterExpression(filter_expression);
        }
    }

    if (st.has_names())
        request->SetExpressionAttributeNames(st.get_names());

    if(st.has_values())
        request->SetExpressionAttributeValues(st.get_values());

    // for debugging, set the limit to 1
    // request->SetLimit(1);

    std::string requeststr =  strip_newlines(request->SerializePayload());
    logdebug("[%s, %d] request = %s\n", __FILENAME__, __LINE__, requeststr.c_str());

    return request;
}

std::string CSelectHelper::serialize_projection(CSymbolTable * st)
{
    std::string projection;

    logdebug("[%s, %d] serialize_projection() - m_projection %s null\n", __FILENAME__, __LINE__,
             m_projection ? "is NOT" : "is");

    if (m_projection && !m_projection->empty())
    {
        for (const auto &p: *m_projection)
        {
            logdebug("[%s, %d] found projection entry %s\n", __FILENAME__, __LINE__, p.c_str());
            if (projection.empty())
                projection += (st ? st->new_attribute(p) : p);
            else
                projection = projection + ", " + (st ? st->new_attribute(p) : p);
        }
    }

    logdebug("[%s, %d] serialize_projection() returning %s\n", __FILENAME__, __LINE__, projection.c_str());
    return projection;
}

std::string CSelectHelper::serialize_projection()
{
    return serialize_projection(NULL);
}

Aws::DynamoDB::Model::TransactGetItem * CSelectHelper::txget()
{
    if (!can_getitem())
    {
        logerror("The SELECT cannot be used in a transaction (it cannot be reduced to a GetItem() request).\n");
        return NULL;
    }

    Aws::DynamoDB::Model::TransactGetItem * rv = NEW Aws::DynamoDB::Model::TransactGetItem;
    Aws::DynamoDB::Model::Get get;

    CSymbolTable st;

    get.SetTableName(m_table_name);
    if(!m_projection->empty())
        get.SetProjectionExpression(serialize_projection(&st));

    if (st.has_names())
        get.SetExpressionAttributeNames(st.get_names());

    get.SetKey(m_where_clone->get_gud_key(m_pk, m_rk));
    rv->SetGet(get);
    return rv;
}

Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * CSelectHelper::txwrite()
{
    std::string check_func;
    if (m_exists)
        check_func = "attribute_exists";
    else if (m_not_exists)
        check_func = "attribute_not_exists";
    else
    {
        logerror("A SELECT command without an EXISTS or NOT EXISTS cannot be used in a write transaction.\n");
        return NULL;
    }

    Aws::DynamoDB::Model::TransactWriteItem tx;
    Aws::DynamoDB::Model::ConditionCheck cc;
    Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * rv = new Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem>;
    std::string filter;

    CSymbolTable st;

    // in order to be tx able, this select should able to uniquely identify an item (full primary key)
    cc.SetKey(m_where_clone->get_gud_key(m_pk, m_rk));
    cc.SetTableName(m_table_name);

    filter = m_where_clone->query_filter_expression(m_pk, m_rk, &st);

    if (filter.size() > 0)
        filter += " AND ";

    if (m_projection->empty())
        filter += check_func + "(" + st.new_attribute(m_pk) + ")";
    else
    {
        // if the projection is non-empty, then the requestor wants those attributes to exist
        for (auto p: *m_projection)
            filter += check_func + "(" + st.new_attribute(p) + ")";
    }

    cc.SetConditionExpression(filter);

    if (st.has_names())
        cc.SetExpressionAttributeNames(st.get_names());

    if (st.has_values())
        cc.SetExpressionAttributeValues(st.get_values());

    tx.SetConditionCheck(cc);
    rv->push_back(tx);
    return rv;
}

Aws::Vector<Aws::String> CSelectHelper::show_items(const Aws::Vector<Aws::Map<Aws::String,
                                                   Aws::DynamoDB::Model::AttributeValue>>& items)
{
    Aws::Vector<Aws::String> rv;
    for (const auto &item : items)
    {
        Aws::String i = show_item(item);
        if (i != "")
            rv.push_back(i);
    }

    return rv;
};

std::string CSelectHelper::quote_if_required(std::string input)
{
    if (std::strcspn(input.c_str(), " \t,:\n[]{}") != input.size())
        return "\"" + input + "\"";
    else
        return input;
}

Aws::String CSelectHelper::serialize_value(Aws::DynamoDB::Model::AttributeValue v)
{
    std::string value;

    switch (v.GetType())
    {
    case Aws::DynamoDB::Model::ValueType::STRING:
        value = quote_if_required(v.GetS());
        break;

    case Aws::DynamoDB::Model::ValueType::NUMBER:
        value = v.GetN();
        break;

    case Aws::DynamoDB::Model::ValueType::BOOL:
        if (v.GetBool())
            value = "TRUE";
        else
            value = "FALSE";

        break;

    case Aws::DynamoDB::Model::ValueType::NULLVALUE:
        if (v.GetNull())
            value = "NULL";
        else
            value = "NOT NULL";

        break;

    case Aws::DynamoDB::Model::ValueType::ATTRIBUTE_LIST:
        for (auto f: v.GetL())
        {
            if (value.empty())
                value = "[" + serialize_value(*f);
            else
                value = value + ", " + serialize_value(*f);
        }

	if (value.empty())
	    value = "[]";
	else
	    value = value + "]";

        break;

    case Aws::DynamoDB::Model::ValueType::ATTRIBUTE_MAP:
        for (const auto& m: v.GetM())
        {
            if (value.empty())
                value = "{" + quote_if_required(m.first) + ":" + serialize_value(*(m.second));
            else
                value =  value + ", " + quote_if_required(m.first) + ":" + serialize_value(*(m.second));
        }

	if (value.empty())
	    value = "{}";
	else
	    value = value + "}";

        break;

    default:
        value = "Unknown Type";
        break;
    }

    return value;
}

Aws::String CSelectHelper::show_item(const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>& item)
{
    std::string itemstring = "";

    if (item.size() > 0)
    {
        for (const auto& i : item)
        {
            std::string value = serialize_value(i.second);

            if (itemstring == "")
                itemstring = "{" + quote_if_required(i.first) + ": " + value;
            else
                itemstring = itemstring + ", " + quote_if_required(i.first) + ": " + value;
        }

        if (itemstring == "")
            itemstring = "{}";
        else
            itemstring = itemstring + "}";
    }

    return itemstring;
}
