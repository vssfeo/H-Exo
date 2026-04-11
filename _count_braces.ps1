$lines = Get-Content (Join-Path $PSScriptRoot 'deploy_tftp_fixed.ps1')
$depth = 0
$ln = 0
foreach ($line in $lines) {
    $ln++
    if ($ln -lt 86) { continue }
    if ($ln -gt 375) { break }
    $opens = ([regex]::Matches($line, '\{')).Count
    $closes = ([regex]::Matches($line, '\}')).Count
    $depth += $opens - $closes
    if ($opens -gt 0 -or $closes -gt 0) {
        Write-Host "${ln}: depth=$depth | $line"
    }
}
Write-Host "final depth 86-375: $depth"
