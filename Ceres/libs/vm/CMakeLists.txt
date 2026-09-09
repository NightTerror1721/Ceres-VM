# ceres-vm - the running machine.
#
# Memory, live register banks, execution engine, interrupt controller, I/O ports and the BIOS.
# What's needed to load a .cres and run it, and nothing more: it doesn't assemble, doesn't know
# what a source file is, and doesn't know about any specific peripheral.
#
# It's the piece that would make sense to embed on its own - in wasm, in a game, in a web panel.

ceres_add_library(vm DEPENDS core)
ceres_add_tests(vm)
