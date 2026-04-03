руускийц# setup_tftp_final.ps1 - Final TFTP Setup Script

# Create TFTP directory if it doesn't exist
$tftpDir = "C:\tftpboot"
if (-not (Test-Path $tftpDir)) {
    Write-Host "Creating TFTP directory: $tftpDir" -ForegroundColor Yellow
    try {
        New-Item -ItemType Directory -Path $tftpDir -Force | Out-Null
        Write-Host "  [OK] Directory created" -ForegroundColor Green
    } catch {
        Write-Error "  [ERROR] Failed to create directory: $_"
        exit 1
    }
} else {
    Write-Host "  [OK] TFTP directory already exists: $tftpDir" -ForegroundColor Green
}

# Copy kernel_neuro.bin to TFTP directory if file exists
$kernelSource = "$PSScriptRoot\kernel_neuro.bin"
$kernelDest = "$tftpDir\kernel_neuro.bin"

if (Test-Path $kernelSource) {
    Write-Host "Copying kernel file to TFTP directory..." -ForegroundColor Yellow
    try {
        Copy-Item $kernelSource $kernelDest -Force
        Write-Host "  [OK] Kernel file copied" -ForegroundColor Green
        
        # Get file size
        $fileInfo = Get-Item $kernelDest
        Write-Host "  [INFO] File size: $($fileInfo.Length) bytes" -ForegroundColor Cyan
    } catch {
        Write-Warning "  [WARNING] Failed to copy kernel file: $_"
    }
} else {
    Write-Host "  [INFO] Kernel file not found in project root" -ForegroundColor Cyan
    Write-Host "  You can manually copy kernel_neuro.bin to $tftpDir after compilation" -ForegroundColor Cyan
}

Write-Host ""

# Display setup information
Write-Host "=== Setup Information ===" -ForegroundColor Yellow
Write-Host "TFTP Server IP: 192.168.1.166" -ForegroundColor Cyan
Write-Host "TFTP Directory: C:\tftpboot" -ForegroundColor Cyan
Write-Host "Kernel File: kernel_neuro.bin" -ForegroundColor Cyan
Write-Host ""

# Display next steps
Write-Host "=== Next Steps ===" -ForegroundColor Yellow
Write-Host "1. Install and start TFTP server (e.g., Tftpd64)" -ForegroundColor Cyan
Write-Host "2. Configure NanoPi M4 in U-Boot:" -ForegroundColor Cyan
Write-Host "   setenv ipaddr 192.168.1.10" -ForegroundColor Gray
Write-Host "   setenv serverip 192.168.1.166" -ForegroundColor Gray
Write-Host "   setenv bootfile kernel_neuro.bin" -ForegroundColor Gray
Write-Host "   setenv netboot 'tftp \${loadaddr} \${bootfile}; go \${loadaddr}'" -ForegroundColor Gray
Write-Host "   saveenv" -ForegroundColor Gray
Write-Host "3. Deploy and load kernel:" -ForegroundColor Cyan
Write-Host "   Run deploy script and then 'run netboot' in U-Boot" -ForegroundColor Gray

Write-Host ""
Write-Host "=== Setup Complete ===" -ForegroundColor Green
Write-Host "You can now significantly speed up your development cycle!" -ForegroundColor Green