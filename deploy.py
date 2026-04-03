DiskNumber = 1  # ПРОВЕРЬ НОМЕР ИЗ GET-DISK!
Path = "C:\Users\SERYOGA\AndroidStudioProjects\H-Exo\kernel.bin"

DriveHandle = [Microsoft.Win32.SafeHandles.SafeFileHandle]::new(
    [Microsoft.Win32.Win32Native]::CreateFile(
        "\\.\PhysicalDrive$DiskNumber", 
        [System.IO.FileAccess]::ReadWrite, 
        [System.IO.FileShare]::ReadWrite, 
        [System.IntPtr]::Zero, 
        [System.IO.FileMode]::Open, 
        0, 
        [System.IntPtr]::Zero
    ), 
    true
)

Stream = [System.IO.FileStream]::new($DriveHandle, [System.IO.FileAccess]::ReadWrite)
Bytes = [System.IO.File::ReadAllBytes($Path)]

# Смещение 32768 байт = Сектор 64
Stream.Position = 32768
Stream.Write($Bytes, 0, $Bytes.Length)
Stream.Flush()
Stream.Close()

Write-Host "--- Aleph записан на сектор 64 диска $DiskNumber ---" -ForegroundColor Green