@echo off
setlocal
if not exist build mkdir build
cl /nologo /std:c11 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I include src\main.c src\gui.c src\process_monitor.c src\classifier.c /Fe:build\ProcessManager.exe /link comctl32.lib psapi.lib user32.lib gdi32.lib advapi32.lib
if errorlevel 1 (
  echo Build failed. Check the compiler messages above.
  exit /b 1
)
echo.
echo Build successful: build\ProcessManager.exe
endlocal
