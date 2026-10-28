# Only ephemeral hosted VMs: never create a test account on a developer machine.
param([Parameter(Mandatory)][string]$Packages, [Parameter(Mandatory)][string]$Output)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'This account-isolation harness is restricted to disposable GitHub-hosted VMs'
}
$archive = @(Get-ChildItem $Packages -Filter '*.zip')
$msi = @(Get-ChildItem $Packages -Filter '*.msi')
if ($archive.Count -ne 1 -or $msi.Count -ne 1) { throw 'Expected one ZIP and one MSI' }
$python = (Get-Command python).Source
$script = Join-Path $PSScriptRoot 'qualify_windows.py'
$arguments = @('-B', $script, '--archive', $archive[0].FullName, '--msi', $msi[0].FullName,
    '--output', $Output, '--sha', $env:GITHUB_SHA, '--run-id', $env:GITHUB_RUN_ID, '--attempt', $env:GITHUB_RUN_ATTEMPT)
& $python @arguments --phase package
if ($LASTEXITCODE) { throw 'MSI metadata/payload qualification failed' }
$user = 'shinproof' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$password = 'Sh1n!' + [guid]::NewGuid().ToString('N')
$account = $null
$task = $null
try {
    $account = New-LocalUser -Name $user -Password (ConvertTo-SecureString $password -AsPlainText -Force)
    Add-LocalGroupMember -SID 'S-1-5-32-545' -Member $user
    # The test owns its profile/TEMP. It only reads source and packages, writes proof.
    & icacls $env:GITHUB_WORKSPACE /grant "${user}:(OI)(CI)RX" /T /Q | Out-Null
    if ($LASTEXITCODE) { throw 'Source access could not be granted' }
    & icacls $Packages /grant "${user}:(OI)(CI)RX" /T /Q | Out-Null
    if ($LASTEXITCODE) { throw 'Package access could not be granted' }
    & icacls $Output /grant "${user}:(OI)(CI)M" /T /Q | Out-Null
    if ($LASTEXITCODE) { throw 'Proof access could not be granted' }
    # Password logon gives this standard user a private noninteractive desktop;
    # Monitor's render test uses WARP. No administrative runtime or WSL substitute.
    $quoted = ($arguments + @('--phase', 'runtime') | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $action = New-ScheduledTaskAction -Execute $python -Argument $quoted -WorkingDirectory $Output
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 6)
    $task = Register-ScheduledTask -TaskName $user -Action $action -Settings $settings -User $user -Password $password -RunLevel Limited
    Start-ScheduledTask -TaskName $user
    $deadline = (Get-Date).AddMinutes(7)
    do {
        Start-Sleep -Seconds 2
        $info = Get-ScheduledTaskInfo -TaskName $user
        $state = (Get-ScheduledTask -TaskName $user).State
        if ((Get-Date) -gt $deadline) { throw 'Unelevated runtime timed out' }
    } while ($state -eq 'Running' -or $info.LastRunTime.Year -lt 2020)
    if ($info.LastTaskResult -ne 0 -or !(Test-Path (Join-Path $Output 'runtime.json'))) {
        throw "Unelevated runtime failed: $($info.LastTaskResult)"
    }
    & $python @arguments --phase receipt
    if ($LASTEXITCODE) { throw 'Windows receipt did not confirm qualification' }
} finally {
    if ($task) {
        Stop-ScheduledTask -TaskName $user -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $user -Confirm:$false
    }
    if ($account) {
        $sid = $account.SID.Value
        Get-CimInstance Win32_UserProfile | Where-Object SID -eq $sid | Remove-CimInstance
        Remove-LocalUser -Name $user
    }
    $password = $null
}
