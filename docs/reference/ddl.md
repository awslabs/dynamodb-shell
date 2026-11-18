<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Data Definition Language

## ALTER TABLE

**Purpose:** Make changes to a tables settings including billing mode, table class, streams, and TTL. Also create and drop Global Secondary Indexes (GSI).

**Syntax:** ALTER TABLE *table* [alter-table-options]

### SET billing\_mode\_and\_throughput

Sets table and index billing mode and throughput.

BILLING MODE PROVISIONED ( *NN* RCU, *MM* WCU )

Set the table to provisioned mode with the specified throughput.

BILLING MODE ON DEMAND [throughput]

Set the table to on demand billing mode.

throughput: THROUGHPUT ( *NN* RCU, *MM* WCU )

**Example 1**

```
us-east-1> alter table singlekey set billing mode on demand;
ALTER
us-east-1> describe singlekey;
Name: singlekey (ACTIVE)
[...]
Billing Mode: On Demand
[...]
us-east-1>
```

**Example 2**

```
us-east-1> alter table singlekey set billing mode provisioned ( 200 RCU, 300 WCU );
ALTER
us-east-1> describe singlekey;
[...]
Billing Mode: Provisioned (200 RCU, 300 WCU)
[...]
us-east-1>
```

### SET TABLE CLASS *class*

Alters the table class.

SET TABLE CLASS STANDARD INFREQUENT ACCESS

Set the table to table class standard infrequent access.

SET TABLE CLASS STANDARD

Set the table to standard table class.

**Example 1**

```
us-east-1> alter table singlekey set table class standard infrequent access;
ALTER
us-east-1> describe singlekey;
[...]
Table Class: STANDARD_INFREQUENT_ACCESS
[...]
us-east-1>
```

**Example 2**

```
us-east-1> alter table singlekey set table class standard;
ALTER
us-east-1> describe singlekey;
[...]
Table Class: STANDARD_INFREQUENT_ACCESS
[...]
us-east-1>
```

**Warning** There is a limit on the number of times you can change a table class. If you do it too often, you will get an error like this.

```
us-east-1> alter table singlekey set table class standard infrequent access;
Error: Subscriber limit exceeded: Updates to TableClass are limited to 2 times in 30 day(s).
us-east-1>
```

**Example 3**

Specify an on-demand throughput limit

```
us-east-1> alter table icdthroughput set billing mode on demand throughput (200 wcu, 10 rcu );
ALTER
us-east-1> describe icdthroughput;
[...]
Billing Mode: On Demand
On Demand Throughput: (10 RCU, 200 WCU)
[...]
```

Clear the on-demand throughput limit for reads, but not writes.

```
us-east-1> alter table icdthroughput set billing mode on demand throughput (200 wcu, -1 rcu );
ALTER
us-east-1> describe icdthroughput;
[...]
Billing Mode: On Demand
On Demand Throughput: (-1 RCU, 200 WCU)
[...]
```

Clear all on-demand throghput limits.

```
us-east-1> alter table icdthroughput set billing mode on demand throughput unlimited;
ALTER
```

### SET STREAM

Sets options related to DynamoDB Streams.

SET STREAM DISABLED

Disables DynamoDB Streams on the table.

SET STREAM ( KEYS ONLY )

The DynamoDB Stream will contain only key fields.

SET STREAM ( NEW IMAGE )

The DynamoDB Stream will contain the new item.

SET STREAM ( OLD IMAGE )

The DynamoDB Stream will contain the old item.

SET STREAM ( BOTH IMAGES )

The DynamoDB Stream will contain both the old and new item images.

**Example 1**

```
us-east-1> alter table singlekey set stream (new image);
ALTER
us-east-1> describe singlekey;
[...]
Stream: NEW_IMAGE
[...]
us-east-1>

```

**Example 2**

```
us-east-1> alter table singlekey set stream disabled;
ALTER
us-east-1> describe singlekey;
[...]
Stream: Disabled
[...]
us-east-1>
```

### CREATE GSI

CREATE GSI *name* ON *key\_schema* *projection* *optional\_billing\_mode\_and\_throughput*

Allows you to create a GSI on a table that already exists. A GSI has a key schema (those are the item attributes that form part of the index) and a projection (the attributes in the item that are part of the index).

The projection can be one of *all*, *keys only*, or specify which keys to include.

DROP GSI *name*

**Example 1**

```
us-east-1> alter table singlekey (x number, y number) create gsi xygsi on (x hash, y range) projecting all billing mode provisioned (22 RCU, 41 WCU);
ALTER
us-east-1> describe singlekey;
[...]
GSI xygsi: ( HASH x, RANGE y ),  Provisioned (22 RCU, 41 WCU), Projecting (ALL), Status: CREATING, Backfilling: YES
[...]
us-east-1>

```

The index created above projects all attributes in the item.

**Example 2**

```
us-east-1> alter table table1 (x number, y number) create gsi xykeysgsi on (x hash, y range) projecting keys only billing mode provisioned (22 RCU, 41 WCU);
ALTER
us-east-1> describe table1;
[...]
GSI xykeysgsi: ( HASH x, RANGE y ),  Provisioned (22 RCU, 41 WCU), Projecting (KEYS_ONLY), Status: CREATING, Backfilling: NO
[...]us-east-1>

```

This index only projects the keys.

**Example 3**

```
us-east-1> alter table singlekey (x number, y number, a number)
us-east-1> create gsi xyagsi on (x hash) projecting include (y, a)
us-east-1> billing mode provisioned (20 RCU, 30 WCU);
ALTER
us-east-1>
```

This index will project the keys as well as the attributes *y* and *a*.

**Example 4**

```
us-east-1> alter table singlekey drop gsi xygsi;
ALTER
us-east-1>
```

### SET *billing\_mode\_and\_throughput*

You can change the billing mode and throughput on a table, and its associated GSIs, all in one command.

**Example 5**

```
us-east-1> describe table1;
[...]
Billing Mode: Provisioned (10 RCU, 20 WCU)
GSI pqgsi: ( HASH p ),  Provisioned (20 RCU, 30 WCU), Projecting (INCLUDE) (q), Status: CREATING, Backfilling: YES
GSI xykeysgsi: ( HASH x, RANGE y ),  Provisioned (22 RCU, 41 WCU), Projecting (KEYS_ONLY), Status: ACTIVE, Backfilling: NO
[...]
us-east-1>

```

To change this table to on demand billing mode, we could do this.

```
us-east-1> alter table table1 set billing mode on demand;
ALTER
us-east-1> describe table1;
[...]
Billing Mode: On Demand
GSI pqgsi: ( HASH p ), Billing Mode: On Demand (mirrors table), Projecting (INCLUDE) (q), Status: UPDATING, Backfilling: YES
GSI xykeysgsi: ( HASH x, RANGE y ), Billing Mode: On Demand (mirrors table), Projecting (KEYS_ONLY), Status: UPDATING, Backfilling: NO
[...]
us-east-1>
```

Now we convert it back to a provisioned table.

```
us-east-1> us-east-1> alter table table1
us-east-1> set billing mode provisioned (20 rcu, 30 wcu)
us-east-1> update gsi (pqgsi set billing mode provisioned (20 rcu, 30 wcu), xykeysgsi set billing mode provisioned (3 rcu, 5 wcu));
ALTER
```

### SET PITR [ENABLED|DISABLED]

Enables or disables the setting for PITR (Point In Time Recovery) for a table.

```
us-east-1> describe ipt1;
Name: ipt1 (ACTIVE)
[...]
PITR is Disabled.
[...]
us-east-1> alter table ipt1 set pitr enabled;
ALTER
us-east-1> describe ipt1;
Name: ipt1 (ACTIVE)
[...]
PITR is Enabled: [Mon, 21 Nov 2022 21:29:49 GMT to Mon, 21 Nov 2022 21:29:49 GMT]
[...]
us-east-1>  alter table ipt1 set pitr disabled;
ALTER
us-east-1> describe ipt1;
Name: ipt1 (ACTIVE)
[...]
PITR is Disabled.
[...]
us-east-1>
```

### ADD REPLICA, UPDATE REPLICA, and DROP REPLICA

Enables creation, update and drop of a Global Table replica. ddbsh implements Global Tables (2019).

Make a table and add a replica.

``` SQL

us-east-2> create table global_customers ( custid number ) primary key (custid hash);
us-east-2> alter table global_customers add replica us-west-1;
ALTER
us-east-2>

```

Describe the table.


``` SQL

us-east-2> describe global_customers;
[...]
Replica Region: us-east-2 (Status: ACTIVE)
Replica Region: us-west-1 (Status: ACTIVE)

```

Now add some data to the table.

``` SQL

us-east-2> insert into global_customers (custid, name) values ( 101, "Alice" ), (102, "Bob");
INSERT
INSERT
us-east-2> select * from global_customers;
{custid: 102, name: Bob}
{custid: 101, name: Alice}
us-east-2>

us-east-2> connect us-west-1;
CONNECT
us-west-1> select * from global_customers;
{custid: 102, name: Bob}
{custid: 101, name: Alice}
us-west-1>


```

The data is propagated from us-east-2 to us-west-1 by Global
Tables. Similar updates and another replica with Standard Infrequent
Access table class below.


``` SQL

us-west-1> update global_customers set active = True where custid = 101;
UPDATE (0 read, 1 modified, 0 ccf)
us-west-1> select * from global_customers;
{custid: 102, name: Bob}
{active: TRUE, custid: 101, name: Alice}
us-west-1> connect us-east-2;
CONNECT
us-east-2> select * from global_customers;
{custid: 102, name: Bob}
{active: TRUE, custid: 101, name: Alice}
us-east-2>

us-east-2> alter table global_customers add replica us-west-2 table class standard infrequent access;
ALTER
us-east-2> connect us-west-2;
CONNECT
us-west-2>

...

us-west-2> describe global_customers;
Name: global_customers (CREATING)
Key: HASH custid
[...]
Table Class: STANDARD_INFREQUENT_ACCESS
[...]
Replica Region: us-east-2 (Status: ACTIVE)
Replica Region: us-west-1 (Status: ACTIVE)
Replica Region: us-west-2 (Status: ACTIVE)
us-west-2>

us-west-2> select * from global_customers;
{custid: 102, name: Bob}
{active: TRUE, custid: 101, name: Alice}
us-west-2>

us-west-2> insert into global_customers (custid, name) values ( 103, "Charlie");
INSERT
us-west-2> connect us-east-2;
CONNECT
us-east-2> select * from global_customers;
{custid: 102, name: Bob}
{custid: 103, name: Charlie}
{active: TRUE, custid: 101, name: Alice}
us-east-2>

```

### SET DELETION PROTECTION [ENABLED|DISABLED]

Enables the setting and disabling of deletion protection on tables.

``` SQL
us-east-1> create table dpe ( pk number ) primary key ( pk hash ) deletion protection enabled;
CREATE
us-east-1> describe dpe;
Name: dpe (ACTIVE)
[...]
Deletion Protection: Enabled
[...]

us-east-1> alter table dpe set deletion protection disabled;
ALTER
us-east-1> describe dpe;
[...]
Deletion Protection: Disabled
[...]
us-east-1>

us-east-1> alter table dpe set deletion protection enabled;
ALTER
us-east-1> describe dpe;
Name: dpe (ACTIVE)
[...]
Deletion Protection: Enabled
[...]
us-east-1>

```

## BACKUP TABLE

**Purpose:** Take a backup of a table.

**Syntax:** BACKUP TABLE *name* CALLED *backup name*

Takes a backup of a table's data.

**Example:**

```
us-east-1> backup table lsitest called lsitest-backup;
BACKUP
us-east-1>
```

The backup can be seen in the list of backups.

```
us-east-1> show backups;
[...]
Table: lsitest, Backup: lsitest-backup, Status: AVAILABLE, ARN: arn:aws:dynamodb:us-east-1:3221123459165:table/lsitest/backup/01667964345110-f260cfff, On: 2022-11-09T03:25:45Z, Expires: 1970-01-01T00:00:00Z
us-east-1>
```

## CREATE TABLE

**Purpose:** Creates a table (and GSIs, LSIs, and sets other initial parameters).

**Syntax:**

```
   CREATE TABLE [IF NOT EXISTS][NOWAIT] <name>
         ( attribute_name, attribute_type [,...] )
   primary_key billing_mode_and_throughput
   [gsi_list] [lsi_list] [streams] [table_class] [tags] ;

   attribute_type := NUMBER|STRING|BINARY
   primary_key := PRIMARY KEY key_schema
   key_schema := ( attribute_name HASH [, attribute_name RANGE])

   billing_mode_and_throughput := (BILLING MODE ON DEMAND [throughput])|BILLING MODE provisioned)
   provisioned := ( RR RCU, WW WCU )

   gsi_list := GSI ( gsi_spec )
   gsi_spec := gsi [, gsi ...]
   gsi := gsi_name ON key_schema index_projection [billing_mode_and_throughput]
   index_projection := (PROJECTING ALL) | (PROJECTING KEYS ONLY) | (PROJECTING INCLUDE projection_list)
   projection_list := ( attribute [, attribute ...] )

   lsi_list := LSI ( lsi_spec )
   lsi_spec := lsi [, lsi ...]
   lsi := lsi_name ON key_schema index_projection

   streams := STREAM ( stream_type ) | STREAM DISABLED
   stream_type := KEYS ONLY | NEW IMAGE | OLD IMAGE | BOTH IMAGES

   throughput := THROUGHPUT [( RR RCU, WW WCU ) | UNLIMITED]
   table_class := TABLE CLASS STANDARD | TABLE CLASS STANDARD INFREQUENT ACCESS
   deletion_protection := DELETION PROTECTION [ENABLED|DISABLED]
   tags := TAGS ( tag [, tag ...] )
   tag := name : value
```

## SHOW CREATE TABLE

**Purpose:** The SHOW CREATE TABLE command will generate the CREATE TABLE command required to generate a table.

**Syntax:**

``` SQL
SHOW CREATE TABLE [IF NOT EXISTS][NOWAIT] <name>
```

**Note:** The SHOW CREATE TABLE command was added in DynamoDB Shell version 0.5.1.

The "IF NOT EXISTS" and "NOWAIT" options are transferred into the CREATE TABLE command.

**Example**

``` SQL
us-east-1> show create table icd3;
CREATE TABLE "icd3" ("k1" number, "k2" number, "pk" number, "rk" number) PRIMARY KEY ("pk" HASH, "rk" RANGE) BILLING MODE ON DEMAND GSI ("gsi1" ON ( "k1" HASH, "k2" RANGE ) PROJECTING INCLUDE (k3) BILLING MODE ON DEMAND ) TABLE CLASS STANDARD DELETION PROTECTION DISABLED
us-east-1>
```

## DESCRIBE

**Purpose:** Provides a description of a table.

**Syntax:** DESCRIBE *name*

Provides a description of a table.

## DESCRIBE BACKUPS

**Purpose:** Describes a backup.

**Syntax:** DESCRIBE BACKUP *arn*

**Example**

```
us-east-1> describe backup "arn:aws:dynamodb:us-east-1:3221123459165:table/singlekey/backup/01661373083721-5d1c1146";
Table Name: singlekey
Tabld Id: 2435eaef-96cf-495a-a4ed-589966d77ba7
Table Arn: arn:aws:dynamodb:us-east-1:3221123459165:table/foo
Table Size (bytes): 0
Item Count: 0
Table Key: ( HASH a, RANGE b )
Table Creation Date/Time: 2022-08-24T20:04:08Z
Billing Mode: PROVISIONED
Provisioned Throughput: 20 RCU, 30 WCU
Backup Name: backup71
Backup Arn: arn:aws:dynamodb:us-east-1:3221123459165:table/foo/backup/01661373083721-5d1c1146
Backup Size (bytes): 0
Backup Status: AVAILABLE
Backup Type: USER
Backup Creation Date/Time: 2022-08-24T20:31:23Z
Backup Expiry Date/Time: 1970-01-01T00:00:00Z
us-east-1>
```

## DROP BACKUP

**Purpose:** Deletes a previously created backup.

**Syntax:** DROP BACKUP *arn*

Deletes a backup.

**Example**

```
us-east-1> drop backup "arn:aws:dynamodb:us-east-1: 3221123459165:table/backuptest/backup/01661373083721-5d1c1146";
DROP BACKUP
us-east-1>
```

## DROP TABLE

**Purpose:** Drops a table.

**Syntax:** DROP TABLE [*if exists*] [*no wait*] *name*

Drops a table. If a table by the name doesn't exist, an error is generated. If the "if exists" clause is provided, no error is generated if the table does not exist. By default, the command waits for the table to be dropped. Optionally, the "no wait" clause can be provided, and the command returns immediately. The table is dropped in the background.

**Example 1**

```
us-east-1> drop table unknown;
Drop Table Failed.
	Requested resource not found: Table: unknown not found
us-east-1>
```

```
us-east-1> drop table if exists unknown;
us-east-1>
```

**Example 2**

```
us-east-1> drop table sample13;
..
DROP
us-east-1>
```

## RESTORE TABLE

**Purpose:** Restores a backup and creates a new table.

**Syntax:** RESTORE TABLE *new table name* FROM BACKUP *arn*

Creates a new table from a backup.

**Example**

```
us-east-1> restore table "new-lsitest" from backup "arn:aws:dynamodb:us-east-1: 3221123459165:table/lsitest/backup/01667964345110-f260cfff";
RESTORE
us-east-1>
```

This will create a new table called "new-lsitest" which will have the indexes and billing mode that was stored in the backup.

```
us-east-1> describe new-lsitest;
Name: new-lsitest (CREATING)
Key: HASH a, RANGE d
Attributes:  b, N,  a, N,  d, N,  c, N
Created at: 2022-11-09T03:32:15Z
Table ARN: arn:aws:dynamodb:us-east-1:3221123459165:table/new-lsitest
Table ID: ddfab172-1007-457e-b0bc-922480f03f61
Table size (bytes): 0
Item Count: 0
Billing Mode: On Demand
GSI gsi1: ( HASH c ), Billing Mode: On Demand (mirrors table), Projecting (ALL), Status: , Backfilling: NO
LSI lsi1: ( HASH a, RANGE b ),
Stream: Disabled
Table Class: Has not been set.
SSE: Not set
us-east-1>
```

Note that restores are run asynchronously and the table above is in the *CREATING* state.

**Syntax:** RESTORE TABLE **new table name** FROM **old table name** TO PITR "YYYY-MM-DDTHH:MM:SSZ"

Restores a table to a specific point in time, and creates a new table with that version.

```
us-east-1> describe ipt1;
Name: ipt1 (ACTIVE)
Key: HASH a, RANGE b
Attributes:  a, N,  b, N
Created at: 2022-11-21T20:05:22Z
Table ARN: arn:aws:dynamodb:us-east-1:123456789012:table/ipt1
Table ID: 6b1078cb-e81d-419f-99e8-e6c28e071a47
Table size (bytes): 0
Item Count: 0
Billing Mode: Provisioned (10 RCU, 10 WCU)
PITR is Enabled: [Tue, 22 Nov 2022 01:50:34 GMT to Tue, 22 Nov 2022 01:51:23 GMT]
GSI: None
LSI: None
Stream: Disabled
Table Class: STANDARD
SSE: Not set
us-east-1> restore table cloneipt1 from ipt1 to pitr "2022-11-22T01:50:34Z";
RESTORE
us-east-1> describe cloneipt1;
Name: cloneipt1 (CREATING)
Key: HASH a, RANGE b
Attributes:  b, N,  a, N
Created at: 2022-11-22T01:57:11Z
Table ARN: arn:aws:dynamodb:us-east-1:123456789012:table/cloneipt1
Table ID: 514794c5-bc60-4c54-9fdf-e2c1c670074e
Table size (bytes): 0
Item Count: 0
Billing Mode: Provisioned (10 RCU, 10 WCU)
PITR is Disabled.
GSI: None
LSI: None
Stream: Disabled
Table Class: Has not been set.
SSE: Not set
us-east-1>

```


## SHOW BACKUPS

**Purpose:** Lists backups.

**Syntax:** SHOW BACKUPS

Shows the backups available to the connected user.

**Example**

```
us-east-1> show backups;
Table: singlekey, Backup: backup71, Status: AVAILABLE, ARN: arn:aws:dynamodb:us-east-1:3221123459165:table/singlekey/backup/01661373083721-5d1c1146, On: 2022-08-24T20:31:23Z, Expires: 1970-01-01T00:00:00Z
[...]
```

The output lists the table name, the backup name, status, ARN, and the timestamps (creation, and expiry).

## SHOW LIMITS

**Purpose:** Shows limits for the connected user.

**Syntax:** SHOW LIMITS

Shows the limits and consumption of RCU and WCU for the connected user.

**Example**

```
us-east-1> show limits;
Region: us-east-1
Account Max (RCU, WCU): 580000, 580000
Table Max (RCU, WCU):  540000, 540000
Total Tables (RCU, WCU): 520, 830
Total GSI (RCU, WCU): 23, 35
Total (RCU, WCU): 543, 865
us-east-1>
```

## SHOW TABLES

**Purpose:** Lists tables.

**Syntax:** SHOW TABLES

Lists tables, and provides some basic information about each.

**Example**

```
us-east-1> show tables;
 dml_test | ACTIVE | PAY_PER_REQUEST | STANDARD | cd6257e9-5947-45d1-9b1c-2fbaf06d59e0 | arn:aws:dynamodb:us-east-1:3221123459165:table/dml_test | TTL DISABLED | GSI: 0 | LSI : 0 |
```

The output lists the table name, its current status, billing mode, table class, table-id, table-arn, TTL status, and counts of GSIs and LSIs.

## EXPLAIN

**Purpose:** Explain the API calls performed to execute a DDL or DML command.

**Syntax:** EXPLAIN <command>

The EXPLAIN command returns the API request that must be executed in
order to perform the command provided.

**Example**

```
us-east-1> explain create table sampletable ( pk number, rk string, zip number, ht number ) primary key ( pk hash, rk range) billing mode provisioned ( 20 wcu, 30 rcu) gsi ( gsi1 on (zip hash, ht range) projecting include ( rk ) billing mode provisioned (  10 rcu, 20 wcu )) stream ( new image ) table class standard infrequent access tags ( name : sampletable, type : demonstration );
CreateTable({
   "AttributeDefinitions":   [{
         "AttributeName":   "pk",
         "AttributeType":   "N"
      }, {
         "AttributeName":   "rk",
         "AttributeType":   "S"
      }, {
         "AttributeName":   "zip",
         "AttributeType":   "N"
      }, {
         "AttributeName":   "ht",
         "AttributeType":   "N"
      }],
   "TableName":   "sampletable",
   "KeySchema":   [{
         "AttributeName":   "pk",
         "KeyType":   "HASH"
      }, {
         "AttributeName":   "rk",
         "KeyType":   "RANGE"
      }],
   "GlobalSecondaryIndexes":   [{
         "IndexName":   "gsi1",
         "KeySchema":   [{
               "AttributeName":   "zip",
               "KeyType":   "HASH"
            }, {
               "AttributeName":   "ht",
               "KeyType":   "RANGE"
            }],
         "Projection":   {
            "ProjectionType":   "INCLUDE",
            "NonKeyAttributes":   ["rk"]
         },
         "ProvisionedThroughput":   {
            "ReadCapacityUnits":   10,
            "WriteCapacityUnits":   20
         }
      }],
   "BillingMode":   "PROVISIONED",
   "ProvisionedThroughput":   {
      "ReadCapacityUnits":   30,
      "WriteCapacityUnits":   20
   },
   "StreamSpecification":   {
      "StreamEnabled":   true,
      "StreamViewType":   "NEW_IMAGE"
   },
   "Tags":   [{
         "Key":   "name",
         "Value":   "sampletable"
      }, {
         "Key":   "type",
         "Value":   "demonstration"
      }],
   "TableClass":   "STANDARD_INFREQUENT_ACCESS"
})
CREATE
```

