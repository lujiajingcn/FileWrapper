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
#        [-QtBin <qt bin dir>] [-Overwrite]
#
#   OutDir is cleared only when it is an empty directory, a directory created
#   by this script (marked .filewrapper-package), or when -Overwrite is given.
#   It is refused when it equals / is an ancestor of BuildDir or the repo root.
#
#   Exit code: 0 = packaged, 1 = bad invocation / windeployqt failure.
# =============================================================================

param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$QtBin = '',
    [switch]$Overwrite
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

$outFull   = [System.IO.Path]::GetFullPath($OutDir).TrimEnd('\')
$buildFull = [System.IO.Path]::GetFullPath($BuildDir).TrimEnd('\')
$repoRoot  = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')).TrimEnd('\')
$outRoot   = [System.IO.Path]::GetPathRoot($outFull).TrimEnd('\')

if ($outFull -ieq $buildFull) { Fail "OutDir must not be the build dir: $OutDir" }
if ($outRoot -ne '' -and $outFull -ieq $outRoot) { Fail "OutDir must not be a drive root: $OutDir" }
# Test equality separately: the ancestor check below requires $protected to
# start with "$outFull\", which is never true when both paths are equal.
# Without this, -OutDir pointing at the repo root would recursively delete it.
if ($outFull -ieq $repoRoot) { Fail "OutDir must not be the repository root: $OutDir" }
# Refuse any directory *inside* the repository as well.  The equality check above
# alone still allowed -OutDir <repo>\.git (or any source subdirectory) to reach
# the recursive delete below: a descendant is neither equal to nor an ancestor of
# the repo root, so the StartsWith() guard did not catch it.
if ($outFull.ToLowerInvariant().StartsWith($repoRoot.ToLowerInvariant() + '\')) {
    Fail "OutDir must not be inside the repository: $OutDir"
}
foreach ($protected in @($buildFull, $repoRoot)) {
    if ($protected.ToLowerInvariant().StartsWith($outFull.ToLowerInvariant() + '\')) {
        Fail "OutDir must not be an ancestor of $protected : $OutDir"
    }
}

if (Test-Path -LiteralPath $OutDir) {
    $existing = @(Get-ChildItem -LiteralPath $OutDir -Force)
    $marker = Join-Path $OutDir '.filewrapper-package'
    if ($existing.Count -gt 0 -and -not (Test-Path -LiteralPath $marker) -and -not $Overwrite) {
        Fail "OutDir is not empty and was not created by this script: $OutDir (pass -Overwrite to replace it)"
    }
    Remove-Item -LiteralPath $OutDir -Recurse -Force
}
New-Item -ItemType Directory -Path $OutDir | Out-Null
Set-Content -LiteralPath (Join-Path $OutDir '.filewrapper-package') -Value 'created by deploy/package.ps1' -Encoding ASCII

# 1) main binary + plugin folders
Copy-Item -LiteralPath $exe -Destination $OutDir -Force
foreach ($sub in @('plugins', 'imageformats', 'multimedia', 'platforms', 'styles', 'translations')) {
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
