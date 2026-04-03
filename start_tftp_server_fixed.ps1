# start_tftp_server_fixed.ps1 - TFTP Server Startup Script

Write-Host "=== Starting TFTP Server ===" -ForegroundColor Green
Write-Host ""

# Check if Tftpd64 is installed
$Tftpd64Path = "C:\Program Files\Tftpd64\tftpd64.exe"
$Tftpd32Path = "C:\Program Files\Tftpd32\tftpd32.exe"
$TftpdPortablePath = "$PSScriptRoot\tftpd64.exe"

function Download-Tftpd64 {
    Write-Host "Downloading Tftpd64..." -ForegroundColor Yellow
    
    $downloadUrl = "https://bitbucket.org/phjounin/tftpd64/downloads/Tftpd64-4.64-setup.exe"
    $installerPath = "$env:TEMP\Tftpd64-setup.exe"
    
    try {
        Invoke-WebRequest -Uri $downloadUrl -OutFile $installerPath
        Write-Host "Installation file downloaded to $installerPath"
        
        Write-Host "Starting Tftpd64 installation..."
        Start-Process -FilePath $installerPath -ArgumentList "/S" -Wait
        
        Write-Host "Tftpd64 installed successfully!" -ForegroundColor Green
        return $true
    } catch {
        Write-Error "Failed to download or install Tftpd64: $_"
        return $false
    }
}

function Start-TftpServer {
    $tftpExecutable = $null
    
    # Check for Tftpd64 in various locations
    if (Test-Path $Tftpd64Path) {
        $tftpExecutable = $Tftpd64Path
    } elseif (Test-Path $Tftpd32Path) {
        $tftpExecutable = $Tftpd32Path
    } elseif (Test-Path $TftpdPortablePath) {
        $tftpExecutable = $TftpdPortablePath
    } else {
        Write-Host "Tftpd64 not found, attempting to download..." -ForegroundColor Yellow
        if (Download-Tftpd64) {
            if (Test-Path $Tftpd64Path) {
                $tftpExecutable = $Tftpd64Path
            } else {
                Write-Error "Could not find Tftpd64 after installation"
                return $false
            }
        } else {
            Write-Error "Failed to download Tftpd64"
            return $false
        }
    }
    
    Write-Host "Starting TFTP server: $tftpExecutable" -ForegroundColor Yellow
    
    try {
        # Start Tftpd64 with configuration file
        $configPath = "$PSScriptRoot\Tftpd64.ini"
        if (Test-Path $configPath) {
            Start-Process -FilePath $tftpExecutable -ArgumentList "-i $configPath" -WindowStyle Minimized
        } else {
            Start-Process -FilePath $tftpExecutable -WindowStyle Minimized
        }
        
        Write-Host "TFTP server started successfully!" -ForegroundColor Green
        return $true
    } catch {
        Write-Error "Failed to start TFTP server: $_"
        return $false
    }
}

# Main process
Write-Host "=== TFTP Server Setup ===" -ForegroundColor Green

# Create TFTP directory if it doesn't exist
$tftpDir = "C:\tftpboot"
if (-not (Test-Path $tftpDir)) {
    New-Item -ItemType Directory -Path $tftpDir -Force | Out-Null
    Write-Host "Created TFTP directory: $tftpDir" -ForegroundColor Green
}

# Start TFTP server
if (Start-TftpServer) {
    Write-Host "TFTP server is now running and listening on port 69" -ForegroundColor Green
    Write-Host "TFTP Directory: C:\tftpboot" -ForegroundColor Cyan
    Write-Host "Server IP: 192.168.1.166" -ForegroundColor Cyan
} else {
    Write-Error "Failed to start TFTP server"
    Write-Host "Try manually installing and running Tftpd64" -ForegroundColor Yellow
}

Write-Host ""
