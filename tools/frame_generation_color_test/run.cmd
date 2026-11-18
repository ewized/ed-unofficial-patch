@echo off
setlocal
for %%I in ("%~dp0..\..") do set "FG_COLOR_ROOT=%%~fI"
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > "%~dp0vcvars.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
if not exist "%~dp0out" mkdir "%~dp0out"
cd /d "%FG_COLOR_ROOT%"
cl /nologo /MT /std:c++17 /EHsc /W4 /WX /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tools\frame_generation_color_test\frame_generation_color_test.cpp src\d3d11\frame_generation_color.cpp /Fo"%~dp0out\\" /Fe"%~dp0out\frame_generation_color_test.exe" /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib delayimp.lib /DELAYLOAD:d3d11.dll /DELAYLOAD:d3dcompiler_47.dll
if errorlevel 1 exit /b %errorlevel%
"%~dp0out\frame_generation_color_test.exe" %*
exit /b %errorlevel%
