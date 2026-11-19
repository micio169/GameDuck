@echo off
REM GameDuck build - requires mingw-w64 (x86_64-w64-mingw32-g++)
x86_64-w64-mingw32-windres GameDuck.rc -O coff -o GameDuck.res
x86_64-w64-mingw32-g++ -std=c++17 -O2 -municode -mwindows -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE -static-libgcc -static-libstdc++ main.cpp GameDuck.res -o GameDuck.exe -lcomctl32 -lgdi32 -luser32 -lshell32 -lole32 -luuid -lpsapi -lshlwapi
echo Build done: GameDuck.exe
pause
