@echo off
setlocal
cd /d "%~dp0..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
python tools\compile_cache.py cl /nologo /c /W4 /WX /wd4458 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS /I"third_party\openxr\include" /I"build\gen" /Fo"build\obj\fg-host-compile.obj" /DEDVR_HAVE_VERSION_HEADER=1 src\openxr\native_module.cpp
exit /b %errorlevel%
