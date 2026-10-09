$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Start-Transcript -Path "$root\test-output\hotplug.log" -Force | Out-Null
$id = [guid]::NewGuid().ToString('N')
$image = [IO.Path]::GetFullPath((Join-Path "$root\test-output" "wlhotplug-$id.vhd"))
$testRoot = [IO.Path]::GetFullPath((Join-Path $root 'test-output')) + '\'
if (-not $image.StartsWith($testRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'VHD fixture escaped test-output.' }
$letter = @('Z','Y','X','W','V','U','T' | Where-Object { -not (Test-Path "${_}:\") -and -not (Test-Path "$env:ProgramData\WLocate\$_.widx") })[0]
if (-not $letter) { throw 'No unused test drive letter is available.' }
$diskScript = Join-Path "$root\test-output" "diskpart-$id.txt"
$exe = Join-Path $root 'build\bin\Release\locate.exe'
$name = "wlhotplug_$id.txt"
try {
    # Every disk operation is scoped to this newly created virtual image. No
    # physical disk is selected by number and no existing partition is touched.
    @("create vdisk file=`"$image`" maximum=64 type=expandable", "select vdisk file=`"$image`"", 'attach vdisk', 'create partition primary', 'format fs=ntfs quick label=WLocateTest', "assign letter=$letter") | Set-Content -LiteralPath $diskScript -Encoding ascii
    & diskpart.exe /s $diskScript | Out-File "$root\test-output\diskpart-create.log"
    $virtual = Get-DiskImage -ImagePath $image | Get-Disk
    $mounted = Get-Partition -DriveLetter $letter | Get-Disk
    if ($mounted.Number -ne $virtual.Number -or $virtual.BusType -ne 'File Backed Virtual') { throw 'Fixture drive did not map to the expected virtual disk.' }
    [IO.File]::WriteAllText("${letter}:\$name", 'WLocate hotplug fixture')
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $result = & $exe $name 2>$null
        if ($result -eq "${letter}:\$name") { break }
        if ($timer.Elapsed.TotalSeconds -gt 30) { throw 'Attached NTFS volume was not indexed.' }
        Start-Sleep -Milliseconds 100
    } while ($true)
    $attachMs = $timer.Elapsed.TotalMilliseconds
    Dismount-DiskImage -ImagePath $image | Out-Null
    $timer.Restart()
    do {
        $status = & $exe -S
        if (-not ($status -match "^${letter}:")) { break }
        if ($timer.Elapsed.TotalSeconds -gt 15) { throw 'Detached volume remained published.' }
        Start-Sleep -Milliseconds 100
    } while ($true)
    $report = @{ volume = $letter; attach_to_search_ms = $attachMs; detach_to_removed_ms = $timer.Elapsed.TotalMilliseconds; other_volumes_searchable = [bool](& $exe -n 1 'python*.exe') }
    $report | ConvertTo-Json | Set-Content "$root\test-output\hotplug.json" -Encoding utf8
    $report
} catch {
    $_ | Out-String | Write-Output
    throw
} finally {
    if (Test-Path -LiteralPath $image) {
        $state = Get-DiskImage -ImagePath $image -ErrorAction SilentlyContinue
        if ($state.Attached) { Dismount-DiskImage -ImagePath $image | Out-Null }
        Remove-Item -LiteralPath $image
    }
    if (Test-Path -LiteralPath $diskScript) { Remove-Item -LiteralPath $diskScript }
    Stop-Transcript | Out-Null
}
