# Extract neural weights from kernel binary and compute CRC32

param(
    [string]$KernelPath = "kernel_neuro.bin",
    [string]$ElfPath = "kernel_neuro.elf"
)

# Get weights address from ELF
$nmOutput = & "C:/gcc-arm/bin/aarch64-none-elf-nm" $ElfPath | Select-String "default_weights"
if ($nmOutput -match "([0-9a-f]+)\s+r\s+default_weights") {
    $weightsAddr = [Convert]::ToInt64($matches[1], 16)
    Write-Host "[*] Weights address: 0x$($matches[1])" -ForegroundColor Yellow
} else {
    Write-Host "[ERROR] Could not find default_weights symbol" -ForegroundColor Red
    exit 1
}

# Calculate offset in binary (subtract load address 0x02080000)
$loadAddr = 0x02080000
$offset = $weightsAddr - $loadAddr

Write-Host "[*] Weights offset in binary: 0x$($offset.ToString('X'))" -ForegroundColor Yellow

# Size of neural_weights_t structure
# w1[6][8] = 192 bytes (48 i32)
# b1[8] = 32 bytes (8 i32)
# w2[8][4] = 128 bytes (32 i32)
# b2[4] = 16 bytes (4 i32)
# Total = 368 bytes (92 i32)
$weightsSize = 368

# Read weights from binary
$bytes = [System.IO.File]::ReadAllBytes($KernelPath)
$weightsBytes = $bytes[$offset..($offset + $weightsSize - 1)]

Write-Host "[*] Extracted $weightsSize bytes from offset 0x$($offset.ToString('X'))" -ForegroundColor Yellow

# Compute CRC32 (IEEE 802.3 polynomial)
function Compute-CRC32 {
    param([byte[]]$data)
    
    # CRC32 lookup table
    $table = @(0) * 256
    for ($i = 0; $i -lt 256; $i++) {
        $c = $i
        for ($j = 0; $j -lt 8; $j++) {
            if ($c -band 1) {
                $c = (($c -shr 1) -bxor 0xEDB88320)
            } else {
                $c = $c -shr 1
            }
        }
        $table[$i] = $c
    }
    
    # Compute CRC
    $crc = 0xFFFFFFFF
    foreach ($byte in $data) {
        $index = ($crc -bxor $byte) -band 0xFF
        $crc = (($crc -shr 8) -bxor $table[$index]) -band 0xFFFFFFFF
    }
    
    return (-bnot $crc) -band 0xFFFFFFFF
}

$crc = Compute-CRC32 $weightsBytes

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Neural Weights CRC32" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "CRC32: 0x$($crc.ToString('X8'))" -ForegroundColor Green
Write-Host ""
Write-Host "Update neuro/weight_validation.c:" -ForegroundColor Yellow
Write-Host "u32 get_expected_weights_crc(void) {" -ForegroundColor Gray
Write-Host "    return 0x$($crc.ToString('X8'));" -ForegroundColor Gray
Write-Host "}" -ForegroundColor Gray
