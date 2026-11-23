# Writes the build stamp (main/CMakeLists.txt runs this on every build).
# MINUTE precision, and it only rewrites when the text changes: two builds in the same
# minute recompile nothing, and the "Compilado em" (built on) in Settings > About never
# falls behind.
string(TIMESTAMP M5_QUANDO "%d/%m/%Y %H:%M")
set(M5_TXT "// Gerado por main/carimbo.cmake a cada build - nao editar.\nconst char* const m5tabos_compilado = \"${M5_QUANDO}\";\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" M5_ANTES)
endif()
if(NOT "${M5_ANTES}" STREQUAL "${M5_TXT}")
  file(WRITE "${OUT}" "${M5_TXT}")
endif()
