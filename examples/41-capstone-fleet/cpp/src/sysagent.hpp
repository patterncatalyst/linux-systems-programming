// sysagent.hpp — the book's /proc-reading metrics agent (ch36), reduced to
// three signals (cpu%, mem%, load1) exported both as a stdout line and, when
// OTEL_EXPORTER_OTLP_ENDPOINT is set, as OTel gauges over OTLP. See
// sysagent.cpp for the /proc/stat delta math, ported field-for-field from the
// Go reference (deliberately simpler than ch36's fuller procfs.cpp: this
// capstone only ever needed the three USE-method numbers pmon's health line
// and the chaos drill care about).
#pragma once

#include <string>

#include "telemetry.hpp"

namespace sysagent {

int run(const std::string& node, int interval_ms, bool once, telemetry::Handle& tel);

// saturate — chaos helper: busy-spin threads (cpu) or hold touched memory
// (mem) for `seconds`, so a concurrently-running sysagent's cpu_pct/mem_pct
// visibly rises.
int saturate(const std::string& resource, int seconds, int workers, int mb);

} // namespace sysagent
