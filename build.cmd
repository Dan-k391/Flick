@echo off
rem Usage: build.cmd [Release|Debug]   -> build\<config>\Flick.exe
setlocal
set CFG=%1
if "%CFG%"=="" set CFG=Release
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS=%%i
rem The linker can't overwrite a running exe.
taskkill /im Flick.exe /f >nul 2>&1
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%~dp0." -B "%~dp0build\%CFG%" -G Ninja -DCMAKE_BUILD_TYPE=%CFG% >nul || exit /b 1
cmake --build "%~dp0build\%CFG%" || exit /b 1
echo Built %~dp0build\%CFG%\Flick.exe
