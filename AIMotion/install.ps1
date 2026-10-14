<#
  AI Motion - Windows installer. Everything large goes to D: (or -Root).

    powershell -ExecutionPolicy Bypass -File install.ps1
    powershell -ExecutionPolicy Bypass -File install.ps1 -WithWan          # also the frames model (~30+ GB)
    powershell -ExecutionPolicy Bypass -File install.ps1 -SkipModels       # code only

  Needs: Python 3.10-3.12 (py launcher or -Python), Git with Git LFS, an NVIDIA
  driver new enough for CUDA 12.4, internet for the downloads.

  What it does
    1. D:\AIMotion\{venv,models,cache,repos,jobs,outbox,state,logs,tmp}
    2. user environment variables AIMOTION_ROOT, HF_HOME, PIP_CACHE_DIR, TORCH_HOME -> D:
    3. Python venv on D: with torch 2.5.1 + CUDA 12.4 and requirements.txt
    4. HY-Motion-1.0 code (git + LFS) -> D:\AIMotion\repos\HY-Motion-1.0
    5. model weights from their official Hugging Face repos -> D:\AIMotion\models
    6. puppet_render.exe (preview renderer): uses bin\ if present, else builds with CMake + MSVC
    7. VEGAS extension + import script (vegas\build_extension.bat)
    8. "doctor" check
#>
param(
  [string]$Root = "D:\AIMotion",
  [string]$Python = "",
  [switch]$WithWan,
  [switch]$Lite,
  [switch]$SkipModels,
  [switch]$SkipVegas
)
$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Step($t) { Write-Host ""; Write-Host "== $t" -ForegroundColor Cyan }
function Run($exe, [string[]]$a) {
  & $exe @a
  if ($LASTEXITCODE -ne 0) { throw "Komut başarısız ($LASTEXITCODE): $exe $($a -join ' ')" }
}

Step "Kök klasör: $Root"
$drive = Split-Path -Qualifier $Root
if (-not (Test-Path "$drive\")) { throw "$drive sürücüsü bulunamadı. -Root ile başka bir D: yolu verin." }
foreach ($d in "venv","models","cache","cache\pip","cache\huggingface","cache\torch","repos","jobs","outbox","state","logs","tmp") {
  New-Item -ItemType Directory -Force -Path (Join-Path $Root $d) | Out-Null
}
$cfg = @{ root = $Root; required_drive = $drive } | ConvertTo-Json
Set-Content -Path (Join-Path $Here "aimotion.json") -Value $cfg -Encoding UTF8
Set-Content -Path (Join-Path $Root "app_home.txt") -Value $Here -Encoding UTF8

Step "Ortam değişkenleri (kullanıcı) -> $Root"
$vars = @{
  "AIMOTION_ROOT" = $Root
  "HF_HOME"       = (Join-Path $Root "cache\huggingface")
  "PIP_CACHE_DIR" = (Join-Path $Root "cache\pip")
  "TORCH_HOME"    = (Join-Path $Root "cache\torch")
}
foreach ($k in $vars.Keys) {
  [Environment]::SetEnvironmentVariable($k, $vars[$k], "User")
  Set-Item -Path "env:$k" -Value $vars[$k]
}
$env:TMP = Join-Path $Root "tmp"; $env:TEMP = $env:TMP

Step "Python"
if (-not $Python) {
  foreach ($v in "3.12","3.11","3.10") {
    try { $p = & py "-$v" -c "import sys; print(sys.executable)" 2>$null; if ($LASTEXITCODE -eq 0 -and $p) { $Python = $p.Trim(); break } } catch {}
  }
}
if (-not $Python) { throw "Python 3.10-3.12 bulunamadı (python.org'dan kurun veya -Python verin). torch 2.5.1 3.13'ü desteklemez." }
Write-Host "Python: $Python"
$VenvPy = Join-Path $Root "venv\Scripts\python.exe"
if (-not (Test-Path $VenvPy)) { Run $Python @("-m","venv",(Join-Path $Root "venv")) }
Run $VenvPy @("-m","pip","install","--upgrade","pip")
Run $VenvPy @("-m","pip","install","torch==2.5.1","torchvision==0.20.1","--index-url","https://download.pytorch.org/whl/cu124")
Run $VenvPy @("-m","pip","install","-r",(Join-Path $Here "requirements.txt"))

Step "HY-Motion-1.0 kodu (git + LFS)"
$Repo = Join-Path $Root "repos\HY-Motion-1.0"
Run "git" @("lfs","install")
if (-not (Test-Path (Join-Path $Repo ".git"))) {
  Run "git" @("clone","https://github.com/Tencent-Hunyuan/HY-Motion-1.0",$Repo)
}
Run "git" @("-C",$Repo,"lfs","pull")

if (-not $SkipModels) {
  Step "Model ağırlıkları -> $Root\models"
  $dl = @("-m","aimotion.download")
  if ($WithWan) { $dl += "--wan" }
  if ($Lite) { $dl += "--lite" }
  Push-Location $Here; try { Run $VenvPy $dl } finally { Pop-Location }
}

Step "Önizleme işleyicisi (puppet_render.exe)"
$Bin = Join-Path $Here "bin\puppet_render.exe"
if (Test-Path $Bin) {
  Write-Host "Hazır: $Bin"
} elseif (Get-Command cmake -ErrorAction SilentlyContinue) {
  $B = Join-Path $Root "cache\build-native"
  Run "cmake" @("-S",(Join-Path $Here "native"),"-B",$B,"-A","x64")
  Run "cmake" @("--build",$B,"--config","Release")
  New-Item -ItemType Directory -Force -Path (Join-Path $Here "bin") | Out-Null
  Copy-Item (Join-Path $B "Release\puppet_render.exe") $Bin
  Write-Host "Derlendi: $Bin"
} else {
  Write-Warning "puppet_render.exe yok ve CMake/MSVC bulunamadı. GitHub Actions 'AIMotion-Win64' paketindeki bin\puppet_render.exe dosyasını $Bin konumuna kopyalayın; o zamana kadar önizleme yalnızca iskelet gösterir."
}

if (-not $SkipVegas) {
  Step "VEGAS uzantısı ve içe aktarma betiği"
  & cmd /c "`"$(Join-Path $Here 'vegas\build_extension.bat')`""
  if ($LASTEXITCODE -ne 0) { Write-Warning "VEGAS uzantısı derlenemedi; AIMotion_Import.cs betiği yine de Tools > Scripting ile kullanılabilir." }
}

Step "Kontrol"
Push-Location $Here; try { & $VenvPy -m aimotion.cli doctor } finally { Pop-Location }
Write-Host ""
Write-Host "Sıradaki adımlar:" -ForegroundColor Green
Write-Host "  1) selftest.bat        - GPU'nuzda gerçek VRAM ve FP16 ölçümü"
Write-Host "  2) first_generation.bat - örnek karakterle ilk gerçek üretim ve VEGAS'a aktarım"
Write-Host "  3) start_ui.bat        - arayüz (http://127.0.0.1:7860)"
