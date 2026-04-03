param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Omni-Core: Optimized Test" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

$KernelPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\kernel_optimized.bin"
$KernelSize = (Get-Item $KernelPath).Length
$SectorCount = [Math]::Ceiling($KernelSize / 512)

Write-Host "[*] Kernel: $KernelPath" -ForegroundColor Yellow
Write-Host "[*] Size: $KernelSize bytes ($SectorCount sectors)" -ForegroundColor Yellow
Write-Host "[*] Optimizations:" -ForegroundColor Yellow
Write-Host "    - Exception Level detection & transition" -ForegroundColor Gray
Write-Host "    - Multi-core foundation (6 cores)" -ForegroundColor Gray
Write-Host "    - Fine-grained MMU (2MB blocks)" -ForegroundColor Gray
Write-Host "    - Hardware RNG for crypto-addressing" -ForegroundColor Gray
Write-Host "    - Full exception vector table" -ForegroundColor Gray
Write-Host "    - Modular HAL architecture" -ForegroundColor Gray
Write-Host ""
Write-Host "[*] REBOOT THE BOARD NOW!" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Cyan

# Run YMODEM transfer with optimized kernel
& "$PSScriptRoot\send_ymodem.ps1" `
    -PortName $PortName `
    -BaudRate $BaudRate `
    -FilePath $KernelPath `
    -LoadAddress 0x02080000 `
    -AutoBoot `
    -KernelSector 500000 `
    -KernelSectorCount $SectorCount

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "[*] H-Exo Omni-Core test completed" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Cyan
