# SPDX-License-Identifier: GPL-3.0-or-later
# Writes src/ashita/prelude.lua into a C++ source as xi::ashita::kPrelude (cmake -DIN=... -DOUT=... -P this file).
# Raw string literals have a length limit on some compilers, so the text goes in as 4000-byte pieces.
# IN: files joined with "|", in order; they become one Lua chunk.
string(REPLACE "|" ";" files "${IN}")
set(text "")
foreach(f IN LISTS files)
    file(READ ${f} part)
    string(APPEND text "${part}\n")
endforeach()
string(LENGTH "${text}" len)
set(out "// Generated from src/ashita/prelude.lua by cmake/embed_prelude.cmake; do not edit.\nnamespace xi::ashita\n{\nextern const char* const kPrelude;\nconst char* const kPrelude =\n")
set(pos 0)
while(pos LESS len)
    string(SUBSTRING "${text}" ${pos} 4000 piece)
    string(APPEND out "R\"XIPRELUDE(${piece})XIPRELUDE\"\n")
    math(EXPR pos "${pos} + 4000")
endwhile()
string(APPEND out ";\n} // namespace xi::ashita\n")
file(WRITE ${OUT} "${out}")
