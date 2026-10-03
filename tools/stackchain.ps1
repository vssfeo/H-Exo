<#
.SYNOPSIS
    Worst-case call-chain stack depth for the H-Exo kernel.

.DESCRIPTION
    -fstack-usage answers "how big is this frame". It cannot answer the question
    that actually breaks bare metal: how deep can the stack get before it runs off
    the top of its slot. This measures that, on the linked image, for the build
    tools/verify.ps1 ships.

    Method:
      1. compile tools/sources.json with the SAME codegen flags verify.ps1 uses,
         plus -fstack-usage and -g, and link with linker.ld. Out of tree.
         Same flags on purpose: measuring a different binary than the one that
         ships makes the number a decoration.
      2. C frame sizes        <- *.su. Static functions can share a name across
         translation units; on collision the LARGER frame is taken, so the result
         errs toward over-reporting rather than under-reporting.
      3. assembly stub frames <- the `sub sp, sp, #imm` that precedes a stub's
         first call. There is no .su for assembly, and the exception stubs are the
         single biggest per-exception cost, so measuring them is not optional.
      4. call graph           <- objdump -d:
             bl <other function> = CALL, adds the callee's frame
             b  <other function> = TAIL CALL, adds nothing, chain continues
             branch inside own range = intra-function, ignored
         Assembly labels that carry no FUNC symbol (the exception stubs, _start)
         are injected as nodes. Without this every branch out of them is dropped
         and the interrupt chain silently measures as zero.
      5. cycles -> recursion would be unbounded; reported as a failure.
      6. longest path per root, roots being functions with no caller.
      7. REACHABILITY. vectors.s branches to exception_handler_irq without
         switching SP, so an exception lands on top of the interrupted frame, and
         the worst case is a SUM of two chains, not the max of them. The
         interrupt chain is only addable to code that actually runs with
         interrupts enabled, so the `msr daifclr, #2` inside kmain is located by
         address and only call sites after it are considered.

    Why this is a safety check and not a style check: a stack overflow on this
    target corrupts memory with no MMU fault to catch it, and the symptom appears
    as unrelated nonsense later.

.PARAMETER Repo
    Repository root. Defaults to the parent of this script's directory, which is
    correct once the script lives in tools/.

.PARAMETER GCC
    C compiler. Resolved the same way verify.ps1 resolves it, so both scripts
    measure and gate with one toolchain.

.PARAMETER PerCoreSlot
    Bytes of stack each core actually gets. boot.s computes
    sp = __stack_top - 0x1000*(core+1), i.e. 4 KiB per core out of the 0x8000
    pool. The pool size is NOT the per-core budget and comparing against it hides
    a four-fold overshoot.

.PARAMETER WarnHeadroomPct
    Warn when the remaining headroom drops below this fraction of the slot.

.EXAMPLE
    pwsh -File tools/stackchain.ps1

.NOTES
    Known limits, so nobody mistakes this for a proof:
      * indirect calls. There is exactly one `blr` in the image today, the
        workqueue function-pointer dispatch. Callers of an indirect target are not
        discovered, so such a target is treated as its own root: its own chain is
        counted, the dispatching frame above it is not.
      * nested exceptions. A fault taken inside an interrupt handler is not
        modelled; add the interrupt chain once more for that case.
      * static analysis only. It reflects the linked image, not runtime behaviour.
      * kmain's own frame is counted once, for the whole of kmain.
#>
[CmdletBinding()]
param(
    [string]$Repo,
    [string]$GCC,
    [int]$PerCoreSlot = 4096,
    [double]$WarnHeadroomPct = 25
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path (Join-Path $Repo 'tools\sources.json'))) {
    Write-Host "FAIL - not a H-Exo checkout (no tools/sources.json under '$Repo')" -ForegroundColor Red
    exit 1
}
$WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) 'hexo_stackchain'

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
function Step { param([string]$m) Write-Host ""; Write-Host "== $m" -ForegroundColor Cyan }
function Ok    { param([string]$m) Write-Host "   OK    $m" }
function Warn  { param([string]$m) Write-Host "   WARN  $m" -ForegroundColor Yellow }
function Bad   { param([string]$m) Write-Host "   FAIL  $m" -ForegroundColor Red; $script:Failures.Add($m) }
function Note  { param([string]$m) Write-Host "   ..    $m" -ForegroundColor DarkGray }

$script:Failures = New-Object System.Collections.Generic.List[string]

# Identical to verify.ps1 $BaseFlags / $StrictOnly codegen half. If these drift,
# this measures a binary nobody ships.
$BaseFlags = @(
    '-O3','-ffreestanding','-nostdlib','-nostartfiles',
    '-fno-common','-fno-builtin',
    '-march=armv8-a+fp+simd','-mno-outline-atomics','-I.',
    '-fstack-usage','-g'
)

Set-Location $Repo

$gccPath = Resolve-Tool -Explicit $GCC -SiblingOf $null -Candidates @(
    'C:\gcc-arm\bin\aarch64-none-elf-gcc.exe','aarch64-none-elf-gcc','aarch64-linux-gnu-gcc','gcc')
if (-not $gccPath) {
    Write-Host "FAIL - no aarch64 cross-compiler found. Depth cannot be measured, and must not be assumed." -ForegroundColor Red
    exit 1
}
$prefix = if ($gccPath -match 'aarch64-none-elf') { 'aarch64-none-elf' } else { 'aarch64-linux-gnu' }
$objdumpPath = Resolve-Tool -Explicit $null -SiblingOf $gccPath -SiblingSuffix $prefix -Candidates @(
    "$prefix-objdump.exe","$prefix-objdump",'objdump')
$readelfPath = Resolve-Tool -Explicit $null -SiblingOf $gccPath -SiblingSuffix $prefix -Candidates @(
    "$prefix-readelf.exe","$prefix-readelf",'readelf')
foreach ($t in @($objdumpPath, $readelfPath)) {
    if (-not $t) { Write-Host "FAIL - objdump/readelf not found next to $gccPath" -ForegroundColor Red; exit 1 }
}

Step 'toolchain'
Ok "gcc     $gccPath"
Ok "objdump $objdumpPath"
Ok "readelf $readelfPath"

$sources = (Get-Content (Join-Path $Repo 'tools\sources.json') -Raw | ConvertFrom-Json).sources
$missing = @($sources | Where-Object { -not (Test-Path (Join-Path $Repo $_)) })
if ($missing.Count -gt 0) { foreach ($m in $missing) { Bad "source listed in sources.json is absent: $m" }; exit 1 }

# ----------------------------------------------------------------- 1. build
Step "build out of tree ($($sources.Count) sources, -fstack-usage -g)"
if (Test-Path $WorkDir) { Remove-Item $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Force $WorkDir | Out-Null

$objs = New-Object System.Collections.Generic.List[string]
$buildErr = New-Object System.Collections.Generic.List[string]
foreach ($s in $sources) {
    $o = Join-Path $WorkDir ((($s -replace '[\\/]','_') -replace '\.[cs]$','.o'))
    if ($s -like '*.s') {
        $out = & $gccPath '-x' 'assembler-with-cpp' '-march=armv8-a+fp+simd' '-c' $s '-o' $o 2>&1
    } else {
        $out = & $gccPath @BaseFlags '-c' $s '-o' $o 2>&1
    }
    if ($LASTEXITCODE -eq 0) { $objs.Add($o) } else { $buildErr.Add("$s :: $(($out | Out-String).Trim())") }
}
foreach ($e in $buildErr) { Bad "compile failed: $e" }
if ($buildErr.Count -gt 0) { exit 1 }

$elf = Join-Path $WorkDir 'kernel.elf'
# The link is retried once. A first-run failure of "cannot find <obj>" for objects
# that are demonstrably on disk was observed on this host; a flaky gate is worse
# than no gate because it teaches people to ignore it.
$linked = $false
foreach ($attempt in 1..2) {
    $lout = & $gccPath '-T' 'linker.ld' '-o' $elf @($objs.ToArray()) '-ffreestanding' '-nostdlib' 2>&1
    if ($LASTEXITCODE -eq 0) { $linked = $true; break }
    if ($attempt -eq 1) {
        Note 'link failed on attempt 1; verifying every object exists, then retrying'
        $absent = @($objs | Where-Object { -not (Test-Path $_) })
        if ($absent.Count -gt 0) { foreach ($a in $absent) { Bad "object missing before link: $a" }; exit 1 }
    }
}
if (-not $linked) {
    foreach ($l in ($lout | Select-Object -First 10)) { Bad "link: $l" }
    exit 1
}
Ok "linked kernel.elf"

# ------------------------------------------------------- 2. C frame sizes
Step 'C frame sizes (.su)'
$frame   = @{}
$qualOf  = @{}
$collide = @{}
$script:NonStatic = New-Object System.Collections.Generic.List[string]
Get-ChildItem $WorkDir -Filter '*.su' | ForEach-Object {
    foreach ($line in (Get-Content $_.FullName)) {
        # format:  file:line:col:FUNC<TAB>BYTES<TAB>QUALIFIER   (TAB separated,
        # and there is no space between col and the function name)
        if ($line -match ':\d+:\d+:(\S+)\s+(\d+)\s+(\S+)\s*$') {
            $fn = $Matches[1]; $b = [int]$Matches[2]; $q = $Matches[3]
            if ($q -notlike 'static*') { $script:NonStatic.Add($fn) }
            if ($frame.ContainsKey($fn)) {
                if ($frame[$fn] -ne $b) { $collide[$fn] = $true; if ($b -gt $frame[$fn]) { $frame[$fn] = $b } }
            } else { $frame[$fn] = $b; $qualOf[$fn] = $q }
        }
    }
}
Ok "$($frame.Count) frame sizes; $(@($collide.Keys).Count) cross-TU name collision(s), larger taken"
if ($script:NonStatic.Count -gt 0) {
    Warn "non-static frame size (VLA or dynamic): $($script:NonStatic -join ', ') - depth NOT bounded by this measurement"
}

# ------------------------------------------------- 3. nodes: FUNC + asm labels
Step 'symbols and assembly stubs'
$funcs = New-Object System.Collections.Generic.List[object]
& $readelfPath '-sW' $elf 2>$null | ForEach-Object {
    if ($_ -match '^\s*\d+:\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+FUNC\s+\S+\s+\S+\s+(\S+)\s+(\S+)\s*$') {
        $sz = [Convert]::ToInt64($Matches[2],16)
        if ($sz -gt 0) {
            $funcs.Add([pscustomobject]@{ Addr=[Convert]::ToInt64($Matches[1],16); Size=$sz; Name=$Matches[4]; Asm=$false })
        }
    }
}
$disasm = @(& $objdumpPath '-d' $elf 2>$null)

$labels = @()
foreach ($line in $disasm) {
    if ($line -match '^([0-9a-f]+) <([^>]+)>:') {
        $labels += [pscustomobject]@{ Addr=[Convert]::ToInt64($Matches[1],16); Name=$Matches[2] }
    }
}
$labels = @($labels | Sort-Object Addr -Unique)

$funcs = @($funcs | Sort-Object Addr)
$isFunc = @{}; foreach ($f in $funcs) { $isFunc[$f.Name] = $true }
$added = 0
for ($i = 0; $i -lt $labels.Count; $i++) {
    $L = $labels[$i]; $covered = $false
    foreach ($f in $funcs) { if ($L.Addr -ge $f.Addr -and $L.Addr -lt ($f.Addr + $f.Size)) { $covered = $true; break } }
    if (-not $covered) {
        $next = if ($i + 1 -lt $labels.Count) { $labels[$i+1].Addr } else { $L.Addr + 0x40 }
        $funcs += [pscustomobject]@{ Addr=$L.Addr; Size=($next - $L.Addr); Name=$L.Name; Asm=$true }
        $added++
    }
}
$funcs = @($funcs | Sort-Object Addr)
Ok "$($funcs.Count - $added) FUNC + $added assembly label node(s)"

# Assembly stub frames. objdump prints a label for C functions too, so this must
# be restricted to nodes with no FUNC symbol or every C prologue lands here.
$stub = @{}
$cur = $null; $seen = $false
foreach ($line in $disasm) {
    if ($line -match '^([0-9a-f]+) <([^>]+)>:') { $cur = $Matches[2]; $seen = $false; continue }
    if ($cur -and -not $seen -and -not $isFunc.ContainsKey($cur) -and $line -match 'sub\s+sp, sp, #0x([0-9a-f]+)') {
        $stub[$cur] = [Convert]::ToInt64($Matches[1],16); $seen = $true
    }
}
foreach ($k in $stub.Keys) { if (-not $frame.ContainsKey($k)) { $frame[$k] = $stub[$k] } }
Ok "$($stub.Count) assembly stub frame(s) measured from the disassembly"

# ------------------------------------------------------------ 4. call graph
Step 'call graph'
$starts = @($funcs | ForEach-Object { $_.Addr })
function Resolve-Func {
    param([int64]$Addr)
    $lo = 0; $hi = $starts.Count - 1
    while ($lo -le $hi) {
        $m = [int](($lo + $hi) / 2)
        if ($starts[$m] -eq $Addr) { return $funcs[$m].Name }
        if ($starts[$m] -lt $Addr) { $lo = $m + 1 } else { $hi = $m - 1 }
    }
    $lo = 0; $hi = $funcs.Count - 1
    while ($lo -le $hi) {
        $m = [int](($lo + $hi) / 2)
        if ($funcs[$m].Addr -le $Addr) { $lo = $m + 1 } else { $hi = $m - 1 }
    }
    if ($hi -ge 0 -and $Addr -lt ($funcs[$hi].Addr + $funcs[$hi].Size)) { return $funcs[$hi].Name }
    return $null
}
$succ = @{}; $sites = 0; $lost = 0
foreach ($line in $disasm) {
    if ($line -match '^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(bl|b)\s+([0-9a-f]+)\s') {
        $sites++
        $from = [Convert]::ToInt64($Matches[1],16)
        $mn   = $Matches[2]
        $to   = [Convert]::ToInt64($Matches[3],16)
        $c = Resolve-Func $from; if (-not $c) { $lost++; continue }
        $k = Resolve-Func $to;  if (-not $k -or $k -eq $c) { continue }
        if (-not $succ.ContainsKey($c)) { $succ[$c] = @() }
        $succ[$c] += ,@($k, ($mn -eq 'bl'))
    }
}
$hasCaller = @{}
foreach ($k in $succ.Keys) { foreach ($e in $succ[$k]) { $hasCaller[$e[0]] = $true } }
Ok "$sites branch site(s), $lost unresolvable source address(es)"
if ($lost -gt 0) { Warn "$lost branch site(s) could not be attributed to a function; depth may be understated" }

# -------------------------------------------------- 5. longest path + cycles
Step 'longest path and recursion check'
$state = @{}; $best = @{}; $via = @{}
$script:Cycles = New-Object System.Collections.Generic.List[string]
function Get-Depth {
    param([string]$Fn, [System.Collections.Generic.List[string]]$Path)
    if ($state[$Fn] -eq 2) { return $best[$Fn] }
    if ($state[$Fn] -eq 1) { $script:Cycles.Add(($Path + $Fn) -join ' -> '); return 0 }
    $state[$Fn] = 1; $Path.Add($Fn)
    $d = 0; $who = $null
    if ($succ.ContainsKey($Fn)) {
        foreach ($e in $succ[$Fn]) {
            $callee = $e[0]; $isCall = $e[1]
            $sub = Get-Depth $callee $Path
            $w = 0; if ($isCall -and $frame.ContainsKey($callee)) { $w = [int]$frame[$callee] }
            if (($sub + $w) -gt $d) { $d = $sub + $w; $who = $callee }
        }
    }
    $Path.RemoveAt($Path.Count - 1)
    $state[$Fn] = 2; $best[$Fn] = $d; $via[$Fn] = $who
    return $d
}
$all = @($funcs | ForEach-Object { $_.Name })
$p = New-Object System.Collections.Generic.List[string]
foreach ($n in $all) { $null = Get-Depth $n $p }
$roots = @($all | Where-Object { -not $hasCaller.ContainsKey($_) })
$rows = @(
    foreach ($r in $roots) {
        $own = 0; if ($frame.ContainsKey($r)) { $own = [int]$frame[$r] }
        [pscustomobject]@{ Root=$r; OwnFrame=$own; ChainBelow=$best[$r]; Total=$own + $best[$r] }
    }
) | Sort-Object Total -Descending

if ($script:Cycles.Count -gt 0) {
    foreach ($c in ($script:Cycles | Select-Object -Unique)) { Bad "recursion, depth unbounded: $c" }
} else {
    Ok "no recursion: $($script:Cycles.Count) cycle(s)"
}

# ------------------------------------------- 6. exception entries and worst case
# The CPU jumps to a vector; it does not call it. The stub frame is therefore
# always consumed, even though the table reaches it with `b` - which the
# tail-call rule above would otherwise discount to zero.
$exc = @(
    foreach ($s in $stub.Keys) {
        $sub = 0; if ($best.ContainsKey($s)) { $sub = $best[$s] }
        [pscustomobject]@{ Root=$s; OwnFrame=[int]$frame[$s]; ChainBelow=$sub; Total=[int]$frame[$s] + $sub }
    }
) | Sort-Object Total -Descending
$worstExc = if ($exc.Count -gt 0) { $exc[0] } else { [pscustomobject]@{ Root='(none)'; Total=0 } }
$worstNorm = $rows[0]
$kmainFrame = 0; if ($frame.ContainsKey('kmain')) { $kmainFrame = [int]$frame['kmain'] }

Step 'reachability: interrupt unmask inside kmain'
$kmainNode = $funcs | Where-Object { $_.Name -eq 'kmain' } | Select-Object -First 1
$irqOn = $null
$postMax = $worstNorm.Total - $kmainFrame
$postRoot = $worstNorm.Root
if ($kmainNode) {
    $kEnd = $kmainNode.Addr + $kmainNode.Size
    foreach ($line in $disasm) {
        if ($line -match '^\s*([0-9a-f]+):.*msr\s+daifclr') {
            $a = [Convert]::ToInt64($Matches[1],16)
            if ($a -ge $kmainNode.Addr -and $a -lt $kEnd) { $irqOn = $a; break }
        }
    }
}
if ($irqOn) {
    Ok ("kmain 0x{0:X}..0x{1:X}, `msr daifclr, #2` at 0x{2:X}" -f $kmainNode.Addr, ($kmainNode.Addr + $kmainNode.Size), $irqOn)
    $cands = @()
    foreach ($line in $disasm) {
        if ($line -match '^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+bl\s+([0-9a-f]+)\s') {
            $a = [Convert]::ToInt64($Matches[1],16)
            if ($a -lt $kmainNode.Addr -or $a -ge $kEnd) { continue }
            if ($a -le $irqOn) { continue }
            $callee = Resolve-Func ([Convert]::ToInt64($Matches[2],16))
            if (-not $callee -or $callee -eq 'kmain') { continue }
            $w = 0; if ($frame.ContainsKey($callee)) { $w = [int]$frame[$callee] }
            $sub = 0; if ($best.ContainsKey($callee)) { $sub = $best[$callee] }
            $cands += [pscustomobject]@{ Callee=$callee; Chain=($w + $sub) }
        }
    }
    $cands = @($cands | Sort-Object Chain -Descending)
    if ($cands.Count -gt 0) { $postMax = $cands[0].Chain; $postRoot = $cands[0].Callee }
    Ok "$($cands.Count) call site(s) after the unmask; deepest $postMax bytes via $postRoot"
} else {
    Warn 'no `msr daifclr` located inside kmain; falling back to the naive bound, which over-reports'
}

# Three distinct figures, and they are not interchangeable:
#
#   worstRoot   deepest chain from any root. A stack overflow does NOT require an
#               interrupt, so this is a real overflow risk on its own and must not
#               be discarded.
#   reachable   kmain + deepest callee chain after the unmask + exception chain.
#               This is the interrupt-stacking risk.
#   naive       worstRoot + exception chain, which over-reports: it adds the
#               handler to a chain that may run with interrupts still masked.
#
# The gate uses max(worstRoot, reachable). Using reachable alone is what this
# script originally did, which happens to be safe today only because reachable
# (2128) exceeds worstRoot (1712) - a coincidence, not a property. A future change
# that pushes the deepest non-interrupt chain above the interrupt one would then
# silently under-report, which is the one failure mode a safety gate must not have.
$worstRoot  = [int]$worstNorm.Total
$reachable  = $kmainFrame + $postMax + $worstExc.Total
$naive      = $worstRoot + $worstExc.Total
$gated      = [Math]::Max($worstRoot, $reachable)

Step 'worst-case stack depth'
Write-Host ("   kmain frame                        : {0,6} bytes" -f $kmainFrame)
Write-Host ("   deepest callee chain (IRQs on)     : {0,6} bytes   via {1}" -f $postMax, $postRoot)
Write-Host ("   exception chain on top             : {0,6} bytes   via {1}" -f $worstExc.Total, $worstExc.Root)
Write-Host ("   deepest root chain (any root)      : {0,6} bytes   via {1}" -f $worstRoot, $worstNorm.Root)
Write-Host ("   interrupt-reachable worst case    : {0,6} bytes" -f $reachable)
Write-Host ("   GATED (max of the two)             : {0,6} bytes" -f $gated)
Write-Host ("   naive bound (over-reports)         : {0,6} bytes" -f $naive)
Write-Host ("   per-core slot (boot.s)             : {0,6} bytes" -f $PerCoreSlot)

$headroom = $PerCoreSlot - $gated
Write-Host ("   HEADROOM                           : {0,6} bytes   ({1:P1} of the slot)" -f $headroom, ($headroom / $PerCoreSlot))
Write-Host "   deepest chain, frame by frame:"
$n = $worstNorm.Root
while ($n) { $f = 0; if ($frame.ContainsKey($n)) { $f = [int]$frame[$n] }; Write-Host ("     {0,6}  {1}" -f $f, $n); $n = $via[$n] }

if ($gated -gt $PerCoreSlot) {
    Bad "worst-case stack $gated B exceeds the $PerCoreSlot B per-core slot"
} elseif (($headroom / $PerCoreSlot) -lt ($WarnHeadroomPct / 100)) {
    Warn ("headroom is only {0:P1} of the slot; reduce kmain's frame before adding more call depth" -f ($headroom / $PerCoreSlot))
} else {
    Ok "worst-case stack fits, $([math]::Round(100 * $headroom / $PerCoreSlot, 1))% headroom"
}

# The single biggest lever, named rather than left implicit.
$biggest = @($frame.GetEnumerator() | Where-Object { $isFunc.ContainsKey($_.Key) } | Sort-Object Value -Descending | Select-Object -First 5)
Write-Host "   largest individual frames:"
foreach ($b in $biggest) { Write-Host ("     {0,6}  {1}" -f $b.Value, $b.Key) -ForegroundColor DarkGray }

Step 'summary'
[pscustomobject]@{
    generated            = (Get-Date).ToString('s')
    toolchain            = (Split-Path -Leaf $gccPath)
    per_core_slot        = $PerCoreSlot
    nodes                = $funcs.Count
    c_frames             = $frame.Count
    asm_stub_frames      = $stub
    recursion_cycles     = $script:Cycles.Count
    kmain_frame          = $kmainFrame
    irq_unmask_address   = $(if ($irqOn) { '0x{0:X}' -f $irqOn } else { $null })
    deepest_callee_chain = $postMax
    deepest_callee_root  = $postRoot
    exception_chain      = $worstExc.Total
    exception_root       = $worstExc.Root
    deepest_root_chain   = $worstRoot
    interrupt_reachable  = $reachable
    gated_worst_case     = $gated
    naive_bound          = $naive
    headroom             = $headroom
    su_name_collisions   = @($collide.Keys)
    top_chains           = @($rows | Select-Object -First 20)
} | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $WorkDir 'stackchain.json')
Note "report: $(Join-Path $WorkDir 'stackchain.json')"

if ($script:Failures.Count -eq 0) {
    Write-Host ""
    Write-Host "PASS - stack depth within budget" -ForegroundColor Green
    Write-Host ""
    exit 0
}
Write-Host ""
Write-Host "FAIL - $($script:Failures.Count) check(s) failed:" -ForegroundColor Red
foreach ($f in $script:Failures) { Write-Host "   - $f" -ForegroundColor Red }
Write-Host ""
exit 1