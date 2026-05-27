@echo off
setlocal

cd /d "%~dp0"
if not exist build mkdir build
windres src/resource.rc -O coff -o build/resource.o
g++ -mwindows -municode -O2 src/main.cpp build/resource.o -o build/AVIFMaster2.exe -lcomctl32 -lshlwapi -lgdi32 -ladvapi32 -lole32 -lwindowscodecs -luuid -static
echo Build completed.
