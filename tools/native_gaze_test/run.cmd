@echo off
setlocal
for %%I in ("%~dp0..\..") do set "NATIVE_GAZE_ROOT=%%~fI"
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > "%~dp0vcvars.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
if not exist "%~dp0out" mkdir "%~dp0out"
cd /d "%NATIVE_GAZE_ROOT%"
cl /nologo /MT /std:c++17 /EHsc /W4 /WX /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%~dp0win_stub" tools\native_gaze_test\native_gaze_test.cpp src\d3d11\native_gaze.cpp /Fo"%~dp0out\\" /Fe"%~dp0out\native_gaze_test.exe" /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
for %%M in (--self-test --adapter-missing-get --adapter-path-mismatch --adapter-missing-init --adapter-load-failed --adapter-init-failed) do (
  "%~dp0out\native_gaze_test.exe" %%M
  if errorlevel 1 exit /b 1
)
cl /nologo /c /MT /std:c++17 /EHsc /W4 /WX /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX src\d3d11\native_gaze.cpp /Fo"%~dp0out\native_gaze_production.obj"
exit /b %errorlevel%
