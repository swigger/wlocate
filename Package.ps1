$ErrorActionPreference = 'Stop'
$destination = Join-Path $PSScriptRoot 'out\WLocate-1.0.0-win-x64'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
foreach ($name in @('wlocate.exe', 'locate.exe')) { Copy-Item -LiteralPath "$PSScriptRoot\build\bin\Release\$name" -Destination $destination -Force }
foreach ($name in @('README.md', 'PERFORMANCE.md', 'THIRD-PARTY-NOTICES.md', 'Install.ps1')) { Copy-Item -LiteralPath "$PSScriptRoot\$name" -Destination $destination -Force }
Copy-Item -LiteralPath "$PSScriptRoot\licenses" -Destination $destination -Recurse -Force
$hashes = Get-FileHash -LiteralPath "$destination\wlocate.exe", "$destination\locate.exe" -Algorithm SHA256
$hashes | ForEach-Object { "$($_.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($_.Path))" } | Set-Content -LiteralPath "$destination\SHA256SUMS.txt" -Encoding ascii
$archive = "$destination.zip"
Compress-Archive -LiteralPath $destination -DestinationPath $archive -CompressionLevel Optimal -Force
Get-Item -LiteralPath $archive
$hashes
