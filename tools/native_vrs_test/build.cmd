@echo off
setlocal
cd /d "%~dp0"
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /MT /std:c++17 /EHsc /W4 /O2 /DEDVR_NATIVE_VRS_TEST native_vrs_test.cpp ..\..\src\d3d11\native_vrs.cpp /Fenative_vrs_test.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
native_vrs_test.exe
exit /b %errorlevel%
