# Isolated Conan sub-target: only add_subdirectory()'d by the parent
# cpp/CMakeLists.txt when TOOLBOX_ENABLE_CONAN=ON (set by the "conan"
# CMakePresets.json preset). find_package(fmt) resolves against the
# CMakeDeps config Conan generates into build/conan/generators/.
find_package(fmt REQUIRED)

add_executable(toolbox-conan-demo src/main.cpp)
target_link_libraries(toolbox-conan-demo PRIVATE fmt::fmt)
target_compile_features(toolbox-conan-demo PRIVATE cxx_std_23)
