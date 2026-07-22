cmake_minimum_required(VERSION 3.25)
project(capstone_fleet CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(opentelemetry-cpp CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(app
    src/main.cpp
    src/proto.cpp
    src/telemetry.cpp
    src/caps.cpp
    src/chatterd.cpp
    src/sysagent.cpp
    src/fwatch.cpp
    src/pmon.cpp
)

target_link_libraries(app PRIVATE
    opentelemetry-cpp::trace
    opentelemetry-cpp::metrics
    opentelemetry-cpp::otlp_http_exporter
    opentelemetry-cpp::otlp_http_metric_exporter
    Threads::Threads
)
