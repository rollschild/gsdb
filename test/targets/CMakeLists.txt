function(add_test_cpp_target name)
  add_executable(${name} "${name}.cpp")
  # cmake-format: off
  # `-pie`: tells compiler/assembler to generate position-independent executable code 
  # -g: generate debug info
  # -O0: disable optimizations
  # cmake-format: on
  # `-gdwarf-4`: outputs DWARF 4 information
  target_compile_options(${name} PRIVATE -g -O0 -pie -gdwarf-4)

  # Ensure whenever we build the `tests` target, we also build the test programs
  add_dependencies(tests ${name})
endfunction()

function(add_test_asm_target name)
  add_executable(${name} "${name}.s")
  target_compile_options(${name} PRIVATE -pie)
  add_dependencies(tests ${name})
endfunction()

add_test_cpp_target(run_endlessly)
add_test_cpp_target(end_immediately)
add_test_cpp_target(hello_gsdb)
add_test_cpp_target(memory)
add_test_cpp_target(anti_debugger)
add_test_cpp_target(overloaded)
add_test_cpp_target(step)
add_test_cpp_target(marshmallow)
add_test_cpp_target(multi_threaded)

# SHARED: build a shared lib, rather than a static one
add_library(meow SHARED "libmeow.cpp")

add_test_asm_target(reg_write)
add_test_asm_target(reg_read)

add_executable(multi_cu multi_cu_main.cpp multi_cu_other.cpp)
target_compile_options(multi_cu PRIVATE -g -O0 -pie -gdwarf-4)
add_dependencies(tests multi_cu)

# -fPIC: tell compiler to output position-independent code
target_compile_options(meow PRIVATE -g -O0 -fPIC -gdwarf-4)
target_link_libraries(marshmallow PRIVATE meow)

target_link_libraries(multi_threaded pthread)
