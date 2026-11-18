@echo off
setlocal
cd /d "%~dp0..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
if not exist build\obj\native_fg_cpu mkdir build\obj\native_fg_cpu
python tools\compile_cache.py cl /nologo /c /W4 /WX /wd4458 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS /I"third_party\openxr\include" /I"build\gen" /Fo"build\obj\fg-host-compile.obj" /DEDVR_HAVE_VERSION_HEADER=1 src\openxr\native_module.cpp
if errorlevel 1 exit /b %errorlevel%
python tools\compile_cache.py cl /nologo /W4 /WX /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" /Fo"build\obj\native_fg_cpu\\" /Fe"build\native_fg_test.exe" tools\native_fg_test\native_fg_test.cpp /link /INCREMENTAL:NO user32.lib
if errorlevel 1 exit /b %errorlevel%
build\native_fg_test.exe --self-test
if errorlevel 1 exit /b %errorlevel%
python tools\compile_cache.py cl /nologo /W4 /WX /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" /Fo"build\obj\native_fg_cpu\\" /Fe"build\openxr_frame_fg_test.exe" tools\openxr_frame_test\openxr_frame_test.cpp /link /INCREMENTAL:NO user32.lib
if errorlevel 1 exit /b %errorlevel%
build\openxr_frame_fg_test.exe --self-test
if errorlevel 1 exit /b %errorlevel%
python tools\compile_cache.py cl /nologo /W4 /WX /O2 /EHsc /std:c++17 /MT /Fo"build\obj\native_fg_cpu\\" /Fe"build\openxr_owner_fg_test.exe" tools\openxr_owner_test\openxr_owner_test.cpp /link /INCREMENTAL:NO user32.lib
if errorlevel 1 exit /b %errorlevel%
build\openxr_owner_fg_test.exe --self-test
exit /b %errorlevel%
