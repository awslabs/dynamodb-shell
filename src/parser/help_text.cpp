// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// help_text.cpp — verbatim HELP usage strings (port of parser.y help_command).
//
// Each string below is the exact concatenation of the original's printf
// argument(s) for the corresponding HELP form, reproduced byte-for-byte so
// `help ...;` output matches `ddbsh-old` (tenet 1, D-B). Do NOT reflow or
// re-indent the string contents: the leading spaces and blank lines are part of
// the observable output. See src/parser/help_text.hpp.

#include "parser/help_text.hpp"

namespace ddbsh::engine {

namespace {

// HELP
constexpr std::string_view k_general =
    "HELP - provide help in ddbsh\n\n"
    "   HELP <keyword> [keyword [keyword ...]]\n\n"
    "      Provides help about the specified keyword, or statement.\n\n"
    "      HELP ALTER TABLE\n"
    "      HELP BACKUP\n"
    "      HELP BEGIN\n"
    "      HELP COMMIT\n"
    "      HELP CONNECT\n"
    "      HELP CREATE\n"
    "      HELP DELETE\n"
    "      HELP DESCRIBE BACKUP\n"
    "      HELP DESCRIBE\n"
    "      HELP DROP BACKUP\n"
    "      HELP DROP TABLE\n"
    "      HELP DROP\n"
    "      HELP EXPLAIN\n"
    "      HELP INSERT\n"
    "      HELP REPLACE\n"
    "      HELP RESTORE\n"
    "      HELP ROLLBACK\n"
    "      HELP SELECT\n"
    "      HELP SHOW BACKUPS\n"
    "      HELP SHOW CREATE TABLE\n"
    "      HELP SHOW LIMITS\n"
    "      HELP SHOW VERSION\n"
    "      HELP SHOW\n"
    "      HELP UPDATE\n"
    "      HELP UPSERT\n\n";

// HELP EXPLAIN
constexpr std::string_view k_explain =
    "EXPLAIN <dml_command>\n\n"
    "   EXPLAIN will accept any DML command (SELECT, INSERT, UPDATE, DELETE, REPLACE, UPSERT, ..) "
    "and "
    "provide\n"
    "   the caller with the DynamoDB API calls that must be performed to effectuate that SQL.\n\n";

// HELP BEGIN
constexpr std::string_view k_begin =
    "BEGIN\n\n"
    "   BEGIN is used to start a DynamoDB transaction. A group of DML commands can be executed as "
    "a "
    "single transaction.\n"
    "   For example:\n"
    "      BEGIN;\n"
    "      SELECT * FROM TBL1 where <primary key condition>;\n"
    "      SELECT * FROM TBL2 where <primary key condition>;\n"
    "      COMMIT;\n\n"
    "      BEGIN;\n"
    "      UPDATE ...;\n"
    "      DELETE ...;\n"
    "      COMMIT;\n"
    "   This will perform the update and the delete as a single transaction.\n\n"
    "   Transactions are committed with a COMMIT, and can be canceled with a ROLLBACK.\n\n";

// HELP COMMIT
constexpr std::string_view k_commit = "COMMIT - See BEGIN\n";

// HELP ROLLBACK
constexpr std::string_view k_rollback = "ROLLBACK - See BEGIN\n";

// HELP CONNECT
constexpr std::string_view k_connect =
    "CONNECT - Connects to a DynamoDB region.\n\n"
    "   CONNECT <region>\n"
    "   CONNECT <region> WITH ENDPOINT <endpoint>\n\n"
    "   Here, <endpoint> is the URI for an internal (zeta) endpoint.\n"
    "   CONNECT us-east-1;\n"
    "   CONNECT us-east-1 with endpoint \"https://...\"\n\n";

// HELP DROP
constexpr std::string_view k_drop =
    "DROP - Drops a table or backup.\n\n"
    "   See DROP TABLE, or DROP BACKUP\n\n";

// HELP DROP TABLE
constexpr std::string_view k_drop_table =
    "DROP TABLE - drops a DynamoDB table.\n\n"
    "   DROP TABLE [IF EXISTS] [NOWAIT] <table name>;\n\n"
    "   Drops a DynamoDB table. By default the operation blocks till the table is dropped, and "
    "will "
    "error\n"
    "   if the table does not exist. Use IF EXISTS and NO WAIT to change these behaviors.\n\n";

// HELP DESCRIBE
constexpr std::string_view k_describe =
    "DESCRIBE - Describe tables or backups.\n\n"
    "   See also DESCRIBE BACKUP\n"
    "   DESCRIBE provides metadata information about a table.\n\n";

// HELP SHOW
constexpr std::string_view k_show =
    "SHOW - Show tables or limits.\n\n"
    "   See SHOW TABLES or SHOW LIMITS.\n\n";

// HELP SHOW TABLES
constexpr std::string_view k_show_tables =
    "SHOW TABLES - Provide a list of tables\n\n"
    "   SHOW TABLES [BRIEF] [LIKE <template>]\n"
    "      The template is a double quoted regular expression.\n"
    "      For example: SHOW TABLES LIKE \"^bal.*\"\n\n";

// HELP CREATE
constexpr std::string_view k_create = "CREATE - Create a table. See CREATE TABLE\n";

// HELP CREATE TABLE
constexpr std::string_view k_create_table =
    "CREATE TABLE - Creates a DynamoDB table.\n\n"
    "   CREATE TABLE [IF NOT EXISTS][NOWAIT] <name>\n"
    "         ( attribute_name, attribute_type [,...] )\n"
    "   primary_key [billing_mode_and_throughput] [warm_throughput]\n"
    "   [gsi_list] [lsi_list] [vsi_list] [streams] [table_class] [tags] ;\n\n"
    "   attribute_type := NUMBER|STRING|BINARY\n"
    "   primary_key := PRIMARY KEY table_key_schema\n"
    "   table_key_schema := ( attribute_name HASH [, attribute_name RANGE] )\n"
    "      A base-table primary key (and an LSI) has one partition (HASH) key and\n"
    "      an optional single sort (RANGE) key; each role is a single attribute.\n"
    "   index_key_schema := ( key_role [, key_role] )\n"
    "   key_role := key_attribute HASH | key_attribute RANGE\n"
    "   key_attribute := attribute_name | ( attribute_name [, attribute_name ...] )\n"
    "      A GSI key_schema may use composite (multi-part) HASH and/or RANGE keys\n"
    "      (up to 4 attributes per role): a parenthesized, comma-separated group\n"
    "      of attribute names is one composite role. Composite keys are supported\n"
    "      on GSIs ONLY.\n\n"
    "   billing_mode_and_throughput := (BILLING MODE ON DEMAND [throughput])|BILLING MODE "
    "provisioned)\n"
    "   provisioned := ( RR RCU, WW WCU )\n\n"
    "   gsi_list := GSI ( gsi_spec )\n"
    "   gsi_spec := gsi [, gsi ...]\n"
    "   gsi := gsi_name ON index_key_schema index_projection "
    "[billing_mode_and_throughput][warm_throughput]\n"
    "      A GSI key_schema may use composite (multi-part) HASH and RANGE keys,\n"
    "      for example: GSI ( g ON ( (a, b, c) HASH, (d, e) RANGE ) PROJECTING ALL )\n"
    "   index_projection := (PROJECTING ALL) | (PROJECTING KEYS ONLY) | (PROJECTING INCLUDE "
    "projection_list)\n"
    "   projection_list := ( attribute [, attribute ...] )\n\n"
    "   lsi_list := LSI ( lsi_spec )\n"
    "   lsi_spec := lsi [, lsi ...]\n"
    "   lsi := lsi_name ON table_key_schema index_projection\n\n"
    "   vsi_list := (VSI | VECTOR | VECTOR INDEX) ( vsi_spec )\n"
    "   vsi_spec := vsi [, vsi ...]\n"
    "   vsi := vsi_name ON vector_attribute DIMENSIONS n DISTANCE distance_function\n"
    "          [SEARCH KEY ( search_element [, ...] )] index_projection\n"
    "      A vector index (VSI) enables real-time vector search. vector_attribute\n"
    "      is a list-of-number attribute and is NOT declared in the attribute\n"
    "      list. Query it with SELECT ... NEAREST (see HELP SELECT).\n"
    "   distance_function := COSINE | EUCLIDEAN | DOT_PRODUCT\n"
    "   search_element := attribute_name HASH | attribute_name FILTER\n"
    "      e.g. VSI ( v ON embedding DIMENSIONS 1536 DISTANCE COSINE\n"
    "                 SEARCH KEY ( tenant HASH, category FILTER ) PROJECTING ALL )\n\n"
    "   streams := STREAM ( stream_type ) | STREAM DISABLED\n"
    "   stream_type := KEYS ONLY | NEW IMAGE | OLD IMAGE | BOTH IMAGES\n\n"
    "   throughput := THROUGHPUT [( RR RCU, WW WCU ) | UNLIMITED]\n"
    "   warm_throughput := WARM THROUGHPUT ( RR RCU, WW WCU )\n"
    "   table_class := TABLE CLASS STANDARD | TABLE CLASS STANDARD INFREQUENT ACCESS\n"
    "   deletion_protection := DELETION PROTECTION [ENABLED|DISABLED]\n"
    "   tags := TAGS ( tag [, tag ...] )\n"
    "   tag := name : value\n\n";

// HELP ALTER TABLE
constexpr std::string_view k_alter_table =
    "ALTER TABLE - make changes to an existing table.\n\n"
    "   ALTER TABLE <name> SET billing_mode_and_throughput [warm_throughput]\n"
    "      billing_mode_and_throughput := (BILLING MODE ON DEMAND [throughput])|BILLING MODE "
    "provisioned)\n"
    "      provisioned := ( RR RCU, WW WCU )\n\n"
    "   ALTER TABLE <name> SET warm_throughput\n\n"
    "   ALTER TABLE <name> SET table_class\n"
    "      table_class := TABLE CLASS STANDARD | TABLE CLASS INFREQUENT ACCESS\n\n"
    "   ALTER TABLE <name> SET streams\n"
    "      streams := STREAM ( stream_type ) | STREAM DISABLED\n"
    "      stream_type := KEYS ONLY | NEW IMAGE | OLD IMAGE | BOTH IMAGES\n\n"
    "   ALTER TABLE <name> ( attribute_name attribute_type [, ...] ) CREATE GSI create_gsi_spec\n"
    "      create_gsi_spec := <name> ON index_key_schema index_projection "
    "[billing_mode_and_throughput]\n"
    "      index_key_schema := ( key_role [, key_role] )\n"
    "      key_role := key_attribute HASH | key_attribute RANGE\n"
    "      key_attribute := attribute_name | ( attribute_name [, attribute_name ...] )\n"
    "         A parenthesized group is a composite (multi-part) GSI HASH/RANGE key\n"
    "         (up to 4 attributes per role),\n"
    "         e.g. CREATE GSI g ON ( (a, b) HASH, (c, d) RANGE ) PROJECTING ALL\n"
    "   ALTER TABLE <name> DROP GSI <gsiname>\n\n"
    "   ALTER TABLE <name> [( attribute_name attribute_type [, ...] )] CREATE (VSI|VECTOR|VECTOR "
    "INDEX) vsi_spec\n"
    "      vsi_spec := <name> ON vector_attribute DIMENSIONS n DISTANCE distance_function\n"
    "                 [SEARCH KEY ( attr HASH|FILTER [, ...] )] index_projection\n"
    "      distance_function := COSINE | EUCLIDEAN | DOT_PRODUCT\n"
    "      The leading attribute list declares the SEARCH KEY / projected key\n"
    "      attributes; it (with its parentheses) may be omitted, or given empty\n"
    "      as ( ), for a vector index with no SEARCH KEY.\n"
    "      The 1.11.916 API supports create and drop only (no in-place update;\n"
    "      modify a vector index by dropping and recreating it).\n"
    "   ALTER TABLE <name> DROP (VSI|VECTOR|VECTOR INDEX) <vsiname>\n\n"
    "   ALTER TABLE <name> SET billing_mode_and_throughput UPDATE GSI ( gsi_update [, ...] )\n"
    "      gsi_update := <gsiname> SET billing_mode_and_throughput\n"
    "      gsi_update := <gsiname> SET warm_throughput\n\n"
    "   ALTER TABLE <name> UPDATE GSI ( gsi_update [, ...] )\n\n"
    "   ALTER TABLE <name> SET TTL DISABLED\n\n"
    "   ALTER TABLE <name> SET TTL ( attribute_name )\n\n"
    "   ALTER TABLE <name> SET PITR ENABLED\n\n"
    "   ALTER TABLE <name> SET PITR DISABLED\n\n"
    "   ALTER TABLE <name> ADD REPLICA <region> [replica_definition]\n"
    "   ALTER TABLE <name> UPDATE REPLICA <region> replica_definition\n"
    "   ALTER TABLE <name> DROP REPLICA <region>\n"
    "   ALTER TABLE <name> SET DELETION PROTECTION [ENABLED|DISABLED]\n"
    "   throughput := THROUGHPUT [( RR RCU, WW WCU ) | UNLIMITED]\n"
    "   warm_throughput := WARM THROUGHPUT ( RR RCU, WW WCU )\n";

// HELP SHOW LIMITS
constexpr std::string_view k_show_limits = "SHOW LIMITS -- Show user limit information.\n";

// HELP SHOW BACKUPS
constexpr std::string_view k_show_backups = "SHOW BACKUPS - SHOW [type] BACKUPS <tablename>\n\n";

// HELP DESCRIBE BACKUP
constexpr std::string_view k_describe_backup =
    "DESCRIBE BACKUP <arn> - Describe a backup given an ARN\n\n";

// HELP DROP BACKUP
constexpr std::string_view k_drop_backup = "DROP BACKUP <arn> - Drop a backup given an ARN\n\n";

// HELP BACKUP
constexpr std::string_view k_backup =
    "BACKUP TABLE <table> CALLED <name> - Create a backup of a table with a given name\n\n";

// HELP RESTORE
constexpr std::string_view k_restore =
    "RESTORE - Restore a backup\n\n"
    "   RESTORE TABLE <table> FROM BACKUP <arn> - Restore a table from a backup.\n\n"
    "   RESTORE TABLE <table> FROM <arn> TO PITR <timestamp> - Restore a table from a backup at a "
    "point in time.\n\n";

// HELP SELECT
constexpr std::string_view k_select =
    "SELECT - Select item(s) from a table.\n\n"
    "SELECT [CONSISTENT] attribute_list | * FROM <table>[.<index>] [WHERE where_clause]\n"
    "       [vector_search] [return_consumed_capacity|ratelimit]]\n\n"
    "   vector_search := NEAREST <topk> TO [ number [, number ...] ]\n"
    "      When <index> is a vector index (VSI), NEAREST routes the SELECT to a\n"
    "      SearchVectors call: <topk> is the number of nearest items to return\n"
    "      and the bracketed numeric list is the query vector (its length must\n"
    "      equal the index DIMENSIONS). Each result row is shown with a trailing\n"
    "      ` score <n>` (the similarity score for the match). Each row carries\n"
    "      the attributes named in the projection plus the score; the stored\n"
    "      vector is omitted by `SELECT *` but IS returned when named in the\n"
    "      projection (SELECT id, embedding FROM t.vix NEAREST ...), in the same\n"
    "      SearchVectors call.\n"
    "      A WHERE clause becomes the search filter (it may\n"
    "      reference only the vector index's HASH / FILTER search-key attributes).\n"
    "      e.g. SELECT * FROM docs.embidx WHERE tenant = \"acme\" NEAREST 5 TO [0.1, 0.2, 0.3]\n\n"
    "   where_clause := logical_element [AND|OR logical_element]\n"
    "   logical_element := [NOT] logical_expression\n"
    "   logical_element := ( logical_element )\n"
    "   logical_element := lhs <comparison op> rhs\n"
    "   logical_element := lhs BETWEEN bound AND bound\n"
    "   logical_element := lhs IN ( list )\n"
    "   logical_element := ATTRIBUTE_EXISTS (attribute)\n"
    "   logical_element := ATTRIBUTE_TYPE (attribute, type)\n"
    "   logical_element := BEGINS WITH (attribute, prefix)\n"
    "   logical_element := CONTAINS (attribute, substring)\n"
    "   logical_element := SIZE (attribute) <comparison op> NUMBER\n\n"
    "   attribute_list - any attribute in the table, be it a scalar, boolean, null, list or map\n\n"
    "   return_consumed_capacity := RETURN CONSUMED CAPACITY INDEXES | RETURN CONSUMED CAPACITY "
    "TOTAL\n\n"
    "   ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |\n"
    "                WITH RATELIMIT ( RR RCU ) |\n"
    "                WITH RATELIMIT ( WW WCU )\n\n"
    "       NOTES: 1. The WHERE clause for a NULL field uses the SQL syntax of <field> IS [NOT] "
    "NULL\n"
    "                 The equality operator cannot be used with NULL.\n"
    "              2. To reference list elements in projection or where clause uses 0 based "
    "indexing.\n"
    "              3. You can reference maps (dictionaries) in projection and where clause.\n"
    "              4. You can rate limit a select by specifying the optional ratelimit parameter.\n"
    "              5. You cannot specify both a rate limit and a return clause.\n\n";

// HELP INSERT
constexpr std::string_view k_insert =
    "INSERT INTO <table> ( column_list ) VALUES (values) [, ...] [ratelimit]\n"
    "ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |\n"
    "             WITH RATELIMIT ( RR RCU ) |\n"
    "             WITH RATELIMIT ( WW WCU )\n\n";

// HELP REPLACE
constexpr std::string_view k_replace =
    "REPLACE - Replace items in a table\n\n"
    "REPLACE INTO <table> ( column [, column ...] ) VALUES ( values ) [ratelimit] [return_values]\n"
    "ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |\n"
    "             WITH RATELIMIT ( RR RCU ) |\n"
    "             WITH RATELIMIT ( WW WCU )\n\n"
    "return_values: RETURN VALUES [ALL OLD|ALL NEW|UPDATED OLD|UPDATED NEW]\n\n";

// HELP UPDATE
constexpr std::string_view k_update =
    "UPDATE - Update items in a table\n\n"
    "   UPDATE <name> SET <update_set> [where clause] [ratelimit][return_values]\n\n"
    "   UPDATE <name> REMOVE <remove list> [where clause] [ratelimit][return_values]\n\n"
    "ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |\n"
    "             WITH RATELIMIT ( RR RCU ) |\n"
    "             WITH RATELIMIT ( WW WCU )\n\n"
    "return_values: RETURN VALUES [ALL OLD|ALL NEW|UPDATED OLD|UPDATED NEW]\n\n";

// HELP UPSERT
constexpr std::string_view k_upsert =
    "UPSERT - Upsert items in a table\n\n"
    "   UPSERT <name> SET <upsert_set> [where clause] [ratelimit][return_values]\n\n"
    "ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |\n"
    "             WITH RATELIMIT ( RR RCU ) |\n"
    "             WITH RATELIMIT ( WW WCU )\n\n"
    "return_values: RETURN VALUES [ALL OLD|ALL NEW|UPDATED OLD|UPDATED NEW]\n\n";

// HELP DELETE
constexpr std::string_view k_delete =
    "DELETE - Delete items from a table\n\n"
    "   DELETE FROM <table> [where clause] [ratelimit] [return_values]\n\n"
    "ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |\n"
    "             WITH RATELIMIT ( RR RCU ) |\n"
    "             WITH RATELIMIT ( WW WCU )\n\n"
    "return_values: RETURN VALUES [ALL OLD|ALL NEW|UPDATED OLD|UPDATED NEW]\n\n";

// HELP SHOW VERSION
constexpr std::string_view k_show_version =
    "SHOW VERSION\n\n   Show the version of DynamoDB Shell.\n\n";

// HELP SHOW CREATE TABLE
constexpr std::string_view k_show_create_table =
    "SHOW CREATE TABLE [IF NOT EXISTS][NOWAIT] <table>\n\n   Show the CREATE TABLE command to "
    "create "
    "the table.\n\n";

}  // namespace

std::string_view help_text(HelpTopic topic) {
    switch (topic) {
        case HelpTopic::general:
            return k_general;
        case HelpTopic::explain:
            return k_explain;
        case HelpTopic::begin:
            return k_begin;
        case HelpTopic::commit:
            return k_commit;
        case HelpTopic::rollback:
            return k_rollback;
        case HelpTopic::connect:
            return k_connect;
        case HelpTopic::drop:
            return k_drop;
        case HelpTopic::drop_table:
            return k_drop_table;
        case HelpTopic::describe:
            return k_describe;
        case HelpTopic::show:
            return k_show;
        case HelpTopic::show_tables:
            return k_show_tables;
        case HelpTopic::create:
            return k_create;
        case HelpTopic::create_table:
            return k_create_table;
        case HelpTopic::alter_table:
            return k_alter_table;
        case HelpTopic::show_limits:
            return k_show_limits;
        case HelpTopic::show_backups:
            return k_show_backups;
        case HelpTopic::describe_backup:
            return k_describe_backup;
        case HelpTopic::drop_backup:
            return k_drop_backup;
        case HelpTopic::backup:
            return k_backup;
        case HelpTopic::restore:
            return k_restore;
        case HelpTopic::select_statement:
            return k_select;
        case HelpTopic::insert:
            return k_insert;
        case HelpTopic::replace:
            return k_replace;
        case HelpTopic::update:
            return k_update;
        case HelpTopic::upsert:
            return k_upsert;
        case HelpTopic::del:
            return k_delete;
        case HelpTopic::show_version:
            return k_show_version;
        case HelpTopic::show_create_table:
            return k_show_create_table;
    }
    return k_general;
}

}  // namespace ddbsh::engine
