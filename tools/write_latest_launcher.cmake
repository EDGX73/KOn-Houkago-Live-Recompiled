# Post-build step of kon_boot_probe: keeps one launcher of the newest build in
# the project root, named after the build time, e.g.
# "Ultima version 2026-10-06 14.30.cmd".  The frame rate is chosen in the
# game's Settings screen (FPS row) and kept in config/settings.ini.
# Usage: cmake -DROOT=<project> -DEXE=<path to kon_boot_probe.exe> -P this_file
string(TIMESTAMP stamp "%Y-%m-%d %H.%M")
file(GLOB old "${ROOT}/Ultima version *.cmd")
if(old)
    file(REMOVE ${old})
endif()
file(RELATIVE_PATH exe "${ROOT}" "${EXE}")
string(REPLACE "/" "\\" exe "${exe}")
set(nl "\r\n")
function(write_launcher name extra description)
    file(WRITE "${ROOT}/${name}"
    "@echo off${nl}"
    "rem Ultima version compilada el ${stamp} (${description}). Se regenera en cada compilacion.${nl}"
    "rem Idioma: el elegido en Ajustes (ingles la primera vez); para forzarlo: \"%~nx0\" ja o \"%~nx0\" en${nl}"
    "setlocal${nl}"
    "set LANG_OPT=${nl}"
    "if /I \"%~1\"==\"ja\" set LANG_OPT=--language ja${nl}"
    "if /I \"%~1\"==\"en\" set LANG_OPT=--language en${nl}"
    "pushd \"%~dp0\"${nl}"
    "\"%~dp0${exe}\" --root \"%~dp0.\" %LANG_OPT% --window --hold-on-stop 300000${extra}${nl}"
    "popd${nl}")
endfunction()
write_launcher("Ultima version ${stamp}.cmd" "" "FPS elegibles en Ajustes")
