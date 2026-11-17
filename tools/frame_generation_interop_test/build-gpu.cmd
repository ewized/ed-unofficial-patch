@echo off
setlocal
for %%I in ("%~dp0..\..") do set "FG_INTEROP_GPU_ROOT=%%~fI"
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > "%~dp0gpu-vcvars.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
if not exist "%~dp0gpu-out" mkdir "%~dp0gpu-out"
cd /d "%FG_INTEROP_GPU_ROOT%"
cl /nologo /MT /std:c++17 /EHsc /W4 /WX /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tools\frame_generation_interop_test\gpu_test.cpp src\d3d11\frame_generation_interop.cpp src\d3d11\frame_generation_color.cpp /Fo"%~dp0gpu-out\\" /Fe"%~dp0gpu-out\interop_gpu_test.exe" /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
"%~dp0gpu-out\interop_gpu_test.exe" --self-test
exit /b %errorlevel%
