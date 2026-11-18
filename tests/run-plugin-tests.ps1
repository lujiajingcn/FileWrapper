<#
run-plugin-tests.ps1 -- build and run the plugin test suite.

    Builds the application (debug) if needed, builds tests/plugin_tests.pro,
    copies the built plugins next to the test executable, then runs the suite
    and returns its exit code.

    ASCII-only on purpose: Windows PowerShell 5.1 reads BOM-less .ps1 files as
    ANSI, so non-ASCII literals here would be mangled.

    Examples:
        powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-plugin-tests.ps1
        powershell ... -File tests\run-plugin-tests.ps1 -AppBuildDir build\Desktop_Qt_6_9_3_MinGW_64_bit_Debug
        powershell ... -File tests\run-plugin-tests.ps1 -SkipProjectBuild -SkipShots
        powershell ... -File tests\run-plugin-tests.ps1 -RegenerateFixtures
#>
param(
    [string]$QtBin = '',
    [string]$MingwBin = '',
    [string]$AppBuildDir = '',
    [string]$FixturesDir = '',
    [string]$Python = '',
    [ValidateSet('debug','release')][string]$Config = 'debug',
    [switch]$SkipProjectBuild,
    [switch]$RegenerateFixtures,
    [switch]$SkipShots
)

$ErrorActionPreference = 'Stop'

$TestsDir = $PSScriptRoot
$RepoRoot = Split-Path -Parent $TestsDir
$BuildRoot = Join-Path $TestsDir 'build'

function Step([string]$msg) { Write-Host ""; Write-Host "== $msg" }

function Find-Tool([string]$exeName, [string[]]$candidates) {
    foreach ($c in $candidates) {
        if ([string]::IsNullOrWhiteSpace($c)) { continue }
        $p = Join-Path $c $exeName
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    $cmd = Get-Command $exeName -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

try {
    # ---------------------------------------------------------------- Qt / MinGW
    Step 'locating Qt and MinGW'

    $qtCandidates = @()
    if ($QtBin)        { $qtCandidates += $QtBin }
    if ($env:QTDIR)    { $qtCandidates += (Join-Path $env:QTDIR 'bin') }
    $qtCandidates += 'E:\V_C\Qt\6.9.3\mingw_64\bin'
    $QMake = Find-Tool 'qmake.exe' $qtCandidates
    if (-not $QMake) {
        throw "qmake.exe not found. Pass -QtBin <path to Qt's bin dir>."
    }
    $QtBinResolved = Split-Path -Parent $QMake
    Write-Host "   qmake : $QMake"

    $mingwCandidates = @()
    if ($MingwBin) { $mingwCandidates += $MingwBin }
    if ($env:MINGW_BIN) { $mingwCandidates += $env:MINGW_BIN }
    # <Qt>\6.9.3\mingw_64\bin -> <Qt>\Tools\mingw1310_64\bin
    $qtVersionDir = Split-Path -Parent (Split-Path -Parent $QtBinResolved)
    $qtRoot = Split-Path -Parent $qtVersionDir
    $mingwCandidates += (Join-Path $qtRoot 'Tools\mingw1310_64\bin')
    $Make = Find-Tool 'mingw32-make.exe' $mingwCandidates
    if (-not $Make) {
        throw "mingw32-make.exe not found. Pass -MingwBin <path to MinGW bin dir>."
    }
    $MingwBinResolved = Split-Path -Parent $Make
    Write-Host "   make  : $Make"

    # Keep PATH consistent for qmake-driven compiler lookups and for the test run
    $AppExeDirGuess = if ($AppBuildDir) { Join-Path $AppBuildDir (Join-Path 'FileWrapper' $Config) } else { '' }
    $env:PATH = "$MingwBinResolved;$QtBinResolved;$AppExeDirGuess;$env:PATH"

    # ------------------------------------------------------------ app build dir
    if (-not $AppBuildDir) { $AppBuildDir = Join-Path $BuildRoot 'app' }
    $AppPro = Join-Path $RepoRoot 'FileWrapper.pro'
    if (-not (Test-Path $AppPro)) { throw "FileWrapper.pro not found under $RepoRoot" }

    if (-not $SkipProjectBuild) {
        Step "building application -> $AppBuildDir"
        New-Item -ItemType Directory -Force -Path $AppBuildDir | Out-Null
        Push-Location $AppBuildDir
        try {
            & $QMake -o Makefile $AppPro -spec win32-g++ "CONFIG+=$Config" 'CONFIG+=qml_debug'
            if ($LASTEXITCODE -ne 0) { throw "qmake failed for the application" }
            & $Make -j4
            if ($LASTEXITCODE -ne 0) { throw "make failed for the application" }
        } finally { Pop-Location }
    } else {
        Write-Host "   (skipped application build)"
    }

    if (-not (Test-Path $AppBuildDir)) {
        throw "app build dir not found: $AppBuildDir -- build the application first (drop -SkipProjectBuild) or pass a valid -AppBuildDir."
    }
    $AppExeDir = Join-Path $AppBuildDir (Join-Path 'FileWrapper' $Config)
    $AppPluginsDir = Join-Path $AppExeDir 'plugins'
    if (-not (Test-Path $AppPluginsDir)) {
        throw "plugins dir not found: $AppPluginsDir -- build the application first (drop -SkipProjectBuild) or pass -AppBuildDir."
    }
    if (-not (Test-Path (Join-Path $AppExeDir 'plugin-deploy-manifest.txt'))) {
        Write-Host "   WARNING: plugin-deploy-manifest.txt missing next to the exe;" -ForegroundColor Yellow
        Write-Host "            dependency self-check (D group) will report 'no manifest'." -ForegroundColor Yellow
    }

    # ---------------------------------------------------------------- fixtures
    if (-not $FixturesDir) { $FixturesDir = Join-Path $TestsDir 'fixtures' }
    if ($RegenerateFixtures) {
        Step 'regenerating fixtures (python + python-docx + Pillow required)'
        $py = $Python
        if (-not $py) {
            $cmd = Get-Command python -ErrorAction SilentlyContinue
            if ($cmd) { $py = $cmd.Source }
        }
        if (-not $py) { throw "python not found; pass -Python <path to python.exe>" }
        New-Item -ItemType Directory -Force -Path $FixturesDir | Out-Null
        & $py (Join-Path $TestsDir 'fixtures\make_fixtures.py') $FixturesDir
        if ($LASTEXITCODE -ne 0) { throw "fixture generation failed" }
    }
    if (-not (Test-Path (Join-Path $FixturesDir 'basic.docx'))) {
        throw "fixtures missing: $(Join-Path $FixturesDir 'basic.docx')"
    }

    # ------------------------------------------------------------ test program
    Step 'building test program'
    $TestBuild = Join-Path $BuildRoot 'plugin_tests'
    New-Item -ItemType Directory -Force -Path $TestBuild | Out-Null
    Push-Location $TestBuild
    try {
        & $QMake -o Makefile (Join-Path $TestsDir 'plugin_tests.pro') -spec win32-g++ "CONFIG+=$Config"
        if ($LASTEXITCODE -ne 0) { throw "qmake failed for the test program" }
        & $Make -j4
        if ($LASTEXITCODE -ne 0) { throw "make failed for the test program" }
    } finally { Pop-Location }

    $TestRunDir = Join-Path $TestBuild $Config
    $TestExe = Join-Path $TestRunDir 'plugin_tests.exe'
    if (-not (Test-Path $TestExe)) { throw "test executable not found: $TestExe" }

    # PluginManager scans <exe dir>/plugins, so mirror the built plugins there
    Step 'preparing runtime dir'
    $DestPlugins = Join-Path $TestRunDir 'plugins'
    if (Test-Path $DestPlugins) { Remove-Item $DestPlugins -Recurse -Force }
    Copy-Item $AppPluginsDir $DestPlugins -Recurse -Force
    Write-Host "   plugins: $DestPlugins"

    # ---------------------------------------------------------------- run
    $LogDir = Join-Path $BuildRoot 'logs'
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
    $LogPath = Join-Path $LogDir 'plugin_tests.log'
    # 每次运行前清掉旧日志：测试崩溃/提前 return 时不能让上次的 RESULT 冒充本次结果
    if (Test-Path $LogPath) { Remove-Item $LogPath -Force }
    $ShotsDir = Join-Path $BuildRoot 'shots'
    if (-not $SkipShots) { New-Item -ItemType Directory -Force -Path $ShotsDir | Out-Null }

    # Make the plugins' runtime deps (Qt6Xml, Qt6Multimedia, ffmpeg...) resolvable
    $env:PATH = "$AppExeDir;$env:PATH"
    $env:QT_FORCE_STDERR_LOGGING = '1'

    $testArgs = @($FixturesDir, $AppPluginsDir, $LogPath)
    if (-not $SkipShots) { $testArgs += @('--shots', $ShotsDir) }
    $testArgs += '--integrate'

    Step "running tests"
    & $TestExe @testArgs
    $rc = $LASTEXITCODE

    # ---------------------------------------------------------------- summary
    Write-Host ""
    if (Test-Path $LogPath) {
        Select-String -Path $LogPath -Pattern '\[FAIL\]' | ForEach-Object { Write-Host ("   " + $_.Line) -ForegroundColor Red }
        Select-String -Path $LogPath -Pattern '^RESULT:' | ForEach-Object { Write-Host ("   " + $_.Line) -ForegroundColor Green }
        Write-Host "   log  : $LogPath"
    } else {
        Write-Host "   NO RESULT LOG (test crashed or exited early)" -ForegroundColor Red
    }
    if (-not $SkipShots) { Write-Host "   shots: $ShotsDir" }
    Write-Host "   exit : $rc"
    exit $rc
}
catch {
    Write-Host ""
    Write-Host ("ERROR: " + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
