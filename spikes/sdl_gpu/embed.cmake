# Turns a compiled shader into a C array: cmake -DINPUT=<blob> -DOUTPUT=<header> -DNAME=<symbol> -P embed.cmake
file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT} by embed.cmake.\nstatic const unsigned char ${NAME}[] = { ${bytes} };\n")
