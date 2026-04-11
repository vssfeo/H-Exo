$lines = Get-Content (Join-Path $PSScriptRoot 'deploy_tftp_fixed.ps1')
$ln = $lines[316]
Write-Host $ln
$bytes = [System.Text.Encoding]::UTF8.GetBytes($ln)
Write-Host (($bytes | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
