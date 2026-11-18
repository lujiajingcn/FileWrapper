# =============================================================================
# deploy-deps.ps1 - copy plugin runtime dependencies into the app directory.
#
#   This script is invoked by deploy.pri (QMAKE_POST_LINK) after the main app is
#   linked. It reads deploy/plugin-deps.manifest (the single source of truth),
#   copies every entry into the app directory, and writes a runtime manifest
#   (plugin-deploy-manifest.txt) that the application itself reads for
#   self-diagnosis ("plugin unavailable" dialog).
#
#   Deliberately ASCII-only: Windows PowerShell 5.1 reads BOM-less .ps1 files as
#   ANSI, so non-ASCII literals here would be mangled. Chinese hints come from
#   the manifest, which is read with an explicit UTF-8 encoding.
#
#   Usage:
#     powershell -NoProfile -ExecutionPolicy Bypass -File deploy-deps.ps1 `
#        -Manifest <path> -TargetDir <app dir> -QtBin <qt bin> `
#        -QtPlugins <qt plugins> -FfmpegBin <ffmpeg bin> -RepoRoot <repo root>
#
#   Exit code: 0 = deployment performed (missing sources are warnings, because
#   third-party drop-in dirs may not exist on every machine and those gaps are
#   reported at runtime by the app); 1 = broken invocation (bad manifest, bad
#   target dir).
# =============================================================================

param(
    [Parameter(Mandatory = $true)][string]$Manifest,
    [Parameter(Mandatory = $true)][string]$TargetDir,
    [Parameter(Mandatory = $true)][string]$QtBin,
    [Parameter(Mandatory = $true)][string]$QtPlugins,
    [Parameter(Mandatory = $true)][string]$FfmpegBin,
    [Parameter(Mandatory = $true)][string]$RepoRoot
)

$ErrorActionPreference = 'Stop'

function Fail([string]$msg) {
    Write-Host "[deploy][ERROR] $msg"
    exit 1
}

if (-not (Test-Path -LiteralPath $Manifest)) { Fail "manifest not found: $Manifest" }
if (-not (Test-Path -LiteralPath $TargetDir)) {
    try { New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null }
    catch { Fail "cannot create target dir: $TargetDir" }
}

$runtimeLines = New-Object System.Collections.Generic.List[string]
$copied  = 0
$warned  = 0

foreach ($raw in (Get-Content -LiteralPath $Manifest -Encoding UTF8)) {
    $line = "$raw".Trim()
    if ($line.Length -eq 0) { continue }
    if ($line.StartsWith('//')) { continue }

    $parts = $line.Split('|')
    if ($parts.Count -lt 3) {
        Fail "malformed manifest line (need kind|src|dst): $line"
    }

    $kind = $parts[0].Trim()
    $src  = $parts[1].Trim()
    $dst  = $parts[2].Trim()
    $hint = ''
    if ($parts.Count -ge 4) { $hint = $parts[3].Trim() }
    if ($kind.Length -eq 0 -or $src.Length -eq 0 -or $dst.Length -eq 0) {
        Fail "empty field in manifest line: $line"
    }

    $srcPath = $null
    switch ($kind) {
        'qt-bin'  { $srcPath = Join-Path $QtBin     $src }
        'qt-plug' { $srcPath = Join-Path $QtPlugins $src }
        'ffmpeg'  { $srcPath = Join-Path $FfmpegBin $src }
        'repo'    { $srcPath = Join-Path $RepoRoot  $src }
        default   {
            Fail "unknown source kind '$kind': $line"
        }
    }

    # The runtime manifest always lists every declared file, even when the copy
    # could not be performed: the application compares it against reality and
    # reports what is actually missing (with the hint) at startup/failure time.
    $runtimeLines.Add("$dst|$hint")

    $dstPath = Join-Path $TargetDir $dst
    $dstDir  = Split-Path -Parent $dstPath
    if ($dstDir -and -not (Test-Path -LiteralPath $dstDir)) {
        New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    }

    if (Test-Path -LiteralPath $srcPath) {
        try {
            Copy-Item -LiteralPath $srcPath -Destination $dstPath -Force
            $copied++
        } catch {
            Fail ("cannot copy '" + $srcPath + "' -> '" + $dstPath + "' (target in use / access denied?): " + $_.Exception.Message)
        }
    } else {
        Write-Host "[deploy][WARN] source missing, skipped: $srcPath"
        $warned++
    }
}

$runtimeManifest = Join-Path $TargetDir 'plugin-deploy-manifest.txt'
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllLines($runtimeManifest, $runtimeLines, $utf8)

Write-Host "[deploy] copied $copied file(s) to $TargetDir"
Write-Host "[deploy] declared $($runtimeLines.Count) entr(ies), warnings $warned"
exit 0
