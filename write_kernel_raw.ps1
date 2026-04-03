$DiskNumber = 1
$KernelPath = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\kernel.bin"
$KernelSector = 32600
$SectorSize = 512

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
    if (-not (Test-Path $KernelPath)) {
        throw "Missing kernel binary: $KernelPath"
    }

    $KernelBytes = [System.IO.File]::ReadAllBytes($KernelPath)
    $remainder = $KernelBytes.Length % $SectorSize
    if ($remainder -ne 0) {
        $paddingSize = $SectorSize - $remainder
        $padding = New-Object byte[] $paddingSize
        $KernelBytes += $padding
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
        $byteOffset = [int64]($KernelSector * $SectorSize)
        $newPos = 0
        if (-not [DiskWriter]::SetFilePointerEx($Handle, $byteOffset, [ref]$newPos, [DiskWriter]::FILE_BEGIN)) {
            $errorCode = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "Failed to seek to sector $KernelSector. Error code: $errorCode"
        }

        $written = 0
        if (-not [DiskWriter]::WriteFile($Handle, $KernelBytes, $KernelBytes.Length, [ref]$written, [IntPtr]::Zero)) {
            $errorCode = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "Failed to write kernel at sector $KernelSector. Error code: $errorCode"
        }

        $sectors = [int]($KernelBytes.Length / $SectorSize)
        Write-Host "[+] kernel.bin written: $written bytes at sector $KernelSector ($sectors sectors)" -ForegroundColor Green
        Write-Host "[+] U-Boot: mmc dev 1; mmc read 0x02080000 $KernelSector $sectors; go 0x02080000" -ForegroundColor Yellow
    }
    finally {
        $Handle.Close()
    }
}
catch {
    Write-Host "[-] ERROR: $($_.Exception.Message)" -ForegroundColor Red
}
