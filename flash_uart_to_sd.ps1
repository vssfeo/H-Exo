# Запись образа на SD в слоте одноплатника по UART (без кардридера на ПК):
# YMODEM (loady) в RAM -> mmc write на карту в U-Boot.
param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 115200,
    [string]$FilePath = "",
    [UInt32]$LoadAddress = 0x02080000,
    [UInt32]$KernelSector = 500000,
    [int]$MmcDevice = 1,
    [UInt32]$KernelSectorCount = 0,
    [int]$InterPacketDelayMs = 0
)

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $FilePath) {
    $FilePath = Join-Path $root "kernel_neuro.bin"
}
if (-not (Test-Path $FilePath)) {
    throw "File not found: $FilePath"
}
$FilePath = (Resolve-Path $FilePath).Path

& "$root\send_ymodem.ps1" `
    -PortName $PortName `
    -BaudRate $BaudRate `
    -FilePath $FilePath `
    -LoadAddress $LoadAddress `
    -AutoBoot `
    -KernelSector $KernelSector `
    -KernelSectorCount $KernelSectorCount `
    -MmcDevice $MmcDevice `
    -InterPacketDelayMs $InterPacketDelayMs
