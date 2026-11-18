@echo off
setlocal
for %%I in ("%~dp0..\..") do set "FG_INTEROP_ROOT=%%~fI"
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > "%~dp0vcvars.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
if not exist "%~dp0out" mkdir "%~dp0out"
cd /d "%FG_INTEROP_ROOT%"
cl /nologo /MT /std:c++17 /EHsc /W4 /WX /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tools\frame_generation_interop_test\frame_generation_interop_test.cpp src\d3d11\frame_generation_interop.cpp /Fo"%~dp0out\\" /Fe"%~dp0out\frame_generation_interop_test.exe" /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
"%~dp0out\frame_generation_interop_test.exe"
if errorlevel 1 exit /b %errorlevel%
"C:\Program Files\Python313\python.exe" tools\frame_generation_interop_test\source_check.py --self-test
exit /b %errorlevel%
