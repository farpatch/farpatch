
#define MCOUNT_INSN_SIZE 3

.macro abi_entry_default
.endm

.macro abi_ret_default
   ret
.endm

   .section .text
   .align 4
   .global  _mcount
   .type   _mcount, @function
_mcount:
#if defined(__XTENSA_WINDOWED_ABI__)
	abi_entry_default

	movi	a4, ftrace_trace_function
	l32i	a4, a4, 0
	movi	a3, ftrace_stub
	bne	a3, a4, 1f
	abi_ret_default

1: 	xor	a7, a2, a1
	movi	a3, 0x3fffffff
	and	a7, a7, a3
	xor	a7, a7, a1

	xor	a6, a0, a1
	and	a6, a6, a3
	xor	a6, a6, a1
	addi	a6, a6, -MCOUNT_INSN_SIZE
	callx4	a4

	abi_ret_default
#elif defined(__XTENSA_CALL0_ABI__)
	abi_entry_default

	movi	a9, gmon_trace_function
	l32i	a9, a9, 0
	movi	a11, gmon_stub
	bne	a9, a11, 1f
	abi_ret_default

1:	abi_entry(28)
	s32i	a0, sp, 0
	s32i	a2, sp, 4
	s32i	a3, sp, 8
	s32i	a4, sp, 12
	s32i	a5, sp, 16
	s32i	a6, sp, 20
	s32i	a7, sp, 24
	addi	a2, a10, -MCOUNT_INSN_SIZE
	callx0	a9
	l32i	a0, sp, 0
	l32i	a2, sp, 4
	l32i	a3, sp, 8
	l32i	a4, sp, 12
	l32i	a5, sp, 16
	l32i	a6, sp, 20
	l32i	a7, sp, 24
	abi_ret(28)
#else
#error Unsupported Xtensa ABI
#endif
    .size _mcount, .-_mcount