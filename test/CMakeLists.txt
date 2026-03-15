# How tests link
add_executable(tests tests.cpp)
# Catch2WithMain supplies its own main function, which deals command line args
# on its own
target_link_libraries(tests PRIVATE gsdb::libgsdb Catch2::Catch2WithMain)
