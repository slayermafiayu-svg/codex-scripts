@echo off
rem Ilk gercek uretim: ornek yandan karakter (parca katmanlariyla) + HY-Motion-1.0.
rem Iki aday (seed 11 ve 22) uretir, onizlemeleri yazar ve ilkini VEGAS'a hazirlar.
rem Sonra VEGAS'ta: Tools > Extensions > AI Motion: Import Latest Result
setlocal
if "%AIMOTION_ROOT%"=="" set "AIMOTION_ROOT=D:\AIMotion"
set "PY=%AIMOTION_ROOT%\venv\Scripts\python.exe"
cd /d "%~dp0"
"%PY%" -m aimotion.cli doctor
if errorlevel 1 (
  echo Eksikler var; yukaridaki listeye bakin.
  pause
  exit /b 1
)
"%PY%" -m aimotion.cli pins ^
  --image examples\demo_side\character.png ^
  --rig examples\demo_side\rig.json ^
  --layers examples\demo_side\layers\00_uzak_kol.png examples\demo_side\layers\01_uzak_bacak.png examples\demo_side\layers\02_govde.png examples\demo_side\layers\03_yakin_bacak.png examples\demo_side\layers\04_yakin_kol.png ^
  --prompt "A person walks forward at a relaxed pace, arms swinging naturally." ^
  --duration 4 --seeds 11,22 --stage 1920x1080@29.97 --center-x 0.2 --height 0.45 --export %*
pause
