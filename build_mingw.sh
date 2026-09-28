#!/bin/sh
# MSYS2 (UCRT64) or any mingw-w64 cross compiler
${CXX:-g++} -O2 -std=c++17 -municode -mwindows -DUNICODE -D_UNICODE ShaderOverlay.cpp -o ShaderOverlay.exe \
  -ld3d11 -ldxgi -ld3dcompiler -lgdiplus -lshell32 -lole32 -lgdi32 -luser32 -ldwmapi -static
