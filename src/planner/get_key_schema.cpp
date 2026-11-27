// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// get_key_schema.cpp — see get_key_schema.hpp.

#include "planner/get_key_schema.hpp"

namespace ddbsh::engine {

namespace {

ResolvedKeySchema from_table_key(const TableKeySchema& ks, const DescribeTableOut& desc) {
    const TableKeySchema& table = desc.key_schema;
    ResolvedKeySchema out{ks.hash_keys, ks.range_keys};
    out.table_keys = table.hash_keys;
    out.table_keys.insert(out.table_keys.end(), table.range_keys.begin(), table.range_keys.end());
    for (const auto& def : desc.attribute_definitions) {
        out.key_types.emplace(def.name, def.type);
    }
    return out;
}

}  // namespace

Result<ResolvedKeySchema> get_key_schema(IDDBClient& client, const std::string& table,
                                         const std::string& index) {
    auto described = client.describe_table(DescribeTableIn{table});
    if (!described.has_value()) {
        return fail(described.error());
    }
    const DescribeTableOut& desc = described.value();

    if (index.empty()) {
        return from_table_key(desc.key_schema, desc);
    }

    // A named index: LSIs are searched before GSIs (arch §10).
    for (const auto& lsi : desc.local_secondary_indexes) {
        if (lsi.index_name == index) {
            return from_table_key(lsi.key_schema, desc);
        }
    }
    for (const auto& gsi : desc.global_secondary_indexes) {
        if (gsi.index_name == index) {
            return from_table_key(gsi.key_schema, desc);
        }
    }

    return fail(Errc::no_such_table, "Unable to find primary key for " + table + "." + index);
}

}  // namespace ddbsh::engine
