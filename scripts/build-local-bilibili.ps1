param([ValidateRange(1,2)][int]$Jobs = 1)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$process = Get-Process -Id $PID
$priority = $process.PriorityClass
$reuse = $env:MSBUILDDISABLENODEREUSE
$guard = [Threading.Mutex]::new($false,'Global\Utae.StreamingBuild.Single')
$held = $false
try {
    try { $held = $guard.WaitOne(0) } catch [Threading.AbandonedMutexException] { $held = $true }
    if (!$held -or (Get-Process cargo,rustc,cl,link,cmake -ErrorAction SilentlyContinue)) { throw 'Another build is running' }
    if (Get-CimInstance Win32_Process -Filter "name='MSBuild.exe'" | Where-Object { $_.CommandLine -notmatch '/nodemode:' }) { throw 'Another native build is running' }
    $process.PriorityClass = 'BelowNormal'
    $env:MSBUILDDISABLENODEREUSE = '1'
    & cmake -S $root -B $build -A x64
    if ($LASTEXITCODE) { throw 'bilibili configure failed' }
    & cmake --build $build --config Release --parallel $Jobs --target BILIBILILocalFx BilibiliTests
    if ($LASTEXITCODE) { throw 'bilibili build failed' }
    $versionCache = Get-Content (Join-Path $build 'CMakeCache.txt') | Where-Object { $_ -match '^CMAKE_PROJECT_VERSION:STATIC=' }
    if ($versionCache -notmatch '=([0-9]+\.[0-9]+\.[0-9]+)$') { throw 'Could not read the package version' }
    $version = $Matches[1]
    $artifact = Join-Path $build "package/bilibili-v$version.fx.audcore"
    $env:PYTHONUTF8 = '1'
    & python (Join-Path $PSScriptRoot 'test-bilibili-fx.py') $artifact (Join-Path $build 'Release/BilibiliTests.exe')
    if ($LASTEXITCODE) { throw 'bilibili signal or media test failed' }
    Write-Output $artifact
} finally {
    $process.PriorityClass = $priority
    $env:MSBUILDDISABLENODEREUSE = $reuse
    if ($held) { $guard.ReleaseMutex() }
    $guard.Dispose()
}



