@echo off
setlocal
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > "%~dp0vcvars.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0..\.."
set "UIFG_GEN=%~1"
if not defined UIFG_GEN set "UIFG_GEN=%CD%\build\gen"
if not exist "%UIFG_GEN%\temporal_shader_bytecode.h" (echo Missing generated production shader header & popd & exit /b 2)
set "UIFG_OUT=%CD%\build\ui-layer-fg-targeted"
if not exist "%UIFG_OUT%" mkdir "%UIFG_OUT%"
cl /nologo /c /MT /std:c++17 /EHsc /W4 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"%UIFG_GEN%" tools\ui_layer_fg_test\ui_layer_fg_test.cpp /Fo"%UIFG_OUT%\ui_layer_fg_test.obj"
if errorlevel 1 (popd & exit /b 1)
cl /nologo /c /MT /std:c++17 /EHsc /W4 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS src\d3d11\map_smaa.cpp /Fo"%UIFG_OUT%\map_smaa.obj"
if errorlevel 1 (popd & exit /b 1)
link /nologo /INCREMENTAL:NO /OUT:"%UIFG_OUT%\ui_layer_fg_test.exe" "%UIFG_OUT%\ui_layer_fg_test.obj" "%UIFG_OUT%\map_smaa.obj" d3d11.lib
set "UIFG_RESULT=%errorlevel%"
popd
exit /b %UIFG_RESULT%
