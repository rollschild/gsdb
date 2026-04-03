# declare symbol main so that the linker can find the `main` function 
# when we link an executable from this code
.global main

.section .data
my_double: .double 64.125

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

    # store to %r13 
    movq $0xcafecafe, %r13
    trap # so that we can read it from the debugger

    # store to r13b
    movb $42, %r13b # subregister
    trap

    # store to mm0
    movq $0xdeadbeef, %r13
    movq %r13, %mm0 
    trap

    # store to xmm0
    movsd my_double(%rip), %xmm0
    trap

    # store to st0 
    # empty MMX technology state, to clear the MMX state
    # as MMX and x87 share registers
    emms 
    # fldl: load floating-point value
    # to push a floating-point number to FPU stack
    fldl my_double(%rip)
    trap

    popq %rbp 
    movq $0, %rax
    ret
