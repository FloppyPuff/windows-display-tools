@echo off
setlocal
cd /d "%~dp0"

windres app.rc -O coff -o app_res.o
if errorlevel 1 (echo [FAILED] windres & exit /b 1)

g++ -mwindows -municode -O2 -s -static ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE ^
    main.cpp app_res.o -o refresh280.exe -lshell32
if errorlevel 1 (echo [FAILED] g++ & exit /b 1)

echo [OK] refresh280.exe
