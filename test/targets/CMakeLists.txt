function(add_test_cpp_target name)
  add_executable(${name} "${name}.cpp")
  # cmake-format: off
  # `-pie`: tells compiler/assembler to generate position-independent executable code 
  # -g: generate debug info
  # -O0: disable optimizations
  # cmake-format: on
  target_compile_options(${name} PRIVATE -g -O0 -pie)

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

add_test_asm_target(reg_write)
add_test_asm_target(reg_read)
