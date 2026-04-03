# H-Exo: List Physical Drives Helper
# Shows all physical drives to help identify SD card

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Physical Drives on this system" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

Get-WmiObject Win32_DiskDrive | ForEach-Object {
    $drive = $_
    $driveNumber = $drive.Index
    $size = [math]::Round($drive.Size / 1GB, 2)
    $model = $drive.Model
    $interface = $drive.InterfaceType
    
    Write-Host "PhysicalDrive$driveNumber" -ForegroundColor Green
    Write-Host "  Model: $model" -ForegroundColor White
    Write-Host "  Size: $size GB" -ForegroundColor White
    Write-Host "  Interface: $interface" -ForegroundColor White
    Write-Host "  Path: \\.\PhysicalDrive$driveNumber" -ForegroundColor Yellow
    Write-Host ""
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Identify your SD card by size/model, then use its PhysicalDrive number" -ForegroundColor Yellow
Write-Host "Example: .\deploy_raw_sector.ps1 -PhysicalDrive '\\.\PhysicalDrive2'" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Cyan
