@echo off
rem build-msvc.cmd : configure + build the emulator natively on Windows with MSVC (x64) and Ninja -> build-msvc\rio-emu.exe
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > nul
cd /d D:\rio3224-emu
if not exist build-msvc\build.ninja cmake -S emu -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
ninja -C build-msvc
