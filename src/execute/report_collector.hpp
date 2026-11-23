// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// report_collector.hpp — fold streamed report fragments into one report.
//
// PRIVATE engine header (not installed). SHOW TABLES, SHOW BACKUPS, SHOW LIMITS
// and DESCRIBE emit one ReportOutcome fragment per unit of output as the API
// responses arrive (P12, D-P12-11). Their buffering forms, and the buffering
// Session::execute(stmt), still return the single ReportOutcome these commands
// returned before P12 (D-P12-15). ReportCollector is the sink that does that
// fold: it appends every fragment's lines, in order, to one report.

#ifndef DDBSH_SRC_EXECUTE_REPORT_COLLECTOR_HPP
#define DDBSH_SRC_EXECUTE_REPORT_COLLECTOR_HPP

#include <variant>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/outcome_sink.hpp"

namespace ddbsh::engine {

/// An OutcomeSink that concatenates the lines of every ReportOutcome fragment
/// it receives into `report`. A report-streaming executor emits only
/// ReportOutcome fragments; any other arm is ignored (it cannot occur).
struct ReportCollector : OutcomeSink {
    ReportOutcome report;

    void emit(const ExecOutcome& fragment) override {
        if (const auto* part = std::get_if<ReportOutcome>(&fragment)) {
            report.lines.insert(report.lines.end(), part->lines.begin(), part->lines.end());
        }
    }
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_REPORT_COLLECTOR_HPP
