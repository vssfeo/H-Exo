# setup_tftp_network.ps1 - Complete TFTP Network Setup

Write-Host "=== Complete TFTP Network Setup ===" -ForegroundColor Green
Write-Host ""

# 1. Create TFTP directory
Write-Host "1. Creating TFTP directory..." -ForegroundColor Yellow
$tftpDir = "C:\tftpboot"
if (-not (Test-Path $tftpDir)) {
    try {
        New-Item -ItemType Directory -Path $tftpDir -Force | Out-Null
        Write-Host "   [OK] TFTP directory created at $tftpDir" -ForegroundColor Green
    } catch {
        Write-Error "   [ERROR] Failed to create TFTP directory: $_"
        exit 1
    }
} else {
    Write-Host "   [OK] TFTP directory already exists at $tftpDir" -ForegroundColor Green
}

# 2. Copy kernel file to TFTP directory
Write-Host "2. Copying kernel file to TFTP directory..." -ForegroundColor Yellow
$kernelSource = "$PSScriptRoot\kernel_neuro.bin"
$kernelDest = "$tftpDir\kernel_neuro.bin"

if (Test-Path $kernelSource) {
    try {
        Copy-Item $kernelSource $kernelDest -Force
        Write-Host "   [OK] Kernel file copied to $kernelDest" -ForegroundColor Green
        
        # Get file size
        $fileInfo = Get-Item $kernelDest
        Write-Host "   [INFO] Kernel file size: $($fileInfo.Length) bytes" -ForegroundColor Cyan
    } catch {
        Write-Warning "   [WARNING] Failed to copy kernel file: $_"
    }
} else {
    Write-Host "   [INFO] Kernel file not found in project root" -ForegroundColor Cyan
    Write-Host "   Please compile your kernel and copy kernel_neuro.bin to $tftpDir" -ForegroundColor Cyan
}

# 3. Check network configuration
Write-Host "3. Checking network configuration..." -ForegroundColor Yellow

# Get network interfaces
$ipAddresses = Get-NetIPAddress | Where-Object {$_.AddressFamily -eq "IPv4" -and $_.InterfaceAlias -notlike "*Loopback*"}

$tftpServerIP = $null
foreach ($ip in $ipAddresses) {
    Write-Host "   Interface: $($ip.InterfaceAlias)" -ForegroundColor Cyan
    Write-Host "   IP Address: $($ip.IPAddress)/$($ip.PrefixLength)" -ForegroundColor Cyan
    
    # Check if this is our expected TFTP server IP
    if ($ip.IPAddress -eq "192.168.1.166") {
        $tftpServerIP = $ip.IPAddress
        Write-Host "   [MATCH] This is the TFTP server IP address" -ForegroundColor Green
    }
}

if ($tftpServerIP -eq $null) {
    # Use the first available IP address
    $firstIP = ($ipAddresses | Select-Object -First 1).IPAddress
    if ($firstIP) {
        $tftpServerIP = $firstIP
        Write-Host "   [INFO] Using $tftpServerIP as TFTP server IP" -ForegroundColor Cyan
    }
}

# 4. Check TFTP port availability
Write-Host "4. Checking TFTP port availability..." -ForegroundColor Yellow

try {
    $udp = New-Object System.Net.Sockets.UdpClient(69)
    Write-Host "   [OK] Successfully bound to UDP port 69" -ForegroundColor Green
    $udp.Close()
} catch {
    Write-Host "   [WARNING] Could not bind to UDP port 69: $($_.Exception.Message)" -ForegroundColor Yellow
    Write-Host "   This might indicate that another TFTP server is running" -ForegroundColor Cyan
}

# 5. Display setup summary
Write-Host "" 
Write-Host "=== TFTP Network Setup Summary ===" -ForegroundColor Yellow
Write-Host "TFTP Directory: $tftpDir" -ForegroundColor Cyan
Write-Host "TFTP Server IP: $tftpServerIP" -ForegroundColor Cyan
Write-Host "TFTP Port: 69 (UDP)" -ForegroundColor Cyan
Write-Host "Kernel File: kernel_neuro.bin" -ForegroundColor Cyan
Write-Host ""

# 6. Display NanoPi M4 configuration instructions
Write-Host "=== NanoPi M4 Configuration Instructions ===" -ForegroundColor Yellow
Write-Host "To configure your NanoPi M4 for TFTP boot:" -ForegroundColor Cyan
Write-Host "1. Connect to NanoPi M4 U-Boot console" -ForegroundColor Cyan
Write-Host "2. Set the following environment variables:" -ForegroundColor Cyan
Write-Host "   setenv ipaddr 192.168.1.10" -ForegroundColor Gray
Write-Host "   setenv serverip $tftpServerIP" -ForegroundColor Gray
Write-Host "   setenv bootfile kernel_neuro.bin" -ForegroundColor Gray
Write-Host "   setenv netboot 'tftp \${loadaddr} \${bootfile}; go \${loadaddr}'" -ForegroundColor Gray
Write-Host "   saveenv" -ForegroundColor Gray
Write-Host "3. Test network connectivity:" -ForegroundColor Cyan
Write-Host "   ping $tftpServerIP" -ForegroundColor Gray
Write-Host "4. Boot from TFTP:" -ForegroundColor Cyan
Write-Host "   run netboot" -ForegroundColor Gray

Write-Host "" 
Write-Host "=== Setup Complete ===" -ForegroundColor Green
Write-Host "Your TFTP network setup is ready!" -ForegroundColor Green