# Verify kernel on SD card
param(
    [int]$DiskNumber = 1,
    [int]$TargetSector = 500000
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  Verify Kernel on SD Card" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

# Check admin
if (-NOT ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")) {
    Write-Host "[ERROR] This script requires Administrator privileges!" -ForegroundColor Red
    exit 1
}

try {
    $diskPath = "\\.\PhysicalDrive$DiskNumber"
    $stream = [System.IO.File]::Open($diskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    
    # Seek to target sector
    $offset = [long]$TargetSector * 512
    $stream.Seek($offset, [System.IO.SeekOrigin]::Begin) | Out-Null
    
    # Read first 512 bytes
    $buffer = New-Object byte[] 512
    $stream.Read($buffer, 0, 512) | Out-Null
    $stream.Close()
    
    Write-Host "[*] First 64 bytes from sector $TargetSector (0x$($TargetSector.ToString('X'))):" -ForegroundColor Yellow
    
    # Display as hex
    for ($i = 0; $i -lt 64; $i += 16) {
        $hex = ""
        $ascii = ""
        for ($j = 0; $j -lt 16; $j++) {
            if ($i + $j -lt 64) {
                $byte = $buffer[$i + $j]
                $hex += "{0:X2} " -f $byte
                if ($byte -ge 32 -and $byte -le 126) {
                    $ascii += [char]$byte
                } else {
                    $ascii += "."
                }
            }
        }
        Write-Host ("{0:X4}: {1} {2}" -f $i, $hex, $ascii) -ForegroundColor Gray
    }
    
    # Check if it looks like ARM64 code
    $magic = [BitConverter]::ToUInt32($buffer, 0)
    Write-Host ""
    Write-Host "[*] First 4 bytes: 0x$($magic.ToString('X8'))" -ForegroundColor Yellow
    
    if ($magic -eq 0x14000000 -or ($magic -band 0xFF000000) -eq 0x14000000) {
        Write-Host "[+] Looks like ARM64 branch instruction (b/bl)" -ForegroundColor Green
    } elseif ($magic -eq 0xD503201F) {
        Write-Host "[+] Looks like ARM64 NOP instruction" -ForegroundColor Green
    } elseif ($magic -eq 0x00000000 -or $magic -eq 0xFFFFFFFF) {
        Write-Host "[WARN] Sector appears to be empty or erased!" -ForegroundColor Red
    } else {
        Write-Host "[?] Unknown pattern - may or may not be valid code" -ForegroundColor Yellow
    }
    
} catch {
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
