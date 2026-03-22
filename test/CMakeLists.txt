# How tests link
add_executable(tests tests.cpp)
# Catch2WithMain supplies its own main function, which deals command line args
# on its own
target_link_libraries(tests PRIVATE gsdb::libgsdb Catch2::Catch2WithMain)

add_subdirectory("targets")

# CMAKE_CURRENT_BINARY_DIR is the build-tree counterpart of the `CMakeLists.txt`
# currently being processed
target_compile_definitions(
  tests PRIVATE TARGETS_DIR="${CMAKE_CURRENT_BINARY_DIR}/targets")
