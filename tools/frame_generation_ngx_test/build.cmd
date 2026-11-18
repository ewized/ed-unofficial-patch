@echo off
setlocal
cd /d "%~dp0"
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
REM Match build.bat's SDK selection; no vendored copy is required.
for %%R in ("%~dp0..\..") do set "ROOT=%%~fR"
set "NGX="
if defined EDVR_NGX_SDK set "NGX=%EDVR_NGX_SDK%"
if not defined NGX if exist "%ROOT%\third_party\ngx\include\nvsdk_ngx.h" set "NGX=%ROOT%\third_party\ngx"
if not defined NGX if defined LOCALAPPDATA if exist "%LOCALAPPDATA%\EDVR\ngx-sdk\include\nvsdk_ngx.h" set "NGX=%LOCALAPPDATA%\EDVR\ngx-sdk"
set "NGXFLAGS="
set "NGXLIB="
if defined NGX (
    python "%ROOT%\tools\fetch_ngx.py" --verify "%NGX%"
    if errorlevel 1 exit /b 1
    set NGXFLAGS=/DEDVR_HAVE_NGX=1 /I"%NGX%\include"
    set NGXLIB="%NGX%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib"
)
cl /nologo /MT /std:c++17 /EHsc /W4 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN %NGXFLAGS% /DEDVR_FRAME_GENERATION_NGX_TEST cpu_test.cpp ..\..\src\d3d11\frame_generation_ngx.cpp /Fecpu_test.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
cpu_test.exe
if errorlevel 1 exit /b %errorlevel%
REM Pin the SDK-absent contract even when the selected SDK exists.
cl /nologo /MT /std:c++17 /EHsc /W4 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DEDVR_FRAME_GENERATION_NGX_TEST cpu_test.cpp ..\..\src\d3d11\frame_generation_ngx.cpp /Fecpu_no_sdk_test.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
cpu_no_sdk_test.exe
if errorlevel 1 exit /b %errorlevel%
cl /nologo /MT /std:c++17 /EHsc /W4 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN %NGXFLAGS% probe.cpp ..\..\src\d3d11\frame_generation_ngx.cpp /Feprobe.exe /link /INCREMENTAL:NO %NGXLIB% advapi32.lib user32.lib d3d12.lib dxgi.lib
if errorlevel 1 exit /b %errorlevel%
probe.exe
if errorlevel 1 exit /b %errorlevel%
if defined NGX goto sdk_probes
probe.exe --run
if not "%errorlevel%"=="3" exit /b 1
exit /b 0
:sdk_probes
probe.exe --official-fg-helper --multiplier 2 --width 1280 --height 720
if errorlevel 1 exit /b %errorlevel%
probe.exe --official-fg-helper --multiplier 4 --width 3200 --height 3200 --guide-width 2133 --guide-height 2133
if errorlevel 1 exit /b %errorlevel%
probe.exe --official-fg-helper --multiplier 4 --width 3200 --height 3200 --guide-width 2133 --guide-height 2133 --native-frames 12 --warmup-frames 4 --diagnostic-suppressed-output
if errorlevel 1 exit /b %errorlevel%
probe.exe --official-fg-helper --shared-native-pair-suppression --multiplier 4 --width 3200 --height 3200 --guide-width 2133 --guide-height 2133 --native-frames 12 --warmup-frames 4 --diagnostic-suppressed-output
exit /b %errorlevel%
