$path = Join-Path $PSScriptRoot 'deploy_tftp_fixed.ps1'
$c = Get-Content -LiteralPath $path -Raw
$err = $null
$t = [System.Management.Automation.PSParser]::Tokenize($c, [ref]$err)
$t | Where-Object { $_.Type -eq 'GroupStart' -or $_.Type -eq 'GroupEnd' } | ForEach-Object {
    "${($_.StartLine)}:$($_.Type):$($_.Content)"
} | Select-Object -Last 40
