# How the CLI links.

# create target `gsdb` that represents an executable file, also called gsdb
add_executable(gsdb gsdb.cpp)

# cmake-format: off
# dependency on `libgsdb`:
#   - add the public `include` directory to the include paths for the build 
#   - link against the library
# cmake-format: on
target_link_libraries(gsdb PRIVATE gsdb::libgsdb PkgConfig::libedit)

# install `gsdb` CLI tool to the default binary directory
include(GNUInstallDirs)
install(TARGETS gsdb RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
