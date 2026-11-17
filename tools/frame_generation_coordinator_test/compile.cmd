@echo off
setlocal
for %%I in ("%~dp0..\..") do set "FG_COORDINATOR_ROOT=%%~fI"
set "FG_COORDINATOR_GENERATED=%~1"
if not defined FG_COORDINATOR_GENERATED set "FG_COORDINATOR_GENERATED=%FG_COORDINATOR_ROOT%\build\gen"
if not exist "%FG_COORDINATOR_GENERATED%\temporal_shader_bytecode.h" exit /b 2
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > "%~dp0vcvars.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
if not exist "%~dp0out" mkdir "%~dp0out"
cd /d "%FG_COORDINATOR_ROOT%"
cl /nologo /MT /std:c++17 /EHsc /W4 /WX /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /c src\d3d11\frame_generation_coordinator.cpp /Fo"%~dp0out\\"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /MT /std:c++17 /EHsc /W4 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%FG_COORDINATOR_GENERATED%" /c src\d3d11\native_temporal.cpp src\d3d11\temporal_pass.cpp src\d3d11\native_sharpen.cpp /Fo"%~dp0out\\"
exit /b %errorlevel%
