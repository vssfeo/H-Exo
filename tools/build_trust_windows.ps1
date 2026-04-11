param(
    [string]$ConfigPath = "third_party/rkbin/RKTRUST/RK3399TRUST.ini",
    [string]$OutputPath = "C:/tftpboot/trust-local.img",
    [string]$Bl31Path = "",
    [int]$ImageSizeKB = 2048,
    [int]$BackupCount = 2
)

$ErrorActionPreference = "Stop"

function Read-U16LE([byte[]]$b, [int]$o) {
    return [uint16](([uint32]$b[$o]) -bor (([uint32]$b[$o + 1]) -shl 8))
}

function Read-U32LE([byte[]]$b, [int]$o) {
    return [uint32](([uint32]$b[$o]) -bor (([uint32]$b[$o + 1]) -shl 8) -bor (([uint32]$b[$o + 2]) -shl 16) -bor (([uint32]$b[$o + 3]) -shl 24))
}

function Read-U64LE([byte[]]$b, [int]$o) {
    $lo = [uint64](Read-U32LE $b $o)
    $hi = [uint64](Read-U32LE $b ($o + 4))
    return ($lo -bor ($hi -shl 32))
}

function Write-U32LE([byte[]]$b, [int]$o, [uint32]$v) {
    $b[$o] = [byte]($v -band 0xFF)
    $b[$o + 1] = [byte](($v -shr 8) -band 0xFF)
    $b[$o + 2] = [byte](($v -shr 16) -band 0xFF)
    $b[$o + 3] = [byte](($v -shr 24) -band 0xFF)
}

function Get-BcdByte([int]$v) {
    if ($v -lt 0) { return [byte]0 }
    $ones = $v % 10
    $tens = [int](($v / 10) % 10)
    return [byte](($tens -shl 4) -bor $ones)
}

function Get-AlignUp([int]$v, [int]$a) {
    if ($v -le 0) { return 0 }
    return [int]([Math]::Ceiling($v / [double]$a) * $a)
}

function Read-IniFile([string]$path) {
    $ini = @{}
    $sec = ""
    foreach ($lineRaw in [IO.File]::ReadAllLines($path)) {
        $line = $lineRaw.Trim()
        if ($line.Length -eq 0) { continue }
        if ($line.StartsWith("#") -or $line.StartsWith(";")) { continue }
        if ($line.StartsWith("[") -and $line.EndsWith("]")) {
            $sec = $line.Substring(1, $line.Length - 2).Trim()
            if (-not $ini.ContainsKey($sec)) { $ini[$sec] = @{} }
            continue
        }
        $eq = $line.IndexOf("=")
        if ($eq -lt 0 -or $sec -eq "") { continue }
        $k = $line.Substring(0, $eq).Trim()
        $v = $line.Substring($eq + 1).Trim()
        $ini[$sec][$k] = $v
    }
    return $ini
}

function Resolve-LocalPath([string]$baseDir, [string]$p) {
    if ([IO.Path]::IsPathRooted($p)) { return $p }
    $a = [IO.Path]::GetFullPath((Join-Path $baseDir $p))
    if (Test-Path $a) { return $a }
    $b = [IO.Path]::GetFullPath((Join-Path (Split-Path $baseDir -Parent) $p))
    return $b
}

function Get-ElfLoadSegments([byte[]]$fileBytes, [string]$id, [string]$path, [uint32]$fallbackAddr) {
    $segs = @()
    $isElf = ($fileBytes.Length -ge 4 -and $fileBytes[0] -eq 0x7F -and $fileBytes[1] -eq 0x45 -and $fileBytes[2] -eq 0x4C -and $fileBytes[3] -eq 0x46)
    if (-not $isElf) {
        $segs += [pscustomobject]@{
            Id = $id; Path = $path; Offset = 0; Size = $fileBytes.Length; LoadAddr = $fallbackAddr; Data = $fileBytes
        }
        return $segs
    }

    $eiClass = $fileBytes[4] # 1=32,2=64
    $eiData = $fileBytes[5]  # 1=little
    if ($eiData -ne 1) { throw "Unsupported ELF endianness in $path" }
    $eType = Read-U16LE $fileBytes 16
    if ($eType -ne 2) { throw "Unsupported ELF type (not executable) in $path" }

    if ($eiClass -eq 2) {
        $ePhoff = [int](Read-U64LE $fileBytes 32)
        $ePhentsize = [int](Read-U16LE $fileBytes 54)
        $ePhnum = [int](Read-U16LE $fileBytes 56)
        for ($i = 0; $i -lt $ePhnum; $i++) {
            $ph = $ePhoff + $i * $ePhentsize
            $pType = Read-U32LE $fileBytes $ph
            if ($pType -ne 1) { continue } # PT_LOAD
            $pOffset = [int](Read-U64LE $fileBytes ($ph + 8))
            $pVaddr = [uint32](Read-U64LE $fileBytes ($ph + 16))
            $pFilesz = [int](Read-U64LE $fileBytes ($ph + 32))
            if ($pFilesz -le 0) { continue }
            $seg = New-Object byte[] $pFilesz
            [Array]::Copy($fileBytes, $pOffset, $seg, 0, $pFilesz)
            $segs += [pscustomobject]@{
                Id = $id; Path = $path; Offset = $pOffset; Size = $pFilesz; LoadAddr = $pVaddr; Data = $seg
            }
        }
    } elseif ($eiClass -eq 1) {
        $ePhoff = [int](Read-U32LE $fileBytes 28)
        $ePhentsize = [int](Read-U16LE $fileBytes 42)
        $ePhnum = [int](Read-U16LE $fileBytes 44)
        for ($i = 0; $i -lt $ePhnum; $i++) {
            $ph = $ePhoff + $i * $ePhentsize
            $pType = Read-U32LE $fileBytes $ph
            if ($pType -ne 1) { continue }
            $pOffset = [int](Read-U32LE $fileBytes ($ph + 4))
            $pVaddr = [uint32](Read-U32LE $fileBytes ($ph + 8))
            $pFilesz = [int](Read-U32LE $fileBytes ($ph + 16))
            if ($pFilesz -le 0) { continue }
            $seg = New-Object byte[] $pFilesz
            [Array]::Copy($fileBytes, $pOffset, $seg, 0, $pFilesz)
            $segs += [pscustomobject]@{
                Id = $id; Path = $path; Offset = $pOffset; Size = $pFilesz; LoadAddr = $pVaddr; Data = $seg
            }
        }
    } else {
        throw "Unsupported ELF class in $path"
    }
    return $segs
}

$cfgFull = [IO.Path]::GetFullPath($ConfigPath)
if (-not (Test-Path $cfgFull)) { throw "Config not found: $cfgFull" }
$cfgDir = Split-Path $cfgFull -Parent
$ini = Read-IniFile $cfgFull

$major = [int]$ini["VERSION"]["MAJOR"]
$minor = [int]$ini["VERSION"]["MINOR"]

$sections = @(
    @{ Name = "BL30_OPTION"; Id = "BL30" },
    @{ Name = "BL31_OPTION"; Id = "BL31" },
    @{ Name = "BL32_OPTION"; Id = "BL32" },
    @{ Name = "BL33_OPTION"; Id = "BL33" }
)

$allComponents = @()
foreach ($s in $sections) {
    if (-not $ini.ContainsKey($s.Name)) { continue }
    $sec = $ini[$s.Name]
    $enabled = [int]$sec["SEC"]
    if ($enabled -ne 1) { continue }
    $path = $sec["PATH"]
    if ($s.Id -eq "BL31" -and $Bl31Path -ne "") { $path = $Bl31Path }
    $addrStr = $sec["ADDR"]
    $addr = [uint32]([Convert]::ToUInt32($addrStr.Replace("0x",""), 16))
    $fullPath = Resolve-LocalPath $cfgDir $path
    if (-not (Test-Path $fullPath)) { throw "Component file missing: $fullPath" }
    $bytes = [IO.File]::ReadAllBytes($fullPath)
    $comps = Get-ElfLoadSegments $bytes $s.Id $fullPath $addr
    foreach ($c in $comps) {
        $alignSize = Get-AlignUp $c.Size 2048
        if ($alignSize -gt 512KB) { throw "$($c.Id) segment too large: $alignSize bytes from $($c.Path)" }
        $padded = New-Object byte[] $alignSize
        [Array]::Copy($c.Data, 0, $padded, 0, $c.Size)
        $allComponents += [pscustomobject]@{
            Id = $c.Id
            LoadAddr = [uint32]$c.LoadAddr
            Size = [int]$c.Size
            AlignSize = [int]$alignSize
            Data = $padded
            Path = $c.Path
        }
    }
}

if ($allComponents.Count -eq 0) { throw "No BL3x secure components found to pack." }

$TRUST_HEADER_SIZE = 2048
$TRUST_HEADER_STRUCT_SIZE = 800
$COMPONENT_DATA_SIZE = 48
$SIGNATURE_SIZE = 256
$TRUST_COMPONENT_SIZE = 16

$signOffset = $TRUST_HEADER_STRUCT_SIZE + $allComponents.Count * $COMPONENT_DATA_SIZE
$headSizeField = [uint32](($allComponents.Count -shl 16) -bor ($signOffset -shr 2))

$perImageSize = $ImageSizeKB * 1024
$outSizeUsed = $TRUST_HEADER_SIZE
foreach ($c in $allComponents) { $outSizeUsed += $c.AlignSize }
if ($outSizeUsed -gt $perImageSize) {
    throw "trust image overflow: used=$outSizeUsed bytes, limit=$perImageSize bytes"
}

$single = New-Object byte[] $perImageSize

# Header
$tag = [Text.Encoding]::ASCII.GetBytes("BL3X")
[Array]::Copy($tag, 0, $single, 0, 4)
Write-U32LE $single 4 ([uint32](([uint32](Get-BcdByte $major) -shl 8) -bor [uint32](Get-BcdByte $minor)))
Write-U32LE $single 8 ([uint32]0x23) # SHA256 + RSA2048, matches trust_merger defaults
Write-U32LE $single 12 $headSizeField

$sha = [Security.Cryptography.SHA256]::Create()

$compDataBase = $TRUST_HEADER_STRUCT_SIZE
$compMetaBase = $signOffset + $SIGNATURE_SIZE
$payloadPtr = $TRUST_HEADER_SIZE

for ($i = 0; $i -lt $allComponents.Count; $i++) {
    $c = $allComponents[$i]
    $hash = $sha.ComputeHash($c.Data)

    # COMPONENT_DATA[i]
    $cd = $compDataBase + $i * $COMPONENT_DATA_SIZE
    [Array]::Copy($hash, 0, $single, $cd, 32)
    Write-U32LE $single ($cd + 32) ([uint32]$c.LoadAddr)

    # TRUST_COMPONENT[i]
    $cm = $compMetaBase + $i * $TRUST_COMPONENT_SIZE
    $idBytes = [Text.Encoding]::ASCII.GetBytes($c.Id)
    [Array]::Copy($idBytes, 0, $single, $cm, 4)
    Write-U32LE $single ($cm + 4) ([uint32]($payloadPtr -shr 9))
    Write-U32LE $single ($cm + 8) ([uint32]($c.AlignSize -shr 9))

    # payload
    [Array]::Copy($c.Data, 0, $single, $payloadPtr, $c.AlignSize)
    $payloadPtr += $c.AlignSize
}

$fullOut = New-Object byte[] ($perImageSize * $BackupCount)
for ($n = 0; $n -lt $BackupCount; $n++) {
    [Array]::Copy($single, 0, $fullOut, $n * $perImageSize, $perImageSize)
}

$outFull = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory((Split-Path $outFull -Parent)) | Out-Null
[IO.File]::WriteAllBytes($outFull, $fullOut)

Write-Host "Built trust image: $outFull" -ForegroundColor Green
Write-Host ("Components: {0}, per-image: {1} bytes, backups: {2}, total: {3} bytes" -f $allComponents.Count, $perImageSize, $BackupCount, $fullOut.Length) -ForegroundColor Cyan
for ($i = 0; $i -lt $allComponents.Count; $i++) {
    $c = $allComponents[$i]
    Write-Host ("  [{0}] {1} load=0x{2:X8} size={3} align={4} path={5}" -f $i, $c.Id, $c.LoadAddr, $c.Size, $c.AlignSize, $c.Path) -ForegroundColor DarkCyan
}
