# declare symbol main so that the linker can find the `main` function 
# when we link an executable from this code
.global main

.section .data
# .asciz: encodes the string into ASCII with a null terminator
# %#x: the `printf` format string for hexadecimal integers
hex_format: .asciz "%#x"
float_format: .asciz "%.2f"
long_float_format: .asciz "%.2Lf" # long float

.section .text

.macro trap
    # trap 
    # syscall ID for `kill` is 62
    movq $62, %rax 
    # syscall expects first arg in `%rdi`
    movq %r12, %rdi 

    # second arg of `kill` - the signal ID we want to send 
    # signal ID for `SIGTRAP` is 5
    movq $5, %rsi
    syscall
.endm

main:
    # rbp points to the start of the stack frame for the caller 
    # save it on the stack
    push %rbp 
    movq %rsp, %rbp 

    # get PID 
    # use the `getpid` syscall, which has syscall ID 39
    movq $39, %rax
    syscall
    movq %rax, %r12

    trap

    # print contents of %rsi 
    # leaq: load effective address quadword
    # put address of `hex_format` into `rdi`
    # `label(%rip)`: RIP-relative addressing, required when passed the `-pie` flag
    leaq hex_format(%rip), %rdi
    # In SYSV ABI, vararg functions expects `%rax` to contain 
    # the number of vector registers that are involved in passing arguments
    movq $0, %rax
    # @plt: **procedure linkage table** - call functions defined in shared libs
    call printf@plt
    # flush all streams
    movq $0, %rdi
    call fflush@plt

    # stops the process after it has printed %rsi via printf
    # gives debugger a chance to inspect the result
    trap

    # print contents of mm0
    movq %mm0, %rsi
    leaq hex_format(%rip), %rdi
    movq $0, %rax
    call printf@plt
    movq $0, %rdi
    call fflush@plt

    trap

    # print contents of xmm0
    leaq float_format(%rip), %rdi
    # keep the value to print in xmm0, and write 1 into %rax to tell 
    # printf that there is a vector argument
    movq $1, %rax
    call printf@plt
    movq $0, %rdi
    call fflush@plt

    trap

    # print contents of %st0
    #
    # allocate 16 bytes on stack to store contents of st0
    # x64 stack grows _downwards_, thus subtraction
    subq $16, %rsp 
    # pop st0 from the top of FPU stack
    fstpt (%rsp)
    leaq long_float_format(%rip), %rdi
    movq $0, %rax
    call printf@plt
    movq $0, %rdi
    call fflush@plt
    # cleanup stack allocation
    addq $16, %rsp

    trap

    popq %rbp 
    movq $0, %rax
    ret
