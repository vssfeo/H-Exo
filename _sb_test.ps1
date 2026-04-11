try {
    [scriptblock]::Create((Get-Content -LiteralPath (Join-Path $PSScriptRoot 'deploy_tftp_fixed.ps1') -Raw)) | Out-Null
    Write-Host 'SCRIPTBLOCK OK'
} catch {
    Write-Host $_.Exception.Message
    Write-Host $_.InvocationInfo.PositionMessage
}
