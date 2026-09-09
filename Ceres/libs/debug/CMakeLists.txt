# ceres-debug - the debugger.
#
# Debug session, backward execution history, expression evaluator, the protocol's minimal JSON,
# the interactive CLI and the NDJSON server that talks to the editor.
#
# Depends on ceres-asm because the session assembles source on the fly. That's not a cycle: it's
# the debugger sitting above the assembler, which is where it belongs.
#
# Note: debug information (the format, the line and symbol tables) is NOT here. It lives in
# ceres-core, alongside the rest of the formats.

ceres_add_library(debug DEPENDS core vm devices asm)
ceres_add_tests(debug)
