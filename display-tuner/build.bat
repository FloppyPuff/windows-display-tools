@echo off
setlocal
cd /d "%~dp0"

dotnet publish -c Release
if errorlevel 1 (echo [FAILED] dotnet publish & exit /b 1)

echo [OK] bin\Release\net10.0-windows\win-x64\publish\DisplayTuner.exe
