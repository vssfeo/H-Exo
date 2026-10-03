# Mali-T860: the 4x4 matmul is blocked by a 1-thread job limit, not by our code

**Date:** 2026-10-03. **Board:** NanoPi M4 (RK3399), BL31 = rkbin v1.36 prebuilt.
**Status:** root cause localised, fix deliberately NOT applied. Read this before
touching `build_matmul_4x4_shader()` or `local_size` again.

## Symptom

```
[MATMUL] wait res=0x6 done=0x0 fail=0x1 js=0x51 irq=0x20000
[WARN] Mali-T860: MATMUL 4x4 FAILED
```

`res=0x6` is `ERR_HARDWARE_FAULT`. `done=0` with a set fail bit and
`JS_STATUS = 0x51` (`READY` set, `FAILED` clear) means the Job Manager rejected
the job at submit; it never started executing. The same run reports the GPU stack
itself healthy:

```
[OK] Mali-T860: GPU powered on / Job Manager ready / MMU identity-mapped
[OK] WRITE_VALUE side-effect OK, CACHE_FLUSH completed OK
[MALI_P3.1] *** SHADER_STORE_OK *** [0]=0x00000000
```

`SHADER_STORE_OK` is a real store: the buffer was pre-filled with `0xDEADBEEF`
and the GPU wrote `0x00000000` to slot 0.

## What was eliminated, and how

| hypothesis | eliminated by |
|---|---|
| MMIO / Job Manager / MMU / descriptors broken | smoke test passes end to end this boot |
| the store primitive is wrong | `SHADER_STORE_OK`, storage[0] changed from `DEADBEEF` to 0 |
| the matmul shader is wrong | crossing B below: matmul shader completes at `local_size=1` |
| the descriptor layout is wrong | our `pan_pack_invocation` matches Mesa `pan_encoder.h:196-235` line for line; `job_task_split` matches `panvk_vX_cmd_precomp.c:63-67` |
| a thread-count limit on the part | `MAX_THREADS = MAX_WORKGROUP_SIZE = MAX_BARRIER_SIZE = 0x100` (256) |
| TLS block sized wrong | `GPU_THREAD_TLS_ALLOC` reads 0, so sizing from it allocates nothing and changes nothing |

## The decisive crossing test

Same RSD, same buffers, only the shader or the thread counts swapped:

```
[AB] A  proven 20-word st_32 shader + 16 threads   res=6 done=0 fail=1   <- same failure
[AB] B  84-word matmul shader        +  1 thread   res=0 done=1 fail=0   <- completes
```

`local_size` is the discriminator. It is not the shader.

## The limit is exactly one invocation

Full factorial sweep, `num_groups` x `local_size`, each 1..4, plus an 8x1 case:

```
[GRID]  num_groups(local_size=1..4)
g=1    r  X  X  X
g=2    X  X  X  X
g=3    X  X  X  X
g=4    X  X  X  X
[GRID] g=8 l=1 -> X   w8=0x7
```

**Only 1 group x 1 thread is accepted.** Not 2 threads. Not 2 groups.

A separate scan held `w8 = w9 = 0` and swept `job_task_split` over 0..15: **all sixteen accepted.** So `job_task_split` is irrelevant; the single gate is `job[8][0]`, i.e. `size_x - 1`.

Per `docs/vendor/panfrost/mesa_v5.xml` (`struct Invocation`, `job[8]` = packed
`Invocations`, `job[9]` = Size Y/Z shift, Workgroups X/Y/Z shift, Thread group
split), `job[8][0] = 1` encodes `size_x = 2`.

## Most consistent explanation, and its limits

`GPU_THREAD_TLS_ALLOC` (0x310, RO) reads **0** while the part advertises 256
threads per core. A part with no thread-local storage cannot spawn more than one
thread per group, and the Job Manager refuses such a job at submit rather than
faulting it mid-execution.

This is a hypothesis, not a proof. What is proven is the behaviour above; the
mechanism is inferred from two registers. It cannot be confirmed from H-Exo alone:
deciding between "the part disagrees with its own RO register" and "we read the
wrong register" needs either a Linux driver on this board or a TF-A patch.

## Why `local_size = 1` is NOT the fix

`build_matmul_4x4_shader()` derives which output element to compute from the
global thread id:

```c
mm_emit_alu_imm(..., MIDG_OP_IAND, 3, 2, 12);   // r3 = TID & 12
mm_emit_alu_imm(..., MIDG_OP_ISHL, 3, 3, 2);    // r3 = (TID & 15) * 4
mm_emit_alu_imm(..., MIDG_OP_IAND, 4, 2, 3);    // r4 = TID & 3
mm_emit_alu_imm(..., MIDG_OP_ISHL, 4, 4, 4);    // r4 = (TID & 3) * 16
```

With one thread `TID` is always 0, so the shader would write `C[0]` and leave
`C[1..15]` at the `0xDEADBEEF` sentinel. The job would submit and "pass" while
computing 1/16 of the product. That is a silently wrong answer.

**The worse half: the bench verifies nothing.** There is no comparison of `C`
against the expected identity matrix anywhere in `mali_compute_matmul_4x4()`. The
only signal is the job failing. So no configuration of `local_size` can currently
produce a trustworthy PASS, including the one that works.

## To actually close this, two changes are needed together

1. A single-threaded shader that computes all 16 output elements sequentially
   instead of indexing by `TID`. Same arithmetic, loop unrolled in the shader,
   addressing driven by an incrementing constant rather than by thread id.
   Arithmetic is affordable: 16 outputs x 4 MAC = 64 fixed-point
   multiply-accumulates, well under the ~200 ns the CPU path spends on the entire
   6-8-4 net.
2. **A real verification in the bench**: compare all 16 outputs of `C` against the
   CPU reference and report the number of matching elements. Without this, the
   next "PASS" is as untrustworthy as the last one.

Change (2) first. It is small, it is the reason this bug was able to hide, and it
converts every future experiment into a measurement.

## Also worth knowing

`Midgard_ISA.md:271-280` lists the load/store opcodes and contains **no `0xC8`**,
yet `0xC8` is our `MIDG_OP_ST_32` and it is measured working. `0x88` (`LD_32`) and
`0x90` (`LD_128`) are likewise absent from that table. **The vendored ISA
document is incomplete and cannot settle encoding questions on this part.** Where
the doc and the hardware disagree, the hardware measurement wins — that is how
`0xC8` was established in the first place.

## History

The `MATMUL 4x4 FAILED` warning is chronic: present in every log from
2026-10-02 15:35 (`exp1/final_run.log`) onward. There are no May 2026 UART logs on
this machine, and `docs/H-Exo_MASTER_PLAN_v3.3.md` (dated 2026-05-03) records
"matmul NOT started" with `st_32` store verified. So the 4x4 matmul has **never**
passed; there is no working version to regress to.

## Resolution (2026-10-04, measured)

The 4x4 matmul now passes **16/16 on the NanoPi M4**. `I * I = I` with exact bit
patterns, verified on-image (final image e904fab9, proof image 9ef229ec, both
FATAL=0):

```text
[MATMUL] wait res=0 done=1 fail=0 js=0
[MATMUL] VERIFY match=16/16 bad_mask=0x00000000 first_bad=0xFFFFFFFF
[MATMUL] C=3F800000 0 0 0 | 0 3F800000 0 0 | 0 0 3F800000 0 | 0 0 0 3F800000
[OK] Mali-T860: MATMUL 4x4 CORRECT (16/16)
[OK] Mali-T860: MATMUL 4x4 PASSED (I*I=I)
```

What the two failure classes actually were (each established by measurement,
not inference):

1. **"Long shader rejected, js=0x52" was NOT a length limit.** Every faulting
   shader had a broken bundle chain: a `next_type` that was not the tag of the
   bundle actually emitted after it, or an RSD `first_tag` that was not the tag
   of the shader's first bundle. The fault pointer landed exactly where the
   decode diverged (matmul: word 1 = RSD first_tag LDST vs real ALU8 preamble;
   LDPROBE: word 26 = bundle next ALU4 vs real LDST). The 268-word shader runs
   when every `next_type` matches its successor and `first_tag` matches the
   preamble (midgard_emit.c:959-961; midgard_compile.c:3121 + pan_shader.h:187).
2. **"matmul writes nothing to C" was the stale SHADER SIZE BISECTION block.**
   It ran between the matmul submit/wait and the verify, reset `g_mm_C` to
   0xDEADBEEF on every one of its ~100 iterations and resubmitted jobs with the
   pre-fix first_tag (faulting 0x52). The shader had been writing correct
   results since the chain fix; the BZ block erased them before VERIFY read C.
   The block is removed.

The 1x1 dispatch gate (job[8][0] = size_x - 1) stands: the part still accepts
only num_groups=1 x local_size=1, so the shader is single-threaded and emits all
16 elements itself. This is the shipping design; no dimensional scaling will be
attempted (see HISTORY above for the swept proof).

New capabilities verified on this part while getting here (pipeline document
section 15 items 1-4, now closed by measurement):
- nonzero inline-constant binding into r27 (0x2400 stored);
- LDST loads (ld_32, ld_128) with base r26 and byte offsets;
- stores with byte offsets 0..192 (signed_offset honored, no scaling);
- multi-element shaders up to 268 words (16 elements, all stores land).
