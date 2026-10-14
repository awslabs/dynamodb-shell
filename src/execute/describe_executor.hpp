// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// describe_executor.hpp — execute DESCRIBE <table> behind IDDBClient.
//
// PRIVATE engine header (not installed). SDK-free port of the original
// command/describe.cpp (arch §7): a DescribeTable (+ a DescribeTimeToLive that
// the original fetches but does not display in non-EXPLAIN mode), then the
// multi-section report — basic info, billing/throughput, warm throughput, PITR
// (DescribeContinuousBackups), the per-GSI/LSI detail, stream, table class, SSE,
// and replica auto-scaling (DescribeTableReplicaAutoScaling). The result is a
// ReportOutcome (the executor shapes the lines; the Renderer joins them —
// P2a-03 §4.2). The engine never writes to a stream.
//
// EXPLAIN of a DESCRIBE is NOT handled here (it renders the four serialized
// requests instead of calling them — W6); DESCRIBE is routed here only in the
// non-EXPLAIN case.
//
// D12: never aborts/throws across the boundary. A DescribeTable failure is
// returned as an Error (the original's has_dtr()==false => run() returns -1
// with no output); the PITR and auto-scaling calls are best-effort — their
// failures skip the corresponding section rather than failing the command,
// matching the original's IsSuccess() guards.

#ifndef DDBSH_SRC_EXECUTE_DESCRIBE_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_DESCRIBE_EXECUTOR_HPP

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Execute a (non-EXPLAIN) DESCRIBE. Returns a ReportOutcome with the report
/// lines, or an Error if the table cannot be described.
Result<ExecOutcome> execute_describe(const DescribeCmd& cmd, IDDBClient& client);

/// EXPLAIN a DESCRIBE: serialize the four requests the report WOULD issue, in
/// call order, into an ExplainOutcome — DescribeTable and DescribeTimeToLive in
/// COMPACT form (the original used strip_newlines for these two, via
/// CDescribeHelper), then DescribeContinuousBackups and
/// DescribeTableReplicaAutoScaling in PRETTY form (explain_string, describe.cpp).
/// Makes no service call (every request carries only the table name).
Result<ExecOutcome> execute_describe_explain(const DescribeCmd& cmd, IDDBClient& client);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_DESCRIBE_EXECUTOR_HPP
