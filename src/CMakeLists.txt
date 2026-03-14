# use namespaced library targets
add_library(libgsdb libgsdb.cpp)
add_library(gsdb::libgsdb ALIAS libgsdb)

set_target_properties(libgsdb PROPERTIES OUTPUT_NAME gsdb)

# PUBLIC visibility: any targets that compile against libgsdb will inherit this
# property.
target_compile_features(libgsdb PUBLIC cxx_std_23)

# specify the `include` directories for `libgsdb`
target_include_directories(
  libgsdb
  # private for gsdb/src/include, as dependent targets should not transitively
  # add it to their include directories
  PRIVATE include
  # we DO want dependent targets to inherit gsdb/include
  PUBLIC ${CMAKE_SOURCE_DIR}/include)
