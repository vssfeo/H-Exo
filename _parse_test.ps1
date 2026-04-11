$path = Join-Path $PSScriptRoot 'deploy_tftp_fixed.ps1'
$text = Get-Content -LiteralPath $path -Raw
$err = $null
$null = [System.Management.Automation.Language.Parser]::ParseInput($text, $path, [ref]$null, [ref]$err)
if ($err) { $err | ForEach-Object { "$($_.Extent.StartLineNumber): $($_.Message)" }; exit 1 }
Write-Host 'PARSE OK'
