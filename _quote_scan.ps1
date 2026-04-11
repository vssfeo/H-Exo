$path = Join-Path $PSScriptRoot 'deploy_tftp_fixed.ps1'
$b = [IO.File]::ReadAllBytes($path)
for ($i = 0; $i -lt $b.Length; $i++) {
    $c = $b[$i]
    if ($c -eq 0x22 -or $c -eq 0x27) { continue } # normal
    if ($c -eq 0xE2 -and $i + 2 -lt $b.Length) {
        $w = ($b[$i+1] -shl 8) -bor $b[$i+2]
        # UTF-8 lead E2: check for curly quotes 80 9C / 80 9D or 80 99
        if ($b[$i+1] -eq 0x80 -and ($b[$i+2] -in 0x9C,0x9D,0x98,0x99)) {
            Write-Host "UTF8 smart quote at byte $i"
        }
    }
}
Write-Host 'scan done'
