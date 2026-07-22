cmake_minimum_required(VERSION 3.25)
project(lsp_otel CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# opentelemetry-cpp comes from Conan (see conanfile.txt): the CMakeDeps
# generator maps the package's cpp_info components onto
# opentelemetry-cpp::<component> imported targets. "trace" and "metrics"
# each carry their API *and* SDK pieces (there is no separate ::sdk
# target in this recipe); the exporters are named exporter_otlp_http and
# exporter_otlp_http_metric, not otlp_http_exporter.
find_package(opentelemetry-cpp CONFIG REQUIRED)
find_package(Threads REQUIRED)

# The static archives conan pulls in have a genuine link-time cycle: e.g.
# libopentelemetry_exporter_otlp_http_client.a calls
# sdk::common::Base64Escape (defined in libopentelemetry_common.a), while
# conan's own generated CMake target graph makes nearly every component
# depend back on a shared aggregate target that includes the others --
# a cycle at the CMake target-dependency level, not just the archive-
# symbol level, so plain target_link_libraries ordering (or even CMake's
# $<LINK_GROUP:RESCAN,...>, which refuses a group whose members depend
# back on the group itself) cannot fix it. GNU ld's own answer to
# circular static archives -- wrap the WHOLE library list in
# --start-group/--end-group so it keeps re-scanning until every symbol
# resolves -- has to be spliced directly into the link rule template,
# because CMake always emits the fully-resolved transitive library list
# (<LINK_LIBRARIES>) as one block *after* anything passed to
# target_link_libraries(), so no argument position in that call can
# bracket it.
set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_CXX_COMPILER> <FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group"
)

add_executable(app
    src/main.cpp
    src/telemetry.cpp
)

target_link_libraries(app PRIVATE
    opentelemetry-cpp::api
    opentelemetry-cpp::trace
    opentelemetry-cpp::metrics
    opentelemetry-cpp::resources
    opentelemetry-cpp::exporter_otlp_http
    opentelemetry-cpp::exporter_otlp_http_metric
    Threads::Threads
)
