# =============================================================================
# package.ps1 - build a runnable folder (exe + plugins + Qt runtime).
#
#   The build tree is NOT double-click runnable: it only has plugin-specific
#   dependencies (deploy-deps.ps1), while Qt's own DLLs and platform plugin are
#   supplied by windeployqt. This script assembles a self-contained folder:
#     1) copy FileWrapper.exe + plugins/ + imageformats/ + multimedia/ ...
#     2) copy plugin dependency DLLs already deployed into the build dir
#     3) run windeployqt for Qt6Core/Gui/Widgets + platforms/styles
#
#   Usage:
#     powershell -NoProfile -ExecutionPolicy Bypass -File deploy\package.ps1 `
#        -BuildDir <...\build-...\FileWrapper\debug> -OutDir <target dir> `
#        [-QtBin <qt bin dir>]
#
#   Exit code: 0 = packaged, 1 = bad invocation / windeployqt failure.
# =============================================================================

param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$QtBin = ''
)

$ErrorActionPreference = 'Stop'

function Fail([string]$msg) {
    Write-Host "[package][ERROR] $msg"
    exit 1
}

if (-not (Test-Path -LiteralPath $BuildDir)) { Fail "build dir not found: $BuildDir" }

if (-not $QtBin) {
    $qmake = Get-Command qmake.exe -ErrorAction SilentlyContinue
    if (-not $qmake) { Fail 'QtBin not given and qmake.exe is not in PATH' }
    $QtBin = Split-Path -Parent $qmake.Source
}
$windeploy = Join-Path $QtBin 'windeployqt.exe'
if (-not (Test-Path -LiteralPath $windeploy)) { Fail "windeployqt not found: $windeploy" }

$exe = Join-Path $BuildDir 'FileWrapper.exe'
if (-not (Test-Path -LiteralPath $exe)) { Fail "FileWrapper.exe not found in $BuildDir" }

if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir | Out-Null

# 1) main binary + plugin folders
Copy-Item -LiteralPath $exe -Destination $OutDir -Force
foreach ($sub in @('plugins', 'imageformats', 'multimedia', 'platforms', 'styles')) {
    $src = Join-Path $BuildDir $sub
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $OutDir $sub) -Recurse -Force
    }
}

# 2) plugin dependency DLLs already deployed by deploy-deps.ps1 (Qt6Pdf/Qt6Svg/av*/sw*/...)
Get-ChildItem -LiteralPath $BuildDir -Filter '*.dll' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $OutDir -Force
}
# runtime manifest used by the in-app deployment self-check
$manifest = Join-Path $BuildDir 'plugin-deploy-manifest.txt'
if (Test-Path -LiteralPath $manifest) { Copy-Item -LiteralPath $manifest -Destination $OutDir -Force }

# 3) Qt runtime + platform/style plugins
Push-Location $OutDir
try {
    & $windeploy --no-translations --no-system-d3d-compiler --no-opengl-sw FileWrapper.exe
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($code -ne 0) { Fail "windeployqt failed (exit $code)" }

Write-Host "[package] OK -> $OutDir"
