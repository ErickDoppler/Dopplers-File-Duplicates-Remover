@echo off
set PATH=C:\workenv\w64devkit\bin;%PATH%
cd /d "%~dp0"
if not exist build mkdir build
windres src\app.rc -O coff -o build\app_res.o || exit /b 1
g++ -std=c++17 -O2 -Wall -municode -mwindows -static -s src\main.cpp build\app_res.o -o DopplersFileDuplicatesRemover.exe -lcomctl32 -lshlwapi -lshell32 -lole32 -luuid -lcomdlg32 || exit /b 1
echo Built DopplersFileDuplicatesRemover.exe
