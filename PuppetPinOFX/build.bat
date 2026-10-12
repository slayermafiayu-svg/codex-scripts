@echo off
rem Builds PuppetPin.ofx.bundle (64-bit) with Visual Studio's MSVC, then runs the tests.
rem Needs Visual Studio (or Build Tools) with the "Desktop development with C++" workload.
rem Usage:  build.bat [clean]
setlocal

set "ROOT=%~dp0"
set "OUT=%ROOT%build"
set "OBJ=%OUT%\obj"
set "TOBJ=%OUT%\obj-test"
set "BIN=%OUT%\PuppetPin.ofx.bundle\Contents\Win64"

if /i "%~1"=="clean" if exist "%OUT%" rmdir /s /q "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%TOBJ%" mkdir "%TOBJ%"
if not exist "%BIN%" mkdir "%BIN%"

rem --- find the MSVC toolchain (skip if cl.exe is already on PATH) ---------------
where cl >nul 2>nul
if not errorlevel 1 goto :have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :no_vs
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" goto :no_vs
echo Using MSVC from: %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :no_vs
:have_cl

set "INCS=/I"%ROOT%src" /I"%ROOT%third_party\openfx\include""
rem /MT: static C runtime, so the plug-in needs no Visual C++ redistributable.
set "FLAGS=/nologo /EHsc /O2 /MT /std:c++17 /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS"

rem --- plug-in -------------------------------------------------------------------
cl %FLAGS% %INCS% /c "%ROOT%src\PuppetPin.cpp" /Fo"%OBJ%\PuppetPin.obj"
if errorlevel 1 goto :fail
link /nologo /DLL /OPT:REF /OPT:ICF /OUT:"%BIN%\PuppetPin.ofx" "%OBJ%\PuppetPin.obj"
if errorlevel 1 goto :fail
del /q "%BIN%\PuppetPin.lib" "%BIN%\PuppetPin.exp" 2>nul

rem --- tests ---------------------------------------------------------------------
cl %FLAGS% /I"%ROOT%src" "%ROOT%tests\core_test.cpp" /Fo"%TOBJ%\core_test.obj" /Fe"%OUT%\core_test.exe"
if errorlevel 1 goto :fail
"%OUT%\core_test.exe"
if errorlevel 1 goto :fail

cl %FLAGS% /I"%ROOT%third_party\openfx\include" "%ROOT%tests\mock_host.cpp" /Fo"%TOBJ%\mock_host.obj" /Fe"%OUT%\mock_host.exe"
if errorlevel 1 goto :fail
"%OUT%\mock_host.exe" "%BIN%\PuppetPin.ofx"
if errorlevel 1 goto :fail
"%OUT%\mock_host.exe" "%BIN%\PuppetPin.ofx" --no-overlays
if errorlevel 1 goto :fail

echo.
echo Built and tested: %BIN%\PuppetPin.ofx
echo Install it with (administrator PowerShell):
echo   powershell -ExecutionPolicy Bypass -File "%ROOT%install.ps1"
exit /b 0

:no_vs
echo ERROR: Visual Studio with the "Desktop development with C++" workload was not found.
echo Install Visual Studio Community or Build Tools, or run this from a "x64 Native Tools Command Prompt".
exit /b 1

:fail
echo.
echo BUILD OR TESTS FAILED
exit /b 1
