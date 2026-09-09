# ceres-core - the contract everything else shares.
#
# Three things, and none of them execute anything:
#
#   base/    types, u24/i24, fixed_vector, assertions, string utilities
#   isa/     opcodes, the instruction format, the register banks, the disassembler
#   format/  CRES (.cres), debug information, the platform's memory map
#
# The disassembler is here and not in the VM on purpose: decoding a 32-bit word is a function of
# the instruction set, not of a running machine's state, so `ceres asm --listing` doesn't have to
# link the execution engine.
#
# So is debug information, and that's one of the separation's two cuts: the assembler produces it
# and the debugger consumes it, so it can't live inside either one. The other cut is the memory
# map constants, which the linker needed and used to force it to include the whole of vm::Memory.

ceres_add_library(core)
ceres_add_tests(core)
