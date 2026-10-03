<#
.SYNOPSIS
    The one command that decides whether H-Exo is in a shippable state.

.DESCRIPTION
    Exists because there was no green command. Four Makefiles with four different
    flag sets, none of them complete: `make -f Makefile.optimized` dies on a
    missing boot_optimized.o, `make -f Makefile.neuro` dies at link with undefined
    references, and the only recipe that actually worked lived in a scratch
    directory outside the repo. Self-checking therefore depended on an agent
    remembering a command line, which is not verification.

    This script does, in order and with a real exit code:

      1. cross-check tools/sources.json against build_now.ps1  (list drift)
      2. strict compile of every source, out of tree           (-Werror= by class)
      3. -fanalyzer pass over every C source                   (never with -fsyntax-only)
      4. cppcheck, if a portable binary is present in tools/bin (overflow, lifetime,
         format strings -- the classes -fanalyzer provably does not look for)
      5. link with linker.ld
      6. size + SHA256 + CRC32 against tools/baseline.json

    It builds OUT OF TREE, into $WorkDir. It never writes into the source tree and
    never touches the SD card.

    Strict flags are -Werror= per class rather than blanket -Werror. Blanket
    -Werror on this tree fails on 7 -Wextra diagnostics that are all cosmetic
    (unused stub parameters, one signed/unsigned comparison), which would train
    everyone to ignore the gate. -Wall alone is demonstrably not enough: it does
    not flag shift-negative-value, sign-compare or unused-parameter, so the
    INT_TO_FIXED undefined-behaviour class was invisible to the old build.

.PARAMETER Update
    Rewrite tools/baseline.json from the binary just built. Review the diff before
    committing it: this file is the tripwire, and editing it silently is exactly
    the failure mode it exists to prevent.

.PARAMETER SkipAnalyzer
    Skip pass 3. The analyzer is the slow part; skipping is for iterating on code,
    not for deciding whether to ship.

.PARAMETER Log
    Optional path to a captured UART log. When given, every marker in
    docs/expected/markers.txt is checked offline and a missing marker fails the run.

.PARAMETER GCC
    C compiler to use. Defaults to the project's own bare-metal toolchain, then to
    whichever aarch64 cross-compiler is on PATH. CI uses aarch64-linux-gnu-gcc, so
    the path cannot be hardcoded.

.PARAMETER Objcopy
    objcopy matching $GCC.

.PARAMETER OutBin
    Copy the built kernel_neuro.bin to this path. verify.ps1 never writes into the
    source tree, so CI needs somewhere explicit to pick the artifact up from.

.EXAMPLE
    pwsh -File tools/verify.ps1
    pwsh -File tools/verify.ps1 -Log C:\Temp\opencode\exp21\run.log
    pwsh -File tools/verify.ps1 -Update
#>
[CmdletBinding()]
param(
    [switch]$Update,
    [switch]$SkipAnalyzer,
    [string]$Log,
    [string]$GCC,
    [string]$Objcopy,
    [string]$OutBin
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Without this, a compiler diagnostic or a PowerShell error message is rendered in
# the OEM codepage and comes out as mojibake, which makes a real error unreadable.
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$Repo    = Split-Path -Parent $PSScriptRoot
$WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) 'hexo_verify'
$CPPCHECK = Join-Path $PSScriptRoot 'bin\cppcheck.exe'
if (-not (Test-Path $CPPCHECK)) {
    $alt = Join-Path $PSScriptRoot 'bin\cppcheck'
    if (Test-Path $alt) { $CPPCHECK = Join-Path $alt 'cppcheck' }
}

# Prefer the project's own pinned bare-metal toolchain. The exact bytes recorded in
# tools/baseline.json describe THAT compiler's output, so a different compiler must
# be visible rather than silently accepted.
function Resolve-Tool {
    param([string]$Explicit, [string[]]$Candidates, [string]$SiblingOf, [string]$SiblingSuffix)
    if ($Explicit) { return $Explicit }
    $dir = if ($SiblingOf -and $SiblingOf -match '\\') { Split-Path -Parent $SiblingOf } else { '' }
    foreach ($c in $Candidates) {
        if ($dir -and $c -match '\.exe$') {
            $p = Join-Path $dir $c
            if (Test-Path $p) { return $p }
        }
        $g = Get-Command $c -ErrorAction SilentlyContinue
        if ($g) { return $g.Source }
    }
    return $null
}
$gccPath = Resolve-Tool -Explicit $GCC -SiblingOf $null -Candidates @(
    'C:\gcc-arm\bin\aarch64-none-elf-gcc.exe','aarch64-none-elf-gcc','aarch64-linux-gnu-gcc','gcc')
$objcopyPath = Resolve-Tool -Explicit $Objcopy -SiblingOf $gccPath -Candidates @(
    'aarch64-none-elf-objcopy.exe','aarch64-none-elf-objcopy','aarch64-linux-gnu-objcopy','objcopy')
$GCC     = $gccPath
$OBJCOPY = $objcopyPath

# These MUST stay identical to build_now.ps1's $CFLAGS. If they drift, this script
# verifies a binary nobody ships and the baseline stops being a tripwire. The
# codegen-relevant flags are checked against build_now.ps1 further down, so the
# drift is caught rather than assumed away.
$BaseFlags = @(
    '-O3','-ffreestanding','-nostdlib','-nostartfiles',
    '-fno-common','-fno-builtin',
    '-march=armv8-a+fp+simd','-mno-outline-atomics','-I.'
)
$StrictOnly = @(
    '-Wall','-Wextra',
    '-Werror=shift-negative-value',
    '-Werror=array-bounds',
    '-Werror=maybe-uninitialized',
    '-Werror=return-type',
    '-Werror=implicit-function-declaration'
)

$script:Failures = New-Object System.Collections.Generic.List[string]
$script:Notes    = New-Object System.Collections.Generic.List[string]
$script:Warnings = New-Object System.Collections.Generic.List[string]

function Step { param([string]$m) Write-Host ""; Write-Host "== $m" -ForegroundColor Cyan }
function Ok    { param([string]$m) Write-Host "   OK    $m" }
function Warn  { param([string]$m) Write-Host "   WARN  $m" -ForegroundColor Yellow }
function Bad   { param([string]$m) Write-Host "   FAIL  $m" -ForegroundColor Red; $script:Failures.Add($m) }
function Note  { param([string]$m) Write-Host "   ..    $m" -ForegroundColor DarkGray }

function Get-Crc32 {
    param([string]$Path)
    # Local implementation on purpose: System.IO.Hashing.Crc32 does not exist on
    # Windows PowerShell 5.1, and shelling out to a checksum tool we do not have
    # would make this script depend on the same missing tooling it replaces.
    #
    # 0xFFFFFFFF / 0xEDB88320 do NOT survive being written as PowerShell hex
    # literals: PowerShell parses them as Int32 and the cast to UInt32 throws
    # "value was either too large or too small". Convert.ToUInt32(string,16) is
    # unambiguous. Self-test vector: crc32("123456789") == 0xCBF43926.
    $POLY = [Convert]::ToUInt32('EDB88320', 16)
    $MASK = [Convert]::ToUInt32('FFFFFFFF', 16)
    [uint32[]]$table = New-Object uint32[] 256
    for ($i = 0; $i -lt 256; $i++) {
        [uint32]$c = [uint32]$i
        for ($k = 0; $k -lt 8; $k++) {
            if (($c -band 1) -ne 0) { $c = [uint32]($POLY -bxor ($c -shr 1)) }
            else                    { $c = [uint32]($c -shr 1) }
        }
        $table[$i] = $c
    }
    [uint32]$crc = $MASK
    foreach ($b in [System.IO.File]::ReadAllBytes($Path)) {
        [uint32]$idx = ($crc -bxor [uint32]$b) -band [uint32]0xFF
        $crc = [uint32]($table[[int]$idx] -bxor ($crc -shr 8))
    }
    return [uint32]($crc -bxor $MASK)
}

# ---------------------------------------------------------------- 0. preflight
Step 'preflight'
foreach ($t in @($GCC, $OBJCOPY)) {
    if (-not (Test-Path $t)) { Bad "toolchain missing: $t"; break }
}
if ($script:Failures.Count -gt 0) {
    Write-Host ""
    Write-Host "FAIL: toolchain is not present. Nothing below can be trusted." -ForegroundColor Red
    exit 2
}
Ok "toolchain present"

# ------------------------------------------------- 1. one source list, no drift
Step 'source list consistency'
$listFile = Join-Path $PSScriptRoot 'sources.json'
if (-not (Test-Path $listFile)) { Bad 'tools/sources.json missing'; exit 2 }
$sources = @((Get-Content $listFile -Raw | ConvertFrom-Json).sources)

$buildScript = Join-Path $Repo 'build_now.ps1'
if (Test-Path $buildScript) {
    $inBuild = @((Get-Content $buildScript -Raw |
                  Select-String -Pattern '@\{src="([^"]+)";\s*obj=' -AllMatches).Matches |
                 ForEach-Object { $_.Groups[1].Value -replace '\\','/' })
    $onlyJson = @($sources   | Where-Object { $inBuild -notcontains $_ })
    $onlyMks  = @($inBuild   | Where-Object { $sources   -notcontains $_ })
    if ($onlyJson.Count -eq 0 -and $onlyMks.Count -eq 0) {
        Ok "sources.json matches build_now.ps1 exactly ($($sources.Count) files)"
    } else {
        # WARN, not FAIL, and deliberately so. tools/sources.json is the canonical
        # list and it is committed; build_now.ps1 is a separate script that may be
        # mid-refactor and may not even be committed. A gate that turns red because
        # someone touched an unrelated script is a gate that gets bypassed. The
        # guarantee that actually matters - that verify builds the shipped binary -
        # comes from tools/baseline.json, which IS committed.
        Warn ("build_now.ps1 and tools/sources.json disagree. sources.json is canonical.")
        Warn ("  only in sources.json ($($onlyJson.Count)): " + (($onlyJson | Select-Object -First 8) -join ', ') +
              $(if ($onlyJson.Count -gt 8) { ' ...' } else { '' }))
        Warn ("  only in build_now.ps1 ($($onlyMks.Count)): " + (($onlyMks  | Select-Object -First 8) -join ', ') +
              $(if ($onlyMks.Count  -gt 8) { ' ...' } else { '' }))
        Warn 'if build_now.ps1 is what ships, reconcile the two - do not let them drift'
    }

    # Flag drift. If build_now.ps1 changes -O3 to -O2 and verify.ps1 does not,
    # the baseline stops describing the shipped binary and the whole gate becomes
    # decoration. Compare the codegen-relevant flags, not the whole line: verify
    # adds -Wall -Wextra -Werror=... on top, which is the entire point of it.
    $shipFlags = @((Get-Content $buildScript -Raw |
                   Select-String -Pattern '\$CFLAGS\s*=\s*@\(([^)]*)\)' -AllMatches).Matches |
                  ForEach-Object { $_.Groups[1].Value })
    if ($shipFlags.Count -gt 0) {
        $ship = @(($shipFlags -join ' ') -split '[",\s]+' | Where-Object { $_ -match '^-' })
        $codegen = @('-O','-march=','-mtune=','-mno-outline','-ffreestanding','-fno-','-nostdlib','-nostartfiles')
        $mine = @($BaseFlags + $StrictOnly | Where-Object { $_ -match '^-' })
        $drift = @()
        foreach ($f in ($ship | Where-Object { $_ -match '^-O|^-march=|^-mtune=|^-mno-outline' })) {
            if ($mine -notcontains $f) { $drift += "build_now has $f, verify does not" }
        }
        foreach ($f in ($mine | Where-Object { $_ -match '^-O|^-march=|^-mtune=|^-mno-outline' })) {
            if ($ship -notcontains $f) { $drift += "verify has $f, build_now does not" }
        }
        if ($drift.Count -eq 0) {
            Ok "codegen flags match build_now.ps1 ($($ship -join ' '))"
        } else {
            foreach ($x in $drift) { Warn "flag drift: $x" }
            Warn 'sources.json plus the flags above are canonical for this gate.'
            Warn 'A DIFFERENT OPTIMISATION LEVEL PRODUCES DIFFERENT BYTES, so if the'
            Warn 'shipped build uses these flags instead, tools/baseline.json does not'
            Warn 'describe it and the byte comparison below is meaningless.'
        }
    } else {
        Warn 'could not parse $CFLAGS from build_now.ps1, skipping the flag-drift check'
    }
} else {
    Warn 'build_now.ps1 not found, skipping the drift check'
}

$missing = @($sources | Where-Object { -not (Test-Path (Join-Path $Repo $_)) })
if ($missing.Count -gt 0) { Bad ("listed sources do not exist: " + ($missing -join ', ')); exit 2 }
Ok "all $($sources.Count) sources present"

if (Test-Path $WorkDir) { Remove-Item $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
Set-Location $Repo

$cSources = @($sources | Where-Object { $_ -like '*.c' })
$asmSources = @($sources | Where-Object { $_ -like '*.s' })
# .c and .s are mapped to distinct object names so a foo.c / foo.s pair in
# different directories can never collide in the flat work directory.
$objOf = { param($s) Join-Path $WorkDir (((($s -replace '[\\/]','_') -replace '\.c$','.c.o') -replace '\.s$','.s.o')) }

# ------------------------------------------------------- 2. strict compile
Step "strict compile ($($sources.Count) files, -Werror= by class)"
$compileErrors = 0
foreach ($s in $cSources) {
    $out = & $GCC @BaseFlags @StrictOnly -c $s -o (& $objOf $s) 2>&1
    $errs = @($out | Select-String ' error: ')
    $warns = @($out | Select-String ' warning: ')
    if ($LASTEXITCODE -ne 0 -or $errs.Count -gt 0) {
        Bad "$s"
        $errs | Select-Object -First 8 | ForEach-Object { Write-Host "          $($_.Line.Trim())" -ForegroundColor Red }
        $compileErrors++
    }
    foreach ($w in $warns) { $script:Warnings.Add("$s : $($w.Line.Trim())") }
}
if ($compileErrors -eq 0) {
    Ok "no compile errors; $((@($cSources)).Count) C files clean under the strict set"
    if ($script:Warnings.Count -gt 0) {
        Warn "$($script:Warnings.Count) non-fatal warning(s); they are reported, not hidden"
    }
} else { Bad "$compileErrors file(s) failed the strict compile" }

# assembly
foreach ($s in $asmSources) {
    $out = & $GCC @BaseFlags '-c' $s -o (& $objOf $s) 2>&1
    if ($LASTEXITCODE -ne 0) {
        Bad "$s"; $out | Select-Object -First 5 | ForEach-Object { Write-Host "          $_" -ForegroundColor Red }
    }
}
if ($compileErrors -eq 0) { Ok "assembly assembled" }

# ------------------------------------------------------------- 3. -fanalyzer
if ($SkipAnalyzer) {
    Step 'static analysis (-fanalyzer)'
    Warn 'SKIPPED by -SkipAnalyzer. This run does not prove the absence of leaks,'
    Warn 'use-after-free, double free or null dereference paths.'
} elseif ($compileErrors -ne 0) {
    Step 'static analysis (-fanalyzer)'
    Warn 'skipped: the strict compile failed, analyzer output would be noise'
} else {
    Step "static analysis (-fanalyzer, $($cSources.Count) files)"
    Note 'this is the slow pass; -fanalyzer is whole-translation-unit'
    $anFiles = 0
    foreach ($s in $cSources) {
        $out = & $GCC @BaseFlags '-fanalyzer' '-Wall' '-c' $s -o (Join-Path $WorkDir 'an.o') 2>&1
        $diags = @($out | Select-String ' warning: ')
        if ($diags.Count -gt 0) {
            $script:Warnings.Add("$s : ANALYZER")
            $diags | Select-Object -First 4 | ForEach-Object {
                Write-Host "          $($_.Line.Trim())" -ForegroundColor Yellow
            }
        }
        $anFiles++
    }
    Ok "-fanalyzer completed over $anFiles files"
}

# ---------------------------------------------------------------- 4. cppcheck
# Ids that describe a real memory-safety, lifetime or format-string defect. These
# are gated regardless of the severity cppcheck assigned. Everything else
# (constVariablePointer, variableScope, unusedStructMember, badBitmaskCheck,
# knownConditionTrueFalse, unreadVariable ...) is reported and counted but does
# not fail, because on this tree it is almost entirely cosmetic and gating on it
# would bury the classes that matter.
$GatedCppcheckIds = @(
    'uninitvar','unassignedVariable','arrayIndexThenCheck','arrayIndexOutOfBounds',
    'bufferAccessOutOfBounds','negativeIndex','memleak','resourceLeak',
    'danglingLifetime','returnDanglingLifetime','invalidPrintfArgType',
    'formatScanfArgTypeInt','shiftTooManyBits','unsignedLessThanZero',
    'redundantAssignment','duplicateExpression','nullPointer',
    'bufferNotZeroTerminated','memclass','doubleFree'
)

Step 'cppcheck (overflow / lifetime / format strings)'
if (-not (Test-Path $CPPCHECK)) {
    Warn "not installed: $CPPCHECK"
    Warn 'portable cppcheck 2.22.0 lives in tools/bin (extracted from the upstream MSI)'
    Warn '-fanalyzer does not look for buffer overflow or format-string defects,'
    Warn 'so this pass is not redundant'
} else {
    $env:CPPCHECK_CACHE_DIR = Join-Path ([System.IO.Path]::GetTempPath()) 'hexo_cppcheck_cache'
    $abs = @($cSources | ForEach-Object { Join-Path $Repo $_ })
    $out = & $CPPCHECK '--enable=warning,style,performance,portability' '--std=c11' `
                     '--inline-suppr' '--platform=unix64' '--quiet' "-I" $Repo @abs 2>&1

    # Actual cppcheck line format is:
    #   <path>:<line>:<col>: <severity>: <message> [<id>]
    # NOT the ":(severity):" shape used by the xml/template output. Matching the
    # wrong shape silently yields zero findings and a reassuring green run, which
    # is the worst possible failure mode for a static-analysis gate.
    $gated = New-Object System.Collections.Generic.List[string]
    $info  = New-Object System.Collections.Generic.List[string]
    $syntax = @()
    foreach ($l in $out) {
        $s = $l.ToString()
        if ($s -notmatch '^(?<p>.+?):(?<ln>\d+):(?<col>\d+):\s*(?<sev>[a-z]+):\s*(?<msg>.*)$') { continue }
        $sev = $Matches['sev']
        $rel = $Matches['p'].Substring($Repo.Length + 1) -replace '\\','/'
        $msg = $Matches['msg']
        $id  = if ($msg -match '\[(?<i>[A-Za-z0-9_]+)\]') { $Matches['i'] } else { 'unknown' }
        if ($id -eq 'syntaxError') { $syntax += $rel; continue }
        if ($sev -eq 'error' -or $GatedCppcheckIds -contains $id) {
            $gated.Add("$id|$rel|$sev")
        } else {
            $info.Add("$id|$rel")
        }
    }

    if ($syntax.Count -gt 0) {
        Warn ("cppcheck could not parse " + $syntax.Count + " of " + $cSources.Count +
              " file(s); those got NO analysis: " + (($syntax | Select-Object -Unique) -join ', '))
        Warn 'known cause: GCC global register variables, register u64 x0 asm("x0")'
        Warn 'a clean cppcheck run therefore does NOT mean 100% coverage'
    }

    $finger = @($gated | Group-Object | ForEach-Object { "$($_.Name)|$($_.Count)" })
    $cppBase = Join-Path $PSScriptRoot 'cppcheck-baseline.txt'
    if (-not (Test-Path $cppBase)) {
        Bad 'tools/cppcheck-baseline.txt missing'
    } else {
        $known = @{}
        foreach ($line in Get-Content $cppBase) {
            $t = $line.Trim()
            if ($t -eq '' -or $t.StartsWith('#')) { continue }
            $p = $t -split '\|'
            if ($p.Count -ge 2) { $known[($p[0..($p.Count - 2)] -join '|')] = [int]$p[-1] }
        }
        $new = @()
        foreach ($f in $finger) {
            # A fingerprint is "id|path|severity|count". Severity is part of the
            # key on purpose: a finding escalating from style to error must show up
            # as NEW rather than hide behind an existing baseline entry. So the
            # count is the LAST field, never a fixed index.
            $parts = $f -split '\|'
            $key = ($parts[0..($parts.Count - 2)] -join '|')
            $n = [int]$parts[-1]
            if (-not $known.ContainsKey($key))      { $new += "NEW  $key x$n" }
            elseif ($n -gt $known[$key])            { $new += "MORE $key x$n (baseline allows $($known[$key]))" }
        }
        foreach ($k in $known.Keys) {
            if (@($finger | Where-Object { $_ -like "$k|*" }).Count -eq 0) {
                Note "baseline entry no longer reported: $k - tighten cppcheck-baseline.txt"
            }
        }
        $analyzed = $cSources.Count - (@($syntax | Select-Object -Unique)).Count
        if ($new.Count -eq 0) {
            Ok ("cppcheck: $analyzed/" + $cSources.Count + " files analyzed, " +
                $gated.Count + " gated finding(s) all matching the reviewed baseline, " +
                $info.Count + " advisory finding(s) not gated")
        } else {
            foreach ($x in $new) { Bad "cppcheck: $x" }
            $gated | Select-Object -First 15 | ForEach-Object { Write-Host "          $_" -ForegroundColor Yellow }
            Note 'a verified false positive may be added to tools/cppcheck-baseline.txt,'
            Note 'but only with a stated reason - that file is the review record'
        }
    }
}

# ------------------------------------------------------------------ 5. link
Step 'link'
$elf = Join-Path $WorkDir 'kernel_neuro.elf'
$bin = Join-Path $WorkDir 'kernel_neuro.bin'
if ($compileErrors -gt 0) {
    Bad 'not attempted: compile failed'
} else {
    # Link in sources.json order, NOT grouped by file type. Object order changes
    # the layout, so grouping .c before .s yields the same size with different
    # bytes -- which would make the baseline a tripwire for a binary nobody ships.
    # Order is taken straight from the canonical list, which mirrors build_now.ps1.
    $objs = @($sources | ForEach-Object { & $objOf $_ })
    $out = & $GCC '-T' 'linker.ld' '-o' $elf @objs '-ffreestanding' '-nostdlib' 2>&1
    $undef = @($out | Select-String 'undefined reference|ld returned')
    if ($LASTEXITCODE -ne 0 -or $undef.Count -gt 0) {
        Bad 'link failed'
        $out | Select-Object -First 12 | ForEach-Object { Write-Host "          $_" -ForegroundColor Red }
    } else {
        Ok "linked"
        & $OBJCOPY '-O' 'binary' $elf $bin | Out-Null
        if ($LASTEXITCODE -ne 0) { Bad 'objcopy failed' } else { Ok 'binary extracted' }
    }
    if ($OutBin -and (Test-Path $bin)) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $OutBin) -Force | Out-Null
        Copy-Item $bin $OutBin -Force
        Ok "artifact copied to $OutBin"
    }
}

# -------------------------------------------------- 6. size / CRC / SHA vs baseline
Step 'baseline (size + CRC32 + SHA256)'
$baseFile = Join-Path $PSScriptRoot 'baseline.json'
if (-not (Test-Path $bin)) {
    Bad 'no binary to compare'
} else {
    $sz  = (Get-Item $bin).Length
    $sha = (Get-FileHash $bin -Algorithm SHA256).Hash.ToLower()
    $crc = '{0:x8}' -f (Get-Crc32 $bin)
    # Identify the compiler that produced these bytes. The golden values describe
    # ONE compiler's output: aarch64-none-elf and aarch64-linux-gnu produce
    # different code from identical sources, so comparing them would fail forever
    # and train everyone to ignore this check.
    $tcName = Split-Path -Leaf $GCC
    $tcVer  = ((& $GCC '-dumpversion' 2>$null) | Select-Object -First 1)
    $toolchain = "$tcName $tcVer".Trim()
    Write-Host "   built  size=$sz  crc32=$crc"
    Write-Host "   sha256 $sha"
    Write-Host "   toolchain $toolchain"

    if ($Update) {
        $blob = [ordered]@{
            _comment = @(
                'Golden values for tools/verify.ps1. Regenerate with -Update and REVIEW',
                'the diff: this file is the only thing standing between a change and',
                'a silent behavioural difference. An unexplained change here is a bug',
                'These bytes describe ONE compiler. If toolchain does not match,',
                'verify skips the byte comparison and says so, instead of failing.'
            )
            generated_by = 'tools/verify.ps1 -Update'
            toolchain    = $toolchain
            size_bytes    = $sz
            crc32         = $crc
            sha256        = $sha
        }
        $blob | ConvertTo-Json -Depth 4 | Set-Content $baseFile -Encoding utf8
        Ok "baseline.json updated"
    } elseif (-not (Test-Path $baseFile)) {
        Warn 'baseline.json does not exist yet; run once with -Update'
    } else {
        $base = Get-Content $baseFile -Raw | ConvertFrom-Json
        $baseTc = if ($base.PSObject.Properties.Name -contains 'toolchain') { $base.toolchain } else { '' }
        if ($baseTc -and $baseTc -ne $toolchain) {
            Warn "baseline was recorded with '$baseTc', this run used '$toolchain'"
            Warn 'the byte-exact tripwire is INACTIVE for this compiler. Every class-based'
            Warn 'gate above still applied. Do not regenerate the baseline from a'
            Warn 'different compiler just to make this quiet.'
        } else {
            $diff = @()
            if ($sz  -ne [int]$base.size_bytes) { $diff += "size expected $($base.size_bytes) got $sz" }
            if ($crc -ne $base.crc32)              { $diff += "crc32 expected $($base.crc32) got $crc" }
            if ($sha -ne $base.sha256)             { $diff += "sha256 expected $($base.sha256) got $sha" }
            if ($diff.Count -eq 0) {
                Ok "matches baseline (size, CRC32 and SHA256 all identical)"
            } else {
                foreach ($x in $diff) { Bad $x }
                Note 'if this change was intended, re-run with -Update and review the diff'
            }
        }
    }
}

# ------------------------------------------------------- 7. golden log markers
if ($Log) {
    Step 'golden log markers (offline)'
    $markerFile = Join-Path $Repo 'docs\expected\markers.txt'
    if (-not (Test-Path $markerFile)) { Bad 'docs/expected/markers.txt missing' }
    elseif (-not (Test-Path $Log))    { Bad "log not found: $Log" }
    else {
        $text = Get-Content $Log -Raw
        $req = @(); $opt = @()
        foreach ($line in Get-Content $markerFile) {
            $t = $line.Trim()
            if ($t -eq '' -or $t.StartsWith('#')) { continue }
            if ($t.StartsWith('@')) { $opt += $t.Substring(1).Trim() } else { $req += $t }
        }
        $miss = @($req | Where-Object { $text -notmatch [regex]::Escape($_) })
        if ($miss.Count -eq 0) {
            Ok "all $($req.Count) required markers present in $(Split-Path -Leaf $Log)"
            $missInfo = @($opt | Where-Object { $text -notmatch [regex]::Escape($_) })
            if ($missInfo.Count -gt 0) { Note "informational markers absent (not a failure): $($missInfo -join ', ')" }
        } else {
            foreach ($m in $miss) { Bad "missing required marker: $m" }
        }
    }
}

# ---------------------------------------------------------------- summary
Step 'summary'
if ($script:Warnings.Count -gt 0) {
    Write-Host "   $((($script:Warnings | Select-Object -Unique)).Count) distinct warning(s) recorded (non-fatal by design)" -ForegroundColor Yellow
}
if ($script:Failures.Count -eq 0) {
    Write-Host ""
    Write-Host "PASS - H-Exo verified" -ForegroundColor Green
    Write-Host ""
    exit 0
} else {
    Write-Host ""
    Write-Host "FAIL - $($script:Failures.Count) check(s) failed:" -ForegroundColor Red
    foreach ($f in $script:Failures) { Write-Host "   - $f" -ForegroundColor Red }
    Write-Host ""
    exit 1
}
