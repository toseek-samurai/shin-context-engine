# CI-only MSVC/Ninja setup. Keep the interactive Visual Studio presets intact.
$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or -not $env:GITHUB_ENV -or -not $env:RUNNER_TEMP) {
    throw 'Run this setup in a Windows Actions job'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -version '[17.0,18.0)' -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -or -not $vs) { throw 'Visual Studio 2022 C++ tools are required' }
& (Join-Path $vs 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'MSVC environment was not initialized' }
if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
    $ninja = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja'
    if (-not (Test-Path "$ninja/ninja.exe" -PathType Leaf)) { throw 'Ninja is required' }
    $env:PATH = "$ninja;$env:PATH"
}
# Export only toolchain variables, never a complete environment/token dump.
foreach ($key in @('PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'VCToolsInstallDir', 'VSINSTALLDIR',
        'WindowsSdkDir', 'WindowsSDKVersion', 'WindowsSdkBinPath', 'WindowsSdkVerBinPath',
        'VSCMD_ARG_TGT_ARCH', 'VSCMD_ARG_HOST_ARCH', 'UCRTVersion', 'UniversalCRTSdkDir')) {
    $value = [Environment]::GetEnvironmentVariable($key)
    if ($value) {
        if ($value.Contains("`n") -or $value.Contains("`r")) { throw 'Invalid toolchain environment' }
        "$key=$value" >> $env:GITHUB_ENV
    }
}

$launcher = ''
try {
    $archive = Join-Path $env:RUNNER_TEMP 'sccache.zip'
    Invoke-WebRequest 'https://github.com/mozilla/sccache/releases/download/v0.19.0/sccache-v0.19.0-x86_64-pc-windows-msvc.zip' -OutFile $archive -TimeoutSec 60
    if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne '28be8c61f6b2c89b9eb2869f6af7f78e6f992e78a8abae0ce855fcc129cbeac1') {
        throw 'sccache checksum mismatch'
    }
    $directory = Join-Path $env:RUNNER_TEMP 'sccache-tool'
    Expand-Archive $archive -DestinationPath $directory
    $launcher = Join-Path $directory 'sccache-v0.19.0-x86_64-pc-windows-msvc/sccache.exe'
    & $launcher --version
    if ($LASTEXITCODE) { throw 'sccache could not start' }
    (Split-Path $launcher) >> $env:GITHUB_PATH
} catch {
    # A cache/tool download failure still permits ordinary MSVC compilation.
    Write-Warning 'Compiler cache unavailable; building directly with MSVC.'
    $launcher = ''
}
"SHIN_COMPILER_LAUNCHER=$launcher" >> $env:GITHUB_ENV
"SCCACHE_DIR=$env:RUNNER_TEMP/sccache" >> $env:GITHUB_ENV
"SCCACHE_CACHE_SIZE=500M" >> $env:GITHUB_ENV
"SCCACHE_BASEDIRS=$env:GITHUB_WORKSPACE;$env:SHIN_BUILD_ROOT" >> $env:GITHUB_ENV
# sccache's documented direct-compiler fallback for an unavailable local daemon.
"SCCACHE_IGNORE_SERVER_IO_ERROR=1" >> $env:GITHUB_ENV
# A caught cache-tool failure must not leak into Actions' native exit check.
$global:LASTEXITCODE = 0
