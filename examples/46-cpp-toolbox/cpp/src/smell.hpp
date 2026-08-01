#pragma once

#include <string>

// Deliberate, behavior-preserving smell for the clang-tidy gate (ch46 Sec.
// "Errors, three ways" -- the tidy finding). `label` is only ever read, so
// taking it by value forces an avoidable copy on every call; clang-tidy's
// performance-unnecessary-value-param flags this parameter, and the fix is
// `const std::string&`. Left as `std::string` on purpose -- do not "fix"
// this without re-pinning the tidy chapter section.
std::string decorate_label(std::string label);
