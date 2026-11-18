@echo off
cd /d "%~dp0\.."
cl /nologo /W4 /TC /Iinclude tests\classifier_test.c src\classifier.c /Febuild\classifier_test.exe
if errorlevel 1 exit /b 1
build\classifier_test.exe
