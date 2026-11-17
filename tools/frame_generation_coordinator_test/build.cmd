@echo off
setlocal
for %%I in ("%~dp0..\..") do set "FG_COORDINATOR_ROOT=%%~fI"
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > "%~dp0vcvars.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
if not exist "%~dp0out" mkdir "%~dp0out"
cd /d "%FG_COORDINATOR_ROOT%"
cl /nologo /MT /std:c++17 /EHsc /W4 /WX /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DEDVR_FRAME_GENERATION_COORDINATOR_TEST tools\frame_generation_coordinator_test\cpu_test.cpp src\d3d11\frame_generation_coordinator.cpp src\d3d11\frame_generation_color.cpp src\d3d11\frame_generation_interop.cpp src\d3d11\frame_generation_output.cpp /Fo"%~dp0out\\" /Fe"%~dp0out\coordinator_cpu_test.exe" /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
"%~dp0out\coordinator_cpu_test.exe" --self-test
exit /b %errorlevel%
