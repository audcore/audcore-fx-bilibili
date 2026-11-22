param([ValidateRange(1,2)][int]$Jobs = 1, [switch]$Vst3)
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
    $targets = @('BILIBILILocalFx', 'BilibiliTests')
    $vst3Include = ''
    if ($Vst3) {
        $targets += @('BilibiliVst3', 'BilibiliVst3Tests')
        $vst3Include = Join-Path $root 'vst3/Enable.cmake'
        $playerChanges = & git -C $root status --porcelain -- CMakeLists.txt src include vendor
        if ($LASTEXITCODE -or $playerChanges) { throw 'Commit the player sources before building the VST3 for delivery' }
    }
    & cmake -S $root -B $build -A x64 "-DCMAKE_PROJECT_BilibiliFx_INCLUDE=$vst3Include"
    if ($LASTEXITCODE) { throw 'bilibili configure failed' }
    & cmake --build $build --config Release --parallel $Jobs --target @targets
    if ($LASTEXITCODE) { throw 'bilibili build failed' }
    $versionCache = Get-Content (Join-Path $build 'CMakeCache.txt') | Where-Object { $_ -match '^CMAKE_PROJECT_VERSION:STATIC=' }
    if ($versionCache -notmatch '=([0-9]+\.[0-9]+\.[0-9]+)$') { throw 'Could not read the package version' }
    $version = $Matches[1]
    $artifact = Join-Path $build "package/bilibili-v$version.fx.audcore"
    $env:PYTHONUTF8 = '1'
    & python (Join-Path $PSScriptRoot 'test-bilibili-fx.py') $artifact (Join-Path $build 'Release/BilibiliTests.exe')
    if ($LASTEXITCODE) { throw 'bilibili signal or media test failed' }
    Write-Output $artifact
    if ($Vst3) {
        $vst3Artifact = Join-Path $build "package/Bilibili-v$version.vst3"
        if ((Get-Item -LiteralPath $vst3Artifact).VersionInfo.ProductVersion -cne $version) {
            throw 'Bilibili VST3 version resource does not match the package name'
        }
        & python (Join-Path $root '../audcore/scripts/audit-release.py') $vst3Artifact --check
        if ($LASTEXITCODE) { throw 'Bilibili VST3 binary audit failed' }
        & (Join-Path $build 'Release/BilibiliVst3Tests.exe') $vst3Artifact $version
        if ($LASTEXITCODE) { throw 'Bilibili VST3 host validation failed' }
        Write-Output $vst3Artifact
    }
} finally {
    $process.PriorityClass = $priority
    $env:MSBUILDDISABLENODEREUSE = $reuse
    if ($held) { $guard.ReleaseMutex() }
    $guard.Dispose()
}



