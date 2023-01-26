# ddbsh

ddbsh is a simple CLI for DynamoDB modeled on isql, and the MySQL CLIs. ddbsh is open source. For more information about this, see the [Licensing](#Licensing) section below.

ddbsh presents the user with a simple command line interface. Here the user can enter SQL-like commands to DynamoDB. The output is presented in the same window. ddbsh supports many Data Definition Language ([DDL](#ddl)) and Data Manipulation Language ([DML](#dml)) commands.

# Installation

## Download the software

Clone the source code repository from github.

$ git clone https://github.com/awslabs/dynamodb-shell.git

## Build the software

### Pre-requisites

1. You need to get cmake version >= 3.21

2. You need a C++ compiler and make

3. You need the AWS SDK installed

### Building on Ubuntu

To get the latest version of cmake, follow instructions on kitware's website, or in [this](https://askubuntu.com/questions/355565/how-do-i-install-the-latest-version-of-cmake-from-the-command-line} answer.

To get the AWS SDK installed follow instructions [here](https://docs.aws.amazon.com/rekognition/latest/dg/setup-awscli-sdk.html). Detailed setup information can be found [here](https://docs.aws.amazon.com/sdk-for-cpp/v1/developer-guide/setup-linux.html).

### Building on Mac

To get the latest version of cmake, follow instructions [here](https://cmake.org/install/).

### Common build steps

Once all pre-requisites are installed and available, the build is performed with cmake.

```
% git clone git@github.com:awslabs/dynamodb-shell.git
Cloning into 'dynamodb-shell'...
[...]

% mkdir build

% cd build

% cmake ../ddbsh

% make
```

### More build steps

1. Building the SDK. I have found the following to work reliably.

```
% git clone https://github.com/aws/aws-sdk-cpp.git

% mkdir aws-sdk-build

% cd aws-sdk-cpp

% git submodule update --init --recursive

% cd ../aws-sdk-build

% cmake ../aws-sdk-cpp -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/usr/local/ \
        -DCMAKE_INSTALL_PREFIX=/usr/local/ \
        -DBUILD_ONLY="dynamodb" \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_TESTING=OFF \
        -DFORCE_SHARED_CRT=OFF

% make

% sudo make install

```

2. Building ddbsh. I have found teh following to work reliably

```
% git clone https://github.com/awslabs/dynamodb-shell.git

% cd dynamodb-shell

% mkdir build && cd build

% cmake ../ddbsh -DCMAKE_BUILD_TYPE=Release

% make

% sudo make install
```

### Running tests

Once you complete a build you can run the unit tests as shown below.

```
% make test
Running tests...
Test project /Users/amrithie/source/github/dynamodb-shell/build
        Start   1: ddbsh-basic-cli
[...]
```

# Getting Started

Running ddbsh is as simple as executing the ddbsh command. ddbsh is written in C++ and uses the DynamoDB API through the AWS DynamoDB SDK.

```
$ ./ddbsh
ddbsh - version 0.2
us-east-1> _
```

When you connect to ddbsh, you are automatically connected to an AWS region. In the illustration above, the default region was configured in the AWS configuration file ~/.aws/config

```
$ more ~/.aws/config
[default]
region=us-east-1
output=text
```

For more information visit [AWS configuration and credentials files](https://docs.aws.amazon.com/sdkref/latest/guide/file-format.html).

## Environment Variables

ddbsh respects the default AWS environment variables, see [How to set environment variables](https://docs.aws.amazon.com/sdkref/latest/guide/environment-variables.html).

# Interface

ddbsh exposes a readline based interface. The UP-ARROW and DOWN-ARROW keys allow you to view previous commands, and pick a previous command to reexecute.

Commands are terminated with the ';' character.

# Control Commands

## CONNECT

**Purpose:** Connect to a different region or endpoint

**Syntax:** CONNECT *region* [WITH ENDPOINT endpoint] ;

```
$ ./ddbsh
ddbsh - version 0.2
us-east-1> connect us-west-1;
CONNECT
us-west-1> 
```

The prompt shows the region name, and the additional (*) if you are connected to a non-standard endpoint.

## QUIT

**Purpose:** You have had a good day, and you want to do something else.

**Syntax:** QUIT

# Data Definition Language

## ALTER TABLE

**Purpose:** Make changes to a tables settings including billing mode, table class, streams, and TTL. Also create and drop Global Secondary Indexes (GSI).

**Syntax:** ALTER TABLE *table* [alter-table-options]

### SET billing\_mode\_and\_throughput 

Sets table and index billing mode and throughput. 

BILLING MODE PROVISIONED ( *NN* RCU, *MM* WCU )

Set the table to provisioned mode with the specified throughput.

BILLING MODE ON DEMAND

Set the table to on demand billing mode.

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
    [gsi_list] [lsi_list] [streams] [table_class] [tags];

    attribute_type := NUMBER|STRING|BINARY
    primary_key := PRIMARY KEY key_schema
    key_schema := ( attribute_name HASH [, attribute_name RANGE])\n
    billing_mode_and_throughput := (BILLING MODE ON DEMAND)|BILLING MODE provisioned)
    provisioned := ( RR RCU, WW WCU )\n
    gsi_list := GSI ( gsi_spec )
    gsi_spec := gsi [, gsi ...]
    gsi := gsi_name ON key_schema index_projection [billing_mode_and_throughput]
    index_projection := (PROJECTING ALL) | (PROJECTING KEYS ONLY) | (PROJECTING INCLUDE projection_list)
    projection_list := ( attribute [, attribute ...] )\n
    lsi_list := LSI ( lsi_spec )
    lsi_spec := lsi [, lsi ...]
    lsi := lsi_name ON key_schema index_projection\n
    streams := STREAM ( stream_type ) | STREAM DISABLED
    stream_type := KEYS ONLY | NEW IMAGE | OLD IMAGE | BOTH IMAGES\n
    table_class := TABLE CLASS STANDARD | TABLE CLASS STANDARD INFREQUENT ACCESS\n
    tags := TAGS ( tag [, tag ...] )
    tag := name : value
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
Table ARN: arn:aws:dynamodb:us-east-1:632195519165:table/ipt1
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
Table ARN: arn:aws:dynamodb:us-east-1:632195519165:table/cloneipt1
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

**Syntax:** DELETE FROM *table* \[WHERE *condition*\]

Deletes data from a table. When a WHERE clause is provided, items matching the condition are identified and deleted. If no WHERE clause is provided, all items are deleted.

DynamoDB only supports item at a time deletes (or batches which delete batches of items). Every item to be deleted must be identified by its primary key. Therefore, if the DELETE includes a where clause that completely specifies the primary key elements (hash key, and a range key if the table has one) then the item identified is deleted. If the WHERE clause does not completely specify the primary key, an appropriate SELECT is first executed to identify all items that match the WHERE clause, and then those items are deleted one at a time.

In standard SQL, a DELETE statement is an implicit transaction. In ddbsh, DELETE is NOT an implicit transaction. Each item is deleted individually. It is possible that an item is updated in the time between the SELECT and the subsequent DELETE. The item is only deleted if it matches the delete condition at the time when the DeleteItem() API is executed.

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

**Syntax:** INSERT INTO *table* ( <columns> ) VALUES <value>

Inserts an item into a table with the specified attribute values. All primary key attributes must be specified. If an item with the same primary key already exists, an error is generated.

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

**Syntax:** REPLACE INTO *table* ( <columns> ) VALUES <value>

Inserts or replaces an item with the specified primary key. All primary key attributes must be specified. If an item already exists with that primary key, it is replaced.

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

**Syntax:** SELECT \[CONSISTENT\] *|<columns> FROM *table*\[.*index*\] \[WHERE *condition*\]

Selects data from a table or index. If the optional *CONSISTENT* flag is specified, a consistent read is executed.

You can reference all scalar types (number, string and binary) as well as boolean, null, list and map in the projection list (columns) and in the where condition.

List elements are indexed with a 0 base. Map elements use '.' as the separator.

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

## UPDATE

**Purpose:** Update items in a table.

**Syntax:** UPDATE *table* SET *assignments* \[WHERE *condition*\]

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

**Syntax:** UPSERT *table* SET *assignments* \[WHERE *condition*\]

If an item is matched by the WHERE clause, it is updated. If one does not match the where clause then the *assignments* are converted are used to construct a new item.

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

# Contributing

If you find bugs or errors of any kind, please do let the maintainers know. If you are so inclined, please let the maintainers know that you would like to submit pull-requests. We will do our best to respond in a timely manner and where possible accept pull-requests and changes.

# Direction

Here are some things that need to be done:

1. Complete support for AM-CMK and CM-CMK. There is code in CREATE TABLE and ALTER TABLE to handle this but it has not been exhaustively tested (and that is the reason it does not show up in the HELP or this README). More work is required to ensure that it is performing as expected.

2. Update online help and README.

3. Add support for Global Tables. It is fairly clear what "wait" means on CREATE TABLE (with non Global Tables). There is (currently) no "wait" in ALTER TABLE which I see as problematic. I would like to implement both extensions for Global Tables in both CREATE and ALTER (also DROP) keeping "wait" semantics in mind.

4. Add support for additional language constructs. This is a delicate balancing act between attempting to build all of SQL against DynamoDB (which we don't want to do) and providing some more SQL constructs which will help users. For example, we do intend to add support for client side implementations of COUNT and COUNT DISTINCT. Once COUNT DISTINCT is implemented, it is a small matter of providing language support for GROUP BY which is something that I do want to do. However, I explicitly do not want to add support for client-side joins - they are not a construct that is natively supported by DynamoDB. This is, however, a matter of some heated discussions among the development team.

5. Add support for DynamoDB DAX.

6. Add more tests.

7. Distribute signed binaries.

# Licensing

ddbsh is licensed under the terms of the Apache Version 2.0 (January 2004) license. See the License file [here](LICENSE).

