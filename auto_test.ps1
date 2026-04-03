param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [string]$KernelPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\kernel.bin",
    [UInt32]$LoadAddress = 0x02080000,
    [UInt32]$KernelSector = 500000
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Fully Automated Test System" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan
Write-Host "[*] No SD card removal required!" -ForegroundColor Green
Write-Host "[*] Full UART-based workflow" -ForegroundColor Green
Write-Host ""

# Вызываем существующий YMODEM скрипт с правильными параметрами
$sectorCount = [Math]::Ceiling((Get-Item $KernelPath).Length / 512)

Write-Host "[*] Kernel: $KernelPath" -ForegroundColor Yellow
Write-Host "[*] Size: $((Get-Item $KernelPath).Length) bytes ($sectorCount sectors)" -ForegroundColor Yellow
Write-Host "[*] Target sector: $KernelSector" -ForegroundColor Yellow
Write-Host "[*] Load address: 0x$($LoadAddress.ToString('X8'))" -ForegroundColor Yellow
Write-Host ""
Write-Host "[*] REBOOT THE BOARD NOW!" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Cyan

# Запускаем YMODEM передачу с автозагрузкой
& "$PSScriptRoot\send_ymodem.ps1" `
    -PortName $PortName `
    -BaudRate $BaudRate `
    -FilePath $KernelPath `
    -LoadAddress $LoadAddress `
    -AutoBoot `
    -KernelSector $KernelSector `
    -KernelSectorCount $sectorCount

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "[*] Test completed" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Cyan
