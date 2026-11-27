<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Data Manipulation Language

For the examples that follow, we use a table (sampletable) created as shown below.

```
create table sampletable ( pk number, rk string, zip number, ht number )
primary key ( pk hash, rk range)
billing mode provisioned ( 20 wcu, 30 rcu)
gsi ( gsi1 on (zip hash, ht range) projecting include ( rk ) billing mode provisioned (  10 rcu, 20 wcu ))
stream ( new image )
table class standard infrequent access
tags ( name : sampletable, type : demonstration );
```

Some sample data is loaded into this table.

```
{pk: 3, rk: three, x: 61}
{pk: 2, rk: one, x: 21, y: 43}
{pk: 4, rk: three, x: 11, y: 12, z: 13}
{pk: 4, rk: two, x: 11, y: 32}
{pk: 1, rk: one, x: 11}
{pk: 1, rk: three, y: 13}
{pk: 1, rk: two, y: 12}
```

## DELETE

**Purpose:** Delete data from a table.

**Syntax:** DELETE FROM *table_or_index* \[WHERE *condition*\] [ratelimit] [return_values]

Deletes data from a table. When a WHERE clause is provided, items matching the condition are identified and deleted. If no WHERE clause is provided, all items are deleted.

DynamoDB only supports item at a time deletes (or batches which delete batches of items). Every item to be deleted must be identified by its primary key. Therefore, if the DELETE includes a where clause that completely specifies the primary key elements (hash key, and a range key if the table has one) then the item identified is deleted. If the WHERE clause does not completely specify the primary key, an appropriate SELECT is first executed to identify all items that match the WHERE clause, and then those items are deleted one at a time.

In standard SQL, a DELETE statement is an implicit transaction. In ddbsh, DELETE is NOT an implicit transaction. Each item is deleted individually. It is possible that an item is updated in the time between the SELECT and the subsequent DELETE. The item is only deleted if it matches the delete condition at the time when the DeleteItem() API is executed.

For information about rate limiting see the section on [rate limiting](special-topics.md#rate-limiting). You can specify either a table or an index name in the UPDATE. If this is the update of a single item based on the PK and FK of the table, specifying an index name is immaterial. If a direct update of an item in the table based on a PK/FK is not possible, an index name will cause the query or scan to be performed on the index.

For information about return values see the section [Return Values](special-topics.md#return-values).

**Example: 1**

```
us-east-1> delete from sampletable where pk = 1 and rk = "one";
DELETE
us-east-1> select * from sampletable;
{pk: 3, rk: three, x: 61}
{pk: 2, rk: one, x: 21, y: 43}
{pk: 4, rk: three, x: 11, y: 12, z: 13}
{pk: 4, rk: two, x: 11, y: 32}
{pk: 1, rk: three, y: 13}
{pk: 1, rk: two, y: 12}
us-east-1>
```

**Example: 2**

Continuing from the previous example, consider this delete with a partial primary key specified. It would be executed as shown below.

```
us-east-1> explain delete from sampletable where rk = "two";
Scan({
   "TableName":   "sampletable",
   "ReturnConsumedCapacity":   "NONE",
   "ProjectionExpression":   "#a1, #a2",
   "FilterExpression":   "#a3 = :v1",
   "ExpressionAttributeNames":   {
      "#a1":   "pk",
      "#a2":   "rk",
      "#a3":   "rk"
   },
   "ExpressionAttributeValues":   {
      ":v1":   {
         "S":   "two"
      }
   },
   "ConsistentRead":   false
})
DeleteItem({
   "TableName":   "sampletable",
   "Key":   {
      "pk":   {
         "N":   "4"
      },
      "rk":   {
         "S":   "two"
      }
   },
   "ConditionExpression":   "#a1 = :v1",
   "ExpressionAttributeNames":   {
      "#a1":   "rk"
   },
   "ExpressionAttributeValues":   {
      ":v1":   {
         "S":   "two"
      }
   }
})
DeleteItem({
   "TableName":   "sampletable",
   "Key":   {
      "pk":   {
         "N":   "1"
      },
      "rk":   {
         "S":   "two"
      }
   },
   "ConditionExpression":   "#a1 = :v1",
   "ExpressionAttributeNames":   {
      "#a1":   "rk"
   },
   "ExpressionAttributeValues":   {
      ":v1":   {
         "S":   "two"
      }
   }
})
```

First the table is scanned (as the PK was not specified, a Scan() is the only option). The Scan returned one row which is then deleted.

Now consider another example of a partial primary key, this time specifying the PK.

**Example: 3**

```
us-east-1> explain delete from sampletable where pk = 4;
Query({
   "TableName":   "sampletable",
   "ConsistentRead":   false,
   "ReturnConsumedCapacity":   "NONE",
   "ProjectionExpression":   "#a1, #a2",
   "KeyConditionExpression":   "#a3 = :v1",
   "ExpressionAttributeNames":   {
      "#a1":   "pk",
      "#a2":   "rk",
      "#a3":   "pk"
   },
   "ExpressionAttributeValues":   {
      ":v1":   {
         "N":   "4"
      }
   }
})
DeleteItem({
   "TableName":   "sampletable",
   "Key":   {
      "pk":   {
         "N":   "4"
      },
      "rk":   {
         "S":   "three"
      }
   }
})
DeleteItem({
   "TableName":   "sampletable",
   "Key":   {
      "pk":   {
         "N":   "4"
      },
      "rk":   {
         "S":   "two"
      }
   }
})
DELETE
us-east-1>
```

The PK is specified, so we are able to perform a Query() which returns two rows which are then deleted one at a time.

## INSERT

**Purpose:** Inserts an item into a table.

**Syntax:** INSERT INTO *table* ( <columns> ) VALUES <value> [ratelimit]

Inserts an item into a table with the specified attribute values. All primary key attributes must be specified. If an item with the same primary key already exists, an error is generated.

For information about rate limiting see the section on [rate limiting](special-topics.md#rate-limiting).

**Example: 1**

```
insert into sampletable ( pk, rk, x ) values ( 1, "one", 11 );
insert into sampletable ( pk, rk, y ) values ( 1, "two", 12 );
insert into sampletable ( pk, rk, y ) values ( 1, "three", 13 );
insert into sampletable ( pk, rk, x, y ) values ( 2, "one", 21, 43 );
insert into sampletable ( pk, rk, x ) values ( 3, "three",61 );
insert into sampletable ( pk, rk, x, y, z ) values ( 4, "three", 11, 12, 13 );
insert into sampletable ( pk, rk, x, y ) values ( 4, "two", 11, 32 );
```

**Example: 2**

```
us-east-1> insert into sampletable (pk, rk, conflict) values ( 3, "three", "conflict" );
[run, 83] INSERT failed. The conditional request failed
us-east-1>
```
## REPLACE

**Purpose:** Replaces an item in a table, if one exists. Creates a new one if one does not exist.

**Syntax:** REPLACE INTO *table* ( <columns> ) VALUES <value> [ratelimit] [return_values]

Inserts or replaces an item with the specified primary key. All primary key attributes must be specified. If an item already exists with that primary key, it is replaced.

For information about rate limiting see the section on [rate limiting](special-topics.md#rate-limiting).

For information about return values see the section [Return Values](special-topics.md#return-values).

**Example:**

```
us-east-1> select * from sampletable where pk = 3 and rk = "three";
{pk: 3, rk: three, x: 61}
us-east-1> replace into sampletable ( pk, rk, rpl ) values ( 3, "three", "replaced" );
REPLACE
us-east-1> select * from sampletable where pk = 3 and rk = "three";
{pk: 3, rk: three, rpl: replaced}
us-east-1> select * from sampletable where pk = 12 and rk = "nonexistant";
us-east-1> replace into sampletable (pk, rk, ins ) values ( 12, "nonexistant", "inserted" );
REPLACE
us-east-1> select * from sampletable where pk = 12 and rk = "nonexistant";
{ins: inserted, pk: 12, rk: nonexistant}
us-east-1>
```


## SELECT

**Purpose:** Selects items from a table or index.

**Syntax:** SELECT \[CONSISTENT\] *|<columns> FROM *table*\[.*index*\] \[WHERE *condition*\] [consumed_capacity] [ratelimit]

Selects data from a table or index. If the optional *CONSISTENT* flag is specified, a consistent read is executed.

You can reference all scalar types (number, string and binary) as well as boolean, null, list and map in the projection list (columns) and in the where condition.

List elements are indexed with a 0 base. Map elements use '.' as the separator.

For information about rate limiting see the section on [rate limiting](special-topics.md#rate-limiting).

SELECT can return the consumed capacity during the operation.

``` SQL
consumed_capacity := RETURN CONSUMED CAPACITY INDEXES |
    RETURN CONSUMED CAPACITY TOTAL
```

**Example: 1**

```
us-east-1> select * from sampletable where pk = 12;
{ins: inserted, pk: 12, rk: nonexistant}
us-east-1>
```

**Example: 2**

The table has a GSI whose key definition is

gsi ( gsi1 on (zip hash, ht range) projecting include ( rk ) billing mode provisioned (  10 rcu, 20 wcu ))


```
us-east-1> select * from sampletable.gsi1;
us-east-1> insert into sampletable ( pk, rk, zip, ht) values ( 71, "somewhere", 12345, 45);
INSERT
us-east-1> select * from sampletable.gsi1;
{ht: 45, pk: 71, rk: somewhere, zip: 12345}
us-east-1>
```

SELECT will execute either a GetItem(), Query() or Scan() depending on whether the request specifies a complete primary key (or not) and whether the request targets a table or index.

A not-equal on the sort key (`<>`, `!=` or `NOT sk = value`) is not allowed in a DynamoDB key condition. When the partition key is given with `=` and the not-equal is the only condition on the sort key, SELECT runs two Queries instead of a Scan: one for the items below the value, then one for the items above it, each with the same filter for the other conditions. Rows come back in sort-key order, as from one Query, and EXPLAIN prints both Query() requests. The same applies to a `table.index` and the index's sort key (for a composite-key GSI, the last sort-key attribute given). UPDATE, UPSERT and DELETE find the items to change the same way.

The statement stays a Scan when the split cannot be written as two key conditions: the sort key also has another condition (for example `sk <> 5 and sk > 2`), the not-equal is under OR or a negated group, the value is not of the sort key's type, or the not-equal is on the partition key or on a composite sort-key attribute other than the last one given.

```
us-east-1> explain select * from t where pk = 1 and sk <> 5;
Query({
   ...
   "KeyConditionExpression":   "#aaaa1 = :vaaa1 AND #aaaa2 < :vaaa2",
   ...
})
Query({
   ...
   "KeyConditionExpression":   "#aaaa1 = :vaaa1 AND #aaaa2 > :vaaa2",
   ...
})
```

## UPDATE

**Purpose:** Update items in a table.

**Syntax:** UPDATE *table_or_index* SET *assignments* \[WHERE *condition*\] [ratelimit] [return_values]

For information about rate limiting see the section on [rate limiting](special-topics.md#rate-limiting). You can specify either a table or an index name in the UPDATE. If this is the update of a single item based on the PK and FK of the table, specifying an index name is immaterial. If a direct update of an item in the table based on a PK/FK is not possible, an index name will cause the query or scan to be performed on the index.

For information about return values see the section [Return Values](special-topics.md#return-values).

Updates items in the table with assignments as specified. The assignments can be of the form:


```
<column> = <value>

<column> = <column> +/- <value>

<column> = <column>

or

<column> = if_not_exists(*attribute*, *default*) \[+/- <value>\]
```

**Example: 1**

```
us-east-1> select * from sampletable where pk = 1;
{pk: 1, rk: one, x: 11}
{pk: 1, rk: three, y: 13}
{pk: 1, rk: two, y: 12}
us-east-1> update sampletable set newattr = 14 where pk = 1 and rk = "one";
UPDATE
us-east-1> select * from sampletable where pk = 1;
{newattr: 14, pk: 1, rk: one, x: 11}
{pk: 1, rk: three, y: 13}
{pk: 1, rk: two, y: 12}
us-east-1>
```

**Example: 2**

```
us-east-1> select * from sampletable where pk = 1;
{pk: 1, rk: one, x: 11}
{pk: 1, rk: three, y: 13}
{pk: 1, rk: two, y: 12}
us-east-1> update sampletable set newattr = if_not_exists(y, 3) + 6 where pk = 1;
UPDATE
us-east-1> select * from sampletable where pk = 1;
{newattr: 9, pk: 1, rk: one, x: 11}
{newattr: 19, pk: 1, rk: three, y: 13}
{newattr: 18, pk: 1, rk: two, y: 12}
us-east-1>
```

## UPSERT

**Purpose:** Performs an UPDATE or INSERT

**Syntax:** UPSERT *table_or_index* SET *assignments* \[WHERE *condition*\] [ratelimit] [return_values]

If an item is matched by the WHERE clause, it is updated. If one does not match the where clause then the *assignments* are converted are used to construct a new item.

For information about rate limiting see the section on [rate limiting](special-topics.md#rate-limiting). You can specify either a table or an index name in the UPSERT. If this is the update of a single item based on the PK and FK of the table, specifying an index name is immaterial. If a direct update of an item in the table based on a PK/FK is not possible, an index name will cause the query or scan to be performed on the index.

For information about return values see the section [Return Values](special-topics.md#return-values).

**Example: 1**

```
us-east-1> select * from sampletable where pk = 2 and rk = "one";
{pk: 2, rk: one, x: 21, y: 43}
us-east-1> upsert sampletable set op = "upsert" where pk = 2;
UPSERT
us-east-1> select * from sampletable where pk = 2 and rk = "one";
{op: upsert, pk: 2, rk: one, x: 21, y: 43}
us-east-1>
```

**Example: 2**

When using an UPSERT it is important to specify the complete primary key as that specification is used to create the new item.

```
us-east-1> select * from sampletable where pk = 7;
us-east-1> upsert sampletable set op = "upsert" where pk = 7 and rk = "seven";
UPSERT
us-east-1> select * from sampletable where pk = 7;
{op: upsert, pk: 7, rk: seven}
us-east-1>
```

## EXPLAIN

**Purpose:** Explain the API calls performed to execute a DDL or DML command.

**Syntax:** EXPLAIN <command>

The EXPLAIN command returns the API request that must be executed in
order to perform the command provided.

**Example**

A table called sampletable was created with a primary key of (pk HASH, and rk RANGE).

```
us-east-1> explain select * from sampletable where pk = 13 and rk = "abc";
GetItem({
   "TableName":   "sampletable",
   "Key":   {
      "pk":   {
         "N":   "13"
      },
      "rk":   {
         "S":   "abc"
      }
   },
   "ConsistentRead":   false,
   "ReturnConsumedCapacity":   "NONE"
})

us-east-1> explain select * from sampletable where pk = 13 ;
Query({
   "TableName":   "sampletable",
   "ConsistentRead":   false,
   "ReturnConsumedCapacity":   "NONE",
   "KeyConditionExpression":   "#a1 = :v1",
   "ExpressionAttributeNames":   {
      "#a1":   "pk"
   },
   "ExpressionAttributeValues":   {
      ":v1":   {
         "N":   "13"
      }
   }
})

us-east-1> explain select * from sampletable where rk = "abc" ;
Scan({
   "TableName":   "sampletable",
   "ReturnConsumedCapacity":   "NONE",
   "FilterExpression":   "#a1 = :v1",
   "ExpressionAttributeNames":   {
      "#a1":   "rk"
   },
   "ExpressionAttributeValues":   {
      ":v1":   {
         "S":   "abc"
      }
   },
   "ConsistentRead":   false
})

```

Note that some commands (updated, delete, replace, and upsert) can return more than one command. For example, to do a range update on the table above, we would see.

```
us-east-1> explain update sampletable set something = 4 where rk = "abc";
Scan({
   "TableName":   "sampletable",
   "ReturnConsumedCapacity":   "NONE",
   "ProjectionExpression":   "#a1, #a2",
   "FilterExpression":   "#a3 = :v1",
   "ExpressionAttributeNames":   {
      "#a1":   "pk",
      "#a2":   "rk",
      "#a3":   "rk"
   },
   "ExpressionAttributeValues":   {
      ":v1":   {
         "S":   "abc"
      }
   },
   "ConsistentRead":   false
})
UpdateItem({
   "TableName":   "sampletable",
   "Key":   {
      "pk":   {
         "N":   "12"
      },
      "rk":   {
         "S":   "abc"
      }
   },
   "UpdateExpression":   "SET #a1 = :v1",
   "ConditionExpression":   "attribute_exists(#a2) AND attribute_exists(#a3) AND #a4 = :v2",
   "ExpressionAttributeNames":   {
      "#a1":   "something",
      "#a2":   "pk",
      "#a3":   "rk",
      "#a4":   "rk"
   },
   "ExpressionAttributeValues":   {
      ":v1":   {
         "N":   "4"
      },
      ":v2":   {
         "S":   "abc"
      }
   }
})
UPDATE
```

