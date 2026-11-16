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

#include <string>

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
// X-Amz-Target suffix. The control-plane, transaction, and vector ops land in
// later P6 sub-phases (P6c).

WireRequest serialize(const GetItemIn& in);
WireRequest serialize(const QueryIn& in);
WireRequest serialize(const ScanIn& in);
WireRequest serialize(const PutItemIn& in);
WireRequest serialize(const UpdateItemIn& in);
WireRequest serialize(const DeleteItemIn& in);

}  // namespace ddbsh::wire

#endif  // DDBSH_WIRE_CODEC_HPP
