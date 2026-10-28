# Only ephemeral hosted VMs: never create a test account on a developer machine.
param([string]$Packages, [Parameter(Mandatory)][string]$Output, [switch]$Probe)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'This account-isolation harness is restricted to disposable GitHub-hosted VMs'
}
# Pass canonical Win32 directories to the process launcher.
$Output = [System.IO.Path]::GetFullPath($Output)
if ($Packages) { $Packages = [System.IO.Path]::GetFullPath($Packages) }
$python = (Get-Command python).Source
$script = Join-Path $PSScriptRoot 'qualify_windows.py'
$arguments = @('-B', $script, '--output', $Output, '--sha', $env:GITHUB_SHA,
    '--run-id', $env:GITHUB_RUN_ID, '--attempt', $env:GITHUB_RUN_ATTEMPT)
New-Item -ItemType Directory -Path $Output -Force | Out-Null
if (!$Probe) {
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
try {
    $account = New-LocalUser -Name $user -Password (ConvertTo-SecureString $password -AsPlainText -Force)
    Add-LocalGroupMember -SID 'S-1-5-32-545' -Member $user
    # The test owns its profile/TEMP. It only reads source and packages, writes proof.
    & icacls $env:GITHUB_WORKSPACE /grant "${user}:(OI)(CI)RX" /T /Q | Out-Null
    if ($LASTEXITCODE) { throw 'Source access could not be granted' }
    if (!$Probe) {
        & icacls $Packages /grant "${user}:(OI)(CI)RX" /T /Q | Out-Null
        if ($LASTEXITCODE) { throw 'Package access could not be granted' }
    }
    & icacls $Output /grant "${user}:(OI)(CI)M" /T /Q | Out-Null
    if ($LASTEXITCODE) { throw 'Proof access could not be granted' }
    # CreateProcessWithLogonW via Start-Process: a direct, bounded child with a
    # loaded standard-user profile. No Scheduler/interactive-logon dependency.
    $phase = if ($Probe) { 'probe' } else { 'runtime' }
    $quoted = ($arguments + @('--phase', $phase) | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $credential = [System.Management.Automation.PSCredential]::new("$env:COMPUTERNAME\$user",
        (ConvertTo-SecureString $password -AsPlainText -Force))
    $child = Start-Process -FilePath $python -ArgumentList $quoted -WorkingDirectory $Output `
        -Credential $credential -LoadUserProfile -PassThru `
        -RedirectStandardOutput (Join-Path $Output 'launcher.stdout.log') `
        -RedirectStandardError (Join-Path $Output 'launcher.stderr.log')
    $timeout = if ($Probe) { 30000 } else { 300000 }
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
    if (!$Probe) {
        & $python @arguments --phase receipt
        if ($LASTEXITCODE) { throw 'Windows receipt did not confirm qualification' }
    }
} finally {
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
}
