# Only ephemeral hosted VMs: never create a test account on a developer machine.
param([string]$Packages, [Parameter(Mandatory)][string]$Output, [switch]$Probe)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'This account-isolation harness is restricted to disposable GitHub-hosted VMs'
}
# Task Scheduler's working-directory field is a Win32 path, unlike PowerShell
# provider paths. Never hand it the workflow's mixed slash spelling.
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
$task = $null
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
    # Password logon gives this standard user a private noninteractive desktop;
    # Monitor's render test uses WARP. No administrative runtime or WSL substitute.
    $phase = if ($Probe) { 'probe' } else { 'runtime' }
    $quoted = ($arguments + @('--phase', $phase) | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $action = New-ScheduledTaskAction -Execute $python -Argument $quoted -WorkingDirectory $Output
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -Compatibility Win8 `
        -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable
    $principal = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\$user" -LogonType Password -RunLevel Limited
    $definition = New-ScheduledTask -Action $action -Settings $settings -Principal $principal
    $task = Register-ScheduledTask -TaskName $user -InputObject $definition -User "$env:COMPUTERNAME\$user" -Password $password
    Export-ScheduledTask -TaskName $user | Set-Content (Join-Path $Output 'task.xml')
    $started = Get-Date
    Start-ScheduledTask -TaskName $user
    $deadline = $started.AddMinutes(6)
    do {
        Start-Sleep -Seconds 2
        $info = Get-ScheduledTaskInfo -TaskName $user
        $state = (Get-ScheduledTask -TaskName $user).State
        $diagnostic = @{ state = "$state"; lastResult = $info.LastTaskResult; lastRun = $info.LastRunTime.ToString('o') }
        $diagnostic | ConvertTo-Json | Set-Content (Join-Path $Output 'task-state.json')
        $ran = $info.LastRunTime -ge $started.AddSeconds(-5)
        if (!$ran -and (Get-Date) -gt $started.AddSeconds(30)) {
            throw "Standard-user task did not start: $($diagnostic | ConvertTo-Json -Compress)"
        }
        if ((Get-Date) -gt $deadline) { throw "Unelevated runtime timed out: $($diagnostic | ConvertTo-Json -Compress)" }
    } while ($state -eq 'Running' -or $state -eq 'Queued' -or !$ran)
    if ($info.LastTaskResult -ne 0 -or !(Test-Path (Join-Path $Output "$phase.json"))) {
        throw "Unelevated runtime failed: $($diagnostic | ConvertTo-Json -Compress)"
    }
    if (!$Probe) {
        & $python @arguments --phase receipt
        if ($LASTEXITCODE) { throw 'Windows receipt did not confirm qualification' }
    }
} catch {
    if ($task) {
        Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-TaskScheduler/Operational'; StartTime=$started} `
            -ErrorAction SilentlyContinue | Where-Object { $_.Message -like "*$user*" } |
            Select-Object Id, TimeCreated, Message | ConvertTo-Json -Depth 4 |
            Set-Content (Join-Path $Output 'task-events.json')
    }
    throw
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
