# ceres-asm - the assembler and linker.
#
# Lexer, parser, macros, constant expressions, symbol table, binary emitter, .cobj objects, .car
# archives and the linker.
#
# Notice what does NOT appear in DEPENDS: ceres-vm. That's the result of the two cuts. The
# assembler emits a .cres and needs to know where .text starts, but it doesn't need a machine
# - and until now it included vm/memory.h and debug/debug_info.h to get that.
#
# If anyone adds ceres::vm back here, let it be a decision and not an oversight.

ceres_add_library(asm DEPENDS core)
ceres_add_tests(asm)
