param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Neural Arbitrator Test" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

$KernelPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\kernel_neuro.bin"
$KernelSize = (Get-Item $KernelPath).Length
$SectorCount = [Math]::Ceiling($KernelSize / 512)

Write-Host "[*] Kernel: Neural Arbitrator v0.3" -ForegroundColor Yellow
Write-Host "[*] Size: $KernelSize bytes ($SectorCount sectors)" -ForegroundColor Yellow
Write-Host "[*] Features:" -ForegroundColor Yellow
Write-Host "    - TinyML Inference Engine (6->8->4 network)" -ForegroundColor Gray
Write-Host "    - Fixed-Point Arithmetic (Q16.16)" -ForegroundColor Gray
Write-Host "    - Real-time Telemetry (CPU, L2, Memory, Thermal)" -ForegroundColor Gray
Write-Host "    - Predictive Task Migration" -ForegroundColor Gray
Write-Host "    - Adaptive Power Management" -ForegroundColor Gray
Write-Host "    - Self-Healing Mesh Foundation" -ForegroundColor Gray
Write-Host ""
Write-Host "[*] REBOOT THE BOARD NOW!" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Cyan

# Run YMODEM transfer
& "$PSScriptRoot\send_ymodem.ps1" `
    -PortName $PortName `
    -BaudRate $BaudRate `
    -FilePath $KernelPath `
    -LoadAddress 0x02080000 `
    -AutoBoot `
    -KernelSector 500000 `
    -KernelSectorCount $SectorCount

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "[*] Neural Arbitrator test completed" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Cyan
