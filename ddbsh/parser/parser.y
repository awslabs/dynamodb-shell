%{

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tuple>
#include "parser-defs.hpp"
#include "unquote.hpp"
#include "ddbsh.hpp"
#include "logging.hpp"
#include "command.hpp"
#include "connect.hpp"
#include "quit.hpp"
#include "show_tables.hpp"
#include "drop_table.hpp"
#include "create_table.hpp"
#include "describe.hpp"
#include "update_table.hpp"
#include "update_table_ttl.hpp"
#include "show_limits.hpp"
#include "create_backup.hpp"
#include "drop_backup.hpp"
#include "show_backups.hpp"
#include "describe_backup.hpp"
#include "restore_backup.hpp"
#include "restore_pitr.hpp"
#include "select.hpp"
#include "insert.hpp"
#include "update.hpp"
#include "delete.hpp"
#include "pitr.hpp"
#include "transact-read.hpp"
#include "transact-write.hpp"
#include "update_table_replica.hpp"
#include "allocation.hpp"

using namespace ddbsh;

extern int yylex(void);
void yyerror(CCommand **, const char* s);

extern int yylineno, yycolumnno;
int yydebug = 1;

%}

%union {
    Aws::DynamoDB::Model::AttributeDefinition * attribute_name_type;
    Aws::DynamoDB::Model::AttributeValue * attribute_value;
    Aws::DynamoDB::Model::AttributeValue * map_element_list;
    Aws::DynamoDB::Model::CreateGlobalSecondaryIndexAction * create_gsi_action;
    Aws::DynamoDB::Model::DeleteGlobalSecondaryIndexAction * delete_gsi_action;
    Aws::DynamoDB::Model::GlobalSecondaryIndex * gsi;
    Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate * global_secondary_index_update_element;
    Aws::DynamoDB::Model::LocalSecondaryIndex * lsi;
    Aws::DynamoDB::Model::Projection * index_projection;
    Aws::DynamoDB::Model::ProvisionedThroughputOverride * provisioned_throughput_override;
    Aws::DynamoDB::Model::ReplicaGlobalSecondaryIndex * replica_gsi_specification_list_entry;
    Aws::DynamoDB::Model::ReturnConsumedCapacity returns;
    Aws::DynamoDB::Model::SSESpecification * sse_specification;
    Aws::DynamoDB::Model::ScalarAttributeType attribute_type;
    Aws::DynamoDB::Model::StreamSpecification * stream_specification;
    Aws::DynamoDB::Model::StreamViewType stream_view_type;
    Aws::DynamoDB::Model::TableClass table_class;
    Aws::DynamoDB::Model::Tag * tag;
    Aws::DynamoDB::Model::UpdateGlobalSecondaryIndexAction * update_gsi_action;
    Aws::DynamoDB::Model::UpdateTableRequest * update_table_request;
    Aws::String * lee_lhs;
    Aws::String * select_attribute;
    Aws::Vector<Aws::DynamoDB::Model::AttributeDefinition> * attribute_definition;
    Aws::Vector<Aws::DynamoDB::Model::AttributeDefinition> * attribute_name_type_list;
    Aws::Vector<Aws::DynamoDB::Model::AttributeValue> * insert_values;
    Aws::Vector<Aws::DynamoDB::Model::AttributeValue> * lee_rhs_list;
    Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndex> * gsi_list;
    Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate> * global_secondary_index_update;
    Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> * key_schema;
    Aws::Vector<Aws::DynamoDB::Model::LocalSecondaryIndex> * lsi_list;
    Aws::Vector<Aws::DynamoDB::Model::ReplicaGlobalSecondaryIndex> * replica_gsi_specification;
    Aws::Vector<Aws::DynamoDB::Model::Tag> * tags;
    Aws::Vector<Aws::String> * index_projection_list;
    Aws::Vector<Aws::String> * insert_column_list;
    Aws::Vector<Aws::String> * select_projection;
    Aws::Vector<Aws::String> * table_optional_index_name;
    Aws::Vector<Aws::String> * update_remove_list;
    Aws::Vector<Aws::Vector<Aws::DynamoDB::Model::AttributeValue>> * value_list;
    Aws::Vector<CUpdateSetElement> * update_set;
    CLogicalExpression * logical_expression;
    CRateLimit * ratelimit;
    CUpdateSetElement * update_set_element;
    CWhere * where;
    billing_mode_and_throughput_t * billing_mode_and_throughput;
    bool boolval;
    char * strval;
    ddbsh::CCommand * command;
};

%parse-param {CCommand ** pCommand}

%token <strval> K_ACCESS
%token <strval> K_ADD
%token <strval> K_AES256
%token <strval> K_ALL
%token <strval> K_ALTER
%token <strval> K_AND
%token <strval> K_AS
%token <strval> K_ATTRIBUTE_EXISTS
%token <strval> K_ATTRIBUTE_NOT_EXISTS
%token <strval> K_ATTRIBUTE_TYPE
%token <strval> K_ATTRIBUTE_TYPE_BINARY
%token <strval> K_ATTRIBUTE_TYPE_NUMBER
%token <strval> K_ATTRIBUTE_TYPE_STRING
%token <strval> K_BACKUP
%token <strval> K_BACKUPS
%token <strval> K_BEGIN
%token <strval> K_BEGINS_WITH
%token <strval> K_BETWEEN
%token <strval> K_BILLING
%token <strval> K_BOTH
%token <strval> K_BRIEF
%token <strval> K_CALLED
%token <strval> K_CHECK
%token <strval> K_CLASS
%token <strval> K_COMMIT
%token <strval> K_CONNECT
%token <strval> K_CONSISTENT
%token <strval> K_CONTAINS
%token <strval> K_CREATE
%token <strval> K_DELETE
%token <strval> K_DELETION
%token <strval> K_DEMAND
%token <strval> K_DESCRIBE
%token <strval> K_DISABLED
%token <strval> K_DROP
%token <strval> K_ENABLED
%token <strval> K_ENDPOINT
%token <strval> K_EXISTS
%token <strval> K_EXPLAIN
%token <strval> K_FALSE
%token <strval> K_FROM
%token <strval> K_GLOBAL
%token <strval> K_GSI
%token <strval> K_HASH
%token <strval> K_HELP
%token <strval> K_IF
%token <strval> K_IF_NOT_EXISTS
%token <strval> K_IMAGE
%token <strval> K_IMAGES
%token <strval> K_IN
%token <strval> K_INCLUDE
%token <strval> K_INDEX
%token <strval> K_INDEXES
%token <strval> K_INFREQUENT
%token <strval> K_INSERT
%token <strval> K_INTO
%token <strval> K_IS
%token <strval> K_KEY
%token <strval> K_KEYID
%token <strval> K_KEYS
%token <strval> K_KMS
%token <strval> K_LIKE
%token <strval> K_LIMIT
%token <strval> K_LIMITS
%token <strval> K_LSI
%token <strval> K_MODE
%token <strval> K_NEW
%token <strval> K_NOT
%token <strval> K_NOWAIT
%token <strval> K_NULL
%token <strval> K_NUMBER
%token <strval> K_OLD
%token <strval> K_ON
%token <strval> K_ONLY
%token <strval> K_OR
%token <strval> K_PITR
%token <strval> K_PRIMARY
%token <strval> K_PROJECTING
%token <strval> K_PROTECTION
%token <strval> K_PROVISIONED
%token <strval> K_QUIT
%token <strval> K_RANGE
%token <strval> K_RATELIMIT
%token <strval> K_RCU
%token <strval> K_REMOVE
%token <strval> K_REPLACE
%token <strval> K_REPLICA
%token <strval> K_RESTORE
%token <strval> K_RETURN
%token <strval> K_ROLLBACK
%token <strval> K_SELECT
%token <strval> K_SET
%token <strval> K_SHOW
%token <strval> K_SIZE
%token <strval> K_SSE
%token <strval> K_STANDARD
%token <strval> K_STREAM
%token <strval> K_TABLE
%token <strval> K_TABLES
%token <strval> K_TAGS
%token <strval> K_TO
%token <strval> K_TOTAL
%token <strval> K_TRUE
%token <strval> K_TTL
%token <strval> K_UPDATE
%token <strval> K_UPSERT
%token <strval> K_VALUES
%token <strval> K_WCU
%token <strval> K_WHERE
%token <strval> K_WITH

%token <strval> T_DQUOTED_STRING
%token <strval> T_EQUAL
%token <strval> T_FLOATING_POINT_NUMBER
%token <strval> T_GREATER_THAN
%token <strval> T_GREATER_THAN_OR_EQUAL
%token <strval> T_LESS_THAN
%token <strval> T_LESS_THAN_OR_EQUAL
%token <strval> T_NOT_EQUAL
%token <strval> T_TIMESTAMP
%token <strval> T_UNQUOTED_STRING
%token <strval> T_WHOLE_NUMBER

%type <command> alter_table_command
%type <command> command
%type <command> connect_command
%type <command> control_command
%type <command> create_backup_command
%type <command> create_table_command
%type <command> ddl_command
%type <command> delete_command
%type <command> describe_backup_command
%type <command> describe_command
%type <command> dml_command
%type <command> drop_backup_command
%type <command> drop_table_command
%type <command> errors
%type <command> exists_command
%type <command> explain_command
%type <command> get_command
%type <command> help_command
%type <command> insert_command
%type <command> quit_command
%type <command> replace_command
%type <command> restore_backup_command
%type <command> select_command
%type <command> show_backups_command
%type <command> show_limits_command
%type <command> show_tables_command
%type <command> simple_command
%type <command> transact_command
%type <command> transact_get_command
%type <command> transact_write_command
%type <command> update_command
%type <command> upsert_command
%type <command> write_command

%type <attribute_name_type> attribute_name_type
%type <attribute_name_type_list> attribute_name_type_list
%type <attribute_type> attribute_type
%type <attribute_value> insert_value
%type <attribute_value> lee_rhs
%type <billing_mode_and_throughput> billing_mode_and_throughput
%type <billing_mode_and_throughput> optional_billing_mode_and_throughput
%type <boolval> brief_clause
%type <boolval> if_exists_clause
%type <boolval> if_not_exists_clause
%type <boolval> nowait_clause
%type <boolval> optional_consistent
%type <boolval> optional_deletion_protection
%type <boolval> deletion_protection
%type <global_secondary_index_update> alter_table_gsi_update
%type <global_secondary_index_update_element> alter_table_gsi_create
%type <global_secondary_index_update_element> alter_table_gsi_drop
%type <global_secondary_index_update_element> alter_table_gsi_update_element
%type <gsi> gsi
%type <gsi_list> gsi_list
%type <gsi_list> gsi_specification
%type <index_projection> index_projection
%type <index_projection_list> index_projection_list
%type <insert_column_list> insert_column_list
%type <insert_values> insert_values
%type <key_schema> key_schema
%type <key_schema> primary_key
%type <lee_lhs> lee_lhs
%type <lee_rhs_list> lee_rhs_list
%type <logical_expression> logical_expression
%type <logical_expression> logical_expression_element
%type <lsi> lsi
%type <lsi_list> lsi_list
%type <lsi_list> lsi_specification
%type <map_element_list> map_element_list
%type <provisioned_throughput_override> optional_provisioned_throughput_override
%type <provisioned_throughput_override> provisioned_throughput_override
%type <ratelimit> optional_ratelimit
%type <ratelimit> ratelimit
%type <replica_gsi_specification> optional_replica_gsi_specification
%type <replica_gsi_specification> replica_gsi_specification_list
%type <replica_gsi_specification_list_entry> replica_gsi_specification_list_entry
%type <returns> optional_return_clause
%type <select_attribute> select_attribute
%type <select_projection> select_attribute_list
%type <select_projection> select_projection
%type <sse_specification> optional_sse_specification
%type <sse_specification> sse_specification
%type <stream_specification> optional_stream_specification
%type <stream_specification> stream_specification
%type <stream_view_type> stream_view_type
%type <strval> attribute_name
%type <strval> comparison_op
%type <strval> connect_endpoint
%type <strval> insert_column
%type <strval> like_clause
%type <strval> optional_backup_type
%type <strval> optional_table_name
%type <strval> region_name
%type <strval> string
%type <strval> table_name
%type <table_class> optional_table_class
%type <table_class> table_class
%type <table_optional_index_name> table_optional_index_name
%type <tag> table_tag
%type <tags> optional_table_tags
%type <tags> table_tags
%type <tags> table_tags_list
%type <update_remove_list> update_remove_list
%type <update_set> update_set
%type <update_set_element> update_set_element
%type <update_set_element> use_lhs
%type <value_list> value_list
%type <where> optional_where_clause
%type <where> where_clause

%left K_OR
%left K_AND
%left K_NOT
%left K_BETWEEN
%left '.' '[' ']'

%%

command: simple_command
{
    logdebug("[%s, %d] command: simple_command\n", __FILENAME__, __LINE__);
    *pCommand = $1;
    YYACCEPT;
} | transact_command
{
    logdebug("[%s, %d] command: transact_command\n", __FILENAME__, __LINE__);
    *pCommand = $1;
    YYACCEPT;
} | help_command
{
    logdebug("[%s, %d] command: help_command\n", __FILENAME__, __LINE__);
    *pCommand = NULL;
    YYACCEPT;
} | ';'
{
    logdebug("[%s, %d] command: EMPTY\n", __FILENAME__, __LINE__);
    *pCommand = NULL;
    YYACCEPT;
} |
{
    logdebug("[%s, %d] command: EMPTY\n", __FILENAME__, __LINE__);
    *pCommand = NULL;
    YYACCEPT;
} | errors ';'
{
    logdebug("[%s, %d] command: error\n", __FILENAME__, __LINE__);
    yyerrok;
    *pCommand = NULL;
    YYACCEPT;
};

errors: error
{
    logdebug("[%s, %d] command: errors: error\n", __FILENAME__, __LINE__);
    $$ = NULL;
} | errors error
{
    logdebug("[%s, %d] command: errors: errors error\n", __FILENAME__, __LINE__);
    $$ = NULL;
};

simple_command: ddl_command
{
    logdebug("[%s, %d] simple_command: ddl_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | control_command
{
    logdebug("[%s, %d] simple_command: control_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | dml_command
{
    logdebug("[%s, %d] simple_command: dml_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | explain_command
{
    logdebug("[%s, %d] simple_command: explain_command\n", __FILENAME__, __LINE__);
    $$ = $1;
};

help_command: K_HELP ';'
{
    printf("HELP - provide help in ddbsh\n\n" \
           "   HELP <keyword> [keyword [keyword ...]]\n\n" \
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
           "      HELP SHOW LIMITS\n"
           "      HELP SHOW\n"
           "      HELP UPDATE\n"
           "      HELP UPSERT\n\n");
    $$ = NULL;
};

help_command: K_HELP K_EXPLAIN ';'
{
    printf("EXPLAIN <dml_command>\n\n" \
           "   EXPLAIN will accept any DML command (SELECT, INSERT, UPDATE, DELETE, REPLACE, UPSERT, ..) and provide\n"
           "   the caller with the DynamoDB API calls that must be performed to effectuate that SQL.\n\n");
    $$ = NULL;
};

explain_command: K_EXPLAIN ddl_command
{
    logdebug("[%s, %d] explain_commnd: K_EXPLAIN ddl_command\n", __FILENAME__, __LINE__);
    $2->explain();
    $$ = $2;
} | K_EXPLAIN dml_command
{
    logdebug("[%s, %d] explain_commnd: K_EXPLAIN ddl_command\n", __FILENAME__, __LINE__);
    $2->explain();
    $$ = $2;
} | K_EXPLAIN transact_command
{
    logdebug("[%s, %d] explain_commnd: K_EXPLAIN transact_command\n", __FILENAME__, __LINE__);
    $2->explain();
    $$ = $2;
};

help_command: K_HELP K_BEGIN ';'
{
    printf("BEGIN\n\n"
           "   BEGIN is used to start a DynamoDB transaction. A group of DML commands can be executed as a single transaction.\n"
           "   For example:\n"
           "      BEGIN;\n"
           "      SELECT * FROM TBL1 where <primary key condition>;\n"
           "      SELECT * FROM TBL2 where <primary key condition>;\n"
           "      COMMIT;\n\n");
    printf("      BEGIN;\n"
           "      UPDATE ...;\n"
           "      DELETE ...;\n"
           "      COMMIT;\n"
           "   This will perform the update and the delete as a single transaction.\n\n");

    printf("   Transactions are committed with a COMMIT, and can be canceled with a ROLLBACK.\n\n");
    $$ = NULL;
};

help_command: K_HELP K_COMMIT ';'
{
    printf("COMMIT - See BEGIN\n");
};

help_command: K_HELP K_ROLLBACK ';'
{
    printf("ROLLBACK - See BEGIN\n");
};

transact_command: K_BEGIN ';' transact_get_command K_COMMIT ';'
{
    logdebug("[%s, %d] transact_command: K_BEGIN ';' transact_get_command K_COMMIT ';'\n", __FILENAME__, __LINE__);
    ((CTransactReadCommand *)$3)->commit();
    $$ = $3;
} | K_BEGIN ';' transact_write_command K_COMMIT ';'
{
    logdebug("[%s, %d] transact_command: K_BEGIN ';' transact_write_command K_COMMIT ';'\n", __FILENAME__, __LINE__);
    ((CTransactWriteCommand *)$3)->commit();
    $$ = $3;
} | K_BEGIN ';' transact_get_command K_ROLLBACK ';'
{
    logdebug("[%s, %d] transact_command: K_BEGIN ';' transact_get_command K_ROLLBACK ';'\n", __FILENAME__, __LINE__);
    ((CTransactReadCommand *)$3)->rollback();
    $$ = $3;
} | K_BEGIN ';' transact_write_command K_ROLLBACK ';'
{
    logdebug("[%s, %d] transact_command: K_BEGIN ';' transact_write_command K_ROLLBACK ';'\n", __FILENAME__, __LINE__);
    ((CTransactWriteCommand *)$3)->rollback();
    $$ = $3;
} | K_BEGIN ';' K_ROLLBACK ';'
{
    logdebug("[%s, %d] transact_command: K_BEGIN ';' K_ROLLBACK ';'\n", __FILENAME__, __LINE__);
    $$ = NULL;
};

transact_get_command: get_command
{
    logdebug("[%s, %d] transact_command: transact_get_command: get_command\n", __FILENAME__, __LINE__);
    CTransactReadCommand * rc = NEW CTransactReadCommand($1);
    $$ = rc;
} | transact_get_command get_command
{
    logdebug("[%s, %d] transact_command: transact_get_command get_command\n", __FILENAME__, __LINE__);
    ((CTransactReadCommand *)$1)->append($2);
    $$ = $1;
};

transact_write_command: write_command
{
    logdebug("[%s, %d] transact_command: transact_write_command: write_command\n", __FILENAME__, __LINE__);
    CTransactWriteCommand * wc = NEW CTransactWriteCommand($1);
    $$ = wc;
} | transact_write_command write_command
{
    logdebug("[%s, %d] transact_command: transact_write_command write_command\n", __FILENAME__, __LINE__);
    ((CTransactWriteCommand *)$1)->append($2);
    $$ = $1;
};

dml_command: get_command
{
    logdebug("[%s, %d] dml_command: get_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | write_command
{
    logdebug("[%s, %d] dml_command: write_command\n", __FILENAME__, __LINE__);
    $$ = $1;
};

get_command: select_command
{
    logdebug("[%s, %d] get_command: select_command\n", __FILENAME__, __LINE__);
    $$ = $1;
};

write_command: insert_command
{
    logdebug("[%s, %d] write_command: insert_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | replace_command
{
    logdebug("[%s, %d] write_command: replace_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | update_command
{
    logdebug("[%s, %d] write_command: update_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | upsert_command
{
    logdebug("[%s, %d] write_command: upsert_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | delete_command
{
    logdebug("[%s, %d] write_command: delete_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | exists_command
{
    logdebug("[%s, %d] write_command: exists_command\n", __FILENAME__, __LINE__);
    $$ = $1;
};

ddl_command: drop_table_command
{
    logdebug("[%s, %d] ddl_command: drop_table_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | show_tables_command
{
    logdebug("[%s, %d] ddl_command: show_tables_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | create_table_command
{
    logdebug("[%s, %d] ddl_command: create_table_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | describe_command
{
    logdebug("[%s, %d] ddl_command: describe_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | alter_table_command
{
    logdebug("[%s, %d] ddl_command: alter_table_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | show_limits_command
{
    logdebug("[%s, %d] ddl_command: show_limits_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | show_backups_command
{
    logdebug("[%s, %d] ddl_command: show_backups_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | describe_backup_command
{
    logdebug("[%s, %d] ddl_command: describe_backup_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | create_backup_command
{
    logdebug("[%s, %d] ddl_command: create_backup_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | drop_backup_command
{
    logdebug("[%s, %d] ddl_command: drop_backup_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | restore_backup_command
{
    logdebug("[%s, %d] ddl_command: restore_backup_command\n", __FILENAME__, __LINE__);
    $$ = $1;
};

control_command: connect_command
{
    logdebug("[%s, %d] control_command: connect_command\n", __FILENAME__, __LINE__);
    $$ = $1;
} | quit_command
{
    logdebug("[%s, %d] control_command: quit_command\n", __FILENAME__, __LINE__);
    $$ = $1;
};

quit_command: K_QUIT
{
    logdebug("[%s, %d] quit_command: K_QUIT\n", __FILENAME__, __LINE__);
    CQuitCommand *quit = NEW CQuitCommand();
    $$ = quit;
};

help_command: K_HELP K_CONNECT ';'
{
    printf("CONNECT - Connects to a DynamoDB region.\n\n"
           "   CONNECT <region>\n"
           "   CONNECT <region> WITH ENDPOINT <endpoint>\n\n"
           "   Here, <endpoint> is the URI for an internal (zeta) endpoint.\n"
           "   CONNECT us-east-1;\n"
           "   CONNECT us-east-1 with endpoint \"https://...\"\n\n");

    $$ = NULL;
};

connect_command: K_CONNECT region_name connect_endpoint ';'
{
    logdebug("[%s, %d] connect_command: K_CONNECT region_name "
             "connect_endpoint ';'\n", __FILENAME__, __LINE__);
    CConnectCommand *connect = NEW CConnectCommand($2, $3);

    FREE($2);
    FREE($3);
    $$ = connect;
};

help_command: K_HELP K_DROP ';'
{
    printf("DROP - Drops a table or backup.\n\n"
           "   See DROP TABLE, or DROP BACKUP\n\n");
    $$ = NULL;
};

help_command: K_HELP K_DROP K_TABLE ';'
{
    printf("DROP TABLE - drops a DynamoDB table.\n\n"
           "   DROP TABLE [IF EXISTS] [NOWAIT] <table name>;\n\n"
           "   Drops a DynamoDB table. By default the operation blocks till the table is dropped, and will error\n"
           "   if the table does not exist. Use IF EXISTS and NO WAIT to change these behaviors.\n\n" );
    $$ = NULL;
};

drop_table_command: K_DROP K_TABLE if_exists_clause nowait_clause string ';'
{
    logdebug("[%s, %d] drop_table_command: K_DROP K_TABLE if_exists_clause nowait_clause string ';'\n", __FILENAME__, __LINE__);

    CDropTableCommand *dt = NEW CDropTableCommand($5, $3, $4);

    $$ = dt;
    FREE($5);
};

help_command: K_HELP K_DESCRIBE ';'
{
    printf("DESCRIBE - Describe tables or backups.\n\n"
           "   See also DESCRIBE BACKUP\n"
           "   DESCRIBE provides metadata information about a table.\n\n");

    $$ = NULL;
};

describe_command: K_DESCRIBE string ';'
{
    logdebug("[%s, %d] describe_command: K_DESCRIBE string ';'\n", __FILENAME__, __LINE__);

    CDescribeCommand *describe = NEW CDescribeCommand($2);

    $$ = describe;
    FREE($2);
};

like_clause: K_LIKE string
{
    logdebug ("[%s, %d] like_clause %s\n", __FILENAME__, __LINE__, $2);
    $$ = $2;
} |
{
    logdebug ("[%s, %d] like_clause empty\n", __FILENAME__, __LINE__);
    $$ = STRDUP((char *)"");
};

brief_clause: K_BRIEF
{
    logdebug ("[%s, %d] brief_clause: true\n", __FILENAME__, __LINE__);
    $$ = true;
} |
{
    logdebug ("[%s, %d] brief_clause: false\n", __FILENAME__, __LINE__);
    $$ = false;
};

help_command: K_HELP K_SHOW ';'
{
    printf("SHOW - Show tables or limits.\n\n"
           "   See SHOW TABLES or SHOW LIMITS.\n\n");
    $$ = NULL;
} | K_HELP K_SHOW K_TABLES ';'
{
    printf("SHOW TABLES - Provide a list of tables\n\n"
           "   SHOW TABLES [BRIEF] [LIKE <template>]\n"
           "      The template is a double quoted regular expression.\n"
           "      For example: SHOW TABLES LIKE \"^bal.*\"\n\n");

    $$ = NULL;
};

show_tables_command: K_SHOW K_TABLES brief_clause like_clause ';'
{
    logdebug("[%s, %d] show_tables_command: K_SHOW K_TABLES ';'\n",
             __FILENAME__, __LINE__ );

    CShowTablesCommand * st = NEW CShowTablesCommand($3, $4);

    $$ = st;
    FREE($4);
};

help_command: K_HELP K_CREATE ';'
{
    printf("CREATE - Create a table. See CREATE TABLE\n");
    $$ = NULL;
} | K_HELP K_CREATE K_TABLE ';'
{
    printf("CREATE TABLE - Creates a DynamoDB table.\n\n");
    printf("   CREATE TABLE [IF NOT EXISTS][NOWAIT] <name>\n"
           "         ( attribute_name, attribute_type [,...] )\n"
           "   primary_key billing_mode_and_throughput\n"
           "   [gsi_list] [lsi_list] [streams] [table_class] [tags] ;\n\n"

           "   attribute_type := NUMBER|STRING|BINARY\n"
           "   primary_key := PRIMARY KEY key_schema\n"
           "   key_schema := ( attribute_name HASH [, attribute_name RANGE])\n\n"
           "   billing_mode_and_throughput := (BILLING MODE ON DEMAND)|BILLING MODE provisioned)\n"
           "   provisioned := ( RR RCU, WW WCU )\n\n"
           "   gsi_list := GSI ( gsi_spec )\n"
           "   gsi_spec := gsi [, gsi ...]\n"
           "   gsi := gsi_name ON key_schema index_projection [billing_mode_and_throughput]\n"
           "   index_projection := (PROJECTING ALL) | (PROJECTING KEYS ONLY) | (PROJECTING INCLUDE projection_list)\n"
           "   projection_list := ( attribute [, attribute ...] )\n\n"
           "   lsi_list := LSI ( lsi_spec )\n"
           "   lsi_spec := lsi [, lsi ...]\n"
           "   lsi := lsi_name ON key_schema index_projection\n\n"
           "   streams := STREAM ( stream_type ) | STREAM DISABLED\n"
           "   stream_type := KEYS ONLY | NEW IMAGE | OLD IMAGE | BOTH IMAGES\n\n"
           "   table_class := TABLE CLASS STANDARD | TABLE CLASS STANDARD INFREQUENT ACCESS\n"
           "   deletion_protection := DELETION PROTECTION [ENABLED|DISABLED]\n"
           "   tags := TAGS ( tag [, tag ...] )\n"
           "   tag := name : value\n\n");

    $$ = NULL;
};

create_table_command: K_CREATE K_TABLE if_not_exists_clause nowait_clause string '(' attribute_name_type_list ')'
                      primary_key optional_billing_mode_and_throughput gsi_list lsi_list
                      optional_stream_specification
                      optional_sse_specification optional_table_class optional_deletion_protection optional_table_tags ';'
{
    logdebug("[%s, %d] create_table_command\n", __FILENAME__, __LINE__);
    CCreateTableCommand * create = NEW CCreateTableCommand(
        $5, $3, $4, $7, $9, $10, $11, $12, $13, $14, $15, $16, $17);
    FREE($5);
    $$ = create;
};

string: T_DQUOTED_STRING
{
    logdebug("[%s, %d] string: T_DQUOTED_STRING\n", __FILENAME__, __LINE__);
    $$ = unquote($1);
    FREE($1);
} | T_UNQUOTED_STRING
{
    logdebug("[%s, %d] string:T_UNQUOTED_STRING\n", __FILENAME__, __LINE__);
    $$ = $1;
};

region_name: string
{
    logdebug("[%s, %d] region_name: string\n", __FILENAME__, __LINE__);
    $$ = $1;
};

connect_endpoint:
{
    logdebug("[%s, %d] connect_endpoint:\n", __FILENAME__, __LINE__);
    $$ = STRDUP((char *) "");
} | K_WITH K_ENDPOINT string
{
    logdebug("[%s, %d] connect_endpoint: K_WITH K_ENDPOINT string\n", __FILENAME__, __LINE__);
    $$ = $3;
};

if_exists_clause:
{
    logdebug("[%s, %d] if_exists_clause:\n", __FILENAME__, __LINE__);
    $$ = false;
} | K_IF K_EXISTS
{
    logdebug("[%s, %d] if_exists_clause: K_IF K_EXISTS\n", __FILENAME__, __LINE__);
    $$ = true;
};

if_not_exists_clause:
{
    logdebug("[%s, %d] if_not_exists_clause:\n", __FILENAME__, __LINE__);
    $$ = false;
} | K_IF K_NOT K_EXISTS
{
    logdebug("[%s, %d] if_not_exists_clause: K_IF K_NOT K_EXISTS\n",
             __FILENAME__, __LINE__);
    $$ = true;
};

nowait_clause:
{
    /* the default - wait */
    logdebug("[%s, %d] nowait_clause:\n", __FILENAME__, __LINE__);
    $$ = false;
} | K_NOWAIT
{
    logdebug("[%s, %d] nowait_clause: K_NOWAIT\n", __FILENAME__, __LINE__);
    $$ = true;
};

attribute_name_type_list: attribute_name_type
{
    logdebug("[%s, %d] attribute_name_type_list: attribute_name_type\n", __FILENAME__, __LINE__);

    Aws::Vector<Aws::DynamoDB::Model::AttributeDefinition> * attr_name_type_list \
        = NEW Aws::Vector<Aws::DynamoDB::Model::AttributeDefinition>;

    attr_name_type_list->push_back(*$1);
    delete $1;
    $$ = attr_name_type_list;
} | attribute_name_type_list ',' attribute_name_type
{
    logdebug("[%s, %d] attribute_name_type_list: attribute_name_type_list "
             "',' attribute_name_type\n", __FILENAME__, __LINE__);
    $1->push_back(*$3);
    delete $3;
    $$ = $1;
};

attribute_name_type: attribute_name attribute_type
{
    logdebug("[%s, %d] attribute_name_type: attribute_name attribute_type\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeDefinition * attr = NEW Aws::DynamoDB::Model::AttributeDefinition();
    attr->SetAttributeName($1);
    attr->SetAttributeType($2);

    FREE($1);
    $$ = attr;
};

attribute_name: string
{
    logdebug("[%s, %d] attribute_name: string\n", __FILENAME__, __LINE__);
    $$ = $1;
};

attribute_type: K_ATTRIBUTE_TYPE_NUMBER
{
    logdebug("[%s, %d] attribute_type: K_ATTRIBUTE_TYPE_NUMBER\n", __FILENAME__, __LINE__);

    $$ = Aws::DynamoDB::Model::ScalarAttributeType::N;

} | K_ATTRIBUTE_TYPE_STRING
{
    logdebug("[%s, %d] attribute_type: K_ATTRIBUTE_TYPE_STRING\n", __FILENAME__, __LINE__);

    $$ = Aws::DynamoDB::Model::ScalarAttributeType::S;
} | K_ATTRIBUTE_TYPE_BINARY
{
    logdebug("[%s, %d] attribute_type: K_ATTRIBUTE_TYPE_STRING\n", __FILENAME__, __LINE__);

    $$ = Aws::DynamoDB::Model::ScalarAttributeType::B;

};

primary_key: K_PRIMARY K_KEY key_schema
{
    logdebug("[%s, %d] primary_key: K_PRIMARY K_KEY key_schema\n", __FILENAME__, __LINE__);

    $$ = $3;
};

key_schema: '(' string K_HASH ')'
{
    logdebug("[%s, %d] key_schema: '(' string K_HASH ')'\n", __FILENAME__, __LINE__);

    Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> * primary_key = NEW Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement>;
    Aws::DynamoDB::Model::KeySchemaElement * hash = NEW Aws::DynamoDB::Model::KeySchemaElement;
    hash->SetAttributeName($2);
    hash->SetKeyType(Aws::DynamoDB::Model::KeyType::HASH);

    primary_key->push_back(*hash);
    delete hash;

    FREE($2);
    $$ = primary_key;
} | '(' string K_HASH ',' string K_RANGE ')'
{
    logdebug("[%s, %d] key_schema: '(' string K_HASH ',' string K_RANGE ')'\n", __FILENAME__, __LINE__);

    Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> * primary_key = NEW Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement>;
    Aws::DynamoDB::Model::KeySchemaElement * hash = NEW Aws::DynamoDB::Model::KeySchemaElement;
    Aws::DynamoDB::Model::KeySchemaElement * range = NEW Aws::DynamoDB::Model::KeySchemaElement;

    hash->SetAttributeName($2);
    hash->SetKeyType(Aws::DynamoDB::Model::KeyType::HASH);

    range->SetAttributeName($5);
    range->SetKeyType(Aws::DynamoDB::Model::KeyType::RANGE);

    primary_key->push_back(*hash);
    primary_key->push_back(*range);

    delete hash;
    delete range;

    FREE($2);
    FREE($5);

    $$ = primary_key;
} |  '(' string K_RANGE ',' string K_HASH ')'
{
    logdebug("[%s, %d] key_schema: '(' string K_RANGE ',' string K_HASH ')'\n", __FILENAME__, __LINE__);

    Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> * primary_key = NEW Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement>;
    Aws::DynamoDB::Model::KeySchemaElement * hash = NEW Aws::DynamoDB::Model::KeySchemaElement;
    Aws::DynamoDB::Model::KeySchemaElement * range = NEW Aws::DynamoDB::Model::KeySchemaElement;

    hash->SetAttributeName($5);
    hash->SetKeyType(Aws::DynamoDB::Model::KeyType::HASH);

    range->SetAttributeName($2);
    range->SetKeyType(Aws::DynamoDB::Model::KeyType::RANGE);

    primary_key->push_back(*hash);
    primary_key->push_back(*range);

    FREE($2);
    FREE($5);

    delete hash;
    delete range;

    $$ = primary_key;
};

billing_mode_and_throughput: K_BILLING K_MODE K_ON K_DEMAND
{
    logdebug("[%s, %d] billing_mode_and_throughput: K_BILLING K_MODE K_ON K_DEMAND\n", __FILENAME__, __LINE__);

    billing_mode_and_throughput_t * b = NEW billing_mode_and_throughput_t;

    b->mode = Aws::DynamoDB::Model::BillingMode::PAY_PER_REQUEST;
    b->throughput.SetReadCapacityUnits(0);
    b->throughput.SetWriteCapacityUnits(0);

    $$ = b;
} | K_BILLING K_MODE K_PROVISIONED '(' T_WHOLE_NUMBER K_RCU ',' T_WHOLE_NUMBER K_WCU ')'
{
    logdebug("[%s, %d] billing_mode_and_throughput: K_BILLING K_MODE K_PROVISIONED '(' T_WHOLE_NUMBER K_RCU ',' "
             "T_WHOLE_NUMBER K_WCU ')'\n", __FILENAME__, __LINE__);

    billing_mode_and_throughput_t * b = NEW billing_mode_and_throughput_t;

    b->mode = Aws::DynamoDB::Model::BillingMode::PROVISIONED;
    b->throughput.SetReadCapacityUnits(atoll($5));
    b->throughput.SetWriteCapacityUnits(atoll($8));

    FREE($5);
    FREE($8);

    $$ = b;
} | K_BILLING K_MODE K_PROVISIONED '(' T_WHOLE_NUMBER K_WCU ',' T_WHOLE_NUMBER K_RCU ')'
{
    logdebug("[%s, %d] billing_mode_and_throughput: K_BILLING K_MODE K_PROVISIONED '(' T_WHOLE_NUMBER K_WCU ',' T_WHOLE_NUMBER "
             "K_RCU ')'\n", __FILENAME__, __LINE__);

    billing_mode_and_throughput_t * b = NEW billing_mode_and_throughput_t;

    b->mode = Aws::DynamoDB::Model::BillingMode::PROVISIONED;
    b->throughput.SetReadCapacityUnits(atoll($8));
    b->throughput.SetWriteCapacityUnits(atoll($5));

    FREE($5);
    FREE($8);

    $$ = b;
};

optional_billing_mode_and_throughput: billing_mode_and_throughput
{
    logdebug("[%s, %d] optional_billing_mode_and_throughput: billing_mode_and_throughput\n", __FILENAME__, __LINE__);
    $$ = $1;
} |
{
    logdebug("[%s, %d] optional_billing_mode_and_throughput: \n", __FILENAME__, __LINE__);
    $$ = NULL;
};

gsi_list: K_GSI '(' gsi_specification ')'
{
    logdebug("[%s, %d] gsi_list: K_GSI '(' gsi_specification ')'\n", __FILENAME__, __LINE__);
    $$ = $3;
} |
{
    logdebug("[%s, %d] gsi_list: \n", __FILENAME__, __LINE__);
    $$ = NULL;
};

gsi_specification: gsi
{
    logdebug("[%s, %d] gsi_specification: gsi\n", __FILENAME__, __LINE__);

    Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndex> * gsi_list =
        NEW Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndex>;

    gsi_list->push_back(*$1);
    $$ = gsi_list;
    delete $1;
} | gsi_specification ',' gsi
{
    logdebug("[%s, %d] gsi_specification: gsi_specification ',' gsi\n", __FILENAME__, __LINE__);

    $1->push_back(*$3);
    $$ = $1;
    delete $3;
};

lsi_list: K_LSI '(' lsi_specification ')'
{
    logdebug("[%s, %d] lsi_list: K_LSI '(' lsi_specification ')'\n",
             __FILENAME__, __LINE__);

    $$ = $3;
} |
{
    logdebug("[%s, %d] lsi_list: \n", __FILENAME__, __LINE__);

    $$ = NULL;
};

lsi_specification: lsi
{
    logdebug("[%s, %d] lsi_specification: lsi\n", __FILENAME__, __LINE__);

    Aws::Vector<Aws::DynamoDB::Model::LocalSecondaryIndex> * lsi_list =
        NEW Aws::Vector<Aws::DynamoDB::Model::LocalSecondaryIndex>;

    lsi_list->push_back(*$1);
    $$ = lsi_list;
    delete $1;
} | lsi_specification ',' lsi
{
    logdebug("[%s, %d] lsi_specification: lsi_specification ',' lsi\n", __FILENAME__, __LINE__);
    $1->push_back(*$3);
    $$ = $1;
    delete $3;
};

gsi:string K_ON key_schema index_projection optional_billing_mode_and_throughput
{
    logdebug("[%s, %d] gsi:string K_ON key_schema index_projection optional_billing_mode_and_throughput\n",
             __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::GlobalSecondaryIndex * gsi = NEW Aws::DynamoDB::Model::GlobalSecondaryIndex;

    gsi->SetIndexName($1);
    gsi->SetKeySchema(*$3);
    gsi->SetProjection(*$4);

    if ($5 != NULL && $5->mode == Aws::DynamoDB::Model::BillingMode::PROVISIONED)
        gsi->SetProvisionedThroughput($5->throughput);

    if ($5 != NULL)
        FREE($5);

    $$ = gsi;
    FREE($1);
    delete $3;
    delete $4;
};

alter_table_gsi_create: string K_ON key_schema index_projection optional_billing_mode_and_throughput
{
    logdebug("[%s, %d] alter_table_gsi_create: string K_ON key_schema index_projection optional_billing_mode_and_throughput\n",
             __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::CreateGlobalSecondaryIndexAction * cgsia = NEW Aws::DynamoDB::Model::CreateGlobalSecondaryIndexAction;

    cgsia->SetIndexName($1);
    cgsia->SetKeySchema(*$3);
    cgsia->SetProjection(*$4);

    if ($5 != NULL && $5->mode == Aws::DynamoDB::Model::BillingMode::PROVISIONED)
        cgsia->SetProvisionedThroughput($5->throughput);

    if ($5 != NULL)
        FREE($5);

    Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate * gsiu =
        NEW Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate();

    gsiu->SetCreate(*cgsia);
    $$ = gsiu;
    FREE($1);

    delete $3;
    delete $4;
    delete cgsia;
};

alter_table_gsi_update_element: string K_SET billing_mode_and_throughput
{
    logdebug("[%s, %d] alter_table_gsi_update_element: string K_SET billing_mode_and_throughput\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::UpdateGlobalSecondaryIndexAction * ugsia = NEW Aws::DynamoDB::Model::UpdateGlobalSecondaryIndexAction;

    ugsia->SetIndexName($1);

    if ($3 != NULL && $3->mode == Aws::DynamoDB::Model::BillingMode::PROVISIONED)
        ugsia->SetProvisionedThroughput($3->throughput);

    if ($3 != NULL)
        FREE($3);

    Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate * gsiu = NEW Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate();

    gsiu->SetUpdate(*ugsia);

    $$ = gsiu;
    delete ugsia;
    FREE($1);
};

alter_table_gsi_update: alter_table_gsi_update_element
{
    logdebug("[%s, %d] alter_table_gsi_update: alter_table_gsi_update_element\n", __FILENAME__, __LINE__);

    Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate> * vectorgsiu =
        NEW Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate>;

    vectorgsiu->push_back(*$1);
    $$ = vectorgsiu;
    delete $1;
} | alter_table_gsi_update ',' alter_table_gsi_update_element
{
    logdebug("[%s, %d] alter_table_gsi_update ',' alter_table_gsi_update_element\n", __FILENAME__, __LINE__);

    $1->push_back(*$3);
    $$ = $1;

    delete $3;
};

alter_table_gsi_drop: string
{
    logdebug("[%s, %d] alter_table_gsi_drop_element: string\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::DeleteGlobalSecondaryIndexAction * dgsia = NEW Aws::DynamoDB::Model::DeleteGlobalSecondaryIndexAction;

    dgsia->SetIndexName($1);

    Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate * gsiu = NEW Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate();

    gsiu->SetDelete(*dgsia);

    FREE ($1);

    $$ = gsiu;
};

lsi: string K_ON key_schema index_projection
{
    logdebug("[%s, %d] lsi: string K_ON key_schema index_projection\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::LocalSecondaryIndex * lsi = NEW Aws::DynamoDB::Model::LocalSecondaryIndex;

    lsi->SetIndexName($1);
    lsi->SetKeySchema(*$3);
    lsi->SetProjection(*$4);

    $$ = lsi;

    delete $3;
    delete $4;

    FREE($1);
};

index_projection: K_PROJECTING K_ALL
{
    logdebug("[%s, %d] index_projection: K_PROJECTING K_ALL\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::Projection * projection = NEW Aws::DynamoDB::Model::Projection;
    projection->SetProjectionType(Aws::DynamoDB::Model::ProjectionType::ALL);
    $$ = projection;
} | K_PROJECTING K_KEYS K_ONLY
{
    logdebug("[%s, %d] index_projection: K_PROJECTING K_KEYS K_ONLY\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::Projection * projection = NEW Aws::DynamoDB::Model::Projection;

    projection->SetProjectionType(Aws::DynamoDB::Model::ProjectionType::KEYS_ONLY);
    $$ = projection;
} | K_PROJECTING K_INCLUDE '(' index_projection_list ')'
{
    logdebug("[%s, %d] index_projection: K_PROJECTING K_INCLUDE '(' index_projection_list ')'\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::Projection * projection = NEW Aws::DynamoDB::Model::Projection;

    projection->SetProjectionType(Aws::DynamoDB::Model::ProjectionType::INCLUDE);

    projection->SetNonKeyAttributes(*$4);
    $$ = projection;

    delete $4;
};

index_projection_list: string
{
    logdebug("[%s, %d] index_projection_list: string\n", __FILENAME__, __LINE__);

    Aws::Vector< Aws::String> * index_projection_list = NEW Aws::Vector< Aws::String>;
    index_projection_list->push_back($1);
    FREE($1);
    $$ = index_projection_list;
} | index_projection_list ',' string
{
    logdebug("[%s, %d] index_projection_list: index_projection_list ',' string\n", __FILENAME__, __LINE__);

    $1->push_back($3);
    $$ = $1;
    FREE($3);
};

stream_specification: K_STREAM '(' stream_view_type ')'
{
    logdebug("[%s, %d] stream_specification: K_STREAM '(' stream_view_type ')'\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::StreamSpecification * stream_specification = NEW Aws::DynamoDB::Model::StreamSpecification;

    stream_specification->SetStreamEnabled(true);
    stream_specification->SetStreamViewType($3);

    $$ = stream_specification;
} | K_STREAM K_DISABLED
{
    logdebug("[%s, %d] stram_specification: K_STREAM K_DISABLED\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::StreamSpecification * stream_specification = NEW Aws::DynamoDB::Model::StreamSpecification;
    stream_specification->SetStreamEnabled(false);
    $$ = stream_specification;
};

optional_stream_specification: stream_specification
{
    logdebug("[%s, %d] optional_stream_specification: stream_specification\n", __FILENAME__, __LINE__);
    $$ = $1;
} |
{
    logdebug("[%s, %d] optional_stream_specification:\n", __FILENAME__, __LINE__);
    $$ = NULL;
};

stream_view_type: K_KEYS K_ONLY
{
    logdebug("[%s, %d] stream_view_type: K_KEYS K_ONLY\n", __FILENAME__, __LINE__);
    $$ =  Aws::DynamoDB::Model::StreamViewType::KEYS_ONLY;
} | K_NEW K_IMAGE
{
    logdebug("[%s, %d] stream_view_type: K_NEW K_IMAGE\n", __FILENAME__, __LINE__);
    $$ =  Aws::DynamoDB::Model::StreamViewType::NEW_IMAGE;
} | K_OLD K_IMAGE
{
    logdebug("[%s, %d] stream_view_type: K_OLD K_IMAGE\n", __FILENAME__, __LINE__);
    $$ =  Aws::DynamoDB::Model::StreamViewType::OLD_IMAGE;
} | K_BOTH K_IMAGES
{
    logdebug("[%s, %d] stream_view_type: K_BOTH K_IMAGES\n", __FILENAME__, __LINE__);
    $$ =  Aws::DynamoDB::Model::StreamViewType::NEW_AND_OLD_IMAGES;
};

optional_sse_specification: sse_specification
{
    logdebug("[%s, %d] optional_sse_specification: sse_specification\n", __FILENAME__, __LINE__);
    $$ = $1;
} |
{
    logdebug("[%s, %d] optional_sse_specification:\n", __FILENAME__, __LINE__);
    $$ = NULL;
};

sse_specification: K_SSE K_DISABLED
{
    logdebug("[%s, %d] sse_specification: K_SSE K_DISABLED\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::SSESpecification * sse_specification = NEW Aws::DynamoDB::Model::SSESpecification;

    sse_specification->SetEnabled(false);
    $$ = sse_specification;
} | K_SSE '(' K_KEYID string K_KMS ')'
{
    logdebug("[%s, %d] K_SSE '(' K_KEYID string K_KMS ')'\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::SSESpecification * sse_specification = NEW Aws::DynamoDB::Model::SSESpecification;

    sse_specification->SetEnabled(true);
    sse_specification->SetKMSMasterKeyId($4);
    sse_specification->WithSSEType(Aws::DynamoDB::Model::SSEType::KMS);

    FREE($4);
    $$ = sse_specification;
} | K_SSE '(' K_KEYID string K_AES256 ')'
{
    logdebug("[%s, %d] K_SSE '(' K_KEYID string K_AES256 ')'\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::SSESpecification * sse_specification = NEW Aws::DynamoDB::Model::SSESpecification;

    sse_specification->SetEnabled(true);
    sse_specification->SetKMSMasterKeyId($4);
    sse_specification->WithSSEType(Aws::DynamoDB::Model::SSEType::AES256);

    FREE($4);
    $$ = sse_specification;
};

table_class: K_TABLE K_CLASS K_STANDARD
{
    logdebug("[%s, %d] table_class: K_TABLE K_CLASS K_STANDARD\n", __FILENAME__, __LINE__);
    $$ =  Aws::DynamoDB::Model::TableClass::STANDARD;
} | K_TABLE K_CLASS K_STANDARD K_INFREQUENT K_ACCESS
{
    logdebug("[%s, %d] K_TABLE K_CLASS K_STANDARD K_INFREQUENT K_ACCESS\n", __FILENAME__, __LINE__);
    $$ =  Aws::DynamoDB::Model::TableClass::STANDARD_INFREQUENT_ACCESS;
};

optional_table_class: table_class
{
    logdebug("[%s, %d] optional_table_class: table_class\n", __FILENAME__, __LINE__);
    $$ = $1;
} |
{
    logdebug("[%s, %d] optional_table_class:\n", __FILENAME__, __LINE__);
    $$ =  Aws::DynamoDB::Model::TableClass::STANDARD;
};

table_tags: K_TAGS '(' table_tags_list ')'
{
    logdebug("[%s, %d] table_tags: K_TAGS '(' table_tags_list ')'\n", __FILENAME__, __LINE__);
    $$ = $3;
};

deletion_protection: K_DELETION K_PROTECTION K_ENABLED
{
    logdebug("[%s, %d] deletion_protection: K_DELETION K_PROTECTION K_ENABLED\n", __FILENAME__, __LINE__);
    $$ = true;
} | K_DELETION K_PROTECTION K_DISABLED
{
    logdebug("[%s, %d] deletion_protection: K_DELETION K_PROTECTION K_DISABLED\n", __FILENAME__, __LINE__);
    $$ = false;
};

optional_deletion_protection: deletion_protection
{
    logdebug("[%s, %d] optional_deletion_protection: deletion_protection\n", __FILENAME__, __LINE__);
    $$ = $1;
} |
{
    logdebug("[%s, %d] optional_deletion_protection: null\n", __FILENAME__, __LINE__);
    $$ = false;
};

optional_table_tags: table_tags
{
    logdebug("[%s, %d] optional_table_tags: table_tags\n", __FILENAME__, __LINE__);
    $$ = $1;
} |
{
    logdebug("[%s, %d] optional_table_tags:\n", __FILENAME__, __LINE__);
    $$ = NULL;
};

table_tags_list: table_tag
{
    logdebug("[%s, %d] table_tags_list: table_tag\n", __FILENAME__, __LINE__);

    Aws::Vector<Aws::DynamoDB::Model::Tag> * tags =
        NEW Aws::Vector<Aws::DynamoDB::Model::Tag>;;

    tags->push_back(*$1);
    $$ = tags;
    delete $1;
} | table_tags_list ',' table_tag
{
    logdebug("[%s, %d] table_tags_list ',' table_tag\n", __FILENAME__, __LINE__);

    $1->push_back(*$3);
    $$ = $1;
    delete $3;
};

table_tag: string ':' string
{
    logdebug("[%s, %d] table_tag: string ':' string\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::Tag * tag = NEW Aws::DynamoDB::Model::Tag;

    tag->SetKey($1);
    tag->SetValue($3);

    FREE($1);
    FREE($3);
    $$ = tag;
};

help_command: K_HELP K_ALTER K_TABLE ';'
{
    printf("ALTER TABLE - make changes to an existing table.\n\n"
           "   ALTER TABLE <name> SET billing_mode_and_throughput\n"
           "      billing_mode_and_throughput := (BILLING MODE ON DEMAND)|BILLING MODE provisioned)\n"
           "      provisioned := ( RR RCU, WW WCU )\n\n"
           "   ALTER TABLE <name> SET table_class\n"
           "      table_class := TABLE CLASS STANDARD | TABLE CLASS INFREQUENT ACCESS\n\n"
           "   ALTER TABLE <name> SET streams\n"
           "      streams := STREAM ( stream_type ) | STREAM DISABLED\n"
           "      stream_type := KEYS ONLY | NEW IMAGE | OLD IMAGE | BOTH IMAGES\n\n"
           "   ALTER TABLE <name> ( attribute_name attribute_type [, ...] ) CREATE GSI create_gsi_spec\n"
           "      create_gsi_spec := <name> ON key_schema index_projection [billing_mode_and_throughput]\n"
           "   ALTER TABLE <name> DROP GSI <gsiname>\n\n"
           "   ALTER TABLE <name> SET billing_mode_and_throughput UPDATE GSI ( gsi_update [, ...] )\n"
           "      gsi_update := <gsiname> SET billing_mode_and_throughput\n\n"
           "   ALTER TABLE <name> UPDATE GSI ( gsi_update [, ...] )\n\n"
           "   ALTER TABLE <name> SET TTL DISABLED\n\n"
           "   ALTER TABLE <name> SET TTL ( attribute_name )\n\n"
           "   ALTER TABLE <name> SET PITR ENABLED\n\n"
           "   ALTER TABLE <name> SET PITR DISABLED\n\n"
           "   ALTER TABLE <name> ADD REPLICA <region> [replica_definition]\n"
           "   ALTER TABLE <name> UPDATE REPLICA <region> replica_definition\n"
           "   ALTER TABLE <name> DROP REPLICA <region>\n"
           "   ALTER TABLE <name> SET TABLE PROTECTION [ENABLED|DISABLED]\n" );

    $$ = NULL;
};

alter_table_command: K_ALTER K_TABLE string K_SET billing_mode_and_throughput ';'
{
    logdebug("[%s, %d] alter_table_command: K_ALTER K_TABLE string K_SET billing_mode_and_throughput ';'\n",
             __FILENAME__, __LINE__);

    CUpdateTableCommand * utc = NEW CUpdateTableCommand($3, $5);

    FREE($3);
    // $5 is freed in CUpdateTableCommand
    $$ = utc;
} | K_ALTER K_TABLE string K_SET sse_specification ';'
{
    logdebug("[%s, %d] alter_table_command: K_ALTER K_TABLE string K_SET sse_specification ';'\n", __FILENAME__, __LINE__);

    CUpdateTableCommand * utc = NEW CUpdateTableCommand($3, $5);

    FREE($3);
    $$ = utc;
} | K_ALTER K_TABLE string K_SET table_class ';'
{
    logdebug("[%s, %d] alter_table_command: K_ALTER K_TABLE string K_SET table_class ';'\n", __FILENAME__, __LINE__);

    CUpdateTableCommand * utc = NEW CUpdateTableCommand($3, $5);

    FREE($3);
    $$ = utc;
} | K_ALTER K_TABLE string K_SET stream_specification ';'
{
    logdebug("[%s, %d] alter_table_command: K_ALTER K_TABLE string K_SET stream_specification ';'\n", __FILENAME__, __LINE__);

    CUpdateTableCommand * utc = NEW CUpdateTableCommand($3, $5);

    FREE($3);
    $$ = utc;
} | K_ALTER K_TABLE string '(' attribute_name_type_list ')' K_CREATE K_GSI alter_table_gsi_create  ';'
{
    logdebug(
        "[%s, %d] alter_table_command: K_ALTER K_TABLE string '(' attribute_name_type_list ')' K_CREATE K_GSI '(' alter_table_gsi_create ')' ';'\n",
        __FILENAME__, __LINE__);

    CUpdateTableCommand * utc = NEW CUpdateTableCommand($3, $5, $9);
    FREE($3);
    $$ = utc;
} | K_ALTER K_TABLE string K_DROP K_GSI alter_table_gsi_drop ';'
{
    logdebug("[%s, %d] alter_table_command: K_ALTER K_TABLE string K_DROP K_GSI '(' alter_table_gsi_drop ')' ';'\n", __FILENAME__, __LINE__);

    CUpdateTableCommand * utc = NEW CUpdateTableCommand($3, $6);

    FREE($3);
    $$ = utc;
} | K_ALTER K_TABLE string K_SET billing_mode_and_throughput K_UPDATE K_GSI '(' alter_table_gsi_update ')' ';'
{
    logdebug(
        "[%s, %d] alter_table_command: K_ALTER K_TABLE string K_SET billing_mode_and_throughput K_UPDATE K_GSI '(' alter_table_gsi_update ')' ';'\n",
        __FILENAME__, __LINE__);

    // if this command seeks to set the table to billing mode on demand, then one should not specify
    // settings for the indices - those are implied.
    if ($5->mode == Aws::DynamoDB::Model::BillingMode::PROVISIONED)
    {
        CUpdateTableCommand * utc = NEW CUpdateTableCommand($3, $5, $9);
        $$ = utc;
    }
    else
    {
        CUpdateTableCommand * utc = NEW CUpdateTableCommand($3, $5);
        logmsg ("[%s, %d] Setting table to on-demand, ignoring specifications for indices.\n", __FILENAME__, __LINE__);
        $$ = utc;
    }

    FREE($3);
} | K_ALTER K_TABLE string K_UPDATE K_GSI '(' alter_table_gsi_update ')' ';'
{
    logdebug(
        "[%s, %d] alter_table_command: K_ALTER K_TABLE string K_SET billing_mode_and_throughput K_UPDATE K_GSI '(' alter_table_gsi_update ')' ';'\n",
        __FILENAME__, __LINE__);

    CUpdateTableCommand * utc = NEW CUpdateTableCommand($3, $7);
    FREE($3);
    $$ = utc;
} | K_ALTER K_TABLE string K_SET K_TTL K_DISABLED ';'
{
    logdebug("[%s, %d] alter_table_command: K_ALTER K_TABLE string K_SET K_TTL K_DISABLED ';'\n", __FILENAME__, __LINE__);
    CUpdateTableTTLCommand * ttl = NEW CUpdateTableTTLCommand($3, false);
    FREE($3);
    $$ = ttl;
} | K_ALTER K_TABLE string K_SET K_TTL '(' string ')' ';'
{
    logdebug("[%s, %d] alter_table_command: K_ALTER K_TABLE string K_SET K_TTL '(' string ')'  ';'\n", __FILENAME__, __LINE__ );
    CUpdateTableTTLCommand * ttl = NEW CUpdateTableTTLCommand($3, true, $7);
    FREE($3);
    FREE($7);
    $$ = ttl;
} | K_ALTER K_TABLE string K_SET K_PITR K_ENABLED ';'
{
    logdebug("[%s, %d] alter_table_command: K_ALTER K_TABLE string K_PITR K_ENABLED ';'\n", __FILENAME__, __LINE__);
    CUpdatePITRCommand * p = NEW CUpdatePITRCommand($3, true);
    FREE($3);
    $$ = p;
} | K_ALTER K_TABLE string K_SET K_PITR K_DISABLED ';'
{
    logdebug("[%s, %d] alter_table_command: K_ALTER K_TABLE string K_PITR K_DISABLED ';'\n", __FILENAME__, __LINE__);
    CUpdatePITRCommand * p = NEW CUpdatePITRCommand($3, false);
    FREE($3);
    $$ = p;
} | K_ALTER K_TABLE string K_DROP K_REPLICA string ';'
{
    logdebug("[%s, %d] alter_table_command: K_ALTER K_TABLE string K_DROP K_REPLICA string ';'\n", __FILENAME__, __LINE__);

    CUpdateTableReplica * p = NEW CUpdateTableReplica($3);

    p->deleteRegion($6);

    FREE($3);
    FREE($6);

    $$ = p;
} | K_ALTER K_TABLE string K_ADD K_REPLICA string optional_table_class optional_provisioned_throughput_override optional_replica_gsi_specification ';'
{
    logdebug("[%s, %d] alter_table_command: K_ALTER K_TABLE string K_ADD K_REPLICA string optional_table_class optional_provisioned_throughput_override optional_replica_gsi_specification ';'\n",
             __FILENAME__, __LINE__);

    CUpdateTableReplica * p = NEW CUpdateTableReplica($3);
    p->addRegion($6, $7, $8, $9);

    FREE($3);
    FREE($6);

    // table class is a scalar
    delete $8;
    delete $9;

    $$ = p;
} | K_ALTER K_TABLE string K_UPDATE K_REPLICA string optional_table_class optional_provisioned_throughput_override optional_replica_gsi_specification';'
{
    logdebug("[%s, %d] alter_table_command: K_ALTER K_TABLE string K_UPDATE K_REPLICA string optional_table_class optional_provisioned_throughput_override optional_replica_gsi_specification';'\n",
             __FILENAME__, __LINE__);

    CUpdateTableReplica * p = NEW CUpdateTableReplica($3);
    p->updateRegion($6, $7, $8, $9);

    FREE($3);
    FREE($6);

    // table class is a scalar
    delete $8;
    delete $9;

    $$ = p;
} | K_ALTER K_TABLE string K_SET deletion_protection ';'
{
    logdebug("[%s, %d]: alter_table_command: K_ALTER K_TABLE string K_SET deletion_protection\n", __FILENAME__, __LINE__);
    CUpdateTableCommand * atsdp = NEW CUpdateTableCommand($3, $5);

    FREE($3);

    $$ = atsdp;
};

optional_replica_gsi_specification: K_GSI '(' replica_gsi_specification_list ')'
{
    logdebug("[%s, %d]: optional_replica_gsi_specification: K_GSI '(' replica_gsi_specification_list ')' \n",
             __FILENAME__, __LINE__);
    $$ = $3;
} |
{
    logdebug("[%s, %d]: optional_replica_gsi_specification: \n", __FILENAME__, __LINE__);
    $$ = NULL;
};

replica_gsi_specification_list: replica_gsi_specification_list ',' replica_gsi_specification_list_entry
{
    logdebug("[%s, %d] replica_gsi_specification_list: replica_gsi_specification_list ',' replica_gsi_specification_list_entry\n",
             __FILENAME__, __LINE__);
    $1->push_back(*$3);
    delete $3;

    $$ = $1;
} | replica_gsi_specification_list_entry
{
    logdebug("[%s, %d] replica_gsi_specification_list: replica_gsi_specification_list_entry\n", __FILENAME__, __LINE__);
    Aws::Vector<Aws::DynamoDB::Model::ReplicaGlobalSecondaryIndex> * rgsilist = NEW Aws::Vector<Aws::DynamoDB::Model::ReplicaGlobalSecondaryIndex>;

    rgsilist->push_back(*$1);

    delete $1;
    $$ = rgsilist;
};

replica_gsi_specification_list_entry: string provisioned_throughput_override
{
    logdebug("[%s, %d] replica_gsi_specification_list_entry: string provisioned_throughput_override\n",
             __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::ReplicaGlobalSecondaryIndex * rgsi = NEW Aws::DynamoDB::Model::ReplicaGlobalSecondaryIndex;
    rgsi->SetIndexName($1);
    rgsi->SetProvisionedThroughputOverride(*$2);

    delete $2;

    $$ = rgsi;
};

provisioned_throughput_override: '(' T_WHOLE_NUMBER K_RCU ')'
{
    logdebug("[%s, %d] provisioned_throughput_and_override: '(' T_WHOLE_NUMBER K_RCU ',' T_WHOLE_NUMBER K_WCU ')'\n",
             __FILENAME__, __LINE__);
    Aws::DynamoDB::Model::ProvisionedThroughputOverride * pto = NEW Aws::DynamoDB::Model::ProvisionedThroughputOverride;
    pto->SetReadCapacityUnits(atoll($2));
    FREE($2);

    $$ = pto;
};

optional_provisioned_throughput_override: provisioned_throughput_override
{
    $$ = $1;
} |
{
    $$ = NULL;
};

help_command: K_HELP K_SHOW K_LIMITS ';'
{
    printf("SHOW LIMITS -- Show user limit information.\n");
    $$ = NULL;
};

show_limits_command: K_SHOW K_LIMITS ';'
{
    logdebug("[%s, %d] K_SHOW K_LIMITS ';'\n", __FILENAME__, __LINE__ );
    CShowLimitsCommand * sl = NEW CShowLimitsCommand();

    $$ = sl;
};

optional_backup_type: string
{
    $$ = $1;
} |
{
    $$ = STRDUP((char *) "");
}

optional_table_name: string
{
    $$ = $1;
} |
{
    $$ = STRDUP((char *) "");
};

help_command: K_HELP K_SHOW K_BACKUPS ';'
{
    printf("SHOW BACKUPS - SHOW [type] BACKUPS <tablename>\n\n");
    $$ = NULL;
};

show_backups_command: K_SHOW optional_backup_type K_BACKUPS optional_table_name ';'
{
    logdebug("[%s, %d] show_backups_command: K_SHOW optional_backup_type K_BACKUPS optional_table_name ';'\n",
             __FILENAME__, __LINE__);
    CShowBackupsCommand * csbc = NEW CShowBackupsCommand($2, $4);

    FREE($2);
    FREE($4);

    $$ = csbc;
};

help_command: K_HELP K_DESCRIBE K_BACKUP ';'
{
    printf("DESCRIBE BACKUP <arn> - Describe a backup given an ARN\n\n");
    $$ = NULL;
};

describe_backup_command: K_DESCRIBE K_BACKUP string ';'
{
    logdebug("[%s, %d] describe_backup_command: K_DESCRIBE K_BACKUP string ';'\n", __FILENAME__, __LINE__);
    CDescribeBackupCommand * dbc = NEW CDescribeBackupCommand($3);
    FREE($3);
    $$ = dbc;
};

help_command: K_HELP K_DROP K_BACKUP ';'
{
    printf("DROP BACKUP <arn> - Drop a backup given an ARN\n\n");
    $$ = NULL;
};

drop_backup_command: K_DROP K_BACKUP string ';'
{
    logdebug("[%s, %d] drop_backup_command: K_DROP K_BACKUP string ';'\n", __FILENAME__, __LINE__);
    CDropBackupCommand * dbc = NEW CDropBackupCommand($3);
    FREE($3);
    $$ = dbc;
};

help_command: K_HELP K_BACKUP ';'
{
    printf("BACKUP TABLE <table> CALLED <name> - Create a backup of a table with a given name\n\n");
    $$ = NULL;
};

create_backup_command: K_BACKUP K_TABLE string K_CALLED string ';'
{
    logdebug("[%s, %d] create_backup_command: K_BACKUP K_TABLE string K_CALLED string ';'\n", __FILENAME__, __LINE__);

    CCreateBackupCommand * cbc = NEW CCreateBackupCommand($3, $5);
    FREE($3);
    FREE($5);
    $$ = cbc;
};

help_command: K_HELP K_RESTORE ';'
{
    printf("RESTORE - Restore a backup\n\n"
           "   RESTORE TABLE <table> FROM BACKUP <arn> - Restore a table from a backup.\n\n"
           "   RESTORE TABLE <table> FROM <arn> TO PITR <timestamp> - Restore a table from a backup at a point in time.\n\n");
    $$ = NULL;
};

restore_backup_command: K_RESTORE K_TABLE string K_FROM K_BACKUP string ';'
{
    logdebug("[%s, %d] restore_backup_command: K_RESTORE K_TABLE string K_FROM K_BACKUP string ';'\n", __FILENAME__, __LINE__);

    CRestoreBackupCommand * rbc = NEW CRestoreBackupCommand($3, $6);

    FREE($3);
    FREE($6);
    $$ = rbc;
} | K_RESTORE K_TABLE string K_FROM string K_TO K_PITR T_TIMESTAMP ';'
{
    logdebug("[%s, %d] restore_backup_command: K_RESTORE K_TABLE string K_FROM string K_TO K_PITR T_TIMESTAMP ';'\n",
             __FILENAME__, __LINE__);
    char * pt = unquote($8);

    logdebug("[%s, %d] timestamp = %s\n", __FILENAME__, __LINE__, $8);

    CRestorePITRCommand * rpc = NEW CRestorePITRCommand($5, pt, $3);

    FREE($3);
    FREE($6);
    FREE($8);
    FREE(pt);

    $$ = rpc;
};

exists_command: K_EXISTS select_command
{
    logdebug("[%s, %d] exists_command: K_EXISTS select_command\n", __FILENAME__, __LINE__);
    ((CSelectCommand *)$2)->exists();
    $$ = $2;
};

help_command: K_HELP K_SELECT ';'
{
    printf("SELECT - Select item(s) from a table.\n\n"
           "SELECT [CONSISTENT] attribute_list | * FROM <table>[.<index>] [WHERE where_clause] [return_clause|ratelimit]]\n\n"
           "   return_clause := RETURN INDEXES | RETURN TOTAL\n\n"
           "   ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |\n"
           "                WITH RATELIMIT ( RR RCU ) |\n"
           "                WITH RATELIMIT ( WW WCU )\n\n"
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
           "       NOTES: 1. The WHERE clause for a NULL field uses the SQL syntax of <field> IS [NOT] NULL\n"
           "                 The equality operator cannot be used with NULL.\n"
           "              2. To reference list elements in projection or where clause uses 0 based indexing.\n"
           "              3. You can reference maps (dictionaries) in projection and where clause.\n"
           "              4. You can rate limit a select by specifying the optional ratelimit parameter.\n"
           "              5. You cannot specify both a rate limit and a return clause.\n\n");
    $$ = NULL;
};

select_command: K_SELECT optional_consistent select_projection K_FROM table_optional_index_name
                optional_where_clause optional_return_clause optional_ratelimit ';'
{
    logdebug("[%s, %d] select_command: K_SELECT optional_consistent attribute_list_or_wildcard "
             "K_FROM table_optional_index_name optional_where_clause optional_return_clause "
             "optional_ratelimit ';'\n", __FILENAME__, __LINE__);

    if ($8 != NULL && $7 != Aws::DynamoDB::Model::ReturnConsumedCapacity::NONE)
    {
        logerror("You cannot specify both a ratelimit and a return_clause.\n");
        delete $3;
        delete $5;
        delete $6;
        delete $8;
        $$ = NULL;
    }
    else
    {
        CSelectCommand * ns = NEW CSelectCommand($8, $2, $3, $5, $6, $7);
        $$ = ns;
    }
};

optional_consistent: K_CONSISTENT
{
    logdebug("[%s, %d] optional_consistent: K_CONSISTENT\n", __FILENAME__, __LINE__);
    $$ = true;
} |
{
    logdebug("[%s, %d] optional_consistent: <empty>\n", __FILENAME__, __LINE__);
    $$ = false;
};

select_projection: '*'
{
    logdebug("[%s, %d] select_projection_list: '*'\n", __FILENAME__, __LINE__);
    // having no projection list is equivalent to projecting all attributes
    Aws::Vector<Aws::String> * proj = NEW Aws::Vector<Aws::String>;
    $$ = proj;
} | select_attribute_list
{
    logdebug("[%s, %d] select_projection_list: select_attribute_list\n", __FILENAME__, __LINE__);
    $$ = $1;
};

select_attribute_list: select_attribute
{
    logdebug("[%s, %d] select_attribute_list: select_attribute\n", __FILENAME__, __LINE__);
    Aws::Vector<Aws::String> * proj = NEW Aws::Vector<Aws::String>;
    proj->push_back(*$1);
    delete $1;
    $$ = proj;
} | select_attribute_list ',' select_attribute
{
    logdebug("[%s, %d] select_attribute_list: select_attribute_list, select_attribute\n", __FILENAME__, __LINE__);
    $1->push_back(*$3);
    delete $3;
    $$ = $1;
};

select_attribute: lee_lhs
{
    logdebug("[%s, %d] select_attribute: lee_lhs\n", __FILENAME__, __LINE__);
    $$ = $1;
};

table_optional_index_name: table_name '.' table_name
{
    logdebug("[%s, %d] table_optional_index_name: table_name '.' table_name\n", __FILENAME__, __LINE__);
    Aws::Vector<Aws::String> * toin = NEW Aws::Vector<Aws::String>;

    toin->push_back($1);
    toin->push_back($3);

    FREE($1);
    FREE($3);
    $$ = toin;
} | table_name
{
    logdebug("[%s, %d] table_optional_index_name: table_name\n", __FILENAME__, __LINE__);
    Aws::Vector<Aws::String> * toin = NEW Aws::Vector<Aws::String>;
    toin->push_back($1);

    FREE($1);
    $$ = toin;
};

table_name: string
{
    logdebug("[%s, %d] table_name: string\n", __FILENAME__, __LINE__);
    $$ = $1;
}

optional_where_clause:
{
    // the empty where clause
    logdebug("[%s, %d] optional_where_clause: <empty>\n", __FILENAME__, __LINE__);
    $$ = NULL;
} | where_clause
{
    logdebug("[%s, %d] optional_where_clause: where_clause\n", __FILENAME__, __LINE__);
    $$ = $1;
};

where_clause: K_WHERE logical_expression
{
    logdebug("[%s, %d] where_clause: K_WHERE logical_expression\n", __FILENAME__, __LINE__);
    CWhere *wc = NEW CWhere($2);
    $$ = wc;
};

logical_expression: logical_expression_element
{
    logdebug("[%s, %d] logical_expression: logical_expression_element\n", __FILENAME__, __LINE__);
    $$ = $1;
} | K_NOT logical_expression
{
    logdebug("[%s, %d] logical_expression: K_NOT logical_expression\n", __FILENAME__, __LINE__);
    $2->negate();
    $$ = $2;
} | '(' logical_expression ')'
{
    logdebug("[%s, %d] logical_expression: '(' logical_expression ')'\n", __FILENAME__, __LINE__);
    $2->set_paren_group();
    $$ = $2;
} | logical_expression K_AND logical_expression
{
    logdebug("[%s, %d] logical_expression: logical_expression K_AND logical_expression\n", __FILENAME__, __LINE__);
    $$ = CLogicalAnd::logical_and($1, $3);
} | logical_expression K_OR logical_expression
{
    logdebug("[%s, %d] logical_expression: logical_expression K_OR logical_expression\n", __FILENAME__, __LINE__);
    $$ = CLogicalOr::logical_or($1, $3);
};

logical_expression_element: lee_lhs comparison_op lee_rhs
{
    logdebug("[%s, %d] logical_expression_element: lee_lhs comparison_op lee_rhs\n", __FILENAME__, __LINE__);
    CLogicalComparison * lc = NEW CLogicalComparison($1, $2, $3);

    delete $1;
    FREE($2);
    delete $3;

    $$ = lc;
} | lee_lhs K_IS K_NULL
{
    logdebug("[%s, %d] logical_expression_element: lee_lhs K_IS K_NULL\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue nullvalue;
    nullvalue.SetNull(true);

    CLogicalComparison * lc = NEW CLogicalComparison($1, (char *) "=", &nullvalue);

    delete $1;

    $$ = lc;
} | lee_lhs K_IS K_NOT K_NULL
{
    logdebug("[%s, %d] logical_expression_element: lee_lhs K_IS K_NOT K_NULL\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue nullvalue;
    nullvalue.SetNull(true); // null values should always be true. use negate below :)

    CLogicalComparison * lc = NEW CLogicalComparison($1, (char *) "=", &nullvalue);
    lc->negate();
    CLogicalAttributeExists * lae = NEW CLogicalAttributeExists($1, true);

    delete $1;

    $$ = CLogicalAnd::logical_and(lc, lae);
} | lee_lhs K_BETWEEN lee_rhs K_AND lee_rhs
{
    logdebug("[%s, %d] logical_expression_element: lee_lhs K_BETWEEN lee_rhs K_AND lee_rhs\n", __FILENAME__, __LINE__);
    CLogicalBetween * lb = NEW CLogicalBetween($1, $3, $5);

    delete $1;
    delete $3;
    delete $5;

    $$ = lb;
} | lee_lhs K_IN '(' lee_rhs_list ')'
{
    logdebug("[%s, %d] logical_expression_element: lee_lhs K_IN '(' lee_rhs_list ')'\n", __FILENAME__, __LINE__);
    CLogicalIn * li = NEW CLogicalIn($1, $4);

    delete $1;
    delete $4;

    $$ = li;
} | K_ATTRIBUTE_EXISTS '(' lee_lhs ')'
{
    logdebug("[%s, %d] K_ATTRIBUTE_EXISTS '(' lee_lhs ')'\n", __FILENAME__, __LINE__);
    CLogicalAttributeExists * lae = NEW CLogicalAttributeExists($3, true);
    delete $3;
    $$ = lae;
} | K_ATTRIBUTE_NOT_EXISTS '(' lee_lhs ')'
{
    logdebug("[%s, %d] K_ATTRIBUTE_NOT_EXISTS '(' lee_lhs ')'\n", __FILENAME__, __LINE__);
    CLogicalAttributeExists * lae = NEW CLogicalAttributeExists($3, false);
    delete $3;
    $$ = lae;
} | K_ATTRIBUTE_TYPE '(' lee_lhs ',' attribute_type ')'
{
    logdebug("[%s, %d] K_ATTRIBUTE_TYPE '(' lee_lhs ',' attribute_type ')'\n", __FILENAME__, __LINE__);
    CLogicalAttributeType * lat = NEW CLogicalAttributeType($3, $5);
    delete $3;

    $$ = lat;
} | K_BEGINS_WITH '(' lee_lhs ',' string ')'
{
    logdebug("[%s, %d] K_BEGINS_WITH '(' lee_lhs ',' string ')'\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue prefix;

    prefix.SetS($5);

    CLogicalBeginsWith * lbw = NEW CLogicalBeginsWith($3, prefix);

    delete $3;
    delete $5;
    $$ = lbw;
} | K_CONTAINS '(' lee_lhs ',' string ')'
{
    logdebug("[%s, %d] K_CONTAINS '(' lee_lhs ',' string ')'\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::Model::AttributeValue prefix;

    prefix.SetS($5);

    CLogicalContains * lc = NEW CLogicalContains($3, prefix);
    delete $3;
    FREE $5;
    $$ = lc;
} | K_SIZE '(' lee_lhs ')' comparison_op lee_rhs
{
    logdebug("[%s, %d] K_SIZE '(' lee_lhs ')' comparison_op lee_rhs\n", __FILENAME__, __LINE__);
    std::string lhs = "size(" + *$3 + ")";
    CLogicalComparison * lc = NEW CLogicalComparison(&lhs, $5, $6);

    FREE($5);
    delete $6;
    $$ = lc;
};

lee_rhs_list: lee_rhs
{
    logdebug("[%s, %d] lee_rhs_list: lee_rhs\n", __FILENAME__, __LINE__);
    Aws::Vector<Aws::DynamoDB::Model::AttributeValue> * rv = NEW Aws::Vector<Aws::DynamoDB::Model::AttributeValue>();

    rv->push_back(*$1);

    delete $1;
    $$ = rv;
} | lee_rhs_list ',' lee_rhs
{
    logdebug("[%s, %d] lee_rhs_list: lee_rhs_list ',' lee_rhs\n", __FILENAME__, __LINE__);
    $1->push_back(*$3);
    delete $3;
    $$ = $1;
};

lee_rhs: T_WHOLE_NUMBER
{
    logdebug("[%s, %d] lee_rhs: T_WHOLE_NUMBER\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();

    rv->SetN($1);
    FREE($1);

    $$ = rv;
} | T_FLOATING_POINT_NUMBER
{
    logdebug("[%s, %d] lee_rhs: T_FLOATING_POINT_NUMBER\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();

    rv->SetN($1);
    FREE($1);

    $$ = rv;
} | T_DQUOTED_STRING
{
    logdebug("[%s, %d] lee_rhs: T_DQUOTED_STRING\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();

    char * sl = unquote($1);
    rv->SetS(sl);
    FREE($1);
    FREE(sl);

    $$ = rv;
} | '[' lee_rhs_list ']'
{
    logdebug("[%s, %d] lee_rhs: '[' lee_rhs_list ']'\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();

    for (auto f: *$2)
    {
        std::shared_ptr<Aws::DynamoDB::Model::AttributeValue> g = std::make_shared<Aws::DynamoDB::Model::AttributeValue>(f);
        rv->AddLItem(g);
    }

    delete $2;
    $$ = rv;
} | K_TRUE
{
    logdebug("[%s, %d] lee_rhs: K_TRUE\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();
    rv->SetBool(true);

    $$ = rv;
} | K_FALSE
{
    logdebug("[%s, %d] lee_rhs: K_FALSE\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();
    rv->SetBool(false);

    $$ = rv;
} | '{' map_element_list '}'
{
    logdebug("[%s, %d] lee_rhs: '{' map_element_list '}'\n", __FILENAME__, __LINE__);
    $$ = $2;
};

map_element_list: string ':' lee_rhs
{
    logdebug("[%s, %d] map_element_list: string ':' lee_rhs\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();

    std::shared_ptr<Aws::DynamoDB::Model::AttributeValue> g = std::make_shared<Aws::DynamoDB::Model::AttributeValue>(*$3);
    rv->AddMEntry($1, g);

    delete $1;
    delete $3;
    $$ = rv;
} | map_element_list ',' string ':' lee_rhs
{
    logdebug("[%s, %d] map_element_list: map_element_list ',' string ':' lee_rhs\n", __FILENAME__, __LINE__);

    std::shared_ptr<Aws::DynamoDB::Model::AttributeValue> g = std::make_shared<Aws::DynamoDB::Model::AttributeValue>(*$5);
    $1->AddMEntry($3, g);

    delete $3;
    delete $5;
    $$ = $1;
};

comparison_op: T_EQUAL
{
    logdebug("[%s, %d] comparison_op: %s\n", __FILENAME__, __LINE__, $1);
    $$ = $1;
} | T_GREATER_THAN
{
    logdebug("[%s, %d] comparison_op: %s\n", __FILENAME__, __LINE__, $1);
    $$ = $1;
} | T_GREATER_THAN_OR_EQUAL
{
    logdebug("[%s, %d] comparison_op: %s\n", __FILENAME__, __LINE__, $1);
    $$ = $1;
} | T_LESS_THAN
{
    logdebug("[%s, %d] comparison_op: %s\n", __FILENAME__, __LINE__, $1);
    $$ = $1;
} | T_LESS_THAN_OR_EQUAL
{
    logdebug("[%s, %d] comparison_op: %s\n", __FILENAME__, __LINE__, $1);
    $$ = $1;
} | T_NOT_EQUAL
{
    logdebug("[%s, %d] comparison_op: %s\n", __FILENAME__, __LINE__, $1);
    $$ = $1;
};

lee_lhs: string
{
    logdebug("[%s, %d] lee_lhs: string\n", __FILENAME__, __LINE__);

    $$ = NEW Aws::String($1);

    FREE($1);
} | lee_lhs '[' T_WHOLE_NUMBER ']'
{
    logdebug("[%s, %d] lee_lhs: lee_lhs '[' T_WHOLE_NUMBER ']'\n", __FILENAME__, __LINE__);

    $$ = NEW Aws::String(*$1 + "[" + Aws::String($3) + "]");

    delete $1;
    FREE($3);
} | lee_lhs '.' lee_lhs
{
    logdebug("[%s, %d] lee_lhs: lee_lhs '.' lee_lhs\n", __FILENAME__, __LINE__);

    $$ = NEW Aws::String(*$1 + "." + *$3);

    delete $1;
    delete $3;
};

optional_return_clause: K_RETURN K_INDEXES
{
    $$ = Aws::DynamoDB::Model::ReturnConsumedCapacity::INDEXES;
} | K_RETURN K_TOTAL
{
    $$ = Aws::DynamoDB::Model::ReturnConsumedCapacity::TOTAL;
} |
{
    $$ = Aws::DynamoDB::Model::ReturnConsumedCapacity::NONE;
};

help_command: K_HELP K_INSERT ';'
{
    printf("INSERT INTO <table> ( column_list ) VALUES (values) [, ...] [ratelimit]\n"
           "ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |\n"
           "             WITH RATELIMIT ( RR RCU ) |\n"
           "             WITH RATELIMIT ( WW WCU )\n\n");
    $$ = NULL;
};

insert_command: K_INSERT K_INTO table_name '(' insert_column_list ')' K_VALUES value_list optional_ratelimit ';'
{
    logdebug("[%s, %d] insert_command: K_INSERT K_INTO table_name '(' insert_column_list ')' \n"
             "K_VALUES value_list optional_ratelimit';'\n",
             __FILENAME__, __LINE__);
    CInsertCommand * ic = NEW CInsertCommand($3, $5, $8, true, $9);

    FREE($3);

    $$ = ic;
};

value_list: '(' insert_values ')'
{
    Aws::Vector<Aws::Vector<Aws::DynamoDB::Model::AttributeValue>> * rv =
        NEW Aws::Vector<Aws::Vector<Aws::DynamoDB::Model::AttributeValue>>;
    rv->push_back(*$2);
    delete $2;
    $$ = rv;
} | value_list ',' '(' insert_values ')'
{
    $1->push_back(*$4);
    delete $4;
    $$ = $1;
};

help_command: K_HELP K_REPLACE ';'
{
    printf("REPLACE - Replace items in a table\n\n"
           "REPLACE INTO <table> ( column [, column ...] ) VALUES ( values ) [ratelimit]\n"
           "ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |\n"
           "             WITH RATELIMIT ( RR RCU ) |\n"
           "             WITH RATELIMIT ( WW WCU )\n\n");

    $$ = NULL;
};

replace_command: K_REPLACE K_INTO table_name '(' insert_column_list ')' K_VALUES value_list optional_ratelimit ';'
{
    logdebug("[%s, %d] replace_command: K_REPLACE K_INTO table_name '(' insert_column_list ')' K_VALUES value_list ';'\n",
             __FILENAME__, __LINE__);
    CInsertCommand * ic = NEW CInsertCommand($3, $5, $8, false, $9);

    FREE($3);

    $$ = ic;
};

insert_column_list: insert_column
{
    Aws::Vector<Aws::String> * icl = NEW Aws::Vector<Aws::String>;
    logdebug("[%s, %d] insert_column_list: insert_column\n", __FILENAME__, __LINE__);

    icl->push_back($1);
    FREE($1);

    $$ = icl;
} | insert_column_list ',' insert_column
{
    logdebug("[%s, %d] insert_column_list: insert_column_list ',' insert_column\n", __FILENAME__, __LINE__);

    $1->push_back($3);
    FREE($3);

    $$ = $1;
};

insert_values: insert_value
{
    logdebug("[%s, %d] insert_values: insert_value\n", __FILENAME__, __LINE__);
    Aws::Vector<Aws::DynamoDB::Model::AttributeValue> * rv = NEW Aws::Vector<Aws::DynamoDB::Model::AttributeValue>();

    rv->push_back(*$1);

    delete($1);

    $$ = rv;
} | insert_values ',' insert_value
{
    logdebug("[%s, %d] insert_values: insert_values ',' insert_values\n", __FILENAME__, __LINE__);

    $1->push_back(*$3);
    delete ($3);

    $$ = $1;
};

insert_column: string
{
    logdebug("[%s, %d] insert_column: string\n", __FILENAME__, __LINE__);
    $$ = $1;
};

insert_value: T_WHOLE_NUMBER
{
    logdebug("[%s, %d] insert_value: T_WHOLE_NUMBER\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();

    rv->SetN($1);
    FREE($1);

    $$ = rv;
} | T_FLOATING_POINT_NUMBER
{
    logdebug("[%s, %d] insert_value: T_FLOATING_POINT_NUMBER\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();

    rv->SetN($1);
    FREE($1);

    $$ = rv;
} | T_DQUOTED_STRING
{
    logdebug("[%s, %d] insert_value: T_DQUOTED_STRING\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();

    char * sl = unquote($1);
    rv->SetS(sl);
    FREE($1);
    FREE(sl);

    $$ = rv;
} | '[' lee_rhs_list ']'
{
    logdebug("[%s, %d] insert_value: '[' lee_rhs_list ']'\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();

    for  (auto f: *$2)
    {
        std::shared_ptr<Aws::DynamoDB::Model::AttributeValue> g = std::make_shared<Aws::DynamoDB::Model::AttributeValue>(f);
        rv->AddLItem(g);
    }

    delete $2;
    $$ = rv;
} | K_TRUE
{
    logdebug("[%s, %d] insert_value: K_TRUE\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();
    rv->SetBool(true);

    $$ = rv;
} | K_FALSE
{
    logdebug("[%s, %d] insert_value: K_FALSE\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();
    rv->SetBool(false);

    $$ = rv;
} | K_NULL
{
    logdebug("[%s, %d] insert_value: K_NULL\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::Model::AttributeValue * rv = NEW Aws::DynamoDB::Model::AttributeValue();
    rv->SetNull(true);

    $$ = rv;
} | '{' map_element_list '}'
{
    logdebug("[%s, %d] insert_value: '{' map_element_list '}'\n", __FILENAME__, __LINE__);
    $$ = $2;
};

help_command: K_HELP K_UPDATE ';'
{
    printf("UPDATE - Update items in a table\n\n"
           "   UPDATE <name> SET <update_set> [where clause] [ratelimit] \n\n"
           "   UPDATE <name> REMOVE <remove list> [where clause] [ratelimit] \n\n"
           "ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |\n"
           "             WITH RATELIMIT ( RR RCU ) |\n"
           "             WITH RATELIMIT ( WW WCU )\n\n");
    $$ = NULL;
};

update_command: K_UPDATE table_name K_SET update_set optional_where_clause optional_ratelimit ';'
{
    logdebug("[%s, %d] K_UPDATE table_name K_SET update_set_list optional_where_clause ';'\n", __FILENAME__, __LINE__);
    CUpdateCommand * uc = NEW CUpdateCommand;

    uc->set($2, $4, $5, $6);

    FREE($2);
    $$ = uc;
} | K_UPDATE table_name K_REMOVE update_remove_list optional_where_clause optional_ratelimit ';'
{
    logdebug("[%s, %d] K_UPDATE table_name K_REMOVE update_remove_list optional_where_clause ';'\n", __FILENAME__, __LINE__);
    CUpdateCommand * uc = NEW CUpdateCommand;

    uc->remove($2, $4, $5, $6);

    FREE($2);
    $$ = uc;
};

help_command: K_HELP K_UPSERT ';'
{
    printf("UPSERT - Upsert items in a table\n\n"
           "   UPSERT <name> SET <upsert_set> [where clause] [ratelimit]\n\n"
           "ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |\n"
           "             WITH RATELIMIT ( RR RCU ) |\n"
           "             WITH RATELIMIT ( WW WCU )\n\n");

    $$ = NULL;
};

upsert_command: K_UPSERT table_name K_SET update_set optional_where_clause optional_ratelimit ';'
{
    logdebug("[%s, %d] K_UPDATE table_name K_SET update_set_list optional_where_clause optional_ratelimit ';'\n",
             __FILENAME__, __LINE__);

    CUpdateCommand * uc = NEW CUpdateCommand;

    uc->set($2, $4, $5, $6);
    uc->set_upsert();

    FREE($2);
    $$ = uc;
};

update_set: update_set_element
{
    logdebug("[%s, %d] update_set: update_set_element\n", __FILENAME__, __LINE__);

    Aws::Vector<CUpdateSetElement> * us = NEW Aws::Vector<CUpdateSetElement>;

    us->push_back(*$1);
    delete $1;

    $$ = us;
} | update_set ',' update_set_element
{
    logdebug("[%s, %d] update_set: update_set ',' update_set_element\n", __FILENAME__, __LINE__);
    $1->push_back(*$3);

    delete $3;
    $$ = $1;
};

update_remove_list: lee_lhs
{
    logdebug("[%s, %d] update_remove_list: lee_rhs\n", __FILENAME__, __LINE__);

    Aws::Vector<Aws::String> * url = NEW Aws::Vector<Aws::String>;
    url->push_back(*$1);

    delete $1;

    $$ = url;
} | update_remove_list ',' lee_lhs
{
    logdebug("[%s, %d] update_remove_list: update_remove_list ',' lee_rhs\n", __FILENAME__, __LINE__);
    $1->push_back(*$3);

    delete $3;

    $$ = $1;
};

update_set_element: lee_lhs T_EQUAL lee_rhs
{
    logdebug("[%s, %d] update_set_element: lee_lhs T_EQUAL lee_rhs\n", __FILENAME__, __LINE__);
    CUpdateSetElement * use = NEW CUpdateSetElement();

    use->set_lhs($1);
    delete $1;
    use->set_rhs_value($3);
    delete $3;

    FREE($2);

    $$ = use;
} | lee_lhs T_EQUAL use_lhs '+' lee_rhs
{
    logdebug("[%s, %d] update_set_element: lee_lhs T_EQUAL use_lhs '+' lee_rhs\n", __FILENAME__, __LINE__);

    $3->set_lhs($1);
    delete $1;
    $3->set_op("+");
    $3->set_rhs_value($5);
    delete $5;

    FREE($2);
    $$ = $3;
} | lee_lhs T_EQUAL use_lhs '-' lee_rhs
{
    logdebug("[%s, %d] update_set_element: lee_lhs T_EQUAL use_lhs '-' lee_rhs\n", __FILENAME__, __LINE__);

    $3->set_lhs($1);
    delete $1;
    $3->set_op("-");
    $3->set_rhs_value($5);
    delete $5;

    FREE($2);
    $$ = $3;
} | lee_lhs T_EQUAL use_lhs
{
    logdebug("[%s, %d] lee_lhs T_EQUAL use_lhs\n", __FILENAME__, __LINE__);

    $3->set_lhs($1);

    delete $1;
    FREE($2);
    $$ = $3;
};

use_lhs: T_UNQUOTED_STRING
{
    logdebug("[%s, %d] use_lhs: T_UNQUOTED_STRING\n", __FILENAME__, __LINE__);
    CUpdateSetElement * use = NEW CUpdateSetElement();

    use->set_use_lhs($1);
    FREE($1);
    $$ = use;

} | K_IF_NOT_EXISTS '(' lee_lhs ',' lee_rhs ')'
{
    logdebug("[%s, %d] use_lhs: K_IF_NOT_EXISTS '(' lee_lhs ',' lee_rhs ')'\n", __FILENAME__, __LINE__);

    CUpdateSetElement * use = NEW CUpdateSetElement();
    use->set_if_not_exists($3, $5);

    delete $3;
    delete $5;

    $$ = use;
};

help_command: K_HELP K_DELETE ';'
{
    printf("DELETE - Delete items from a table\n\n"
           "   DELETE [ratelimit] FROM <table> [where clause]\n\n"
           "ratelimit := WITH RATELIMIT ( RR RCU, WW WCU ) |\n"
           "             WITH RATELIMIT ( RR RCU ) |\n"
           "             WITH RATELIMIT ( WW WCU )\n\n");

    $$ = NULL;
};

delete_command: K_DELETE K_FROM table_name optional_where_clause optional_ratelimit ';'
{
    logdebug("[%s, %d] K_DELETE K_FROM table_name optional_where_clause optional_ratelimit ';'\n", __FILENAME__, __LINE__);
    CDeleteCommand * dc = NEW CDeleteCommand($3, $4, $5);

    FREE($3);
    $$ = dc;
}

ratelimit: K_WITH K_RATELIMIT '(' T_WHOLE_NUMBER K_RCU ',' T_WHOLE_NUMBER K_WCU ')'
{
    logdebug("[%s, %d] ratelimit: K_WITH K_RATELIMIT '(' T_WHOLE_NUMBER K_RCU ',' T_WHOLE_NUMBER K_WCU ')'\n",
             __FILENAME__, __LINE__);
    CRateLimit * rl = NEW CRateLimit(atof($4), atof($7));
    FREE($4);
    FREE($7);
    $$ = rl;
} | K_WITH K_RATELIMIT '(' T_WHOLE_NUMBER K_WCU ',' T_WHOLE_NUMBER K_RCU ')'
{
    logdebug("[%s, %d] ratelimit: K_WITH K_RATELIMIT '(' T_WHOLE_NUMBER K_WCU ',' T_WHOLE_NUMBER K_RCU ')'\n",
             __FILENAME__, __LINE__);
    CRateLimit * rl = NEW CRateLimit(atof($7), atof($4));
    FREE($4);
    FREE($7);
    $$ = rl;
} | K_WITH K_RATELIMIT '(' T_WHOLE_NUMBER K_RCU ')'
{
    logdebug("[%s, %d] ratelimit: K_WITH K_RATELIMIT '(' T_WHOLE_NUMBER K_RCU ')'\n",
             __FILENAME__, __LINE__);
    CRateLimit * rl = NEW CRateLimit(atof($5), 0);
    FREE($5);
    $$ = rl;
} | K_WITH K_RATELIMIT '(' T_WHOLE_NUMBER K_WCU ')'
{
    logdebug("[%s, %d] ratelimit: K_WITH K_RATELIMIT '(' T_WHOLE_NUMBER K_WCU ')'\n",
             __FILENAME__, __LINE__);
    CRateLimit * rl = NEW CRateLimit(0, atof($5));
    FREE($5);
    $$ = rl;
};

optional_ratelimit: ratelimit
{
    logdebug("[%s, %d] optional_ratelimit: ratelimit\n", __FILENAME__, __LINE__);
    $$ = $1;
} |
{
    logdebug("[%s, %d] optional_ratelimit: <nothing>\n", __FILENAME__, __LINE__);
    $$ = NULL;
};

%%

void yyerror(CCommand **pCommand, const char* s)
{
    extern char * yytext;
    logerror("[%d, %d] %s at %s\n", yylineno, yycolumnno, s, yytext);
    yyclearin;

    if (CDDBSh::getInstance()->abort_on_error())
        exit(1);
}
