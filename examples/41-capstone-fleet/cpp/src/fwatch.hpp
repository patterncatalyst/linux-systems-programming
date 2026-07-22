// fwatch.hpp — the book's recurring file watcher, reduced to what the fleet
// needs (see the .cpp for the Landlock rationale carried over from ch33).
#pragma once

#include <string>

namespace fwatch {

int snapshot(const std::string& dir);
int watch(const std::string& dir, bool sandbox, int timeout_ms);

} // namespace fwatch
