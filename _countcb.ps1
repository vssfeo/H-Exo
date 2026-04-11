$s = Get-Content (Join-Path $PSScriptRoot 'deploy_tftp_fixed.ps1') -Raw
$o = ([regex]::Matches($s, '\{')).Count
$c = ([regex]::Matches($s, '\}')).Count
Write-Host "open $o close $c diff $($o - $c)"
