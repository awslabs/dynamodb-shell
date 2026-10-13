// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// table_schema.hpp — SDK-free value types describing a DynamoDB table
// definition (CREATE TABLE / the CreateTable request shape).
//
// Part of the engine library's SDK-free public API surface (D11): no AWS SDK
// type appears here. These mirror the DynamoDB table-definition shapes the
// original ddbsh built directly as Aws:: model objects (parser.y
// create_table_command). They are the SINGLE source of truth shared by two
// layers:
//   * the parse result — engine::CreateTableCmd (src/parser/ast.hpp) aliases
//     these in namespace ddbsh::engine, so the grammar builds them while the
//     command model stays SDK-free; and
//   * the client seam — ddbsh::CreateTableIn (ddbsh/iddbclient.hpp) carries
//     them to IDDBClient::create_table, which the CLI-side AwsDDBClient
//     translates to Aws::DynamoDB::Model::CreateTableRequest.
// Keeping one definition (rather than a duplicate per layer) avoids the
// "two systems describing the same thing" smell while holding the D11 boundary.

#ifndef DDBSH_TABLE_SCHEMA_HPP
#define DDBSH_TABLE_SCHEMA_HPP

#include <optional>
#include <string>
#include <vector>

namespace ddbsh {

/// A key attribute's scalar type (CREATE TABLE attribute definitions / the
/// original ScalarAttributeType). N=number, S=string, B=binary.
enum class ScalarType { n, s, b };

/// One attribute definition: name + scalar type.
struct AttributeDef {
    std::string name;
    ScalarType type = ScalarType::s;
};

/// A key schema: a required HASH attribute and an optional RANGE attribute.
/// The original always emits [HASH, RANGE] order regardless of written order
/// (parser.y key_schema reorders), which this shape reproduces.
struct KeySchema {
    std::string hash;
    std::optional<std::string> range;
};

/// Table/GSI billing mode (the original BillingMode).
enum class BillingMode { pay_per_request, provisioned };

/// On-demand (PAY_PER_REQUEST) max request units. Both nullopt => a bare
/// `BILLING MODE ON DEMAND` (empty OnDemandThroughput); both set => an explicit
/// `THROUGHPUT (r,w)`; both -1 => `THROUGHPUT UNLIMITED` (matching the
/// original's -1 sentinel, arch §8).
struct OnDemandThroughput {
    std::optional<long long> max_read_request_units;
    std::optional<long long> max_write_request_units;
};

/// Billing mode plus the throughput that goes with it. For `provisioned`, rcu/
/// wcu carry the ProvisionedThroughput; for `pay_per_request`, `od` carries the
/// (optional) OnDemandThroughput.
struct BillingModeAndThroughput {
    BillingMode mode = BillingMode::pay_per_request;
    long long rcu = 0;
    long long wcu = 0;
    OnDemandThroughput od;
};

/// WARM THROUGHPUT (r RCU, w WCU) — the original WarmThroughput.
struct WarmThroughput {
    long long read_units_per_second = 0;
    long long write_units_per_second = 0;
};

/// Index projection type (the original ProjectionType).
enum class ProjectionType { all, keys_only, include };

/// An index projection: type plus the non-key attributes for INCLUDE.
struct Projection {
    ProjectionType type = ProjectionType::all;
    std::vector<std::string> non_key_attributes;  ///< only for `include`
};

/// A global secondary index definition.
struct Gsi {
    std::string name;
    KeySchema key_schema;
    Projection projection;
    std::optional<BillingModeAndThroughput> billing;
    std::optional<WarmThroughput> warm;
};

/// A local secondary index definition.
struct Lsi {
    std::string name;
    KeySchema key_schema;
    Projection projection;
};

/// Stream view type (the original StreamViewType).
enum class StreamViewType { keys_only, new_image, old_image, new_and_old_images };

/// STREAM (...) | STREAM DISABLED. `view_type` is meaningful only when enabled.
struct StreamSpec {
    bool enabled = false;
    StreamViewType view_type = StreamViewType::keys_only;
};

/// Server-side-encryption key type (the original SSEType).
enum class SseType { kms, aes256 };

/// SSE DISABLED | SSE (KEYID <id> KMS|AES256). `kms_master_key_id`/`sse_type`
/// are set only when enabled.
struct SseSpec {
    bool enabled = false;
    std::optional<std::string> kms_master_key_id;
    std::optional<SseType> sse_type;
};

/// Table class (the original TableClass).
enum class TableClass { standard, standard_infrequent_access };

/// A table tag (key:value).
struct Tag {
    std::string key;
    std::string value;
};

}  // namespace ddbsh

#endif  // DDBSH_TABLE_SCHEMA_HPP
