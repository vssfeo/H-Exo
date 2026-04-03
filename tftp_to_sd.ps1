# tftp_to_sd.ps1 - Script to write kernel to SD card via TFTP

Write-Host "=== TFTP to SD Card Write Script ===" -ForegroundColor Green
Write-Host ""

# Ensure kernel file is in TFTP directory
$tftpDir = "C:\tftpboot"
$kernelFile = "$tftpDir\kernel_neuro.bin"

if (Test-Path $kernelFile) {
    $fileInfo = Get-Item $kernelFile
    Write-Host "[OK] Kernel file found: $kernelFile" -ForegroundColor Green
    Write-Host "[INFO] File size: $($fileInfo.Length) bytes" -ForegroundColor Cyan
} else {
    Write-Host "[ERROR] Kernel file not found: $kernelFile" -ForegroundColor Red
    Write-Host "Please ensure kernel_neuro.bin is compiled and copied to TFTP directory" -ForegroundColor Yellow
    exit 1
}

Write-Host "" 
Write-Host "=== Instructions for NanoPi M4 ===" -ForegroundColor Yellow
Write-Host "To write kernel to SD card via TFTP:" -ForegroundColor Cyan
Write-Host "" 
Write-Host "1. Connect to NanoPi M4 U-Boot console" -ForegroundColor Cyan
Write-Host "2. Ensure SD card is inserted" -ForegroundColor Cyan
Write-Host "3. Set environment variables:" -ForegroundColor Cyan
Write-Host "   setenv ipaddr 192.168.1.10" -ForegroundColor Gray
Write-Host "   setenv serverip 192.168.1.166" -ForegroundColor Gray
Write-Host "   setenv bootfile kernel_neuro.bin" -ForegroundColor Gray
Write-Host "" 
Write-Host "4. Use one of these commands:" -ForegroundColor Cyan
Write-Host "   For loading to memory only:" -ForegroundColor Cyan
Write-Host "   tftp \${loadaddr} \${bootfile}" -ForegroundColor Gray
Write-Host "" 
Write-Host "   For writing to SD card:" -ForegroundColor Cyan
Write-Host "   tftp \${loadaddr} \${bootfile}; mmc dev 0; mmc write \${loadaddr} 0x800 0x1000" -ForegroundColor Gray
Write-Host "" 
Write-Host "   Or create a script:" -ForegroundColor Cyan
Write-Host "   setenv sdwrite 'tftp \${loadaddr} \${bootfile}; mmc dev 0; mmc write \${loadaddr} 0x800 0x1000'" -ForegroundColor Gray
Write-Host "   saveenv" -ForegroundColor Gray
Write-Host "   run sdwrite" -ForegroundColor Gray
Write-Host "" 
Write-Host "5. Verify TFTP server is running on PC (192.168.1.166:69)" -ForegroundColor Cyan
Write-Host "" 
Write-Host "=== TFTP Setup Ready ===" -ForegroundColor Green
Write-Host "You can now write kernel to SD card via TFTP network connection" -ForegroundColor Green