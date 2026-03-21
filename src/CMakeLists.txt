# Use namespaced library targets. Creates _static_ library target.
add_library(libgsdb process.cpp pipe.cpp)
# `gsdb::libgsdb` is an alias for `libgsdb`
add_library(gsdb::libgsdb ALIAS libgsdb)

# CMake automatically prepends `lib` on Unix. This is to override this behavior
# so that output target name is libgsdb` instead of `liblibgsdb`
set_target_properties(libgsdb PROPERTIES OUTPUT_NAME gsdb)

# PUBLIC visibility: any targets that compile against libgsdb will inherit this
# property.
target_compile_features(libgsdb PUBLIC cxx_std_23)

# specify the `include` directories for `libgsdb`
target_include_directories(
  libgsdb
  PRIVATE ${CMAKE_SOURCE_DIR}/src/include # internal headers only visible when
                                          # compiling libgsdb itsef
  PUBLIC # if user is accessing an installed version of `libgsdb`, the public
         # headers live in the `include` dir relative to the installed location.
         # Propagated to anything that links against libgsdb. This is why
         # `tools/gsdb.cpp` and `test/tests.cpp` can `#include
         # <libgsdb/libgsdb.hpp>`
         $<INSTALL_INTERFACE:include>
         # if user accessing a version in the build tree (eg. if linking the
         # gsdb executable), we should use the `include` dir inside the source
         # tree
         $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

include(GNUInstallDirs) # contains variables that specify common installation
                        # directories `/include` for headers `/lib` for
                        # libraries

# Tells CMake where to put files when running `cmake --install build` (or `make
# install`)
install(
  TARGETS libgsdb # we are installing `libgsdb`
  EXPORT gsdb-targets # export target
  LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR} # shared libraries (`.so` on Linux;
                                        # `.dylib` on macOS) go to `lib/`
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} # statis libraries (`.a`) go to
                                              # `lib/`
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} # DLL on Windows (`.dll`) go to
                                              # `bin/`
  INCLUDES
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}) # Does _NOT_ copy any headers. It
                                           # tells the exported target that when
                                           # consumers find it via
                                           # `find_package`, the included
                                           # directory is `<prefix>/include/`
                                           # (/usr/local/include/`). It's the
                                           # installed equivalent of
                                           # `$<INSTALL_INTERFACE:include>`
                                           # generator expression. The actual
                                           # header copying is doen separately
                                           # by the `install(DIRECTORY ...)`
                                           # command below:

# Install public headers. Copy everything under `include/` (i.e.
# `include/libgsdb/libgsdb.hpp`) into the install prefix's include directory
# (typically `/usr/local/include/`). So after install, header ends up at
# `/usr/local/include/libgsdb/libgsdb.hpp`
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Make libgsdb importable by other CMake projects. Other projects can do:
# `find_package(gsdb REQUIRED)` and `target_link_libraries(myapp PRIVATE
# gsdb::libgsdb)`
install(
  EXPORT gsdb-targets
  # cmake expects this file to be named either `<project>-config.cmake` or
  # `<project>Config.cmake`
  FILE gsdb-config.cmake
  NAMESPACE gsdb:: # exported targets should be under the `gsdb` namespace so
                   # that users can link to the `gsdb::libgsdb` target
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gsdb)
