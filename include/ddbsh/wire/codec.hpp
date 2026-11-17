// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// codec.hpp — the SDK-free AWS JSON 1.0 request codec (the ddbsh-wire
// component, alias ddbsh::wire). Phase 6 (P6a: the serialize side).
//
// SDK-FREE BOUNDARY (D5 / D-P6-13): this header requires only a C++23 toolchain
// and the engine's SDK-free request structs (ddbsh/iddbclient.hpp). It never
// mentions an Aws:: type. ddbsh-wire is the JSON-string transport counterpart
// to ddbsh-aws (the AWS SDK transport): both are IDDBClient providers over the
// same SDK-free POD structs, neither leaks into the engine (tenet 3). The
// browser (Emscripten) target links ddbsh-wire and NOT ddbsh-aws.
//
// WHY THIS EXISTS (correcting D-P6-3 -> D-P6-3′). The only AWS-JSON-1.0
// serializer in the tree before Phase 6 was the AWS SDK's own
// SerializePayload(), reached through ddbsh::aws (DDBSH_BUILD_AWS). A
// DDBSH_BUILD_AWS=OFF build — the Emscripten target — has NO serializer at all.
// ddbsh-wire supplies one, SDK-free, for both the live wire transport and
// SDK-free EXPLAIN.
//
// BYTE-PARITY CONTRACT (the audit gate, P6-02 §9.1). serialize(in).body must be
// byte-identical to the AWS SDK adapter's SerializePayload() for the same
// request, so (a) the live path sends every field EXPLAIN knows about and (b)
// EXPLAIN output is identical across the CLI and the web app. SerializePayload
// emits the SDK model's Jsonize() in member-declaration order, formatted by
// cJSON's readable printer (tab indentation, `"key":\t` after each key, `,\n`
// between object members, `, ` between array elements, no trailing newline).
// This codec reproduces that byte-for-byte; the differential test
// (tests/wire_serialize_diff_test.cpp) pins it against the real SDK.

#ifndef DDBSH_WIRE_CODEC_HPP
#define DDBSH_WIRE_CODEC_HPP

#include <optional>
#include <string>
#include <vector>

#include "ddbsh/iddbclient.hpp"
namespace ddbsh::wire {

/// A serialized DynamoDB request ready for the wire transport.
///
/// `operation` is the X-Amz-Target suffix (e.g. "GetItem", "Query") — the same
/// token ExtendDB's submit(operation, body) and a SigV4 X-Amz-Target header
/// expect. `body` is the AWS-JSON-1.0 request body, byte-identical to the AWS
/// SDK's SerializePayload() for the equivalent request (the P6-02 §9.1
/// contract).
struct WireRequest {
    std::string operation;
    std::string body;
};

// ---- Data-plane serializers (P6a) ------------------------------------------
// One overload per in-scope request struct. Each builds the AWS-JSON-1.0 body
// in the SDK's Jsonize member order and sets `operation` to the op's
// X-Amz-Target suffix. The control-plane, transaction, and vector ops follow
// below (P6c).

WireRequest serialize(const GetItemIn& in);
WireRequest serialize(const QueryIn& in);
WireRequest serialize(const ScanIn& in);
WireRequest serialize(const PutItemIn& in);
WireRequest serialize(const UpdateItemIn& in);
WireRequest serialize(const DeleteItemIn& in);

// ---- Control-plane, backup/restore, transaction, vector serializers (P6c) --
// The remaining in-scope request structs. Each builds the AWS-JSON-1.0 body in
// the SDK's Jsonize member order and sets `operation` to the op's X-Amz-Target
// suffix. Byte-identical to the SDK adapter's SerializePayload() for the
// translated request (the P6-02 §9.1 contract), pinned by the extended
// differential gate (tests/wire_serialize_diff_test.cpp). The three ops not in
// the ExplainRequest variant (DescribeLimits, ListTables, ListBackups — the
// SHOW commands have no EXPLAIN) are reached only by the WireClient execution
// path (P6c-b); the differential gate pins them against a directly-built SDK
// request.

WireRequest serialize(const CreateTableIn& in);
WireRequest serialize(const UpdateTableIn& in);
WireRequest serialize(const DeleteTableIn& in);
WireRequest serialize(const UpdateTimeToLiveIn& in);
WireRequest serialize(const UpdateContinuousBackupsIn& in);
WireRequest serialize(const DescribeTableIn& in);
WireRequest serialize(const DescribeTimeToLiveIn& in);
WireRequest serialize(const DescribeContinuousBackupsIn& in);
WireRequest serialize(const DescribeTableReplicaAutoScalingIn& in);
WireRequest serialize(const DescribeLimitsIn& in);
WireRequest serialize(const ListTablesIn& in);
WireRequest serialize(const ListBackupsIn& in);
WireRequest serialize(const CreateBackupIn& in);
WireRequest serialize(const DeleteBackupIn& in);
WireRequest serialize(const DescribeBackupIn& in);
WireRequest serialize(const RestoreFromBackupIn& in);
WireRequest serialize(const RestoreToPitrIn& in);
WireRequest serialize(const TransactGetItemsIn& in);
WireRequest serialize(const TransactWriteItemsIn& in);
WireRequest serialize(const SearchVectorsIn& in);

// ---- ListTables cursor-aware serializer (P6c-b paging) ---------------------
// ListTablesIn carries no cursor field, but the SHOW TABLES executor expects
// the full table list and the AWS adapter pages internally on
// LastEvaluatedTableName. The WireClient reproduces that paging loop (it is the
// one op whose request struct cannot express the cursor), so it needs a
// serializer that can thread ExclusiveStartTableName for the second and later
// pages. serialize(const ListTablesIn&) is exactly serialize_list_tables with
// no cursor (the empty first-page body, pinned by the differential gate);
// passing a cursor emits the SDK's ListTablesRequest body with
// ExclusiveStartTableName set (Jsonize order). The adapter never sets Limit for
// ListTables, so neither does this.
WireRequest serialize_list_tables(const std::optional<std::string>& exclusive_start_table_name);

// ---- Data-plane response parsers (P6b) -------------------------------------
// The inverse of the serializers above: turn an AWS-JSON-1.0 response `body`
// into the engine's SDK-free *Out struct, field-identical to what the AWS
// adapter's from_sdk_* helpers produce (aws/aws_ddb_client.cpp, the fidelity
// oracle). Each parse_*:
//   1. reads the body to a JSON tree (wire/json_reader.hpp);
//   2. if the top-level object carries a `__type` member, returns the mapped
//      ddbsh::Error (the body is an error document — a DynamoDB/ExtendDB
//      success response never carries a top-level `__type`);
//   3. otherwise fills the *Out, setting item/attributes/last_evaluated_key
//      ONLY when the member is present and non-empty, and consumed_capacity via
//      the same `any`-flag logic as from_sdk_consumed_capacity.
// AttributeValue mapping is the exact inverse of from_sdk_value; binary (B/BS)
// is carried verbatim (canonical standard-base64, as the serializer emits it).
// A malformed body or a malformed AttributeValue yields Errc::service_error.

Result<GetItemOut> parse_get_item(const std::string& body);
Result<QueryOut> parse_query(const std::string& body);
Result<ScanOut> parse_scan(const std::string& body);
Result<PutItemOut> parse_put_item(const std::string& body);
Result<UpdateItemOut> parse_update_item(const std::string& body);
Result<DeleteItemOut> parse_delete_item(const std::string& body);

// ---- Control-plane / backup-restore / transaction / vector response parsers (P6c-b) -------
// The inverse of the P6c-a serializers, each field-identical to the AWS
// adapter's response assembly (aws/aws_ddb_client.cpp, the fidelity oracle).
// Every parser first checks the top-level `__type` (error document -> mapped
// Error) before reading success members; a malformed body or AttributeValue
// yields Errc::service_error (tenet 6). Enum-valued fields are read as the raw
// response NAME string the SDK mapper would have produced ("ACTIVE",
// "PAY_PER_REQUEST", "HASH", ...), so no enum<->name table is needed (§11).
// Response timestamps are epoch-seconds JSON numbers formatted to ISO-8601 via
// wire/datetime.hpp, matching the adapter's ToGmtString(ISO_8601).

// Data-returning parsers.
Result<DescribeTableOut> parse_describe_table(const std::string& body);
Result<TimeToLiveOut> parse_describe_time_to_live(const std::string& body);
Result<ContinuousBackupsOut> parse_describe_continuous_backups(const std::string& body);
Result<TableReplicaAutoScalingOut> parse_describe_table_replica_auto_scaling(
    const std::string& body);
Result<DescribeLimitsOut> parse_describe_limits(const std::string& body);
Result<DescribeBackupOut> parse_describe_backup(const std::string& body);
Result<ListBackupsOut> parse_list_backups(const std::string& body);
Result<SearchVectorsOut> parse_search_vectors(const std::string& body);
Result<TransactGetItemsOut> parse_transact_get_items(const std::string& body);

/// One page of a ListTables response. ListTablesOut carries no cursor field (the
/// AWS adapter pages internally), so the wire parser surfaces one page and the
/// WireClient drives the paging loop (§4.2): it appends `table_names` and, while
/// `last_evaluated_table_name` is set, re-issues ListTables with that cursor.
struct ListTablesPage {
    std::vector<std::string> table_names;
    std::optional<std::string> last_evaluated_table_name;  ///< empty/absent => last page
};
Result<ListTablesPage> parse_list_tables(const std::string& body);

// Token-only acknowledgements. The mutating control-plane ops and
// TransactWriteItems ignore their success response body entirely (the adapter
// only checks IsSuccess()); the wire parser likewise returns the empty *Out on
// success and the mapped Error on a `__type` document.
Result<CreateTableOut> parse_create_table(const std::string& body);
Result<UpdateTableOut> parse_update_table(const std::string& body);
Result<DeleteTableOut> parse_delete_table(const std::string& body);
Result<UpdateTimeToLiveOut> parse_update_time_to_live(const std::string& body);
Result<UpdateContinuousBackupsOut> parse_update_continuous_backups(const std::string& body);
Result<CreateBackupOut> parse_create_backup(const std::string& body);
Result<DeleteBackupOut> parse_delete_backup(const std::string& body);
Result<RestoreFromBackupOut> parse_restore_from_backup(const std::string& body);
Result<RestoreToPitrOut> parse_restore_to_pitr(const std::string& body);
Result<TransactWriteItemsOut> parse_transact_write_items(const std::string& body);

}  // namespace ddbsh::wire

#endif  // DDBSH_WIRE_CODEC_HPP
