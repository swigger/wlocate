param(
    [string]$BuildDirectory = "$PSScriptRoot\build\bin\Release",
    [string]$InstallDirectory = "$env:ProgramFiles\WLocate",
    [string]$CliDirectory = 'D:\tools\bin',
    [switch]$StageOnly
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory 'wlocate.exe')) -and (Test-Path -LiteralPath "$PSScriptRoot\wlocate.exe")) { $BuildDirectory = $PSScriptRoot }
Start-Transcript -Path "$PSScriptRoot\install-transcript.log" -Append | Out-Null
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this installer with psuac or from an elevated PowerShell.' }
$serviceExe = Join-Path $BuildDirectory 'wlocate.exe'
$cliExe = Join-Path $BuildDirectory 'locate.exe'
if (-not (Test-Path -LiteralPath $serviceExe) -or -not (Test-Path -LiteralPath $cliExe)) { throw 'Build both release executables before installing.' }
$existing = Get-Service -Name WLocateService -ErrorAction SilentlyContinue
if ($existing -and $existing.Status -ne 'Stopped') { Stop-Service -Name WLocateService; $existing.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(120)) }
New-Item -ItemType Directory -Path $InstallDirectory -Force | Out-Null
Copy-Item -LiteralPath $serviceExe -Destination (Join-Path $InstallDirectory 'wlocate.exe') -Force
Copy-Item -LiteralPath $cliExe -Destination (Join-Path $InstallDirectory 'locate.exe') -Force
foreach ($document in @('README.md', 'THIRD-PARTY-NOTICES.md', 'PERFORMANCE.md')) {
    if (Test-Path -LiteralPath (Join-Path $PSScriptRoot $document)) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $document) -Destination $InstallDirectory -Force }
}
if (Test-Path -LiteralPath "$PSScriptRoot\licenses") { Copy-Item -LiteralPath "$PSScriptRoot\licenses" -Destination $InstallDirectory -Recurse -Force }
$install = Start-Process -FilePath (Join-Path $InstallDirectory 'wlocate.exe') -ArgumentList '--install-service' -WindowStyle Hidden -PassThru -Wait
if ($install.ExitCode -ne 0) { throw "Service installation failed with exit code $($install.ExitCode); see ProgramData\WLocate\service.log" }
if (-not $StageOnly) {
    New-Item -ItemType Directory -Path $CliDirectory -Force | Out-Null
    $destination = Join-Path $CliDirectory 'locate.exe'
    $backup = $null
    if (Test-Path -LiteralPath $destination) { $backup = "$destination.backup-$(Get-Date -Format yyyyMMdd-HHmmss)"; Copy-Item -LiteralPath $destination -Destination $backup }
    $rollback = $backup
    $statePath = Join-Path $InstallDirectory 'install-state.json'
    if (Test-Path -LiteralPath $statePath) {
        $previous = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        if ($previous.locate -eq $destination -and $previous.previous_locate -and (Test-Path -LiteralPath $previous.previous_locate)) { $rollback = $previous.previous_locate }
    }
    Copy-Item -LiteralPath $cliExe -Destination $destination -Force
    Copy-Item -LiteralPath $serviceExe -Destination (Join-Path $CliDirectory 'wlocate.exe') -Force
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut("$env:ProgramData\Microsoft\Windows\Start Menu\Programs\WLocate.lnk")
    $shortcut.TargetPath = Join-Path $InstallDirectory 'wlocate.exe'
    $shortcut.WorkingDirectory = $InstallDirectory
    $shortcut.Description = 'WLocate 文件查找'
    $shortcut.IconLocation = (Join-Path $InstallDirectory 'wlocate.exe') + ',0'
    $shortcut.Save()
    @{ installed_at = (Get-Date -Format o); previous_locate = $rollback; last_update_backup = $backup; locate = $destination; gui = (Join-Path $InstallDirectory 'wlocate.exe') } | ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding utf8
}
Get-Service WLocateService | Format-Table Name, Status
Get-FileHash -LiteralPath (Join-Path $InstallDirectory 'wlocate.exe'), (Join-Path $InstallDirectory 'locate.exe') -Algorithm SHA256
Stop-Transcript | Out-Null
