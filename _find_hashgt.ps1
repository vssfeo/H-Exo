$lines = Get-Content (Join-Path $PSScriptRoot 'deploy_tftp_fixed.ps1')
for ($i = 107; $i -lt 343; $i++) {
    $line = $lines[$i]
    if ($line -like '*#>*') { Write-Host ($i + 1) $line }
}
