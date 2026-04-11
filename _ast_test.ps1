$path = Join-Path $PSScriptRoot 'deploy_tftp_fixed.ps1'
$tok = $null
$err = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tok, [ref]$err)
if ($err) { $err | ForEach-Object { $_.ToString() }; exit 1 }
Write-Host "AST OK, tokens: $($tok.Count)"
