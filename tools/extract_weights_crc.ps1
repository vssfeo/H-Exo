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
function Get-Crc32 {
    param([byte[]]$data)
    
    # [uint32] ensures logical (zero-fill) right-shift, matching C's u32 >> operator
    $poly = [uint32]3988292384  # 0xEDB88320
    $table = New-Object 'uint32[]' 256
    for ($i = 0; $i -lt 256; $i++) {
        [uint32]$c = [uint32]$i
        for ($j = 0; $j -lt 8; $j++) {
            if ($c -band 1) { $c = ($c -shr 1) -bxor $poly }
            else             { $c = $c -shr 1 }
        }
        $table[$i] = $c
    }
    
    [uint32]$crc = [uint32]4294967295
    foreach ($b in $data) {
        [uint32]$idx = ($crc -bxor [uint32]$b) -band 255
        $crc = ($crc -shr 8) -bxor $table[$idx]
    }
    
    return -bnot $crc
}

$crc = Get-Crc32 $weightsBytes

$crcHex = "0x$($crc.ToString('X8'))"
Write-Host "[*] Computed CRC32: $crcHex" -ForegroundColor Yellow

# Resolve weight_validation.c - works both from project root (build.bat) and tools\ directory
$projectRoot = if ($PSScriptRoot -match 'tools$') { Split-Path $PSScriptRoot -Parent } else { (Get-Location).Path }
$validationPath = Join-Path $projectRoot "neuro\weight_validation.c"

$currentContent = Get-Content $validationPath -Raw
if ($currentContent -match 'return (0x[0-9A-Fa-f]{8});') {
    $currentCrc = $matches[1]
} else {
    $currentCrc = ""
}

if ($currentCrc -eq $crcHex) {
    Write-Host "[OK] CRC match: $crcHex - no update needed" -ForegroundColor Green
    exit 0
}

Write-Host "[!] CRC mismatch: expected $currentCrc, computed $crcHex" -ForegroundColor Yellow
Write-Host "[*] Auto-updating neuro\weight_validation.c..." -ForegroundColor Cyan

$newContent = $currentContent -replace 'return 0x[0-9A-Fa-f]{8};', "return $crcHex;"
Set-Content $validationPath $newContent -Encoding UTF8 -NoNewline

Write-Host "[OK] Updated: get_expected_weights_crc() now returns $crcHex" -ForegroundColor Green
Write-Host "[!] Rebuild required to embed updated CRC into kernel" -ForegroundColor Yellow

# Exit code 2 = CRC was updated, rebuild needed
exit 2
