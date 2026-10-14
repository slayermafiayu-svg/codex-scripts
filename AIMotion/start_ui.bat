@echo off
rem AI Motion arayuzu: http://127.0.0.1:7860 (yalnizca bu bilgisayardan erisilir)
setlocal
if "%AIMOTION_ROOT%"=="" set "AIMOTION_ROOT=D:\AIMotion"
set "PY=%AIMOTION_ROOT%\venv\Scripts\python.exe"
if not exist "%PY%" (
  echo Kurulum bulunamadi: %PY%
  echo Once install.ps1 calistirin.
  pause
  exit /b 1
)
cd /d "%~dp0"
"%PY%" -m aimotion.server %*
