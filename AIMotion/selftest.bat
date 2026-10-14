@echo off
rem GPU'nuzda gercek olcum: metin kodlayici yerlesimi, DiT FP32 ve FP16 tepe VRAM'i, FP16~FP32 farki.
rem Wan2.2 kare modunu da olcmek icin: selftest.bat --wan   (bf16 ile karsilastirma: --wan-compare)
rem VEGAS oynatirken calismaz; bekler. Sonuc: %AIMOTION_ROOT%\selftest.json
setlocal
if "%AIMOTION_ROOT%"=="" set "AIMOTION_ROOT=D:\AIMotion"
cd /d "%~dp0"
"%AIMOTION_ROOT%\venv\Scripts\python.exe" -m aimotion.selftest %*
pause
