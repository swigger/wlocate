$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Start-Transcript -Path "$root\test-output\restart.log" -Force | Out-Null
$prefix = 'wlrestart_' + [guid]::NewGuid().ToString('N')
$fixture = Join-Path "$root\test-output" "$prefix.txt"
$exe = Join-Path $root 'build\bin\Release\locate.exe'
$service = Get-Service WLocateService
try {
    Stop-Service WLocateService
    $service.WaitForStatus('Stopped', [timespan]::FromSeconds(120))
    [IO.File]::WriteAllText($fixture, 'Created while the index service was stopped.')
    $timer = [Diagnostics.Stopwatch]::StartNew()
    Start-Service WLocateService
    do {
        $result = & $exe "$prefix*.txt" 2>$null
        if ($result -eq $fixture) { break }
        if ($timer.Elapsed.TotalSeconds -gt 30) { throw 'Offline USN update was not recovered within 30 seconds.' }
        Start-Sleep -Milliseconds 100
    } while ($true)
    $report = @{ offline_create_replayed = $true; restart_to_result_ms = $timer.Elapsed.TotalMilliseconds }
    $report | ConvertTo-Json | Set-Content "$root\test-output\restart.json" -Encoding utf8
    $report
} finally {
    if ((Get-Service WLocateService).Status -ne 'Running') { Start-Service WLocateService }
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture }
    Stop-Transcript | Out-Null
}
