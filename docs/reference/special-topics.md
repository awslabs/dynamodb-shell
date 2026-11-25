<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Data Manipulation Language - Special Topics

## WHERE Clause

You can use a WHERE clause with SELECT, UPDATE, DELETE and UPSERT.  In the WHERE clause, you can include multiple logical expressions which can be joined with the logical AND and logical OR. In addition, you can negate a logical expression with the NOT operator.

The following logical expressions are supported.

* Logical comparison of the form attribute op value where attribute is a table attribute, op is one of <, >, <=, >=, = or !=
* attribute_exists(attr) which evaluates to true if attr exists in an item
* begins_with(attr, prefix) which evaluates to true if the attr begins with prefix
* attr between value and value which evaluates to true if attr is between the two values provided
* attribute_type (attr, type) which evaluates to true if attr is of the specified type.
* contains(attr, operand) which evaluates to true if attr is a string that contains the substring operand, or a set or list that contains the element operand. The operand is a scalar literal: a string (quoted or bare), a number, TRUE, FALSE or NULL. A list or map literal is not accepted. In a prepared statement (the embedding API, or the Python package's `Cursor.execute(sql, params)`) the operand may be a `?` parameter marker. An operand whose type cannot appear in attr (for example `contains(s, 5)` on a string attribute) is not an error; the condition is false and the item does not match.

Examples: First some sample data

```
us-east-1 (*)> select * from exprtest;
{a: 2, b: 2}
{a: 1, b: 2}
{a: 3, b: cooperate}
{a: 5, c: coffee}
{a: 4, b: coffee}
us-east-1 (*)>
```

Inequality comparison with AND

```
us-east-1 (*)> select * from exprtest where a > 3 and a < 5;
{a: 4, b: coffee}
```

The BETWEEN operator

```
us-east-1 (*)> select * from exprtest where a between 2 and 5;
{a: 2, b: 2}
{a: 3, b: cooperate}
{a: 5, c: coffee}
{a: 4, b: coffee}
```

The BEGINS_WITH operator

```
us-east-1 (*)> select * from exprtest where begins_with(b, "co");
{a: 3, b: cooperate}
{a: 4, b: coffee}
```

The ATTRIBUTE_EXISTS operator

```
us-east-1 (*)> select * from exprtest where attribute_exists(c);
{a: 5, c: coffee}

us-east-1 (*)> select * from exprtest where attribute_exists(c) and a != 3;
{a: 5, c: coffee}

us-east-1 (*)> select * from exprtest where attribute_exists(c) or a = 3;
{a: 3, b: cooperate}
{a: 5, c: coffee}
```

The ATTRIBUTE_TYPE operator

```
us-east-1 (*)> select * from exprtest where attribute_type(b, string);
{a: 3, b: cooperate}
{a: 4, b: coffee}
us-east-1 (*)>
```

The CONTAINS operator. The second argument is a string, a number, TRUE, FALSE
or NULL, so you can look for a NULL or boolean element of a list, or a number
in a number set (`ns` below holds the number set {5, 6}).

```
us-east-1 (*)> select pk, l from conttest;
{l: [1, NULL, x], pk: 1}
{l: [TRUE, 2], pk: 3}
{pk: 4}
us-east-1 (*)> select * from conttest where contains(l, NULL);
{l: [1, NULL, x], pk: 1}
us-east-1 (*)> select * from conttest where contains(l, TRUE);
{l: [TRUE, 2], pk: 3}
us-east-1 (*)> select pk from conttest where contains(ns, 5);
{pk: 4}
us-east-1 (*)> select pk from conttest where contains(l, x);
{pk: 1}
```

Using parenthesis to group items in a WHERE clause

```

us-east-1 (*)> select * from exprtest where attribute_type(b, string) or ( a = 5 and c = "coffee");
{a: 3, b: cooperate}
{a: 5, c: coffee}
{a: 4, b: coffee}
```

You can always see how ddbsh executes any command with the EXPLAIN command.


```
us-east-1 (*)> explain select * from exprtest where attribute_type(b, string) or ( a = 5 and c = "coffee");
Scan({
   "TableName":   "exprtest",
   "ReturnConsumedCapacity":   "NONE",
   "FilterExpression":   "attribute_type(#apaa1, :vpaa1) OR (#apaa2 = :vpaa2 AND #apaa3 = :vpaa3)",
   "ExpressionAttributeNames":   {
      "#apaa1":   "b",
      "#apaa2":   "a",
      "#apaa3":   "c"
   },
   "ExpressionAttributeValues":   {
      ":vpaa1":   {
         "S":   "S"
      },
      ":vpaa2":   {
         "N":   "5"
      },
      ":vpaa3":   {
         "S":   "coffee"
      }
   },
   "ConsistentRead":   false
})
us-east-1 (*)>
```

You can use a WHERE clause in an UPDATE as well.

```
us-east-1 (*)> help update;
UPDATE - Update items in a table

   UPDATE <name> SET <update_set> [where clause]

   UPDATE <name> REMOVE <remove list> [where clause]

us-east-1 (*)>

us-east-1 (*)> update exprtest set b = 15 where a = 5;
UPDATE (0 read, 1 modified, 0 ccf)
us-east-1 (*)> select * from exprtest where a = 5;
{a: 5, b: 15, c: coffee}
us-east-1 (*)>

```

Now delete the attribute ‘c’

```
us-east-1 (*)> update exprtest remove c where a = 5;
UPDATE (0 read, 1 modified, 0 ccf)
us-east-1 (*)> select * from exprtest where a = 5;
{a: 5, b: 15}
us-east-1 (*)>
```

## Rate Limiting

*Note: Rate Limiting was added in DynamoDB Shell version 0.5.*

DML operations like SELECT, INSERT, UPDATE, DELETE, UPSERT, and REPLACE can operate on a single item, or many items. When a SELECT has a where clause that targets many items, it is translated into a Query() or Scan() operation. An INSERT may be used to insert a single item, or many items. Each of these activities consumes a certain number of RCUs and WCUs. An operation that operates on many items is called a bulk operation.

Bulk operations can interfere with foreground applications and it is therefore useful to be able to rate limit these operations. This can be accomplished by providing a rate limiting clause.

The rate limiting clause takes the form.

``` SQL
ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |
             WITH RATELIMIT ( RR RCU ) |
             WITH RATELIMIT ( WW WCU )
```

Consider this example.

``` SQL
us-east-1> create table rate_limit_test ( a number, b number ) primary key ( a hash ) gsi ( gsione on (b hash) projecting all);
CREATE
us-east-1> insert into rate_limit_test (a, b) values ( 1, 2 ), (2, 3), (3, 4) with ratelimit ( 1 wcu );
INSERT
INSERT
INSERT
us-east-1>
```

When this insert is performed, each write to the table consumes 2 WCUs (one for the base table and one for the index). Since the operation is issued with a ratelimit of 1 WCU, a delay is added automatically after each INSERT.

Rate limiting is implemented with token buckets. An operation can specify a read and a write limit, each gets its own token bucket. All RCUs consumed by operations count against the read limit (read token bucket) and all writes to base tables and indices count against the write limit (write token bucket).

Consider this example.

``` SQL
us-east-1> update rate_limit_test set updated = true with ratelimit ( 1 rcu, 1 wcu );
UPDATE (3 read, 3 modified, 0 ccf)
us-east-1>
```

Each of these updates will consume 2 WCU and therefore a similar delay will be inserted after each to bring the write limit down to 1 WCU. As this is a bulk opeartion, it is performed as a read (in this case, a Scan) followed by writes. The Scan() will be rate limited to the read limit.

Rate limiting works by performing read and write operations, and looking at the consumed capacity of these. If a read rate is specified, a token bucket is created for that. If a write rate is specified, a token bucket is created for that. Each token bucket accumulates tokens at the specified rate. When there are read tokens available, the operation can be performed.

So, assume that this query is issued:

``` SQL
us-east-1> update customers set state = "CA" where zip = "90210" with ratelimit ( 100 rcu, 500 wcu );
```

Reads against the customers table are issued at a read limit of up to 100 RCU/s and updates are done at a limit of up to 500 WCU/s. Assume that the Scan() on the customers table is attempted when there are 2 tokens in the read bucket, and the Scan() consumes 60 RCU. The read token bucket now goes to -58 and it will take about 0.6s before it has a positive number of tokens. Within 0.6s, any attempt to read will block. This same mechanism is also applied to writes. For long running operations, we ensure that neither token bucket accumulates more than one second's credit of tokens.

Finally, if a table (customers) has a GSI on the column `zip` called `zipgsi`.

The query

``` SQL
us-east-1> explain update customers set state = "CA" where zip = "90210" with ratelimit (10 rcu, 20 wcu);
Scan({
   "TableName":   "customers",
   "ReturnConsumedCapacity":   "TOTAL",
   "ProjectionExpression":   "#abaa1",
   "FilterExpression":   "#abaa2 = :vbaa1",
   "ExpressionAttributeNames":   {
      "#abaa1":   "custid",
      "#abaa2":   "zip"
   },
   "ExpressionAttributeValues":   {
      ":vbaa1":   {
         "S":   "90210"
      }
   },
   "ConsistentRead":   false
})
[...]
```

would result in a table scan on the table customers.

However, the query

``` SQL
us-east-1> explain update customers.zipgsi set state = "CA" where zip = "90210" with ratelimit (10 rcu, 20 wcu);
Query({
   "TableName":   "customers",
   "IndexName":   "zipgsi",
   "ConsistentRead":   false,
   "ReturnConsumedCapacity":   "TOTAL",
   "ProjectionExpression":   "#adaa1",
   "KeyConditionExpression":   "#adaa2 = :vdaa1",
   "ExpressionAttributeNames":   {
      "#adaa1":   "custid",
      "#adaa2":   "zip"
   },
   "ExpressionAttributeValues":   {
      ":vdaa1":   {
         "S":   "90210"
      }
   }
})
[...]
```

would be processed as a query on the GSI and an update to the table. Performing an update/delete/upsert in this way reduces the cost by leveraging an index where available.

## Return Values

Support for Return values was added in ddbsh version 0.6.1.

One of the very cool features of DynamoDB is that mutating APIs allow you to return an item either before or after the mutation (at no extra cost). This has some very interesting applications. First, what are return values?

**This description relates to mutations that are not part of a transaction.**

The mutating APIs are PutItem, DeleteItem and UpdateItem. You can read more about return values [here](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/WorkingWithItems.html#WorkingWithItems.ReturnValues).

In DynamoDB Shell, you can specify return values in UPDATE, UPSERT, REPLACE, and DELETE. For obvious reasons, it is not supported on INSERT.

Consider the following example that illustrates the implementation of a simple counter.

First, we create a counter table as shown below.

``` SQL
us-east-1> create table counter (id string) primary key (id hash);
CREATE
us-east-1> insert into counter (id, value) values ("ticketId", 1);
INSERT
us-east-1> select * from counter;
{id: ticketId, value: 1}
us-east-1>
```

To get the next available counter value and update the counter at the same time, we can perform an update like this.

``` SQL
update counter set value = value + 1 where id = "ticketId" return values updated old;
{value: 1}
UPDATE (0 read, 1 modified, 0 ccf)
us-east-1> select * from counter;
{id: ticketId, value: 2}
us-east-1>
```

The UPDATE above did three things:

1. It captured the value of the "value" attribute for the item where id = "ticketId", and
2. It incremented the value, and
3. It returned the value before the UPDATE.

Instead of UPDATED OLD, we could fetch the whole OLD item.

``` SQL
us-east-1> update counter set value = value + 1 where id = "ticketId" return values all old;
{id: ticketId, value: 2}
UPDATE (0 read, 1 modified, 0 ccf)
us-east-1> select * from counter where id = "ticketId";
{id: ticketId, value: 3}
us-east-1>
```

Return Values can also be used in bulk operations. Assume that you had a table with many counters, you could reinitialize them all in this way.

``` SQL
us-east-1> select * from counter;
{id: ticketId, value: 3}
{id: pageId, value: 27}
{id: linkId, value: 403}
{id: customerId, value: 20071}
us-east-1> update counter set value = 1 return values all old;
{id: ticketId, value: 3}
{id: pageId, value: 27}
{id: linkId, value: 403}
{id: customerId, value: 20071}
UPDATE (4 read, 4 modified, 0 ccf)
us-east-1> select * from counter;
{id: ticketId, value: 1}
{id: pageId, value: 1}
{id: linkId, value: 1}
{id: customerId, value: 1}
us-east-1>

```

You can similarly use return values with UPSERT, if there is an old item, it will be returned.


``` SQL
us-east-1> select * from counter where id = "streetId";
us-east-1> upsert counter set value = 5 where id = "streetId" return values all old;

UPSERT (0 read, 1 modified, 0 ccf)
us-east-1>

us-east-1> select * from counter where id = "streetId";
{id: streetId, value: 5}
us-east-1> upsert counter set value = 16 where id = "streetId" return values all old;
{id: streetId, value: 5}
UPSERT (0 read, 1 modified, 0 ccf)
us-east-1> select * from counter where id = "streetId";
{id: streetId, value: 16}
us-east-1>

```

And when you delete, you can get the item being deleted.

``` SQL
us-east-1> delete from counter return values all old;
{id: streetId, value: 16}
{id: ticketId, value: 1}
{id: pageId, value: 1}
{id: linkId, value: 1}
{id: customerId, value: 2067}
DELETE (5 read, 5 modified, 0 ccf)
us-east-1>
```

**Note:** It is cheaper to implement a monotonically increasing counter using return values than it is to do it with a read followed by a conditional write. The return of values is free so all you pay for is a single write with no condition.

## On-Demand Throughput

DynamoDB supports throughput limiting on On-Demand tables.

On-Demand throughput can be set on tables and indexes. The current settings can be displayed with the describe command.

For example, here is the creation of a table with throughput limits specified.

``` SQL
create table customer ( pk number, a number ) primary key (pk hash) billing mode on demand throughput (40 rcu, 30 wcu)
          gsi (gsipka on ( a hash ) projecting all billing mode on demand throughput (50 rcu, 70 wcu))
```

You can observe these settings as shown below.

```
us-east-1> describe customer;
Name: customer (ACTIVE)
Billing Mode: On Demand
On Demand Throughput: (40 RCU, 30 WCU)
[...]
GSI gsipka: ( HASH a ), Billing Mode: On Demand (mirrors table), Throughput (50 RCU, 70 WCU), Projecting (ALL), Status: ACTIVE, Backfilling: NO
[...]
us-east-1> 
```

You can update the on demand throughput like this, here it resets the table to have unlimited WCUs.

``` SQL
us-east-1> alter table customer set billing mode on demand throughput (50 rcu, -1 wcu);
ALTER
us-east-1> describe customer;
[...]
Billing Mode: On Demand
On Demand Throughput: (50 RCU, -1 WCU)
```

And here the throughput limit is removed altogether.

``` SQL
us-east-1> alter table customer set billing mode on demand throughput unlimited;
ALTER
us-east-1> describe customer;
[...]
Billing Mode: On Demand
PITR is Disabled.
```

The index throughputs can also be updated.

``` SQL
us-east-1> alter table customer update gsi ( gsipka set billing mode on demand throughput (-1 rcu, 4 wcu));
ALTER
us-east-1> describe customer;
[...]
GSI gsipka: ( HASH a ), Billing Mode: On Demand (mirrors table), Throughput (-1 RCU, 4 WCU), Projecting (ALL), Status: ACTIVE, Backfilling: NO

```

Or removed altogether.

``` SQL
us-east-1> alter table customer update gsi ( gsipka set billing mode on demand throughput unlimited);
ALTER
us-east-1> describe customer;
[...]
GSI gsipka: ( HASH a ), Billing Mode: On Demand (mirrors table), Projecting (ALL), Status: ACTIVE, Backfilling: NO
[...]
```

# Warm Throughput

DynamoDB added support for warm throughput for tables and indexes. You can now specify warm throughput during table/index creation, or subsequently using ALTER TABLE.

For example, you could create a table with a large warm throughput as an on demand table, without having to switch to provisioned and such.

```
us-east-1> explain create table customers (custid number) primary key (custid hash) warm throughput (2000000 rcu, 1000000 wcu);
CreateTable({
   "AttributeDefinitions":   [{
         "AttributeName":   "custid",
         "AttributeType":   "N"
      }],
   "TableName":   "customers",
   "KeySchema":   [{
         "AttributeName":   "custid",
         "KeyType":   "HASH"
      }],
   "BillingMode":   "PAY_PER_REQUEST",
   "TableClass":   "STANDARD",
   "DeletionProtectionEnabled":   false,
   "WarmThroughput":   {
      "ReadUnitsPerSecond":   2000000,
      "WriteUnitsPerSecond":   1000000
   }
})
CREATE
```

The above would create an on demand table with 2MM RCU and 1MM WCU (if your account permits this).

Similarly, you could add a new index to a table called customertxns this way.

```
us-east-1> alter table customertxns (custid string, txid string) create gsi txngsi on (txid hash, custid range) projecting all warm throughput (15000 rcu, 4000 wcu);
ALTER
us-east-1>
```

You could see the warm throughput of the table in the output of DESCRIBE like this.

```
us-east-1> describe customertxns;
Name: customertxns (ACTIVE)
Key: HASH custid
Attributes:  custid, S,  txid, S
Created at: 2024-09-30T00:57:12Z
...
Table size (bytes): 0
Item Count: 0
Deletion Protection: Disabled
Billing Mode: On Demand
Warm Throughput: (12000 RCU, 4000 WCU, Status ACTIVE)
PITR is Disabled.
GSI txngsi: ( HASH txid, RANGE custid ), Billing Mode: On Demand (mirrors table), Projecting (ALL), Status: CREATING, Backfilling: YES, Warm Throughput: (15000 RCU, 4000 WCU, Status UPDATING)
LSI: None
Stream: Disabled
Table Class: STANDARD
SSE: Not set
us-east-1>
```

