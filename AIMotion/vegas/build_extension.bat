@echo off
rem Builds the AI Motion VEGAS extension (AIMotionBridge.dll) against YOUR VEGAS's
rem ScriptPortal.Vegas.dll and installs it, plus the stand-alone import script.
rem
rem   build_extension.bat                      (finds VEGAS under Program Files)
rem   build_extension.bat "C:\Program Files\VEGAS\VEGAS Pro 22.0\ScriptPortal.Vegas.dll"
rem
rem Needs nothing but Windows: uses the C# compiler that ships with .NET Framework 4.
setlocal EnableExtensions EnableDelayedExpansion
set "HERE=%~dp0"
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" (
  echo [HATA] C# derleyicisi bulunamadi: %CSC%
  exit /b 1
)

set "VEGASDLL=%~1"
if "%VEGASDLL%"=="" (
  for /d %%D in ("%ProgramFiles%\VEGAS\VEGAS Pro *" "%ProgramFiles%\MAGIX\VEGAS Pro *") do (
    if exist "%%~D\ScriptPortal.Vegas.dll" set "VEGASDLL=%%~D\ScriptPortal.Vegas.dll"
  )
)
if "%VEGASDLL%"=="" (
  echo [HATA] ScriptPortal.Vegas.dll bulunamadi. Yolunu parametre olarak verin.
  exit /b 1
)
echo VEGAS betik API'si: %VEGASDLL%

"%CSC%" /nologo /target:library /langversion:5 /optimize+ /out:"%HERE%AIMotionBridge.dll" ^
  /reference:"%VEGASDLL%" /reference:System.Windows.Forms.dll ^
  "%HERE%AIMotionBridge.cs" "%HERE%AIMotion_Import.cs"
if errorlevel 1 (
  echo [HATA] Derleme basarisiz. Hata satirlarini AI Motion gelistiricisine iletin;
  echo        bu VEGAS surumunun betik API'si farkli olabilir.
  exit /b 1
)

rem VEGAS loads application extensions from these folders; the per-user one needs no admin rights.
set "USEREXT=%USERPROFILE%\Documents\Vegas Application Extensions"
if not exist "%USEREXT%" mkdir "%USEREXT%"
copy /y "%HERE%AIMotionBridge.dll" "%USEREXT%\" >nul && echo Kuruldu: %USEREXT%\AIMotionBridge.dll

set "SCRIPTS=%USERPROFILE%\Documents\Vegas Script Menu"
if not exist "%SCRIPTS%" mkdir "%SCRIPTS%"
copy /y "%HERE%AIMotion_Import.cs" "%SCRIPTS%\" >nul && echo Kuruldu: %SCRIPTS%\AIMotion_Import.cs

echo.
echo VEGAS'i yeniden baslatin. Komutlar: Tools ^> Extensions ^> "AI Motion: ..."
echo Uzanti gorunmezse DLL'i "%ProgramData%\VEGAS Pro\Application Extensions" klasorune de kopyalayin (yonetici).
exit /b 0
