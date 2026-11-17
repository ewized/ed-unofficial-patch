@echo off
setlocal
cd /d "%~dp0"
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /MT /std:c++17 /EHsc /W4 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DEDVR_FRAME_GENERATION_NGX_TEST cpu_test.cpp ..\..\src\d3d11\frame_generation_ngx.cpp /Fecpu_test.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
cpu_test.exe
if errorlevel 1 exit /b %errorlevel%
cl /nologo /MT /std:c++17 /EHsc /W4 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN probe.cpp ..\..\src\d3d11\frame_generation_ngx.cpp /Feprobe.exe /link /INCREMENTAL:NO ..\..\third_party\ngx\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib advapi32.lib user32.lib d3d12.lib dxgi.lib
if errorlevel 1 exit /b %errorlevel%
probe.exe
if errorlevel 1 exit /b %errorlevel%
probe.exe --official-fg-helper --multiplier 2 --width 1280 --height 720
if errorlevel 1 exit /b %errorlevel%
probe.exe --official-fg-helper --multiplier 4 --width 3200 --height 3200 --guide-width 2133 --guide-height 2133
if errorlevel 1 exit /b %errorlevel%
probe.exe --official-fg-helper --multiplier 4 --width 3200 --height 3200 --guide-width 2133 --guide-height 2133 --native-frames 12 --warmup-frames 4 --diagnostic-suppressed-output
if errorlevel 1 exit /b %errorlevel%
probe.exe --official-fg-helper --shared-native-pair-suppression --multiplier 4 --width 3200 --height 3200 --guide-width 2133 --guide-height 2133 --native-frames 12 --warmup-frames 4 --diagnostic-suppressed-output
exit /b %errorlevel%
