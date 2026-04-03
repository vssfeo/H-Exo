# Check TFTP server readiness
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  TFTP Server Readiness Check" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check 1: IP address
Write-Host "[1] Checking network configuration..." -ForegroundColor Yellow
$adapters = Get-NetIPAddress -AddressFamily IPv4 | Where-Object { 
    $_.IPAddress -like "192.168.*" 
}

if ($adapters) {
    Write-Host "[+] Found IPv4 addresses:" -ForegroundColor Green
    foreach ($adapter in $adapters) {
        Write-Host "    $($adapter.IPAddress) on $($adapter.InterfaceAlias)" -ForegroundColor White
    }
    
    $has166 = $adapters | Where-Object { $_.IPAddress -eq "192.168.1.166" }
    if ($has166) {
        Write-Host "[+] PC has IP 192.168.1.166 - CORRECT" -ForegroundColor Green
    } else {
        Write-Host "[!] PC does NOT have IP 192.168.1.166" -ForegroundColor Red
        Write-Host "    NanoPi is configured to use serverip=192.168.1.166" -ForegroundColor Yellow
        Write-Host "    Either change PC IP to 192.168.1.166 or update U-Boot serverip" -ForegroundColor Yellow
    }
} else {
    Write-Host "[!] No 192.168.x.x IP addresses found" -ForegroundColor Red
}

Write-Host ""

# Check 2: kernel_neuro.bin exists
Write-Host "[2] Checking kernel_neuro.bin..." -ForegroundColor Yellow
if (Test-Path ".\kernel_neuro.bin") {
    $size = (Get-Item ".\kernel_neuro.bin").Length
    Write-Host "[+] kernel_neuro.bin found - $size bytes" -ForegroundColor Green
} else {
    Write-Host "[!] kernel_neuro.bin NOT found in current directory" -ForegroundColor Red
    exit 1
}

Write-Host ""

# Check 3: TFTP server process
Write-Host "[3] Checking if TFTP server is running..." -ForegroundColor Yellow
$tftpProcess = Get-Process -Name "pwsh","powershell" -ErrorAction SilentlyContinue | Where-Object {
    $_.MainWindowTitle -match "tftp" -or $_.CommandLine -match "ps_tftp_server"
}

if ($tftpProcess) {
    Write-Host "[+] TFTP server process found (PID: $($tftpProcess.Id))" -ForegroundColor Green
} else {
    Write-Host "[!] TFTP server process NOT running" -ForegroundColor Red
    Write-Host "    Run in separate PowerShell window (as Administrator):" -ForegroundColor Yellow
    Write-Host "    .\ps_tftp_server.ps1" -ForegroundColor White
}

Write-Host ""

# Check 4: Firewall
Write-Host "[4] Checking Windows Firewall..." -ForegroundColor Yellow
try {
    $firewallStatus = Get-NetFirewallProfile -Profile Domain,Public,Private | Select-Object Name,Enabled
    $anyEnabled = $firewallStatus | Where-Object { $_.Enabled -eq $true }
    
    if ($anyEnabled) {
        Write-Host "[!] Windows Firewall is ENABLED on:" -ForegroundColor Yellow
        foreach ($profile in $anyEnabled) {
            Write-Host "    $($profile.Name)" -ForegroundColor White
        }
        Write-Host "    This may block TFTP (UDP port 69)" -ForegroundColor Yellow
        Write-Host "    Temporarily disable or add firewall rule for PowerShell" -ForegroundColor Yellow
    } else {
        Write-Host "[+] Windows Firewall is disabled" -ForegroundColor Green
    }
} catch {
    Write-Host "[!] Cannot check firewall status (need Administrator)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "NEXT STEPS:" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "1. Open NEW PowerShell window AS ADMINISTRATOR" -ForegroundColor White
Write-Host "2. cd C:\Users\SERYOGA\AndroidStudioProjects\H-Exo" -ForegroundColor Gray
Write-Host "3. .\ps_tftp_server.ps1" -ForegroundColor Gray
Write-Host "4. Keep that window open" -ForegroundColor White
Write-Host "5. Return here and run: .\deploy_tftp_and_boot.ps1" -ForegroundColor White
Write-Host ""
