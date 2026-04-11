param([string]$Port='COM3', [int]$Baud=1500000, [int]$Secs=30, [string]$Out='boot_log.txt')
$p = New-Object System.IO.Ports.SerialPort $Port,$Baud,([System.IO.Ports.Parity]::None),8,([System.IO.Ports.StopBits]::One)
$p.ReadTimeout = 200
$p.Open()
Write-Host "[*] Logging $Port for $Secs sec -> $Out"
Write-Host "[*] Power-cycle the board NOW..."
$deadline = (Get-Date).AddSeconds($Secs)
$log = [System.Text.StringBuilder]::new()
while ((Get-Date) -lt $deadline) {
    try { $chunk = $p.ReadExisting(); if ($chunk) { [void]$log.Append($chunk); Write-Host $chunk -NoNewline } } catch {}
    Start-Sleep -Milliseconds 20
}
$p.Close()
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $Out), $log.ToString())
Write-Host "`n[*] Saved $($log.Length) chars to $Out"
