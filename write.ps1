$DiskNumber = 1
$IdbloaderPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\idbloader.img"
$IdbloaderBinPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\idbloader.bin"
$UbootItbPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\u-boot.itb"
$UbootImgPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\uboot.img"
$TrustImgPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\trust.img"
$TrustBinPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\trust.bin"

$Signature = @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public class DiskWriter {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern SafeFileHandle CreateFile(
        string lpFileName,
        uint dwDesiredAccess,
        uint dwShareMode,
        IntPtr lpSecurityAttributes,
        uint dwCreationDisposition,
        uint dwFlagsAndAttributes,
        IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool WriteFile(
        SafeFileHandle hFile,
        byte[] lpBuffer,
        uint nNumberOfBytesToWrite,
        out uint lpNumberOfBytesWritten,
        IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool SetFilePointerEx(
        SafeFileHandle hFile,
        long liDistanceToMove,
        out long lpNewFilePointer,
        uint dwMoveMethod);

    public const uint GENERIC_READ = 0x80000000;
    public const uint GENERIC_WRITE = 0x40000000;
    public const uint FILE_SHARE_READ = 1;
    public const uint FILE_SHARE_WRITE = 2;
    public const uint OPEN_EXISTING = 3;
    public const uint FILE_BEGIN = 0;
}
"@

try {
    if (-not (Test-Path $IdbloaderPath) -and (Test-Path $IdbloaderBinPath)) {
        $IdbloaderPath = $IdbloaderBinPath
    }

    if (-not (Test-Path $IdbloaderPath)) {
        throw "Missing idbloader: $IdbloaderPath (or idbloader.bin)"
    }

    $IdbBytes = [System.IO.File]::ReadAllBytes($IdbloaderPath)

    $UseItb = Test-Path $UbootItbPath
    if (-not (Test-Path $TrustImgPath) -and (Test-Path $TrustBinPath)) {
        $TrustImgPath = $TrustBinPath
    }

    $UseMiniloaderChain = (Test-Path $UbootImgPath) -and (Test-Path $TrustImgPath)

    if (-not $UseItb -and -not $UseMiniloaderChain) {
        throw "Provide either u-boot.itb OR both uboot.img + trust.img in project root."
    }

    if ($UseItb) {
        $UbootItbBytes = [System.IO.File]::ReadAllBytes($UbootItbPath)
        Write-Host "[*] Selected chain: idbloader + u-boot.itb" -ForegroundColor Yellow
    } else {
        $UbootImgBytes = [System.IO.File]::ReadAllBytes($UbootImgPath)
        $TrustImgBytes = [System.IO.File]::ReadAllBytes($TrustImgPath)
        Write-Host "[*] Selected chain: idbloader + uboot.img + trust.img" -ForegroundColor Yellow
    }

    $SectorSize = 512

    $idbRemainder = $IdbBytes.Length % $SectorSize
    if ($idbRemainder -ne 0) {
        $idbPaddingSize = $SectorSize - $idbRemainder
        $idbPadding = New-Object byte[] $idbPaddingSize
        $IdbBytes += $idbPadding
    }

    if ($UseItb) {
        $itbRemainder = $UbootItbBytes.Length % $SectorSize
        if ($itbRemainder -ne 0) {
            $itbPaddingSize = $SectorSize - $itbRemainder
            $itbPadding = New-Object byte[] $itbPaddingSize
            $UbootItbBytes += $itbPadding
        }
    } else {
        $ubootRemainder = $UbootImgBytes.Length % $SectorSize
        if ($ubootRemainder -ne 0) {
            $ubootPaddingSize = $SectorSize - $ubootRemainder
            $ubootPadding = New-Object byte[] $ubootPaddingSize
            $UbootImgBytes += $ubootPadding
        }

        $trustRemainder = $TrustImgBytes.Length % $SectorSize
        if ($trustRemainder -ne 0) {
            $trustPaddingSize = $SectorSize - $trustRemainder
            $trustPadding = New-Object byte[] $trustPaddingSize
            $TrustImgBytes += $trustPadding
        }
    }

    Add-Type -TypeDefinition $Signature

    $DrivePath = "\\.\PhysicalDrive$DiskNumber"
    $Handle = [DiskWriter]::CreateFile(
        $DrivePath,
        ([DiskWriter]::GENERIC_READ -bor [DiskWriter]::GENERIC_WRITE),
        ([DiskWriter]::FILE_SHARE_READ -bor [DiskWriter]::FILE_SHARE_WRITE),
        [IntPtr]::Zero,
        [DiskWriter]::OPEN_EXISTING,
        0,
        [IntPtr]::Zero
    )

    if ($Handle.IsInvalid) {
        $errorCode = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Failed to open drive. Error code: $errorCode"
    }

    try {
        function Write-ImageToSector {
            param(
                [Microsoft.Win32.SafeHandles.SafeFileHandle]$Handle,
                [byte[]]$Bytes,
                [int64]$Sector,
                [string]$Label
            )

            $ByteOffset = [int64]($Sector * 512)
            Write-Host "[*] Writing $Label to Sector $Sector (Offset $ByteOffset)..." -ForegroundColor Cyan

            $NewPos = 0
            if (-not [DiskWriter]::SetFilePointerEx($Handle, $ByteOffset, [ref]$NewPos, [DiskWriter]::FILE_BEGIN)) {
                $errorCode = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
                throw "Failed to seek to Sector $Sector for $Label. Error code: $errorCode"
            }

            $Written = 0
            if (-not [DiskWriter]::WriteFile($Handle, $Bytes, $Bytes.Length, [ref]$Written, [IntPtr]::Zero)) {
                $errorCode = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
                throw "Failed to write $Label to Sector $Sector. Error code: $errorCode"
            }

            Write-Host "[+] $Label written: $Written bytes" -ForegroundColor Green
        }

        Write-ImageToSector -Handle $Handle -Bytes $IdbBytes -Sector 64 -Label "idbloader"

        if ($UseItb) {
            Write-ImageToSector -Handle $Handle -Bytes $UbootItbBytes -Sector 16384 -Label "u-boot.itb"
        } else {
            Write-ImageToSector -Handle $Handle -Bytes $UbootImgBytes -Sector 16384 -Label "uboot.img"
            Write-ImageToSector -Handle $Handle -Bytes $TrustImgBytes -Sector 24576 -Label "trust.img"
        }
    }
    finally {
        $Handle.Close()
    }
    Write-Host "`n--- ALL OPERATIONS SUCCESSFUL ---" -ForegroundColor Green
}
catch {
    Write-Host "[-] ERROR: $($_.Exception.Message)" -ForegroundColor Red
}