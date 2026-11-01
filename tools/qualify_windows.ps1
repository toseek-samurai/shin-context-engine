# Only ephemeral hosted VMs: never create a test account on a developer machine.
param([string]$Packages, [Parameter(Mandatory)][string]$Output, [switch]$Probe,
    [string]$FeatureTest, [string]$Cli, [string]$Profiles)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'This account-isolation harness is restricted to disposable GitHub-hosted VMs'
}
function Invoke-HostedMsi([string]$Operation, [string]$Package, [string]$Log) {
    $process = Start-Process -FilePath "$env:WINDIR\System32\msiexec.exe" -ArgumentList "$Operation `"$Package`" /qn /norestart /L*V `"$Log`"" -WindowStyle Hidden -PassThru
    try {
        if (!$process.WaitForExit(180000)) { throw 'Hosted test MSI exceeded its deadline; installation outcome is unknown' }
        $process.Refresh()
        return $process.ExitCode
    } finally { $process.Dispose() }
}
# Pass canonical Win32 directories to the process launcher.
$Output = [System.IO.Path]::GetFullPath($Output)
if ($Packages) { $Packages = [System.IO.Path]::GetFullPath($Packages) }
$features = ![string]::IsNullOrEmpty($FeatureTest)
if ($features) {
    if ($Probe -or $Packages -or !$Cli -or !$Profiles) { throw 'Feature tests require only test, CLI and profile paths' }
    $FeatureTest = [System.IO.Path]::GetFullPath($FeatureTest)
    $Cli = [System.IO.Path]::GetFullPath($Cli)
    $Profiles = [System.IO.Path]::GetFullPath($Profiles)
}
$python = (Get-Command python).Source
$script = Join-Path $PSScriptRoot 'qualify_windows.py'
$arguments = @('-B', $script, '--output', $Output, '--sha', $env:GITHUB_SHA,
    '--run-id', $env:GITHUB_RUN_ID, '--attempt', $env:GITHUB_RUN_ATTEMPT)
New-Item -ItemType Directory -Path $Output -Force | Out-Null
if ($features) { $arguments += @('--feature-test', $FeatureTest, '--cli', $Cli, '--profiles', $Profiles) }
if (!$Probe -and !$features) {
    if (!$Packages) { throw 'Package directory is required' }
    $archive = @(Get-ChildItem $Packages -Filter '*.zip')
    $msi = @(Get-ChildItem $Packages -Filter '*.msi')
    if ($archive.Count -ne 1 -or $msi.Count -ne 1) { throw 'Expected one ZIP and one MSI' }
    $arguments += @('--archive', $archive[0].FullName, '--msi', $msi[0].FullName)
    & $python @arguments --phase package
    if ($LASTEXITCODE) { throw 'MSI metadata/payload qualification failed' }
}
$user = 'shinproof' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$password = 'Sh1n!' + [guid]::NewGuid().ToString('N')
$account = $null
$child = $null
$preflight = $null
$installedPackage = $false
try {
    $account = New-LocalUser -Name $user -Password (ConvertTo-SecureString $password -AsPlainText -Force)
    Add-LocalGroupMember -SID 'S-1-5-32-545' -Member $user
    # The test owns its profile/TEMP. It only reads source and packages, writes proof.
    & icacls $env:GITHUB_WORKSPACE /grant "${user}:(OI)(CI)RX" /T /Q | Out-Null
    if ($LASTEXITCODE) { throw 'Source access could not be granted' }
    if (!$Probe) {
        $readPaths = if ($features) { @((Split-Path $FeatureTest), (Split-Path $Cli), $Profiles) } else { @($Packages) }
        foreach ($readPath in $readPaths) {
            & icacls $readPath /grant "${user}:(OI)(CI)RX" /T /Q | Out-Null
            if ($LASTEXITCODE) { throw 'Test input access could not be granted' }
        }
    }
    & icacls $Output /grant "${user}:(OI)(CI)M" /T /Q | Out-Null
    if ($LASTEXITCODE) { throw 'Proof access could not be granted' }
    # CreateProcessWithLogonW via Start-Process: a direct, bounded child with a
    # loaded standard-user profile. No Scheduler/interactive-logon dependency.
    $phase = if ($Probe) { 'probe' } elseif ($features) { 'features' } else { 'runtime' }
    $quoted = ($arguments + @('--phase', $phase) | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $credential = [System.Management.Automation.PSCredential]::new("$env:COMPUTERNAME\$user",
        (ConvertTo-SecureString $password -AsPlainText -Force))
    $child = Start-Process -FilePath $python -ArgumentList $quoted -WorkingDirectory $Output `
        -Credential $credential -LoadUserProfile -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $Output 'launcher.stdout.log') `
        -RedirectStandardError (Join-Path $Output 'launcher.stderr.log')
    $timeout = if ($Probe) { 30000 } elseif ($features) { 60000 } else { 300000 }
    if (!$child.WaitForExit($timeout)) {
        $child.Kill($true)
        $child.WaitForExit()
        throw 'Standard-user child timed out'
    }
    $child.Refresh()
    @{ pid = $child.Id; exitCode = $child.ExitCode } | ConvertTo-Json | Set-Content (Join-Path $Output 'launcher.json')
    if ($child.ExitCode -ne 0 -or !(Test-Path (Join-Path $Output "$phase.json"))) {
        Get-Content (Join-Path $Output 'launcher.stderr.log') -ErrorAction SilentlyContinue
        throw "Standard-user child failed: $($child.ExitCode)"
    }
    if (!$Probe -and !$features) {
        if ((Test-Path -LiteralPath 'HKLM:\SOFTWARE\Shin\DesktopPreview') -or
            (Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'Shin Desktop Preview'))) { throw 'Hosted VM already has Shin; refusing to replace an existing installation' }
        $installedPackage = $true
        $installLog = Join-Path $Output 'installed-preflight-install.log'
        $installCode = Invoke-HostedMsi '/i' $msi[0].FullName $installLog
        if ($installCode -ne 0) { throw "Exact MSI installation failed: $installCode" }
        $preflightQuoted = ($arguments + @('--phase', 'installed-preflight') | ForEach-Object { '"' + $_ + '"' }) -join ' '
        $preflight = Start-Process -FilePath $python -ArgumentList $preflightQuoted -WorkingDirectory $Output `
            -Credential $credential -LoadUserProfile -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $Output 'installed-preflight.stdout.log') `
            -RedirectStandardError (Join-Path $Output 'installed-preflight.stderr.log')
        if (!$preflight.WaitForExit(60000)) {
            $preflight.Kill($true); $preflight.WaitForExit(); throw 'Installed update preflight timed out'
        }
        $preflight.Refresh()
        if ($preflight.ExitCode -ne 0 -or !(Test-Path (Join-Path $Output 'installed-preflight.json'))) {
            Get-Content (Join-Path $Output 'installed-preflight.stderr.log') -ErrorAction SilentlyContinue
            throw "Installed update preflight failed: $($preflight.ExitCode)"
        }
        & $python @arguments --phase receipt
        if ($LASTEXITCODE) { throw 'Windows receipt did not confirm qualification' }
    }
} finally {
    $cleanupFailure = $null
    if ($preflight) {
        if (!$preflight.HasExited) { $preflight.Kill($true); $preflight.WaitForExit() }
        $preflight.Dispose()
    }
    if ($installedPackage -and (Test-Path -LiteralPath 'HKLM:\SOFTWARE\Shin\DesktopPreview')) {
        try {
            $removeLog = Join-Path $Output 'installed-preflight-remove.log'
            $removeCode = Invoke-HostedMsi '/x' $msi[0].FullName $removeLog
            if ($removeCode -ne 0) { $cleanupFailure = "Hosted test MSI cleanup failed: $removeCode" }
        } catch { $cleanupFailure = $_.Exception.Message }
    }
    if ($child) {
        if (!$child.HasExited) { $child.Kill($true); $child.WaitForExit() }
        $child.Dispose()
    }
    if ($account) {
        $sid = $account.SID.Value
        Get-CimInstance Win32_UserProfile | Where-Object SID -eq $sid | Remove-CimInstance
        Remove-LocalUser -Name $user
    }
    $password = $null
    if ($cleanupFailure) { throw $cleanupFailure }
}
