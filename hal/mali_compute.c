// H-Exo Phase 3.4: Mali Compute Job Submission Implementation

#include "mali_compute.h"
#include "mali.h"
#include "mali_jm.h"
#include "mali_mmu.h"
#include "uart.h"

extern uart_t console;

// Static job descriptor (must be aligned and in identity-mapped memory)
// Placed in .bss with 64-byte alignment
static mali_job_desc_t __attribute__((aligned(64))) g_null_job;
static mali_job_desc_t __attribute__((aligned(64))) g_write_value_job;
static mali_job_desc_t __attribute__((aligned(64))) g_cache_flush_job;
static volatile u32 __attribute__((aligned(64))) g_write_value_target;

// GenXML v5 Compute Job layout (inline aggregate):
//   Header     @0x00 = words 0..7   (32 bytes)
//   Invocation @0x20 = words 8..15  (32 bytes)
//   Draw       @0x40 = words 16..47 (128 bytes)
#define CP_HEADER_W        0
#define CP_INVOCATION_W    8
#define CP_DRAW_W         16
#define CP_DRAW_STATE      (CP_DRAW_W + 14)
#define CP_DRAW_THREAD_STORAGE (CP_DRAW_W + 28)
#define CP_TOTAL_WORDS     48   // 192 bytes

static u32 __attribute__((aligned(64))) g_compute_probe_job[CP_TOTAL_WORDS];
static u32 __attribute__((aligned(64))) g_compute_probe_storage[64];
static u32 __attribute__((aligned(64))) g_compute_thread_storage[8];

// Phase 3.1 v3: Midgard shader built programmatically.
// Structure (4 bundles, 80 bytes = 20 words):
//   Bundle 1 (TAG_ALU_8, 32B): VADD imov constant[0] → r26 (target address)
//   Bundle 2 (TAG_ALU_4, 16B): VADD imov inline_0 → r27 (data = 0)
//   Bundle 3 (TAG_LOAD_STORE_4, 16B): st_32, data=r27, addr=r26
//   Bundle 4 (TAG_ALU_4_WRITEOUT, 16B): br.writeout (commit stores, EOT)
//
// Global store addressing: A + (B << shift) + signed_offset
//   arg_reg=0 (r26 as address A), index_reg=7 (ZERO, no B), offset=0
//   reg=1 (data from r27), bitsize_toggle=1 (64-bit address)
//
#define SHADER_WORDS 20
static u32 __attribute__((aligned(64))) g_shader_binary[SHADER_WORDS];

// Midgard ISA constants
#define TAG_BREAK_          0x1
#define TAG_LOAD_STORE_4_   0x5
#define TAG_ALU_4_          0x8
#define TAG_ALU_8_          0x9
#define TAG_ALU_4_WRITEOUT_ 0xC
#define ALU_ENAB_VEC_ADD    (1u << 21)
#define ALU_ENAB_BR_COMPACT (1u << 26)
#define MIDG_OP_IMOV        0x7B
#define MIDG_OP_ST_32       0xC8
#define MIDG_OP_LDST_NOP    0x03
#define REG_UNUSED          24
#define REG_CONSTANT        26
#define REG_LDST_BASE       26
#define REG_LDST_ZERO_IDX   7
#define MIDGARD_INDEX_ADDRESS_U32 2
#define MIDGARD_INDEX_ADDRESS_S32 3
#define MIDG_REG_MODE_32    2
#define MIDG_SHRINK_NONE    2
#define MIDG_OUTMOD_KEEPLO  2
#define SWIZZLE_IDENTITY    0xE4u

// Matmul shader ISA constants
#define MIDG_OP_LD_32       0x88
#define MIDG_OP_LD_128      0x90
#define MIDG_OP_LDST_MOV    0x10
#define MIDG_OP_FDOT4       0x3E
#define MIDG_OP_ISHL        0x6E
#define MIDG_OP_ISHR        0x69  // ilsr = logical shift right
#define MIDG_OP_IADD        0x40
#define MIDG_OP_IAND        0x70
#define MIDG_OP_IEQ         0xA0
#define ALU_ENAB_VEC_MUL    (1u << 17)
#define ALU_ENAB_SCAL_ADD   (1u << 19)
#define TAG_ALU_12_         0xA
#define REG_LDST_TID        6   // GLOBAL_THREAD_ID index in LDST register file

// Pack midgard_reg_info: {src1_reg:5, src2_reg:5, out_reg:5, src2_imm:1}
static inline u16 pack_reg_info(u32 src1, u32 src2, u32 out, u32 imm) {
    return (u16)((src1 & 0x1F) | ((src2 & 0x1F) << 5) |
                 ((out & 0x1F) << 10) | ((imm & 1) << 15));
}

// Pack midgard_vector_alu_src: {mod:2, src_expand:3, swizzle:8} = 13 bits
static inline u32 pack_vec_src(u32 mod, u32 expand, u32 swizzle) {
    return (mod & 3) | ((expand & 7) << 2) | ((swizzle & 0xFF) << 5);
}

// Pack midgard_vector_alu (48 bits) into 2 u32 values: low32 and high16
// {op:8, reg_mode:2, src1:13, src2:13, shrink:2, outmod:2, mask:8}
static inline void pack_vector_alu(u32 op, u32 reg_mode, u32 src1, u32 src2,
                                   u32 shrink, u32 outmod, u32 mask,
                                   u32* lo32, u32* hi16) {
    u64 v = (u64)(op & 0xFF)
          | ((u64)(reg_mode & 3) << 8)
          | ((u64)(src1 & 0x1FFF) << 10)
          | ((u64)(src2 & 0x1FFF) << 23)
          | ((u64)(shrink & 3) << 36)
          | ((u64)(outmod & 3) << 38)
          | ((u64)(mask & 0xFF) << 40);
    *lo32 = (u32)(v & 0xFFFFFFFF);
    *hi16 = (u32)((v >> 32) & 0xFFFF);
}

// --- Matmul shader helpers ---

// Pack vector ALU body as 48-bit u64
static inline u64 pack_vec_alu_48(u32 op, u32 reg_mode, u32 src1, u32 src2,
                                   u32 shrink, u32 outmod, u32 mask) {
    return (u64)(op & 0xFF)
         | ((u64)(reg_mode & 3) << 8)
         | ((u64)(src1 & 0x1FFF) << 10)
         | ((u64)(src2 & 0x1FFF) << 23)
         | ((u64)(shrink & 3) << 36)
         | ((u64)(outmod & 3) << 38)
         | ((u64)(mask & 0xFF) << 40);
}

// Pack scalar source: {mod:2, full:1, component:3} = 6 bits
// For 32-bit (full=1), component is shifted: comp_idx << 1
static inline u32 pack_scalar_src(u32 mod, u32 full, u32 comp_shifted) {
    return (mod & 3) | ((full & 1) << 2) | ((comp_shifted & 7) << 3);
}

// Encode inline constant for scalar ALU src2 (11 bits)
static inline u32 encode_scalar_inline(int value) {
    int lower_12 = value & ((1 << 12) - 1);
    u32 imm = 0;
    imm |= (lower_12 >> 9) & 3;
    imm |= (lower_12 >> 6) & 4;
    imm |= (lower_12 >> 2) & 0x38;
    imm |= (lower_12 & 63) << 6;
    return imm;
}

// Pack scalar ALU body (32 bits):
// {op:8, src1:6, src2:11, reserved:1, outmod:2, output_full:1, output_component:3}
// Bit layout: op[7:0] src1[13:8] src2[24:14] rsv[25] outmod[27:26] full[28] comp[31:29]
static inline u32 pack_scal_alu_32(u32 op, u32 src1_6, u32 src2_11,
                                    u32 outmod, u32 out_full, u32 out_comp_shifted) {
    return (op & 0xFF) | ((src1_6 & 0x3F) << 8) | ((src2_11 & 0x7FF) << 14)
         | ((outmod & 3) << 26) | ((out_full & 1) << 28)
         | ((out_comp_shifted & 7) << 29);
}

// Encode vector inline constant V (16-bit) into 13-bit alu_field.src2
// Also need: reg_info.src2_reg = V >> 11
static inline u32 encode_vector_inline_src2(u32 V) {
    u32 imm = ((V >> 8) & 0x7) | ((V & 0xFF) << 3);
    return imm << 2;
}

// Pack a 60-bit LDST instruction word as u64
static inline u64 pack_ldst_word(u32 op, u32 reg, u32 mask, u32 swizzle,
                                  u32 arg_comp, u32 arg_reg,
                                  u32 bitsize_toggle, u32 index_format,
                                  u32 index_comp, u32 index_reg,
                                  u32 index_shift, u32 signed_offset) {
    return (u64)(op & 0xFF)
         | ((u64)(reg & 0x1F) << 8)
         | ((u64)(mask & 0xF) << 13)
         | ((u64)(swizzle & 0xFF) << 17)
         | ((u64)(arg_comp & 0x3) << 25)
         | ((u64)(arg_reg & 0x7) << 27)
         | ((u64)(bitsize_toggle & 0x1) << 30)
         | ((u64)(index_format & 0x3) << 31)
         | ((u64)(index_comp & 0x3) << 33)
         | ((u64)(index_reg & 0x7) << 35)
         | ((u64)(index_shift & 0xF) << 38)
         | ((u64)(signed_offset & 0x3FFFF) << 42);
}

// Pack LDST bundle (128 bits = 4 words): type_byte + word1(60) + word2(60) + padding
static inline void pack_ldst_bundle(u32* out, u32 this_tag, u32 next_tag,
                                     u64 word1, u64 word2) {
    u8 type_byte = (this_tag & 0xF) | ((next_tag & 0xF) << 4);
    // Byte layout: [type:1B][word1:7.5B][word2:7.5B] = 16B
    // word1 is 60 bits, word2 is 60 bits, type is 8 bits = 128 bits
    u64 lo = (u64)type_byte | (word1 << 8);
    u64 hi = (word1 >> 56) | (word2 << 4);
    out[0] = (u32)(lo & 0xFFFFFFFF);
    out[1] = (u32)(lo >> 32);
    out[2] = (u32)(hi & 0xFFFFFFFF);
    out[3] = (u32)(hi >> 32);
}

static void mali_decode_st32_word(u64 w);

// Build the shader binary with target_addr patched in.
// target_addr is the 32-bit physical address to write zero into.
static void build_st32_shader(u32* shader, u32 target_addr) {
    for (u32 i = 0; i < SHADER_WORDS; i++) shader[i] = 0;

    // --- Bundle 1: TAG_ALU_8 (words 0-7, 32 bytes) ---
    // VADD imov: move embedded constant[0] (target_addr) into r26
    // NOTE: imov reads from src2 in hardware, so constant must be in src2 position
    u32 ctrl1 = TAG_ALU_8_ | (TAG_ALU_4_ << 4) | ALU_ENAB_VEC_ADD;
    u16 reg1 = pack_reg_info(REG_UNUSED, REG_CONSTANT, REG_LDST_BASE, 0);
    u32 src_id = pack_vec_src(0, 0, SWIZZLE_IDENTITY);  // passthrough
    u32 body1_lo, body1_hi;
    pack_vector_alu(MIDG_OP_IMOV, MIDG_REG_MODE_32, src_id, src_id,
                    MIDG_SHRINK_NONE, MIDG_OUTMOD_KEEPLO, 0xFF, &body1_lo, &body1_hi);

    shader[0] = ctrl1;
    // Word 1: reg_info(16) | body_lo[15:0](16)
    shader[1] = (u32)reg1 | ((body1_lo & 0xFFFF) << 16);
    // Word 2: body_lo[31:16](16) | body_hi[15:0](16)
    shader[2] = (body1_lo >> 16) | (body1_hi << 16);
    // Word 3: padding
    shader[3] = 0;
    // Words 4-7: embedded constants (128 bits)
    shader[4] = target_addr;  // constant[0] = target address
    shader[5] = 0;            // constant[1]
    shader[6] = 0;            // constant[2]
    shader[7] = 0;            // constant[3]

    // --- Bundle 2: TAG_ALU_4 (words 8-11, 16 bytes) ---
    // VADD imov: move inline constant 0 into r27
    u32 ctrl2 = TAG_ALU_4_ | (TAG_LOAD_STORE_4_ << 4) | ALU_ENAB_VEC_ADD;
    // For inline constant: src2_imm=1, src2_reg = inline_constant >> 11 = 0
    u16 reg2 = pack_reg_info(REG_UNUSED, 0, REG_LDST_BASE + 1, 1);
    // For inline constant=0: alu.src2 = 0
    u32 body2_lo, body2_hi;
    pack_vector_alu(MIDG_OP_IMOV, MIDG_REG_MODE_32, src_id, 0,
                    MIDG_SHRINK_NONE, MIDG_OUTMOD_KEEPLO, 0xFF, &body2_lo, &body2_hi);

    shader[8] = ctrl2;
    shader[9] = (u32)reg2 | ((body2_lo & 0xFFFF) << 16);
    shader[10] = (body2_lo >> 16) | (body2_hi << 16);
    shader[11] = 0;

    // --- Bundle 3: TAG_LOAD_STORE_4 (words 12-15, 16 bytes) ---
    // midgard_load_store: type(4) + next_type(4) + word1(60) + word2(60)
    // word1 = st_32 instruction, word2 = NOP (0x03)
    //
    // midgard_load_store_word bitfield layout (from Mesa midgard.h):
    //   op[7:0], reg[12:8], mask[16:13], swizzle[24:17],
    //   arg_comp[26:25], arg_reg[29:27], bitsize_toggle[30],
    //   index_format[32:31], index_comp[34:33], index_reg[37:35],
    //   index_shift[41:38], signed_offset[59:42]
    // For st_32: mask=0xF writes all 4 bytes; swizzle selects source components.
    // Global pointers are 64-bit addresses in Mesa's Midgard path. Bundle 1
    // loads r26.xyzw from embedded constants {target_addr, 0, 0, 0}, so r26.xy
    // is the 64-bit address with high word zero.
    u64 w1 = (u64)MIDG_OP_ST_32             // bits  0- 7: op=0xC8
           | ((u64)1 << 8)                  // bits  8-12: reg=1 (r27 data)
           | ((u64)0x0F << 13)              // bits 13-16: mask=0xF
           | ((u64)0x00 << 17)              // bits 17-24: swizzle=0
           | ((u64)0 << 25)                 // bits 25-26: arg_comp=0
           | ((u64)0 << 27)                 // bits 27-29: arg_reg=0 (r26 address)
           | ((u64)1 << 30)                 // bit  30:    bitsize_toggle=1 (64-bit address)
           | ((u64)1 << 31)                 // bits 31-32: index_format=u64
           | ((u64)0 << 33)                 // bits 33-34: index_comp=0
           | ((u64)REG_LDST_ZERO_IDX << 35) // bits 35-37: index_reg=7 (ZERO)
           | ((u64)0 << 38)                 // bits 38-41: index_shift=0
           | ((u64)0 << 42);               // bits 42-59: signed_offset=0
    u64 w2 = (u64)MIDG_OP_LDST_NOP;        // NOP second instruction
    mali_decode_st32_word(w1);

    // Pack into 128-bit midgard_load_store: type(4)+next_type(4)+word1(60)+word2(60)
    u8 type_byte = TAG_LOAD_STORE_4_ | (TAG_ALU_4_WRITEOUT_ << 4);  // chain to writeout bundle
    // Full 128-bit layout:
    //   bits 0-3:   type = 0x5
    //   bits 4-7:   next_type = 0xC (TAG_ALU_4_WRITEOUT → writeout bundle)
    //   bits 8-67:  word1 (60 bits)
    //   bits 68-127: word2 (60 bits)
    u64 ls_lo = (u64)type_byte | (w1 << 8);               // bits 0-63
    u64 ls_hi = (w1 >> 56) | (w2 << 4);                   // bits 64-127
    // Note: word1 is 60 bits at bit offset 8. bits 8-67 = 60 bits.
    //   ls_lo carries bits 0-63: type_byte(8) + word1[0:55](56 bits)
    //   ls_hi carries bits 64-127: word1[56:59](4 bits) + word2[0:59](60 bits)
    shader[12] = (u32)(ls_lo & 0xFFFFFFFF);
    shader[13] = (u32)(ls_lo >> 32);
    shader[14] = (u32)(ls_hi & 0xFFFFFFFF);
    shader[15] = (u32)(ls_hi >> 32);

    // --- Bundle 4: TAG_ALU_4_WRITEOUT (words 16-19, 16 bytes) --- WRITEOUT ---
    // Compact branch with writeout op commits all pending stores and ends thread.
    // midgard_branch_cond: op=7(writeout), dest_tag=0, offset=0, cond=3(always)
    // Encoding: (cond=3 << 14) | op=7 = 0xC007
    u32 ctrl4 = TAG_ALU_4_WRITEOUT_ | (TAG_BREAK_ << 4) | ALU_ENAB_BR_COMPACT;
    shader[16] = ctrl4;          // control: 0x0400001C
    shader[17] = 0x0000C007;     // compact branch writeout at body start
    shader[18] = 0x00000000;
    shader[19] = 0x00000000;

}

// GenXML v5 Renderer State (64 bytes, 64-byte aligned):
//   word 0-1:  Shader.Shader (64-bit address of shader binary)
//   word 2:    Shader.Sampler count [15:0] | Shader.Texture count [31:16]
//   word 3:    Shader.Attribute count [15:0] | Shader.Varying count [31:16]
//   word 4:    Renderer Properties (work_register_count [20:16], uniform_count [25:21])
//   word 5-7:  depth units/factor/clamp (float, 0 for compute)
//   word 8-12: multisample/stencil/alpha (0 for compute)
//   word 13:   Thread Balancing [15:0]
//   word 14-15: Legacy Blend Shader address (0 for compute)
#define RS_SHADER_ADDR_W   0
#define RS_PROPS_W         4
#define RS_PRELOAD_W       12
static u32 __attribute__((aligned(64))) g_shader_desc[16] = {0};

// Phase 3.1 v3: No SSBO descriptors needed — global store uses direct address in r26.

static void mali_clean_range(void* ptr, u64 size) {
    uintptr_t p = (uintptr_t)ptr & ~63ULL;
    uintptr_t end = ((uintptr_t)ptr + size + 63ULL) & ~63ULL;
    while (p < end) {
        asm volatile("dc cvac, %0" :: "r"(p) : "memory");
        p += 64;
    }
    asm volatile("dsb sy" ::: "memory");
}

static void mali_invalidate_range(void* ptr, u64 size) {
    uintptr_t p = (uintptr_t)ptr & ~63ULL;
    uintptr_t end = ((uintptr_t)ptr + size + 63ULL) & ~63ULL;
    while (p < end) {
        asm volatile("dc ivac, %0" :: "r"(p) : "memory");
        p += 64;
    }
    asm volatile("dsb sy" ::: "memory");
}

static void mali_dump_job_desc(const mali_job_desc_t* job) {
    const u32* w = (const u32*)job;
    uart_puts(&console, "[MALI_JOB_DESC] w0=0x");
    uart_put_hex(&console, w[0]);
    uart_puts(&console, " w1=0x");
    uart_put_hex(&console, w[1]);
    uart_puts(&console, " w2=0x");
    uart_put_hex(&console, w[2]);
    uart_puts(&console, " w3=0x");
    uart_put_hex(&console, w[3]);
    uart_puts(&console, " w4=0x");
    uart_put_hex(&console, w[4]);
    uart_puts(&console, " w5=0x");
    uart_put_hex(&console, w[5]);
    uart_puts(&console, " w6=0x");
    uart_put_hex(&console, w[6]);
    uart_puts(&console, " w7=0x");
    uart_put_hex(&console, w[7]);
    uart_puts(&console, " w8=0x");
    uart_put_hex(&console, w[8]);
    uart_puts(&console, " w9=0x");
    uart_put_hex(&console, w[9]);
    uart_puts(&console, " w10=0x");
    uart_put_hex(&console, w[10]);
    uart_puts(&console, " w11=0x");
    uart_put_hex(&console, w[11]);
    uart_puts(&console, "\r\n");
}

static inline u8 clamp_u8(u32 v) {
    return (u8)((v > 255u) ? 255u : v);
}

static void mali_dump_u32_words(const char* tag, const u32* words, u32 count) {
    uart_puts(&console, tag);
    for (u32 i = 0; i < count; i++) {
        if ((i % 8) == 0) {
            uart_puts(&console, "\r\n  w");
            uart_put_hex(&console, i);
            uart_puts(&console, "=");
        }
        uart_put_hex(&console, words[i]);
        uart_puts(&console, " ");
    }
    uart_puts(&console, "\r\n");
}

static void mali_decode_st32_word(u64 w) {
    uart_puts(&console, "[MALI_ST32_DECODE] op=0x");
    uart_put_hex(&console, (u32)(w & 0xFFu));
    uart_puts(&console, " reg=0x");
    uart_put_hex(&console, (u32)((w >> 8) & 0x1Fu));
    uart_puts(&console, " mask=0x");
    uart_put_hex(&console, (u32)((w >> 13) & 0xFu));
    uart_puts(&console, " swizzle=0x");
    uart_put_hex(&console, (u32)((w >> 17) & 0xFFu));
    uart_puts(&console, " arg_comp=0x");
    uart_put_hex(&console, (u32)((w >> 25) & 0x3u));
    uart_puts(&console, " arg_reg=0x");
    uart_put_hex(&console, (u32)((w >> 27) & 0x7u));
    uart_puts(&console, " bitsize64=0x");
    uart_put_hex(&console, (u32)((w >> 30) & 0x1u));
    uart_puts(&console, " index_format=0x");
    uart_put_hex(&console, (u32)((w >> 31) & 0x3u));
    uart_puts(&console, " index_reg=0x");
    uart_put_hex(&console, (u32)((w >> 35) & 0x7u));
    uart_puts(&console, "\r\n");
}

static void mali_dump_compute_probe(const u32* job) {
    mali_dump_u32_words("[MALI_COMPUTE_PROBE] words:", job, CP_TOTAL_WORDS);
}

static result_t validate_compute_probe_job(const u32* job) {
    if (job[4] != 0x00040009u || job[10] == 0 ||
        job[16] != 0x01010007u || job[20] != 0 || job[21] != 0 ||
        job[30] == 0 || job[44] == 0) {
        uart_puts(&console, "[MALI_COMPUTE] INVALID compute descriptor: w4=0x");
        uart_put_hex(&console, job[4]);
        uart_puts(&console, " w10=0x");
        uart_put_hex(&console, job[10]);
        uart_puts(&console, " w16=0x");
        uart_put_hex(&console, job[16]);
        uart_puts(&console, " w20=0x");
        uart_put_hex(&console, job[20]);
        uart_puts(&console, " w30=0x");
        uart_put_hex(&console, job[30]);
        uart_puts(&console, " w44=0x");
        uart_put_hex(&console, job[44]);
        uart_puts(&console, "\r\n");
        return ERR_INVALID_PARAM;
    }
    return OK;
}

// Build a NULL job descriptor
// NULL jobs signal completion without computation - ideal for smoke testing
static void build_null_job(mali_job_desc_t* job) {
    // Zero out
    u8* p = (u8*)job;
    for (u32 i = 0; i < sizeof(mali_job_desc_t); i++) p[i] = 0;
    
    // Header fields. Midgard packs size+type in byte 0x10:
    //   bit 0    = job_descriptor_size (0 = 32-bit, 1 = 64-bit)
    //   bits 1-7 = job_type            (1 = NULL)
    // Our MMU AS is initialised in legacy LPAE mode (TRANSTAB | 0x17,
    // no AS_TRANSCFG on Midgard), so the JM walker uses 32-bit next-job
    // pointers => size=0.
    job->type_size  = (u8)((MALI_JOB_TYPE_NULL & 0x7F) << 1) | 0;
    job->job_index  = 1;                 // First job
    job->next_job_lo = 0;                // End of chain
    job->next_job_hi = 0;
    
    mali_clean_range(job, sizeof(*job));
}

static void build_compute_probe_job(u32* job, u64 scratch_addr, u64 state_addr, u64 ts_addr) {
    // 1. Clear the entire 192-byte aggregate
    for (u32 i = 0; i < CP_TOTAL_WORDS; i++) job[i] = 0;

    // 2. Header (Words 0-7)
    // w4: is_64b=1, type=4 (COMPUTE), index=4
    job[4] = 0x00040009;

    // 3. Invocation (Words 8-15)
    // Mesa v5.xml COMPUTE_JOB.INVOCATION is at offset 32 (w8), 2 words.
    // pan_pack_work_groups_compute(1,1,1, 1,1,1, false, false) packs:
    // invocations=0 and thread_group_split=workgroups_x_shift=0.
    job[8] = 0;
    job[9] = 0;
    // Mesa pan_jm.c packs COMPUTE_JOB.PARAMETERS at offset 40 (w10):
    // job_task_split = ceil_log2(size_x + 1) + ceil_log2(size_y + 1)
    //                + ceil_log2(size_z + 1) = 1 + 1 + 1 = 3.
    // v5.xml: Compute Job Parameters.Job Task Split starts at bit 26.
    job[10] = (3u << 26);

    uart_puts(&console, "[MALI_P3.1] invocation mesa_1x1x1 w7=0x");
    uart_put_hex(&console, job[7]);
    uart_puts(&console, " w8=0x");
    uart_put_hex(&console, job[8]);
    uart_puts(&console, " w9=0x");
    uart_put_hex(&console, job[9]);
    uart_puts(&console, " w10=0x");
    uart_put_hex(&console, job[10]);
    uart_puts(&console, " w11=0x");
    uart_put_hex(&console, job[11]);
    uart_puts(&console, "\r\n");

    // 4. Draw (Words 16-47)
    // w16: FourComp=1, DrawDesc64=1, Tex64=1, InstanceSize=1, PrimSize=1
    job[16] = 0x01010007;

    // Compute dispatch leaves Draw.Position unset; store target is embedded in shader r26.
    (void)scratch_addr;

    // w30/31: Draw.State (Renderer State)
    job[30] = (u32)(state_addr & 0xFFFFFFFF);
    job[31] = (u32)(state_addr >> 32);

    // w44/45: Draw.Thread Storage
    job[44] = (u32)(ts_addr & 0xFFFFFFFF);
    job[45] = (u32)(ts_addr >> 32);

    // Phase 3.1 v3: No SSBO descriptors — global store uses address in r26.

    // CRITICAL: flush ALL buffers read by GPU
    mali_clean_range(g_shader_binary, sizeof(g_shader_binary));
    if ((state_addr >> 32) == 0)
        mali_clean_range((void*)(uintptr_t)state_addr, 128);
    mali_clean_range((void*)(uintptr_t)ts_addr, 256);
    mali_clean_range(job, CP_TOTAL_WORDS * sizeof(u32));
}

static void build_cache_flush_job(mali_job_desc_t* job) {
    u8* p = (u8*)job;
    for (u32 i = 0; i < sizeof(mali_job_desc_t); i++) p[i] = 0;

    job->type_size  = (u8)((MALI_JOB_TYPE_CACHE_FLUSH & 0x7F) << 1) | 1;
    job->job_index  = 3;
    job->next_job_lo = 0;
    job->next_job_hi = 0;

    // Midgard v5 Cache Flush payload @0x20 (2 words):
    // w0 bits: [0] LS clean, [1] LS inval, [2] SC-other inval,
    //          [16] JM clean, [17] JM inval, [24] tiler clean, [25] tiler inval
    // w1 bits: [0] L2 clean, [1] L2 inval
    u32* payload32 = (u32*)&job->payload;
    payload32[0] = (1u << 0) | (1u << 1) | (1u << 2)
                 | (1u << 16) | (1u << 17)
                 | (1u << 24) | (1u << 25);
    payload32[1] = (1u << 0) | (1u << 1);

    mali_clean_range(job, sizeof(*job));
}

static void build_write_value_zero_job(mali_job_desc_t* job, u64 target_addr) {
    u8* p = (u8*)job;
    for (u32 i = 0; i < sizeof(mali_job_desc_t); i++) p[i] = 0;

    job->type_size  = (u8)((MALI_JOB_TYPE_WRITE_VALUE & 0x7F) << 1) | 1;
    job->job_index  = 2;
    job->next_job_lo = 0;
    job->next_job_hi = 0;

    u64* payload64 = (u64*)&job->payload;
    u32* payload32 = (u32*)&job->payload;
    payload64[0] = target_addr;
    payload32[2] = 3;
    payload32[3] = 0;

    mali_clean_range(job, sizeof(*job));
}

result_t mali_compute_submit_null(void) {
    // Build NULL job descriptor
    build_null_job(&g_null_job);
    
    u64 job_addr = (u64)(uintptr_t)&g_null_job;
    
    uart_puts(&console, "[MALI_COMPUTE] Submitting NULL job @0x");
    uart_put_hex(&console, job_addr);
    uart_puts(&console, "\r\n");

    mali_mmu_clear_irqs();
    mali_mmu_dump_va(job_addr);
    mali_dump_job_desc(&g_null_job);
    mali_mmu_dump(MALI_AS_DEFAULT);
    
    // Submit to compute slot (slot 2 on T860)
    result_t res = mali_jm_submit(MALI_JM_COMPUTE_SLOT, job_addr, 0);
    if (res != OK) {
        uart_puts(&console, "[MALI_COMPUTE] submit failed\r\n");
        return res;
    }

    mali_jm_dump_slot(MALI_JM_COMPUTE_SLOT);
    
    // Wait for completion (1 second timeout)
    res = mali_jm_wait(MALI_JM_COMPUTE_SLOT, 1000000);
    if (res != OK) {
        uart_puts(&console, "[MALI_COMPUTE] NULL job TIMEOUT/FAULT\r\n");
        mali_jm_dump_slot(MALI_JM_COMPUTE_SLOT);
        mali_jm_dump();
        mali_mmu_dump(MALI_AS_DEFAULT);
        return res;
    }
    
    uart_puts(&console, "[MALI_COMPUTE] NULL job COMPLETED OK\r\n");
    return OK;
}

result_t mali_compute_submit_write_value(void) {
    g_write_value_target = 0x11223344u;
    mali_clean_range((void*)&g_write_value_target, 64);

    u64 target_addr = (u64)(uintptr_t)&g_write_value_target;
    build_write_value_zero_job(&g_write_value_job, target_addr);

    u64 job_addr = (u64)(uintptr_t)&g_write_value_job;

    uart_puts(&console, "[MALI_COMPUTE] Submitting WRITE_VALUE zero job @0x");
    uart_put_hex(&console, job_addr);
    uart_puts(&console, " target=0x");
    uart_put_hex(&console, target_addr);
    uart_puts(&console, "\r\n");

    mali_mmu_clear_irqs();
    mali_mmu_dump_va(job_addr);
    mali_mmu_dump_va(target_addr);
    mali_dump_job_desc(&g_write_value_job);

    result_t res = mali_jm_submit(MALI_JM_COMPUTE_SLOT, job_addr, 0);
    if (res != OK) {
        uart_puts(&console, "[MALI_COMPUTE] WRITE_VALUE submit failed\r\n");
        return res;
    }

    mali_jm_dump_slot(MALI_JM_COMPUTE_SLOT);

    res = mali_jm_wait(MALI_JM_COMPUTE_SLOT, 1000000);
    if (res != OK) {
        uart_puts(&console, "[MALI_COMPUTE] WRITE_VALUE job TIMEOUT/FAULT\r\n");
        mali_jm_dump_slot(MALI_JM_COMPUTE_SLOT);
        mali_jm_dump();
        mali_mmu_dump(MALI_AS_DEFAULT);
        return res;
    }

    mali_invalidate_range((void*)&g_write_value_target, 64);

    uart_puts(&console, "[MALI_COMPUTE] WRITE_VALUE readback=0x");
    uart_put_hex(&console, g_write_value_target);
    uart_puts(&console, "\r\n");

    if (g_write_value_target != 0) {
        uart_puts(&console, "[MALI_COMPUTE] WRITE_VALUE side-effect FAILED\r\n");
        return ERR_HARDWARE_FAULT;
    }

    uart_puts(&console, "[MALI_COMPUTE] WRITE_VALUE side-effect OK\r\n");
    return OK;
}

result_t mali_compute_submit_cache_flush(void) {
    build_cache_flush_job(&g_cache_flush_job);

    u64 job_addr = (u64)(uintptr_t)&g_cache_flush_job;

    uart_puts(&console, "[MALI_COMPUTE] Submitting CACHE_FLUSH job @0x");
    uart_put_hex(&console, job_addr);
    uart_puts(&console, "\r\n");

    mali_mmu_clear_irqs();
    mali_mmu_dump_va(job_addr);
    mali_dump_job_desc(&g_cache_flush_job);

    result_t res = mali_jm_submit(MALI_JM_COMPUTE_SLOT, job_addr, 0);
    if (res != OK) {
        uart_puts(&console, "[MALI_COMPUTE] CACHE_FLUSH submit failed\r\n");
        return res;
    }

    mali_jm_dump_slot(MALI_JM_COMPUTE_SLOT);

    res = mali_jm_wait(MALI_JM_COMPUTE_SLOT, 1000000);
    if (res != OK) {
        uart_puts(&console, "[MALI_COMPUTE] CACHE_FLUSH job TIMEOUT/FAULT\r\n");
        mali_jm_dump_slot(MALI_JM_COMPUTE_SLOT);
        mali_jm_dump();
        mali_mmu_dump(MALI_AS_DEFAULT);
        return res;
    }

    uart_puts(&console, "[MALI_COMPUTE] CACHE_FLUSH completed OK\r\n");
    return OK;
}

result_t mali_compute_prepare_compute_probe(void) {
    // Phase 3.1 v3: pre-fill storage with 0xDEADBEEF sentinel so ANY write
    // (even zeros) from the shader is detectable via buffer change.
    for (u32 i = 0; i < 64; i++) g_compute_probe_storage[i] = 0xDEADBEEFu;
    mali_clean_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));
    for (u32 i = 0; i < 8; i++) g_compute_thread_storage[i] = 0;
    g_compute_thread_storage[1] = 0x1F;
    mali_clean_range(g_compute_thread_storage, sizeof(g_compute_thread_storage));

    // Build shader binary with target address = g_compute_probe_storage[0]
    u64 storage_addr = (u64)(uintptr_t)g_compute_probe_storage;
    build_st32_shader(g_shader_binary, (u32)storage_addr);
    mali_clean_range(g_shader_binary, sizeof(g_shader_binary));

    // Build Renderer State descriptor
    for (int i = 0; i < 16; i++) g_shader_desc[i] = 0;

    // word 0-1: shader binary address with TAG.
    // Phase 3.1 v3: TAG = TAG_ALU_8 = 0x9 (first bundle is ALU with constants)
    u64 shader_bin_addr = (u64)(uintptr_t)g_shader_binary;
    g_shader_desc[0] = (u32)((shader_bin_addr | TAG_ALU_8_) & 0xFFFFFFFF);
    g_shader_desc[1] = (u32)(shader_bin_addr >> 32);
    // word 3: Shader.Attribute count = 0, Shader.Varying count = 0
    g_shader_desc[3] = 0x00000000;
    // word 4: Renderer Properties.
    //   bits 16-20 = work_register_count = 4 (Mesa aligns Midgard compute
    //               work register pressure to at least 4)
    //   bit 13     = shader_has_side_effects = 1 (store instruction)
    g_shader_desc[4] = 0x00042000;  // work_reg_count=4, side_effects=1
    // word 12: Alpha reference (float) — 0.0 for compute, no prefetch field here.
    // Prefetch is handled by TAG chain in shader binary (bits 4-7 of each bundle).

    mali_clean_range(g_shader_desc, sizeof(g_shader_desc));

    u64 scratch_addr = storage_addr;
    u64 shader_desc_addr = (u64)(uintptr_t)g_shader_desc;
    u64 thread_storage_addr = (u64)(uintptr_t)g_compute_thread_storage;

    build_compute_probe_job(g_compute_probe_job, scratch_addr, shader_desc_addr, thread_storage_addr);

    uart_puts(&console, "[MALI_P3.1v3] shader_bin=0x");
    uart_put_hex(&console, shader_bin_addr);
    uart_puts(&console, " target=0x");
    uart_put_hex(&console, storage_addr);
    uart_puts(&console, "\r\n");

    mali_dump_u32_words("[MALI_SHADER_BIN]", g_shader_binary, SHADER_WORDS);
    mali_dump_u32_words("[MALI_RENDERER_STATE]", g_shader_desc, 16);
    mali_dump_u32_words("[MALI_THREAD_STORAGE]", g_compute_thread_storage, 8);
    uart_puts(&console, "[AUDIT] compute w4=0x");
    uart_put_hex(&console, g_compute_probe_job[4]);
    uart_puts(&console, " w8=0x");
    uart_put_hex(&console, g_compute_probe_job[8]);
    uart_puts(&console, " w9=0x");
    uart_put_hex(&console, g_compute_probe_job[9]);
    uart_puts(&console, " w10=0x");
    uart_put_hex(&console, g_compute_probe_job[10]);
    uart_puts(&console, " w16=0x");
    uart_put_hex(&console, g_compute_probe_job[16]);
    uart_puts(&console, " w20=0x");
    uart_put_hex(&console, g_compute_probe_job[20]);
    uart_puts(&console, " w30=0x");
    uart_put_hex(&console, g_compute_probe_job[30]);
    uart_puts(&console, " w44=0x");
    uart_put_hex(&console, g_compute_probe_job[44]);
    uart_puts(&console, " ts1=0x");
    uart_put_hex(&console, g_compute_thread_storage[1]);
    uart_puts(&console, " rsd4=0x");
    uart_put_hex(&console, g_shader_desc[4]);
    uart_puts(&console, "\r\n");

    uart_puts(&console, "[MALI_P3.1v3] Prepared COMPUTE probe @0x");
    uart_put_hex(&console, (u64)(uintptr_t)g_compute_probe_job);
    uart_puts(&console, "\r\n");

    mali_mmu_dump_va((u64)(uintptr_t)g_compute_probe_job);
    mali_mmu_dump_va(storage_addr);
    mali_mmu_dump_va(shader_desc_addr);
    mali_mmu_dump_va(shader_bin_addr);
    mali_mmu_dump_va(thread_storage_addr);
    return OK;
}

result_t mali_compute_submit_compute_probe(void) {
    u64 job_addr = (u64)(uintptr_t)g_compute_probe_job;
    mali_jm_wait_diag_t wait_diag = {0, 0, 0, 0, 0};
    result_t res;

    uart_puts(&console, "[MALI_COMPUTE] Submitting COMPUTE probe job @0x");
    uart_put_hex(&console, job_addr);
    uart_puts(&console, "\r\n");

    // ====================================================================
    // BCN1: MEMATTR readback — verify GPU memory attribute configuration.
    // kbase uses 0x48484848 (impl-def cacheable). We set 0xFF (WB alloc).
    // If hardware clamps 0xFF → 0xCF, stores may go to wrong cache domain.
    // ====================================================================
    {
        u32 as0_base = (MALI_MMU_OFFSET + 0x400); // AS0 offset from MALI_BASE
        u32 memattr_lo = mali_reg_read(as0_base + AS_MEMATTR_LO);
        u32 memattr_hi = mali_reg_read(as0_base + AS_MEMATTR_HI);
        uart_puts(&console, "[BCN1] AS0_MEMATTR lo=0x");
        uart_put_hex(&console, memattr_lo);
        uart_puts(&console, " hi=0x");
        uart_put_hex(&console, memattr_hi);
        uart_puts(&console, "\r\n");
    }

    // ====================================================================
    // BCN2-pre: GPU cycle counter BEFORE compute job.
    // If delta==0 after job, shader cores did zero work.
    // ====================================================================
    u32 cyc_lo_pre = mali_reg_read(GPU_CYCLE_COUNT_LO);
    u32 cyc_hi_pre = mali_reg_read(GPU_CYCLE_COUNT_HI);
    uart_puts(&console, "[BCN2] pre-compute GPU_CYCLE=0x");
    uart_put_hex(&console, cyc_hi_pre);
    uart_put_hex(&console, cyc_lo_pre);
    uart_puts(&console, "\r\n");

    // ====================================================================
    // BCN3: Shader binary readback — verify DRAM content after cache clean.
    // If the binary in DRAM is zeroed/wrong, GPU fetches garbage.
    // ====================================================================
    {
        mali_invalidate_range(g_shader_binary, sizeof(g_shader_binary));
        asm volatile("dsb sy" ::: "memory");
        uart_puts(&console, "[BCN3] shader DRAM readback w0-3: 0x");
        uart_put_hex(&console, g_shader_binary[0]);
        uart_puts(&console, " 0x");
        uart_put_hex(&console, g_shader_binary[1]);
        uart_puts(&console, " 0x");
        uart_put_hex(&console, g_shader_binary[2]);
        uart_puts(&console, " 0x");
        uart_put_hex(&console, g_shader_binary[3]);
        uart_puts(&console, "\r\n");
        // Also verify the LDST bundle (words 8-11)
        uart_puts(&console, "[BCN3] shader DRAM readback w8-11: 0x");
        uart_put_hex(&console, g_shader_binary[8]);
        uart_puts(&console, " 0x");
        uart_put_hex(&console, g_shader_binary[9]);
        uart_puts(&console, " 0x");
        uart_put_hex(&console, g_shader_binary[10]);
        uart_puts(&console, " 0x");
        uart_put_hex(&console, g_shader_binary[11]);
        uart_puts(&console, "\r\n");
        // Also RSD readback
        mali_invalidate_range(g_shader_desc, sizeof(g_shader_desc));
        asm volatile("dsb sy" ::: "memory");
        uart_puts(&console, "[BCN3] RSD DRAM readback w0=0x");
        uart_put_hex(&console, g_shader_desc[0]);
        uart_puts(&console, " w1=0x");
        uart_put_hex(&console, g_shader_desc[1]);
        uart_puts(&console, " w4=0x");
        uart_put_hex(&console, g_shader_desc[4]);
        uart_puts(&console, "\r\n");
    }

    // ====================================================================
    // BCN6: Pre-compute WRITE_VALUE — write 0x42424242 to storage[0].
    // If compute overwrites → shader store works. If not → shader didn't write.
    // This also proves the address is writable BEFORE compute runs.
    // ====================================================================
    {
        // Build a SET_VALUE job to write 0x42424242 to storage[0]
        u8* p = (u8*)&g_write_value_job;
        for (u32 i = 0; i < sizeof(mali_job_desc_t); i++) p[i] = 0;
        g_write_value_job.type_size = (u8)((MALI_JOB_TYPE_WRITE_VALUE & 0x7F) << 1) | 1;
        g_write_value_job.job_index = 4;
        u64* payload64 = (u64*)&g_write_value_job.payload;
        u32* payload32 = (u32*)&g_write_value_job.payload;
        payload64[0] = (u64)(uintptr_t)g_compute_probe_storage;
        payload32[2] = 3; // type = immediate_32
        payload32[3] = 0x42424242u; // value
        mali_clean_range(&g_write_value_job, sizeof(g_write_value_job));

        mali_mmu_clear_irqs();
        u64 wv_addr = (u64)(uintptr_t)&g_write_value_job;
        result_t wv_res = mali_jm_submit(MALI_JM_COMPUTE_SLOT, wv_addr, 0);
        if (wv_res == OK) wv_res = mali_jm_wait(MALI_JM_COMPUTE_SLOT, 100000);
        mali_invalidate_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));
        asm volatile("dsb sy" ::: "memory");
        uart_puts(&console, "[BCN6] pre-compute WRITE_VALUE(0x42424242) ");
        uart_puts(&console, wv_res == OK ? "OK" : "FAIL");
        uart_puts(&console, " [0]=0x");
        uart_put_hex(&console, g_compute_probe_storage[0]);
        uart_puts(&console, "\r\n");
        for (u32 i = 0; i < 64; i++) g_compute_probe_storage[i] = 0xDEADBEEFu;
        mali_clean_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));
        uart_puts(&console, "[BCN6] reset probe storage to DEADBEEF before compute submit\r\n");
    }

    mali_mmu_clear_irqs();
    mali_mmu_dump_va(job_addr);
    mali_dump_compute_probe(g_compute_probe_job);

    res = validate_compute_probe_job(g_compute_probe_job);
    if (res != OK) return res;

    res = mali_jm_submit(MALI_JM_COMPUTE_SLOT, job_addr, 0);
    if (res != OK) {
        uart_puts(&console, "[MALI_COMPUTE] COMPUTE probe submit failed\r\n");
        return res;
    }

    mali_jm_dump_slot(MALI_JM_COMPUTE_SLOT);

    res = mali_jm_wait_ex(MALI_JM_COMPUTE_SLOT, 1000000, &wait_diag);
    if (res == OK) {
        // ================================================================
        // BCN2-post: GPU cycle counter AFTER compute job.
        // ================================================================
        u32 cyc_lo_post = mali_reg_read(GPU_CYCLE_COUNT_LO);
        u32 cyc_hi_post = mali_reg_read(GPU_CYCLE_COUNT_HI);
        u64 cyc_pre  = ((u64)cyc_hi_pre << 32) | cyc_lo_pre;
        u64 cyc_post = ((u64)cyc_hi_post << 32) | cyc_lo_post;
        u64 cyc_delta = cyc_post - cyc_pre;
        uart_puts(&console, "[BCN2] post-compute GPU_CYCLE=0x");
        uart_put_hex(&console, cyc_hi_post);
        uart_put_hex(&console, cyc_lo_post);
        uart_puts(&console, " delta=0x");
        uart_put_hex(&console, (u32)cyc_delta);
        uart_puts(&console, "\r\n");

        // ================================================================
        // BCN4: Full job header writeback (words 0-7).
        // JM writes exception_status, fault_addr, etc. to words 0-3.
        // ================================================================
        mali_invalidate_range(g_compute_probe_job, sizeof(g_compute_probe_job));
        asm volatile("dsb sy" ::: "memory");
        uart_puts(&console, "[BCN4] job header wb:");
        for (u32 k = 0; k < 8; k++) {
            uart_puts(&console, " w");
            uart_put_hex(&console, k);
            uart_puts(&console, "=0x");
            uart_put_hex(&console, g_compute_probe_job[k]);
        }
        uart_puts(&console, "\r\n");

        mali_dump_job_desc((const mali_job_desc_t*)g_compute_probe_job);

        uart_puts(&console, "[MALI_COMPUTE] JM wait diag done=");
        uart_put_hex(&console, wait_diag.saw_done);
        uart_puts(&console, " fail=");
        uart_put_hex(&console, wait_diag.saw_fail);
        uart_puts(&console, " js=0x");
        uart_put_hex(&console, wait_diag.js_status);
        uart_puts(&console, " irq_raw=0x");
        uart_put_hex(&console, wait_diag.irq_raw);
        uart_puts(&console, "\r\n");

        u32 exc = g_compute_probe_job[0];
        if (wait_diag.saw_done == 0 || wait_diag.saw_fail != 0 ||
            wait_diag.js_status != 0 ||
            (exc != 0 && exc != 1)) {
            uart_puts(&console, "[MALI_COMPUTE] COMPUTE probe completion inconsistent\r\n");
            mali_jm_dump_slot(MALI_JM_COMPUTE_SLOT);
            mali_jm_dump();
            mali_mmu_dump(MALI_AS_DEFAULT);
            return ERR_HARDWARE_FAULT;
        }

        uart_puts(&console, "[MALI_COMPUTE] COMPUTE probe completed OK\r\n");

        // flush GPU L2 cache before CPU readback
        {
            build_cache_flush_job(&g_cache_flush_job);
            u64 cf_addr = (u64)(uintptr_t)&g_cache_flush_job;
            mali_mmu_clear_irqs();
            result_t cf_res = mali_jm_submit(MALI_JM_COMPUTE_SLOT, cf_addr, 0);
            if (cf_res == OK) {
                cf_res = mali_jm_wait(MALI_JM_COMPUTE_SLOT, 100000);
            }
            uart_puts(&console, "[MALI_P3.1] post-compute L2 flush ");
            uart_puts(&console, cf_res == OK ? "OK\r\n" : "FAIL\r\n");
        }

        // readback verification
        mali_invalidate_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));
        asm volatile("dsb sy" ::: "memory");

        u32 changed = 0;
        uart_puts(&console, "[MALI_P3.1] storage readback:");
        for (u32 k = 0; k < 8; k++) {
            uart_puts(&console, " [");
            uart_put_hex(&console, k);
            uart_puts(&console, "]=0x");
            uart_put_hex(&console, g_compute_probe_storage[k]);
            if (g_compute_probe_storage[k] != 0x42424242u) changed |= (1u << k);
        }
        uart_puts(&console, "\r\n");

        if (g_compute_probe_storage[0] == 0x00000000u) {
            uart_puts(&console, "[MALI_P3.1] *** SHADER_STORE_OK *** [0]=0x00000000\r\n");
            return OK;
        }

        if (g_compute_probe_storage[0] == 0x42424242u) {
            uart_puts(&console, "[MALI_P3.1] STILL_0x42424242 — shader did NOT overwrite WRITE_VALUE\r\n");
        } else if (g_compute_probe_storage[0] == 0xDEADBEEFu) {
            uart_puts(&console, "[MALI_P3.1] STILL_DEADBEEF — neither WRITE_VALUE nor shader wrote\r\n");
        } else {
            uart_puts(&console, "[MALI_P3.1] UNEXPECTED value at [0] — partial write?\r\n");
        }

        {
            uart_puts(&console, "[SLOTPROBE] retry same compute job on JS2\r\n");
            for (u32 i = 0; i < 64; i++) g_compute_probe_storage[i] = 0xDEADBEEFu;
            mali_clean_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));

            u64 sa = (u64)(uintptr_t)g_compute_probe_storage;
            u64 sd = (u64)(uintptr_t)g_shader_desc;
            u64 ts = (u64)(uintptr_t)g_compute_thread_storage;
            build_compute_probe_job(g_compute_probe_job, sa, sd, ts);

            mali_mmu_clear_irqs();
            mali_jm_wait_diag_t wd2 = {0};
            result_t r2 = mali_jm_submit(2, (u64)(uintptr_t)g_compute_probe_job, 0);
            if (r2 == OK) r2 = mali_jm_wait_ex(2, 1000000, &wd2);
            uart_puts(&console, "[SLOTPROBE] JS2 result=");
            uart_puts(&console, r2 == OK ? "OK" : "FAIL");
            uart_puts(&console, " done=");
            uart_put_hex(&console, wd2.saw_done);
            uart_puts(&console, " fail=");
            uart_put_hex(&console, wd2.saw_fail);
            uart_puts(&console, " js=0x");
            uart_put_hex(&console, wd2.js_status);
            uart_puts(&console, "\r\n");

            build_cache_flush_job(&g_cache_flush_job);
            u64 cf_addr = (u64)(uintptr_t)&g_cache_flush_job;
            mali_mmu_clear_irqs();
            result_t cf_res = mali_jm_submit(2, cf_addr, 0);
            if (cf_res == OK) cf_res = mali_jm_wait(2, 100000);
            uart_puts(&console, "[SLOTPROBE] JS2 L2 flush ");
            uart_puts(&console, cf_res == OK ? "OK\r\n" : "FAIL\r\n");

            mali_invalidate_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));
            asm volatile("dsb sy" ::: "memory");
            uart_puts(&console, "[SLOTPROBE] JS2 storage[0]=0x");
            uart_put_hex(&console, g_compute_probe_storage[0]);
            uart_puts(&console, "\r\n");
        }

        // ================================================================
        // BCN5: RSD perturbation — re-run with work_register_count=0.
        // If same result → GPU ignores RSD → Draw.State pointer is wrong.
        // If different (fault) → GPU reads RSD → RSD content is the issue.
        // ================================================================
        {
            uart_puts(&console, "[BCN5] RSD perturbation: setting work_reg_count=0\r\n");
            u32 saved_rsd_w4 = g_shader_desc[4];
            g_shader_desc[4] = 0x00002000; // side_effects=1 but work_reg_count=0
            mali_clean_range(g_shader_desc, sizeof(g_shader_desc));

            // Re-fill storage with fresh sentinel
            for (u32 i = 0; i < 8; i++) g_compute_probe_storage[i] = 0xAAAAAAAAu;
            mali_clean_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));

            // Rebuild job descriptor (reuses same pointers)
            u64 sa = (u64)(uintptr_t)g_compute_probe_storage;
            u64 sd = (u64)(uintptr_t)g_shader_desc;
            u64 ts = (u64)(uintptr_t)g_compute_thread_storage;
            build_compute_probe_job(g_compute_probe_job, sa, sd, ts);

            mali_mmu_clear_irqs();
            mali_jm_wait_diag_t wd5 = {0};
            result_t r5 = mali_jm_submit(MALI_JM_COMPUTE_SLOT, (u64)(uintptr_t)g_compute_probe_job, 0);
            if (r5 == OK) r5 = mali_jm_wait_ex(MALI_JM_COMPUTE_SLOT, 1000000, &wd5);
            uart_puts(&console, "[BCN5] wreg=0 result=");
            uart_puts(&console, r5 == OK ? "OK" : "FAIL");
            uart_puts(&console, " done=");
            uart_put_hex(&console, wd5.saw_done);
            uart_puts(&console, " fail=");
            uart_put_hex(&console, wd5.saw_fail);
            uart_puts(&console, " js=0x");
            uart_put_hex(&console, wd5.js_status);
            uart_puts(&console, "\r\n");

            // Readback
            mali_invalidate_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));
            asm volatile("dsb sy" ::: "memory");
            uart_puts(&console, "[BCN5] storage[0]=0x");
            uart_put_hex(&console, g_compute_probe_storage[0]);
            uart_puts(&console, "\r\n");

            // Restore original RSD
            g_shader_desc[4] = saved_rsd_w4;
            mali_clean_range(g_shader_desc, sizeof(g_shader_desc));
        }

        {
            uart_puts(&console, "[BCN7] invalid Draw.State fault-probe: state=0x0000000100000000\r\n");
            for (u32 i = 0; i < 8; i++) g_compute_probe_storage[i] = 0xBBBBBBBBu;
            mali_clean_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));

            u64 sa = (u64)(uintptr_t)g_compute_probe_storage;
            u64 ts = (u64)(uintptr_t)g_compute_thread_storage;
            build_compute_probe_job(g_compute_probe_job, sa, 0x0000000100000000ULL, ts);

            mali_mmu_clear_irqs();
            mali_jm_wait_diag_t wd7 = {0};
            result_t r7 = mali_jm_submit(MALI_JM_COMPUTE_SLOT, (u64)(uintptr_t)g_compute_probe_job, 0);
            if (r7 == OK) r7 = mali_jm_wait_ex(MALI_JM_COMPUTE_SLOT, 1000000, &wd7);

            mali_invalidate_range(g_compute_probe_job, sizeof(g_compute_probe_job));
            asm volatile("dsb sy" ::: "memory");
            uart_puts(&console, "[BCN7] invalid_state result=");
            uart_puts(&console, r7 == OK ? "OK" : "FAIL");
            uart_puts(&console, " done=");
            uart_put_hex(&console, wd7.saw_done);
            uart_puts(&console, " fail=");
            uart_put_hex(&console, wd7.saw_fail);
            uart_puts(&console, " js=0x");
            uart_put_hex(&console, wd7.js_status);
            uart_puts(&console, " exc=0x");
            uart_put_hex(&console, g_compute_probe_job[0]);
            uart_puts(&console, " fic=0x");
            uart_put_hex(&console, g_compute_probe_job[1]);
            uart_puts(&console, " fp=0x");
            uart_put_hex(&console, g_compute_probe_job[3]);
            uart_put_hex(&console, g_compute_probe_job[2]);
            uart_puts(&console, "\r\n");
            mali_mmu_dump(MALI_AS_DEFAULT);
        }

        {
            uart_puts(&console, "[BCN8] invalid RSD.shader fault-probe: shader=0x0000000100000000\r\n");
            u32 saved_rsd_w0 = g_shader_desc[0];
            u32 saved_rsd_w1 = g_shader_desc[1];
            g_shader_desc[0] = (u32)((0x0000000100000000ULL | TAG_ALU_8_) & 0xFFFFFFFFu);
            g_shader_desc[1] = (u32)(0x0000000100000000ULL >> 32);
            mali_clean_range(g_shader_desc, sizeof(g_shader_desc));

            for (u32 i = 0; i < 8; i++) g_compute_probe_storage[i] = 0xCCCCCCCCu;
            mali_clean_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));

            u64 sa = (u64)(uintptr_t)g_compute_probe_storage;
            u64 sd = (u64)(uintptr_t)g_shader_desc;
            u64 ts = (u64)(uintptr_t)g_compute_thread_storage;
            build_compute_probe_job(g_compute_probe_job, sa, sd, ts);

            mali_mmu_clear_irqs();
            mali_jm_wait_diag_t wd8 = {0};
            result_t r8 = mali_jm_submit(MALI_JM_COMPUTE_SLOT, (u64)(uintptr_t)g_compute_probe_job, 0);
            if (r8 == OK) r8 = mali_jm_wait_ex(MALI_JM_COMPUTE_SLOT, 1000000, &wd8);

            mali_invalidate_range(g_compute_probe_job, sizeof(g_compute_probe_job));
            asm volatile("dsb sy" ::: "memory");
            uart_puts(&console, "[BCN8] invalid_shader result=");
            uart_puts(&console, r8 == OK ? "OK" : "FAIL");
            uart_puts(&console, " done=");
            uart_put_hex(&console, wd8.saw_done);
            uart_puts(&console, " fail=");
            uart_put_hex(&console, wd8.saw_fail);
            uart_puts(&console, " js=0x");
            uart_put_hex(&console, wd8.js_status);
            uart_puts(&console, " exc=0x");
            uart_put_hex(&console, g_compute_probe_job[0]);
            uart_puts(&console, " fic=0x");
            uart_put_hex(&console, g_compute_probe_job[1]);
            uart_puts(&console, " fp=0x");
            uart_put_hex(&console, g_compute_probe_job[3]);
            uart_put_hex(&console, g_compute_probe_job[2]);
            uart_puts(&console, "\r\n");
            mali_mmu_dump(MALI_AS_DEFAULT);

            g_shader_desc[0] = saved_rsd_w0;
            g_shader_desc[1] = saved_rsd_w1;
            mali_clean_range(g_shader_desc, sizeof(g_shader_desc));
        }

        {
            uart_puts(&console, "[BCN9] invalid store-target fault-probe: target=0x0000000100000000\r\n");
            build_st32_shader(g_shader_binary, 0);
            g_shader_binary[5] = 1;
            mali_clean_range(g_shader_binary, sizeof(g_shader_binary));

            g_shader_desc[0] = (u32)(((u64)(uintptr_t)g_shader_binary | TAG_ALU_8_) & 0xFFFFFFFFu);
            g_shader_desc[1] = (u32)(((u64)(uintptr_t)g_shader_binary) >> 32);
            mali_clean_range(g_shader_desc, sizeof(g_shader_desc));

            for (u32 i = 0; i < 8; i++) g_compute_probe_storage[i] = 0xDDDDDDDDu;
            mali_clean_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));

            u64 sa = (u64)(uintptr_t)g_compute_probe_storage;
            u64 sd = (u64)(uintptr_t)g_shader_desc;
            u64 ts = (u64)(uintptr_t)g_compute_thread_storage;
            build_compute_probe_job(g_compute_probe_job, sa, sd, ts);

            mali_mmu_clear_irqs();
            mali_jm_wait_diag_t wd9 = {0};
            result_t r9 = mali_jm_submit(MALI_JM_COMPUTE_SLOT, (u64)(uintptr_t)g_compute_probe_job, 0);
            if (r9 == OK) r9 = mali_jm_wait_ex(MALI_JM_COMPUTE_SLOT, 1000000, &wd9);

            mali_invalidate_range(g_compute_probe_job, sizeof(g_compute_probe_job));
            asm volatile("dsb sy" ::: "memory");
            uart_puts(&console, "[BCN9] invalid_store result=");
            uart_puts(&console, r9 == OK ? "OK" : "FAIL");
            uart_puts(&console, " done=");
            uart_put_hex(&console, wd9.saw_done);
            uart_puts(&console, " fail=");
            uart_put_hex(&console, wd9.saw_fail);
            uart_puts(&console, " js=0x");
            uart_put_hex(&console, wd9.js_status);
            uart_puts(&console, " exc=0x");
            uart_put_hex(&console, g_compute_probe_job[0]);
            uart_puts(&console, " fic=0x");
            uart_put_hex(&console, g_compute_probe_job[1]);
            uart_puts(&console, " fp=0x");
            uart_put_hex(&console, g_compute_probe_job[3]);
            uart_put_hex(&console, g_compute_probe_job[2]);
            uart_puts(&console, "\r\n");
            mali_mmu_dump(MALI_AS_DEFAULT);

            build_st32_shader(g_shader_binary, (u32)(uintptr_t)g_compute_probe_storage);
            mali_clean_range(g_shader_binary, sizeof(g_shader_binary));
        }

        // Post-fail control WRITE_VALUE to prove address is still writable
        {
            build_write_value_zero_job(&g_write_value_job, (u64)(uintptr_t)g_compute_probe_storage);
            u64 wv_addr = (u64)(uintptr_t)&g_write_value_job;
            mali_mmu_clear_irqs();
            result_t wv_res = mali_jm_submit(MALI_JM_COMPUTE_SLOT, wv_addr, 0);
            if (wv_res == OK) {
                wv_res = mali_jm_wait(MALI_JM_COMPUTE_SLOT, 100000);
            }
            mali_invalidate_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));
            uart_puts(&console, "[MALI_P3.1] control WRITE_VALUE(0) ");
            uart_puts(&console, wv_res == OK ? "OK" : "FAIL");
            uart_puts(&console, " [0]=0x");
            uart_put_hex(&console, g_compute_probe_storage[0]);
            uart_puts(&console, "\r\n");
        }

        return ERR_HARDWARE_FAULT;
    }

    uart_puts(&console, "[MALI_COMPUTE] COMPUTE probe fault/timeout\r\n");
    mali_invalidate_range(g_compute_probe_job, sizeof(g_compute_probe_job));
    // BCN4 on fault path too
    uart_puts(&console, "[BCN4] job header on fault:");
    for (u32 k = 0; k < 8; k++) {
        uart_puts(&console, " w");
        uart_put_hex(&console, k);
        uart_puts(&console, "=0x");
        uart_put_hex(&console, g_compute_probe_job[k]);
    }
    uart_puts(&console, "\r\n");

    u64 fault_ptr = ((u64)g_compute_probe_job[3] << 32) | g_compute_probe_job[2];
    u64 shader_ptr = (u64)(uintptr_t)g_shader_binary;
    uart_puts(&console, "[MALI_COMPUTE] fault_ptr=0x");
    uart_put_hex(&console, fault_ptr);
    uart_puts(&console, " shader=0x");
    uart_put_hex(&console, shader_ptr);
    uart_puts(&console, " shader_off=0x");
    uart_put_hex(&console, (fault_ptr >= shader_ptr) ? (fault_ptr - shader_ptr) : 0xFFFFFFFFu);
    uart_puts(&console, "\r\n");
    mali_dump_job_desc((const mali_job_desc_t*)g_compute_probe_job);
    mali_jm_dump_slot(MALI_JM_COMPUTE_SLOT);
    mali_jm_dump();
    mali_mmu_dump(MALI_AS_DEFAULT);

    mali_invalidate_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));
    asm volatile("dsb sy" ::: "memory");
    uart_puts(&console, "[MALI_P3.1] storage after fault:");
    for (u32 k = 0; k < 8; k++) {
        uart_puts(&console, " [");
        uart_put_hex(&console, k);
        uart_puts(&console, "]=0x");
        uart_put_hex(&console, g_compute_probe_storage[k]);
    }
    uart_puts(&console, "\r\n");

    return res;
}

result_t mali_infer_single(const telemetry_t* in, inference_result_t* out) {
    if (!in || !out) return ERR_INVALID_PARAM;

    result_t res = mali_compute_prepare_compute_probe();
    if (res != OK) return res;

    res = mali_compute_submit_compute_probe();
    if (res != OK) return res;

    mali_invalidate_range(g_compute_probe_storage, sizeof(g_compute_probe_storage));

    u32 v0 = g_compute_probe_storage[0];
    u32 v1 = g_compute_probe_storage[1];
    u32 v2 = g_compute_probe_storage[2];
    u32 v3 = g_compute_probe_storage[3];

    // Phase 3.1: sentinel is 0xDEADBEEF; if all words still match sentinel,
    // shader store had no effect — fall back to CPU heuristic.
    if (v0 == 0xDEADBEEFu && v1 == 0xDEADBEEFu &&
        v2 == 0xDEADBEEFu && v3 == 0xDEADBEEFu) {
        out->task_priority = clamp_u8(in->cpu_load);
        out->migration_hint = (u8)((in->l2_latency_us > 120u) ? 1u : 0u);
        out->power_state = (u8)((in->thermal_state > 80u) ? 1u : 2u);
        out->trust_score = clamp_u8((in->memory_pressure >= 100u) ? 0u : (255u - (in->memory_pressure * 2u)));
    } else {
        out->task_priority = (u8)(v0 & 0xFFu);
        out->migration_hint = (u8)(v1 % 3u);
        out->power_state = (u8)(v2 % 4u);
        out->trust_score = (u8)(v3 & 0xFFu);
    }

    return OK;
}

result_t mali_compute_smoke_test(void) {
    uart_puts(&console, "[MALI_COMPUTE] === Smoke Test ===\r\n");
    
    // Verify GPU ID is sane
    u32 gpu_id = mali_get_gpu_id();
    if (gpu_id == 0 || gpu_id == 0xFFFFFFFF) {
        uart_puts(&console, "[MALI_COMPUTE] GPU not powered\r\n");
        return ERR_HARDWARE_FAULT;
    }
    
    // Verify shader cores are ready
    u32 shaders_ready = mali_get_shaders_ready();
    uart_puts(&console, "[MALI_COMPUTE] shaders_ready=0x");
    uart_put_hex(&console, shaders_ready);
    uart_puts(&console, "\r\n");
    
    if (shaders_ready == 0) {
        uart_puts(&console, "[MALI_COMPUTE] no shaders ready\r\n");
        return ERR_HARDWARE_FAULT;
    }
    
    result_t res = mali_compute_submit_null();
    if (res != OK) return res;

    res = mali_compute_submit_write_value();
    if (res != OK) return res;

    res = mali_compute_submit_cache_flush();
    if (res != OK) return res;

    res = mali_compute_prepare_compute_probe();
    if (res != OK) return res;

    res = mali_compute_submit_compute_probe();
    if (res != OK) return res;

    // Print bit-level diagnostic dump of new v2 probe path
    mali_compute_dump_probe_v2();

    return OK;
}

/* =========================================================================
 *  H-Exo Racing Compute Driver — Midgard T860 bare-metal primitives
 *  Zero-latency path: pre-built descriptors, job chaining, ping-pong buffers
 * ========================================================================= */

// --- Mesa-correct invocation packing (from panfrost/lib/pan_encoder.h) ---
static inline u32 util_logbase2_ceil(u32 v) {
    if (v <= 1) return 0;
    u32 r = 0, x = v - 1;
    while (x) { x >>= 1; r++; }
    return r;
}

static inline void pan_pack_invocation(u32* w8, u32* w9,
                                       u32 num_x, u32 num_y, u32 num_z,
                                       u32 size_x, u32 size_y, u32 size_z) {
    u32 values[6] = {size_x, size_y, size_z, num_x, num_y, num_z};
    u32 shifts[7] = {0};
    u32 packed = 0;
    for (u32 i = 0; i < 6; i++) {
        packed |= ((values[i] - 1) << shifts[i]);
        shifts[i + 1] = shifts[i] + util_logbase2_ceil(values[i]);
    }
    *w8 = packed;
    *w9 = (shifts[1] & 0x1F)
        | ((shifts[2] & 0x1F) << 5)
        | ((shifts[3] & 0x3F) << 10)
        | ((shifts[4] & 0x3F) << 16)
        | ((shifts[5] & 0x3F) << 22)
        | ((shifts[3] & 0xF) << 28); // thread_group_split = workgroups_x_shift
}

// --- Shader binary builder ---
typedef struct {
    u32* words;
    u32 capacity;
    u32 count;
} mali_shader_builder_t;

static inline void sb_init(mali_shader_builder_t* sb, u32* buf, u32 words) {
    sb->words = buf;
    sb->capacity = words;
    sb->count = 0;
    for (u32 i = 0; i < words; i++) buf[i] = 0;
}

static inline u32 sb_used(const mali_shader_builder_t* sb) { return sb->count; }

// Append raw bundle words; returns index of first word or 0xFFFFFFFF on overflow
static inline u32 sb_append(mali_shader_builder_t* sb, const u32* w, u32 n) {
    if (sb->count + n > sb->capacity) return 0xFFFFFFFFu;
    u32 idx = sb->count;
    for (u32 i = 0; i < n; i++) sb->words[sb->count++] = w[i];
    return idx;
}

// ALU imov from embedded constant[const_idx] into dest_reg (vector, mask bits)
static inline void sb_alu_imov_const(mali_shader_builder_t* sb, u32 dest_reg,
                                      u32 const_idx, u32 mask) {
    u32 next_tag = TAG_ALU_4_; // assume next is another ALU or LS
    u32 ctrl = TAG_ALU_8_ | (next_tag << 4) | ALU_ENAB_VEC_ADD;
    // NOTE: imov reads from src2, so constant register must be in src2 position
    u16 reg = pack_reg_info(REG_UNUSED, REG_CONSTANT, dest_reg, 0);
    u32 src = pack_vec_src(0, 0, SWIZZLE_IDENTITY);
    u32 lo, hi;
    pack_vector_alu(MIDG_OP_IMOV, MIDG_REG_MODE_32, src, src,
                    MIDG_SHRINK_NONE, MIDG_OUTMOD_KEEPLO, mask, &lo, &hi);
    u32 w[8] = {ctrl, reg | ((lo & 0xFFFF) << 16),
                (lo >> 16) | (hi << 16), 0,
                0, 0, 0, 0};
    w[4 + const_idx] = 0; // constant slot filled by caller after builder
    sb_append(sb, w, 8);
}

// ALU imov inline immediate into dest_reg
static inline void sb_alu_imov_imm(mali_shader_builder_t* sb, u32 dest_reg,
                                    u16 imm, u32 mask) {
    u32 next_tag = TAG_LOAD_STORE_4_;
    u32 ctrl = TAG_ALU_4_ | (next_tag << 4) | ALU_ENAB_VEC_ADD;
    // src2_imm=1, src2_reg = imm >> 11, inline constant in ALU body
    u16 reg = pack_reg_info(REG_UNUSED, (u32)(imm >> 11), dest_reg, 1);
    u32 src = pack_vec_src(0, 0, SWIZZLE_IDENTITY);
    u32 lo, hi;
    pack_vector_alu(MIDG_OP_IMOV, MIDG_REG_MODE_32, src, 0,
                    MIDG_SHRINK_NONE, MIDG_OUTMOD_KEEPLO, mask, &lo, &hi);
    u32 w[4] = {ctrl, reg | ((lo & 0xFFFF) << 16),
                (lo >> 16) | (hi << 16), 0};
    sb_append(sb, w, 4);
}

// Load-store st_32: store data_reg (32-bit) to [addr_reg + offset]
static inline void sb_ldst_st_32(mali_shader_builder_t* sb, u32 data_reg,
                                  u32 addr_reg, u32 offset) {
    u64 w1 = (u64)MIDG_OP_ST_32
           | ((u64)data_reg << 8)
           | ((u64)0x0F << 13)
           | ((u64)0x00 << 17)
           | ((u64)0 << 25)
           | ((u64)addr_reg << 27)
           | ((u64)1 << 30)
           | ((u64)1 << 31)
           | ((u64)0 << 33)
           | ((u64)REG_LDST_ZERO_IDX << 35)
           | ((u64)0 << 38)
           | ((u64)(offset & 0x3FFFF) << 42);
    u64 w2 = (u64)MIDG_OP_LDST_NOP;
    u8 type_byte = TAG_LOAD_STORE_4_ | (TAG_ALU_4_WRITEOUT_ << 4);
    u64 ls_lo = (u64)type_byte | (w1 << 8);
    u64 ls_hi = (w1 >> 56) | (w2 << 4);
    u32 w[4] = {(u32)(ls_lo & 0xFFFFFFFF),
                (u32)(ls_lo >> 32),
                (u32)(ls_hi & 0xFFFFFFFF),
                (u32)(ls_hi >> 32)};
    sb_append(sb, w, 4);
}

// Build a complete probe shader: store zero to target_addr
static void build_probe_shader_v2(u32* buf, u32 words, u32 target_addr) {
    mali_shader_builder_t sb;
    sb_init(&sb, buf, words);
    // Bundle 1: imov const[0]=target_addr -> r26
    sb_alu_imov_const(&sb, REG_LDST_BASE, 0, 0xFF);
    sb.words[4] = target_addr; // patch embedded constant
    // Bundle 2: imov imm=0 -> r27
    sb_alu_imov_imm(&sb, REG_LDST_BASE + 1, 0, 0xFF);
    // Bundle 3: st_32 r27 -> [r26]
    sb_ldst_st_32(&sb, REG_LDST_BASE + 1, REG_LDST_BASE, 0);
    u32 wo[4] = {
        TAG_ALU_4_WRITEOUT_ | (TAG_BREAK_ << 4) | ALU_ENAB_BR_COMPACT,
        0x0000C007,
        0,
        0
    };
    sb_append(&sb, wo, 4);
}

// --- Renderer State builder ---
static inline void build_rsd(u32* rsd, u64 shader_addr, u8 first_tag,
                             u32 work_reg_count, u32 side_effects) {
    for (u32 i = 0; i < 16; i++) rsd[i] = 0;
    rsd[0] = (u32)((shader_addr | first_tag) & 0xFFFFFFFF);
    rsd[1] = (u32)(shader_addr >> 32);
    // Renderer Properties: work_reg_count [20:16], side_effects [13]
    rsd[4] = ((work_reg_count & 0x1F) << 16) | (side_effects ? (1u << 13) : 0);
}

// --- Compute Job builder v2 (Mesa-correct invocation) ---
static void build_compute_job_v2(u32* job, u64 rsd_addr, u64 thread_storage_addr,
                                  u64 scratch_addr,
                                  u32 wg_x, u32 wg_y, u32 wg_z,
                                  u32 ls_x, u32 ls_y, u32 ls_z,
                                  u64 next_job) {
    for (u32 i = 0; i < CP_TOTAL_WORDS; i++) job[i] = 0;
    // Header
    job[4] = 0x00040009; // COMPUTE, 64-bit descriptor
    // Invocation
    u32 w8, w9;
    pan_pack_invocation(&w8, &w9, wg_x, wg_y, wg_z, ls_x, ls_y, ls_z);
    job[8] = w8;
    job[9] = w9;
    // Parameters: job_task_split at bits [29:26]
    u32 split = util_logbase2_ceil(ls_x + 1) + util_logbase2_ceil(ls_y + 1)
              + util_logbase2_ceil(ls_z + 1);
    job[10] = (split & 0xFu) << 26;
    // Draw
    job[16] = 0x01010007; // flags
    (void)scratch_addr;
    job[30] = (u32)(rsd_addr & 0xFFFFFFFF);
    job[31] = (u32)(rsd_addr >> 32);
    job[44] = (u32)(thread_storage_addr & 0xFFFFFFFF);
    job[45] = (u32)(thread_storage_addr >> 32);
    // Next job
    job[6] = (u32)(next_job & 0xFFFFFFFF);
    job[7] = (u32)(next_job >> 32);
}

// --- Job chain submitter ---
result_t mali_compute_submit_chain(u64 first_job, u32 count, u32 timeout_us) {
    (void)count;
    mali_mmu_clear_irqs();
    result_t res = mali_jm_submit(MALI_JM_COMPUTE_SLOT, first_job, 0);
    if (res != OK) return res;
    return mali_jm_wait(MALI_JM_COMPUTE_SLOT, timeout_us);
}

// --- Ping-pong buffer allocator (identity-mapped, cache-line aligned) ---
#define MALI_PP_SLOTS 2
static volatile u32 __attribute__((aligned(64))) g_pp_ready[MALI_PP_SLOTS];
static u32 __attribute__((aligned(64))) g_pp_storage[MALI_PP_SLOTS][64];

void* mali_compute_pp_buffer(u32 slot) {
    if (slot >= MALI_PP_SLOTS) return (void*)0;
    return (void*)g_pp_storage[slot];
}

void mali_compute_pp_mark_ready(u32 slot) { g_pp_ready[slot] = 1; }
void mali_compute_pp_mark_done(u32 slot)  { g_pp_ready[slot] = 0; }
u32  mali_compute_pp_ready(u32 slot)      { return g_pp_ready[slot]; }

// --- Example: asynchronous compute probe using ping-pong ---
result_t mali_compute_probe_async(u32 slot) {
    if (slot >= MALI_PP_SLOTS) return ERR_INVALID_PARAM;
    u32* storage = g_pp_storage[slot];
    for (u32 i = 0; i < 64; i++) storage[i] = 0xDEADBEEFu;
    mali_clean_range(storage, 256);

    static u32 __attribute__((aligned(64))) s_shader[SHADER_WORDS];
    static u32 __attribute__((aligned(64))) s_rsd[16];
    static u32 __attribute__((aligned(64))) s_job[CP_TOTAL_WORDS];
    static u32 __attribute__((aligned(64))) s_ts[8];

    u64 storage_addr = (u64)(uintptr_t)storage;
    build_probe_shader_v2(s_shader, SHADER_WORDS, (u32)storage_addr);
    mali_clean_range(s_shader, sizeof(s_shader));

    build_rsd(s_rsd, (u64)(uintptr_t)s_shader, TAG_ALU_8_, 4, true);
    mali_clean_range(s_rsd, sizeof(s_rsd));

    for (u32 i = 0; i < 8; i++) s_ts[i] = 0;
    s_ts[1] = 0x1F;
    mali_clean_range(s_ts, sizeof(s_ts));

    build_compute_job_v2(s_job,
                         (u64)(uintptr_t)s_rsd,
                         (u64)(uintptr_t)s_ts,
                         storage_addr,
                         1, 1, 1, 1, 1, 1, 0);
    mali_clean_range(s_job, sizeof(s_job));

    result_t res = mali_jm_submit(MALI_JM_COMPUTE_SLOT, (u64)(uintptr_t)s_job, 0);
    if (res == OK) g_pp_ready[slot] = 2; // 2 = submitted, not done
    return res;
}

result_t mali_compute_probe_async_finish(u32 slot, u32 timeout_us) {
    if (slot >= MALI_PP_SLOTS) return ERR_INVALID_PARAM;
    result_t res = mali_jm_wait(MALI_JM_COMPUTE_SLOT, timeout_us);
    if (res == OK) {
        mali_invalidate_range(g_pp_storage[slot], 256);
        g_pp_ready[slot] = 1;
    }
    return res;
}

// --- Neural inference shader: 6 inputs -> 8 hidden (fixed-point Q16.16) ---
// This is a skeleton; full matvec requires multiple ALU bundles.
// Midgard T860: 16 threads per workgroup optimal.
// Shader reads input vector from SSBO[0], weight matrix from SSBO[1],
// writes output to SSBO[2].
#define NEURO_VEC_REGS  8  // r0..r7 = input vector (loaded from memory)
#define NEURO_WGT_REGS 16 // r8..r23 = weights (loaded from memory)
// For a minimal racing driver, we provide the descriptor path;
// the shader binary is built by the shader compiler offline and patched here.

// --- Diagnostic dump: new v2 probe path ---
void mali_compute_dump_probe_v2(void) {
    static u32 __attribute__((aligned(64))) d_shader[SHADER_WORDS];
    static u32 __attribute__((aligned(64))) d_rsd[16];
    static u32 __attribute__((aligned(64))) d_job[CP_TOTAL_WORDS];
    static u32 __attribute__((aligned(64))) d_ts[8];
    static u32 __attribute__((aligned(64))) d_storage[64];

    u64 storage_addr = (u64)(uintptr_t)d_storage;
    build_probe_shader_v2(d_shader, SHADER_WORDS, (u32)storage_addr);
    build_rsd(d_rsd, (u64)(uintptr_t)d_shader, TAG_ALU_8_, 4, 1);
    for (u32 i = 0; i < 8; i++) d_ts[i] = 0;
    d_ts[1] = 0x1F;
    build_compute_job_v2(d_job, (u64)(uintptr_t)d_rsd, (u64)(uintptr_t)d_ts,
                         storage_addr, 1, 1, 1, 1, 1, 1, 0);

    uart_puts(&console, "\r\n=== MALI PROBE V2 DUMP ===\r\n");

    // Shader bundles
    uart_puts(&console, "[SHADER] Bundle1 (ALU8) words 0-7:\r\n");
    for (u32 i = 0; i < 8; i++) {
        uart_puts(&console, "  w"); uart_put_hex(&console, i);
        uart_puts(&console, "=0x"); uart_put_hex(&console, d_shader[i]);
        uart_puts(&console, "\r\n");
    }
    // Decode reg_info word1
    u16 reg1 = (u16)(d_shader[1] & 0xFFFFu);
    uart_puts(&console, "  reg_info src1="); uart_put_hex(&console, reg1 & 0x1F);
    uart_puts(&console, " src2="); uart_put_hex(&console, (reg1 >> 5) & 0x1F);
    uart_puts(&console, " out="); uart_put_hex(&console, (reg1 >> 10) & 0x1F);
    uart_puts(&console, " imm="); uart_put_hex(&console, (reg1 >> 15) & 1);
    uart_puts(&console, "\r\n");

    // Decode load-store bundle
    u64 ls_lo = (u64)d_shader[8] | ((u64)d_shader[9] << 32);
    u64 ls_hi = (u64)d_shader[10] | ((u64)d_shader[11] << 32);
    uart_puts(&console, "[SHADER] Bundle2 (LDST) words 8-11:\r\n");
    uart_puts(&console, "  ls_lo=0x"); uart_put_hex(&console, (u32)(ls_lo >> 8));
    uart_puts(&console, ", ls_hi=0x"); uart_put_hex(&console, (u32)(ls_hi & 0xFFFFFFFFu));
    uart_puts(&console, "\r\n");

    // RSD
    uart_puts(&console, "[RSD] words 0-7:\r\n");
    for (u32 i = 0; i < 8; i++) {
        uart_puts(&console, "  w"); uart_put_hex(&console, i);
        uart_puts(&console, "=0x"); uart_put_hex(&console, d_rsd[i]);
        uart_puts(&console, "\r\n");
    }
    uart_puts(&console, "  shader_ptr=0x");
    u64 sp = ((u64)d_rsd[1] << 32) | (d_rsd[0] & ~0xFFu);
    uart_put_hex(&console, (u32)(sp & 0xFFFFFFFFu));
    uart_puts(&console, " first_tag=0x"); uart_put_hex(&console, d_rsd[0] & 0xFFu);
    uart_puts(&console, "\r\n");

    // Compute job
    uart_puts(&console, "[JOB] Header/Invocation:\r\n");
    for (u32 i = 4; i < 12; i++) {
        uart_puts(&console, "  w"); uart_put_hex(&console, i);
        uart_puts(&console, "=0x"); uart_put_hex(&console, d_job[i]);
        uart_puts(&console, "\r\n");
    }

    // Decode invocation
    u32 w8 = d_job[8];
    u32 w9 = d_job[9];
    u32 s1 = w9 & 0x1F;
    u32 s2 = (w9 >> 5) & 0x1F;
    u32 s3 = (w9 >> 10) & 0x3F;
    u32 s4 = (w9 >> 16) & 0x3F;
    u32 s5 = (w9 >> 22) & 0x3F;
    u32 s6 = (w9 >> 28) & 0xF;
    uart_puts(&console, "  INVOCATION w8=0x"); uart_put_hex(&console, w8);
    uart_puts(&console, " w9=0x"); uart_put_hex(&console, w9);
    uart_puts(&console, "\r\n");
    uart_puts(&console, "  shifts=");
    uart_put_hex(&console, s1); uart_puts(&console, ",");
    uart_put_hex(&console, s2); uart_puts(&console, ",");
    uart_put_hex(&console, s3); uart_puts(&console, ",");
    uart_put_hex(&console, s4); uart_puts(&console, ",");
    uart_put_hex(&console, s5); uart_puts(&console, ",");
    uart_put_hex(&console, s6); uart_puts(&console, "\r\n");
    uart_puts(&console, "  size_x="); uart_put_hex(&console, ((w8 >> 0)  & ((1u << s1) - 1)) + 1);
    uart_puts(&console, " size_y="); uart_put_hex(&console, ((w8 >> s1) & ((1u << (s2-s1)) - 1)) + 1);
    uart_puts(&console, " size_z="); uart_put_hex(&console, ((w8 >> s2) & ((1u << (s3-s2)) - 1)) + 1);
    uart_puts(&console, " groups_x="); uart_put_hex(&console, ((w8 >> s3) & ((1u << (s4-s3)) - 1)) + 1);
    uart_puts(&console, " groups_y="); uart_put_hex(&console, ((w8 >> s4) & ((1u << (s5-s4)) - 1)) + 1);
    uart_puts(&console, " groups_z="); uart_put_hex(&console, ((w8 >> s5) & ((1u << (s6-s5)) - 1)) + 1);
    uart_puts(&console, "\r\n");

    uart_puts(&console, "[JOB] Draw/RSD/TS:\r\n");
    for (u32 i = 16; i < 24; i++) {
        uart_puts(&console, "  w"); uart_put_hex(&console, i);
        uart_puts(&console, "=0x"); uart_put_hex(&console, d_job[i]);
        uart_puts(&console, "\r\n");
    }
    for (u32 i = 30; i < 32; i++) {
        uart_puts(&console, "  w"); uart_put_hex(&console, i);
        uart_puts(&console, "=0x"); uart_put_hex(&console, d_job[i]);
        uart_puts(&console, "\r\n");
    }
    for (u32 i = 44; i < 46; i++) {
        uart_puts(&console, "  w"); uart_put_hex(&console, i);
        uart_puts(&console, "=0x"); uart_put_hex(&console, d_job[i]);
        uart_puts(&console, "\r\n");
    }
    uart_puts(&console, "=== END DUMP ===\r\n");
}

// ==========================================================================
// 4×4 Matrix Multiply Shader (Midgard ISA v5, Mali T860)
// ==========================================================================
// Computes C = A × B^T for 4×4 float matrices (row-major).
// Each thread computes one element: C[row][col] = dot(A[row], BT[col])
// Dispatch: local_size=(4,4,1), num_groups=(1,1,1) → 16 threads
//
// 18 bundles, 84 words = 336 bytes
// Work registers: r0-r4, LDST: r26(addr), r27(data)
// ==========================================================================

// Matmul shader rewrite v2: Mesa-correct TID loading pattern.
// Root cause of v1 JOB_BUS_FAULT: used index_reg=6 (GLOBAL_THREAD_ID) directly
// in LD instructions. Mesa Panfrost NEVER does this — it always reads TID
// via ldst_mov(arg_reg=6) into a work register, then computes the address in
// ALU, then does a plain ld_128 with index_reg=7 (ZERO).
// Also: index_format=1(u64) was wrong for 32-bit TID values.
//
// New layout (18 bundles, 84 words = 336 bytes):
//   B0  (LDST,4w):    ldst_mov r2 ← TID (arg_reg=6, Mesa pattern)
//   B1  (ALU_8,8w):   VADD imov r26 ← const{A_base}
//   B2  (ALU_4,4w):   VADD ishl r3.x ← r2.y, #4  (row offset = TID.y * 16)
//   B3  (ALU_4,4w):   VADD iadd r26.x ← r26.x, r3.x
//   B4  (LDST,4w):    ld_32 r0.x, [r26] (plain, no index)
//   B5  (ALU_8,8w):   VADD imov r26 ← const{BT_base}
//   B6  (ALU_4,4w):   VADD ishl r3.x ← r2.x, #4  (col offset = TID.x * 16)
//   B7  (ALU_4,4w):   VADD iadd r26.x ← r26.x, r3.x
//   B8  (LDST,4w):    ld_32 r1.x, [r26] (plain, no index)
//   B9  (ALU_4,4w):   VMUL fdot4 r0.x ← r0, r1
//   B10 (ALU_8,8w):   VADD imov r26 ← const{C_base}
//   B11 (ALU_4,4w):   VADD ishl r3.x ← r2.y,#4
//   B12 (ALU_4,4w):   VADD ishl r4.x ← r2.x,#2
//   B13 (ALU_4,4w):   VADD iadd r3.x ← r3.x, r4.x
//   B14 (ALU_4,4w):   VADD iadd r26.x ← r26.x, r3.x
//   B15 (ALU_4,4w):   VMUL imov r27.x ← r0.x
//   B16 (LDST,4w):    st_32 r27, [r26]
//   B17 (ALU_4,4w):   br.writeout
//
// Registers: r0(A row/result), r1(BT row), r2(TID), r3/r4(temp offsets),
//            r26(LDST base), r27(LDST store data). Work regs: 5 → rounds to 8.
#define MATMUL_SHADER_WORDS 84
#define LOAD_PROBE_SHADER_WORDS 32
#define LDPROBE_ENABLE 0u
#define LDPROBE_SELECT 7u

static void mm_emit_imov_const(u32* s, u32* w, u32 next_tag, u32 dst_reg, u64 value) {
    u32 src_id = pack_vec_src(0, 0, SWIZZLE_IDENTITY);
    u32 ctrl = TAG_ALU_8_ | (next_tag << 4) | ALU_ENAB_VEC_ADD;
    u16 reg = pack_reg_info(REG_UNUSED, REG_CONSTANT, dst_reg, 0);
    u64 body = pack_vec_alu_48(MIDG_OP_IMOV, MIDG_REG_MODE_32,
                               src_id, src_id, MIDG_SHRINK_NONE,
                               MIDG_OUTMOD_KEEPLO, 0xFF);
    s[*w+0] = ctrl;
    s[*w+1] = (u32)reg | ((u32)(body & 0xFFFF) << 16);
    s[*w+2] = (u32)((body >> 16) & 0xFFFF) | ((u32)((body >> 32) & 0xFFFF) << 16);
    s[*w+4] = (u32)(value & 0xFFFFFFFF);
    s[*w+5] = (u32)(value >> 32);
    *w += 8;
}

static void mm_emit_alu_imm(u32* s, u32* w, u32 next_tag, u32 op,
                            u32 dst_reg, u32 src_reg, u32 imm) {
    u32 src_x = pack_vec_src(0, 0, 0x00);
    u32 ctrl = TAG_ALU_4_ | (next_tag << 4) | ALU_ENAB_VEC_ADD;
    u16 reg = pack_reg_info(src_reg, imm >> 11, dst_reg, 1);
    u64 body = pack_vec_alu_48(op, MIDG_REG_MODE_32, src_x,
                               encode_vector_inline_src2(imm),
                               MIDG_SHRINK_NONE, MIDG_OUTMOD_KEEPLO, 0x03);
    s[*w+0] = ctrl;
    s[*w+1] = (u32)reg | ((u32)(body & 0xFFFF) << 16);
    s[*w+2] = (u32)((body >> 16) & 0xFFFF) | ((u32)((body >> 32) & 0xFFFF) << 16);
    *w += 4;
}

static void mm_emit_alu_reg(u32* s, u32* w, u32 next_tag, u32 op,
                            u32 dst_reg, u32 src1_reg, u32 src2_reg) {
    u32 src_x = pack_vec_src(0, 0, 0x00);
    u32 ctrl = TAG_ALU_4_ | (next_tag << 4) | ALU_ENAB_VEC_ADD;
    u16 reg = pack_reg_info(src1_reg, src2_reg, dst_reg, 0);
    u64 body = pack_vec_alu_48(op, MIDG_REG_MODE_32, src_x, src_x,
                               MIDG_SHRINK_NONE, MIDG_OUTMOD_KEEPLO, 0x03);
    s[*w+0] = ctrl;
    s[*w+1] = (u32)reg | ((u32)(body & 0xFFFF) << 16);
    s[*w+2] = (u32)((body >> 16) & 0xFFFF) | ((u32)((body >> 32) & 0xFFFF) << 16);
    *w += 4;
}

static void mm_emit_fdot4(u32* s, u32* w) {
    u32 src_id = pack_vec_src(0, 0, SWIZZLE_IDENTITY);
    u32 ctrl = TAG_ALU_4_ | (TAG_ALU_4_ << 4) | ALU_ENAB_VEC_MUL;
    u16 reg = pack_reg_info(0, 1, 0, 0);
    u64 body = pack_vec_alu_48(MIDG_OP_FDOT4, MIDG_REG_MODE_32,
                               src_id, src_id, MIDG_SHRINK_NONE, 0, 0x03);
    s[*w+0] = ctrl;
    s[*w+1] = (u32)reg | ((u32)(body & 0xFFFF) << 16);
    s[*w+2] = (u32)((body >> 16) & 0xFFFF) | ((u32)((body >> 32) & 0xFFFF) << 16);
    *w += 4;
}

static void mm_emit_result_to_r27(u32* s, u32* w) {
    u32 src_id = pack_vec_src(0, 0, SWIZZLE_IDENTITY);
    u32 src_xx = pack_vec_src(0, 0, 0x00);
    u32 ctrl = TAG_ALU_4_ | (TAG_LOAD_STORE_4_ << 4) | ALU_ENAB_VEC_ADD;
    u16 reg = pack_reg_info(REG_UNUSED, 0, REG_LDST_BASE + 1, 0);
    u64 body = pack_vec_alu_48(MIDG_OP_IMOV, MIDG_REG_MODE_32,
                               src_id, src_xx, MIDG_SHRINK_NONE,
                               MIDG_OUTMOD_KEEPLO, 0x03);
    s[*w+0] = ctrl;
    s[*w+1] = (u32)reg | ((u32)(body & 0xFFFF) << 16);
    s[*w+2] = (u32)((body >> 16) & 0xFFFF) | ((u32)((body >> 32) & 0xFFFF) << 16);
    *w += 4;
}

static void build_matmul_4x4_shader(u32* s, u32 a_lo, u32 a_hi,
                                     u32 bt_lo, u32 bt_hi,
                                     u32 c_lo, u32 c_hi) {
    for (u32 i = 0; i < MATMUL_SHADER_WORDS; i++) s[i] = 0;

    u32 w = 0;
    u64 a_base = ((u64)a_hi << 32) | a_lo;
    u64 bt_base = ((u64)bt_hi << 32) | bt_lo;
    u64 c_base = ((u64)c_hi << 32) | c_lo;

    {
        u64 mov = pack_ldst_word(MIDG_OP_LDST_MOV, 2, 0x7, 0x24,
                                 0, REG_LDST_TID, 0, 0,
                                 0, 0, 0, 0);
        u64 nop = (u64)MIDG_OP_LDST_NOP;
        pack_ldst_bundle(&s[w], TAG_LOAD_STORE_4_, TAG_ALU_4_, mov, nop);
        w += 4;
    }

    mm_emit_alu_imm(s, &w, TAG_ALU_4_, MIDG_OP_IAND, 3, 2, 12);
    mm_emit_alu_imm(s, &w, TAG_ALU_8_, MIDG_OP_ISHL, 3, 3, 2);
    mm_emit_imov_const(s, &w, TAG_ALU_4_, REG_LDST_BASE, a_base);
    mm_emit_alu_reg(s, &w, TAG_LOAD_STORE_4_, MIDG_OP_IADD, REG_LDST_BASE, REG_LDST_BASE, 3);

    {
        u64 ld = pack_ldst_word(MIDG_OP_LD_128, 0, 0xF, SWIZZLE_IDENTITY,
                                0, 0, 1, 1,
                                0, REG_LDST_ZERO_IDX, 0, 0);
        u64 nop = (u64)MIDG_OP_LDST_NOP;
        pack_ldst_bundle(&s[w], TAG_LOAD_STORE_4_, TAG_ALU_4_, ld, nop);
        w += 4;
    }

    mm_emit_alu_imm(s, &w, TAG_ALU_4_, MIDG_OP_IAND, 4, 2, 3);
    mm_emit_alu_imm(s, &w, TAG_ALU_8_, MIDG_OP_ISHL, 4, 4, 4);
    mm_emit_imov_const(s, &w, TAG_ALU_4_, REG_LDST_BASE, bt_base);
    mm_emit_alu_reg(s, &w, TAG_LOAD_STORE_4_, MIDG_OP_IADD, REG_LDST_BASE, REG_LDST_BASE, 4);

    {
        u64 ld = pack_ldst_word(MIDG_OP_LD_128, 1, 0xF, SWIZZLE_IDENTITY,
                                0, 0, 1, 1,
                                0, REG_LDST_ZERO_IDX, 0, 0);
        u64 nop = (u64)MIDG_OP_LDST_NOP;
        pack_ldst_bundle(&s[w], TAG_LOAD_STORE_4_, TAG_ALU_4_, ld, nop);
        w += 4;
    }

    mm_emit_fdot4(s, &w);
    mm_emit_alu_imm(s, &w, TAG_ALU_8_, MIDG_OP_ISHL, 3, 2, 2);
    mm_emit_imov_const(s, &w, TAG_ALU_4_, REG_LDST_BASE, c_base);
    mm_emit_alu_reg(s, &w, TAG_ALU_4_, MIDG_OP_IADD, REG_LDST_BASE, REG_LDST_BASE, 3);
    mm_emit_result_to_r27(s, &w);

    {
        u64 st = pack_ldst_word(MIDG_OP_ST_32, 1, 0xF, 0x00,
                                0, 0, 1, 1,
                                0, REG_LDST_ZERO_IDX, 0, 0);
        u64 nop = (u64)MIDG_OP_LDST_NOP;
        pack_ldst_bundle(&s[w], TAG_LOAD_STORE_4_, TAG_ALU_4_WRITEOUT_, st, nop);
        w += 4;
    }

    {
        u32 ctrl = TAG_ALU_4_WRITEOUT_ | (TAG_BREAK_ << 4) | ALU_ENAB_BR_COMPACT;
        s[w+0] = ctrl;
        s[w+1] = 0x0000C007;
        w += 4;
    }

    if (w != MATMUL_SHADER_WORDS) {
        uart_puts(&console, "[MATMUL] shader word mismatch w=");
        uart_put_hex(&console, w);
        uart_puts(&console, " expected=");
        uart_put_hex(&console, MATMUL_SHADER_WORDS);
        uart_puts(&console, "\r\n");
    }
}

#if LDPROBE_ENABLE
static void build_load32_store32_probe_shader(u32* s, u32 src_lo, u32 src_hi,
                                              u32 dst_lo, u32 dst_hi,
                                              u32 load_arg_reg,
                                              u32 load_bitsize_toggle,
                                              u32 load_index_format,
                                              u32 load_index_reg) {
    for (u32 i = 0; i < LOAD_PROBE_SHADER_WORDS; i++) s[i] = 0;

    u32 src_id = pack_vec_src(0, 0, SWIZZLE_IDENTITY);
    u32 src_xx = pack_vec_src(0, 0, 0x00);
    u32 w = 0;

    {
        u32 ctrl = TAG_ALU_8_ | (TAG_LOAD_STORE_4_ << 4) | ALU_ENAB_VEC_ADD;
        u16 reg = pack_reg_info(REG_UNUSED, REG_CONSTANT, REG_LDST_BASE, 0);
        u64 body = pack_vec_alu_48(MIDG_OP_IMOV, MIDG_REG_MODE_32,
                                   src_id, src_id, MIDG_SHRINK_NONE,
                                   MIDG_OUTMOD_KEEPLO, 0xFF);
        s[w+0] = ctrl;
        s[w+1] = (u32)reg | ((u32)(body & 0xFFFF) << 16);
        s[w+2] = (u32)((body >> 16) & 0xFFFF) | ((u32)((body >> 32) & 0xFFFF) << 16);
        s[w+4] = src_lo;
        s[w+5] = src_hi;
        w += 8;
    }

    {
        u64 ld = pack_ldst_word(MIDG_OP_LD_32, 1, 0x1, SWIZZLE_IDENTITY,
                                0, load_arg_reg, load_bitsize_toggle, load_index_format,
                                0, load_index_reg, 0, 0);
        u64 nop = (u64)MIDG_OP_LDST_NOP;
        pack_ldst_bundle(&s[w], TAG_LOAD_STORE_4_, TAG_ALU_4_, ld, nop);
        w += 4;
    }

    {
        u32 ctrl = TAG_ALU_4_ | (TAG_ALU_8_ << 4) | ALU_ENAB_VEC_MUL;
        u16 reg = pack_reg_info(0, REG_UNUSED, REG_LDST_BASE + 1, 0);
        u64 body = pack_vec_alu_48(MIDG_OP_IMOV, MIDG_REG_MODE_32,
                                   src_id, src_xx, MIDG_SHRINK_NONE,
                                   MIDG_OUTMOD_KEEPLO, 0x03);
        s[w+0] = ctrl;
        s[w+1] = (u32)reg | ((u32)(body & 0xFFFF) << 16);
        s[w+2] = (u32)((body >> 16) & 0xFFFF) | ((u32)((body >> 32) & 0xFFFF) << 16);
        w += 4;
    }

    {
        u32 ctrl = TAG_ALU_8_ | (TAG_ALU_4_ << 4) | ALU_ENAB_VEC_ADD;
        u16 reg = pack_reg_info(REG_UNUSED, REG_CONSTANT, REG_LDST_BASE, 0);
        u64 body = pack_vec_alu_48(MIDG_OP_IMOV, MIDG_REG_MODE_32,
                                   src_id, src_id, MIDG_SHRINK_NONE,
                                   MIDG_OUTMOD_KEEPLO, 0xFF);
        s[w+0] = ctrl;
        s[w+1] = (u32)reg | ((u32)(body & 0xFFFF) << 16);
        s[w+2] = (u32)((body >> 16) & 0xFFFF) | ((u32)((body >> 32) & 0xFFFF) << 16);
        s[w+4] = dst_lo;
        s[w+5] = dst_hi;
        w += 8;
    }

    {
        u64 st = pack_ldst_word(MIDG_OP_ST_32, 1, 0xF, 0x00,
                                0, 0, 1, MIDGARD_INDEX_ADDRESS_S32,
                                0, REG_LDST_ZERO_IDX, 0, 0);
        u64 nop = (u64)MIDG_OP_LDST_NOP;
        pack_ldst_bundle(&s[w], TAG_LOAD_STORE_4_, TAG_ALU_4_WRITEOUT_, st, nop);
        w += 4;
    }

    {
        u32 ctrl = TAG_ALU_4_WRITEOUT_ | (TAG_BREAK_ << 4) | ALU_ENAB_BR_COMPACT;
        s[w+0] = ctrl;
        s[w+1] = 0x0000C007;
        w += 4;
    }

    if (w != LOAD_PROBE_SHADER_WORDS) {
        uart_puts(&console, "[LDPROBE] shader word mismatch w=");
        uart_put_hex(&console, w);
        uart_puts(&console, " expected=");
        uart_put_hex(&console, LOAD_PROBE_SHADER_WORDS);
        uart_puts(&console, "\r\n");
    }
}
#endif

// --- Static buffers for matmul test ---
static u32 __attribute__((aligned(64))) g_mm_shader[MATMUL_SHADER_WORDS];
static u32 __attribute__((aligned(64))) g_mm_rsd[16];
static u32 __attribute__((aligned(64))) g_mm_job[CP_TOTAL_WORDS];
static u32 __attribute__((aligned(64))) g_mm_ts[8];
#if LDPROBE_ENABLE
static u32 __attribute__((aligned(64))) g_ld_probe_shader[LOAD_PROBE_SHADER_WORDS];
static u32 __attribute__((aligned(64))) g_ld_probe_rsd[16];
static u32 __attribute__((aligned(64))) g_ld_probe_job[CP_TOTAL_WORDS];
static u32 __attribute__((aligned(64))) g_ld_probe_ts[8];
#endif
static volatile u32 __attribute__((aligned(64))) g_mm_A[16];
static volatile u32 __attribute__((aligned(64))) g_mm_BT[16];
static volatile u32 __attribute__((aligned(64))) g_mm_C[16];

#define F32_ONE   0x3F800000u
#define F32_TWO   0x40000000u
#define F32_THREE 0x40400000u
#define F32_FOUR  0x40800000u
#define F32_ZERO  0x00000000u

result_t mali_compute_matmul_4x4(void) {
    // Initialize test matrices:
    // A = identity, BT = identity → C should be identity
    for (u32 i = 0; i < 16; i++) {
        g_mm_A[i] = F32_ZERO;
        g_mm_BT[i] = F32_ZERO;
        g_mm_C[i] = 0xDEADBEEFu;
    }
    // A = I (row-major: A[row*4 + col])
    g_mm_A[0]  = F32_ONE;
    g_mm_A[5]  = F32_ONE;
    g_mm_A[10] = F32_ONE;
    g_mm_A[15] = F32_ONE;
    // BT = I (BT[col*4 + k] = B[k][col], for I this is same as A)
    g_mm_BT[0]  = F32_ONE;
    g_mm_BT[5]  = F32_ONE;
    g_mm_BT[10] = F32_ONE;
    g_mm_BT[15] = F32_ONE;

    mali_clean_range((void*)g_mm_A, sizeof(g_mm_A));
    mali_clean_range((void*)g_mm_BT, sizeof(g_mm_BT));
    mali_clean_range((void*)g_mm_C, sizeof(g_mm_C));

    // Build shader with matrix addresses
    u64 a_addr  = (u64)(uintptr_t)g_mm_A;
    u64 bt_addr = (u64)(uintptr_t)g_mm_BT;
    u64 c_addr  = (u64)(uintptr_t)g_mm_C;

    build_matmul_4x4_shader(g_mm_shader,
                            (u32)(a_addr & 0xFFFFFFFF), (u32)(a_addr >> 32),
                            (u32)(bt_addr & 0xFFFFFFFF), (u32)(bt_addr >> 32),
                            (u32)(c_addr & 0xFFFFFFFF), (u32)(c_addr >> 32));
    mali_clean_range(g_mm_shader, sizeof(g_mm_shader));

#if LDPROBE_ENABLE
    // Multi-variant LD.32 probe: test address operand forms in one boot
    const u32 ld_probe_arg[8]     = {REG_LDST_ZERO_IDX, REG_LDST_ZERO_IDX, 0,               0,                REG_LDST_ZERO_IDX, 0,                 REG_LDST_ZERO_IDX, 0};
    const u32 ld_probe_bitsize[8] = {0,                 1,                 1,               0,                0,                 1,                 0,                 1};
    const u32 ld_probe_fmt[8]     = {MIDGARD_INDEX_ADDRESS_U32, MIDGARD_INDEX_ADDRESS_U32, MIDGARD_INDEX_ADDRESS_U32, MIDGARD_INDEX_ADDRESS_U32, MIDGARD_INDEX_ADDRESS_S32, MIDGARD_INDEX_ADDRESS_S32, 1,                 1};
    const u32 ld_probe_idx[8]     = {0,                 0,                 REG_LDST_ZERO_IDX, REG_LDST_ZERO_IDX, 0,                 REG_LDST_ZERO_IDX, 0,                 REG_LDST_ZERO_IDX};
    for (u32 v = 0; v < 8; v++) {
        if (LDPROBE_SELECT != 0xFFFFFFFFu && v != LDPROBE_SELECT) continue;
        uart_puts(&console, "[LDPROBEv");
        uart_put_hex(&console, v);
        uart_puts(&console, "] arg=");
        uart_put_hex(&console, ld_probe_arg[v]);
        uart_puts(&console, " bitsize=");
        uart_put_hex(&console, ld_probe_bitsize[v]);
        uart_puts(&console, " fmt=");
        uart_put_hex(&console, ld_probe_fmt[v]);
        uart_puts(&console, " idx=");
        uart_put_hex(&console, ld_probe_idx[v]);
        uart_puts(&console, "\r\n");

        build_load32_store32_probe_shader(g_ld_probe_shader,
            (u32)(a_addr & 0xFFFFFFFF), (u32)(a_addr >> 32),
            (u32)(c_addr & 0xFFFFFFFF), (u32)(c_addr >> 32),
            ld_probe_arg[v], ld_probe_bitsize[v], ld_probe_fmt[v], ld_probe_idx[v]);
        mali_clean_range(g_ld_probe_shader, sizeof(g_ld_probe_shader));
        build_rsd(g_ld_probe_rsd, (u64)(uintptr_t)g_ld_probe_shader, TAG_ALU_8_, 8, 1);
        mali_clean_range(g_ld_probe_rsd, sizeof(g_ld_probe_rsd));
        for (u32 i = 0; i < 8; i++) g_ld_probe_ts[i] = 0;
        g_ld_probe_ts[1] = 0x1F;
        mali_clean_range(g_ld_probe_ts, sizeof(g_ld_probe_ts));
        build_compute_job_v2(g_ld_probe_job,
                             (u64)(uintptr_t)g_ld_probe_rsd,
                             (u64)(uintptr_t)g_ld_probe_ts,
                             (u64)(uintptr_t)g_mm_C,
                             1, 1, 1, 1, 1, 1, 0);
        mali_clean_range(g_ld_probe_job, sizeof(g_ld_probe_job));
        mali_mmu_clear_irqs();
        result_t ld_res = mali_jm_submit(MALI_JM_COMPUTE_SLOT,
                                         (u64)(uintptr_t)g_ld_probe_job, 0);
        mali_jm_wait_diag_t ld_wd = {0};
        if (ld_res == OK) ld_res = mali_jm_wait_ex(MALI_JM_COMPUTE_SLOT, 500000, &ld_wd);
        mali_invalidate_range((void*)g_mm_C, sizeof(g_mm_C));
        uart_puts(&console, "[LDPROBEv");
        uart_put_hex(&console, v);
        uart_puts(&console, "] res=");
        uart_put_hex(&console, (u32)ld_res);
        uart_puts(&console, " done=");
        uart_put_hex(&console, ld_wd.saw_done);
        uart_puts(&console, " fail=");
        uart_put_hex(&console, ld_wd.saw_fail);
        uart_puts(&console, " js=0x");
        uart_put_hex(&console, ld_wd.js_status);
        uart_puts(&console, " C0=0x");
        uart_put_hex(&console, g_mm_C[0]);
        uart_puts(&console, "\r\n");
        if (ld_res != OK || ld_wd.saw_fail) {
            mali_invalidate_range(g_ld_probe_job, sizeof(g_ld_probe_job));
            uart_puts(&console, "[LDPROBEv");
            uart_put_hex(&console, v);
            uart_puts(&console, "] fault_ptr=0x");
            uart_put_hex(&console, g_ld_probe_job[3]);
            uart_put_hex(&console, g_ld_probe_job[2]);
            uart_puts(&console, " exc=0x");
            uart_put_hex(&console, g_ld_probe_job[0]);
            uart_puts(&console, "\r\n");
            // Decode LDST bundle (B1, words 8..11 in shader)
            u64 b1_lo = (u64)g_ld_probe_shader[8] | ((u64)g_ld_probe_shader[9] << 32);
            u64 b1_word1 = (b1_lo >> 8) & 0x0FFFFFFFFFFFFFFFULL;
            uart_puts(&console, "[LDPROBEv");
            uart_put_hex(&console, v);
            uart_puts(&console, "] B1 decode: ");
            mali_decode_st32_word(b1_word1);
            return ld_res;
        }
        for (u32 i = 0; i < 16; i++) g_mm_C[i] = 0xDEADBEEFu;
        mali_clean_range((void*)g_mm_C, sizeof(g_mm_C));
    }

#endif

    // RSD: first_tag=TAG_LOAD_STORE_4 (B0 loads GLOBAL_THREAD_ID), side_effects=1
    build_rsd(g_mm_rsd, (u64)(uintptr_t)g_mm_shader, TAG_LOAD_STORE_4_, 8, 1);
    mali_clean_range(g_mm_rsd, sizeof(g_mm_rsd));

    // Thread storage: LOCAL_STORAGE descriptor (genxml/v5.xml)
    // Word 0: TLS Size=0 (no per-thread stack), Stack Ptr Offset=0
    // Word 1: WLS Instances=31 (NO_WORKGROUP_MEM sentinel), Size=0
    // Words 2-5: TLS/WLS base pointers = 0 (unused when size=0)
    for (u32 i = 0; i < 8; i++) g_mm_ts[i] = 0;
    g_mm_ts[1] = 0x1F;
    mali_clean_range(g_mm_ts, sizeof(g_mm_ts));

    // Compute job: 16 local invocations, each writing one C element.
    //
    // MEASURED 2026-10-03, and deliberately NOT changed to 1 thread:
    //   - a groups x local_size sweep accepted ONLY 1x1 on this part; job[8][0]
    //     (size_x - 1) gates acceptance, while job_task_split is irrelevant as
    //     long as job[8] == 0;
    //   - the descriptor is correct: pan_pack_invocation matches Mesa's
    //     pan_encoder.h:196-235 line for line, and job_task_split matches
    //     panvk_vX_cmd_precomp.c:63-67;
    //   - the part reports MAX_THREADS / MAX_WORKGROUP_SIZE / MAX_BARRIER_SIZE all
    //     0x100 = 256, but GPU_THREAD_TLS_ALLOC reads 0, i.e. no thread-local
    //     storage slots exist. That is consistent with spawning a single thread
    //     and with the Job Manager refusing the job at submit (fail flag set,
    //     JS_STATUS stays READY, "done" never seen) instead of faulting mid-run.
    //
    // Setting local_size = 1 does make the job submit, but build_matmul_4x4_shader
    // derives which output element to compute from TID ("TID & 15"), so with one
    // thread TID is always 0: it would write C[0] and leave C[1..15] untouched.
    // That is a silently wrong answer, not a fix, so the 16-thread form stays
    // until the shader computes all 16 elements in one thread AND the bench
    // verifies all 16 - it currently verifies nothing, which is the other half of
    // why this cannot be "fixed" by changing one number.
    // See docs/rk3399/MALI_MATMUL_16THREAD_LIMIT.md.
    build_compute_job_v2(g_mm_job,
                         (u64)(uintptr_t)g_mm_rsd,
                         (u64)(uintptr_t)g_mm_ts,
                         (u64)(uintptr_t)g_mm_C,
                         1, 1, 1,   // num_groups
                         16, 1, 1,  // local_size
                         0);        // no next job
    mali_clean_range(g_mm_job, sizeof(g_mm_job));

    mali_mmu_dump_va((u64)(uintptr_t)g_mm_C);
    {
        u8* p = (u8*)&g_write_value_job;
        for (u32 i = 0; i < sizeof(mali_job_desc_t); i++) p[i] = 0;
        g_write_value_job.type_size = (u8)((MALI_JOB_TYPE_WRITE_VALUE & 0x7F) << 1) | 1;
        g_write_value_job.job_index = 5;
        u64* payload64 = (u64*)&g_write_value_job.payload;
        u32* payload32 = (u32*)&g_write_value_job.payload;
        payload64[0] = (u64)(uintptr_t)g_mm_C;
        payload32[2] = 3;
        payload32[3] = 0;
        mali_clean_range(&g_write_value_job, sizeof(g_write_value_job));
        mali_mmu_clear_irqs();
        result_t wv_res = mali_jm_submit(MALI_JM_COMPUTE_SLOT,
                                         (u64)(uintptr_t)&g_write_value_job, 0);
        if (wv_res == OK) {
            wv_res = mali_jm_wait(MALI_JM_COMPUTE_SLOT, 100000);
        }
        mali_invalidate_range((void*)g_mm_C, sizeof(g_mm_C));
        uart_puts(&console, "[MATMUL] pre WRITE_VALUE(0) ");
        uart_puts(&console, wv_res == OK ? "OK" : "FAIL");
        uart_puts(&console, " C0=0x");
        uart_put_hex(&console, g_mm_C[0]);
        uart_puts(&console, "\r\n");
        for (u32 i = 0; i < 16; i++) g_mm_C[i] = 0xDEADBEEFu;
        mali_clean_range((void*)g_mm_C, sizeof(g_mm_C));
        uart_puts(&console, "[MATMUL] reset C to DEADBEEF before shader submit\r\n");
    }

    // Submit and wait with diagnostics
    mali_mmu_clear_irqs();
    result_t res = mali_jm_submit(MALI_JM_COMPUTE_SLOT,
                                   (u64)(uintptr_t)g_mm_job, 0);
    if (res != OK) {
        uart_puts(&console, "[MATMUL] submit failed res=");
        uart_put_hex(&console, (u32)res);
        uart_puts(&console, "\r\n");
        return res;
    }
    mali_jm_wait_diag_t wd = {0};
    res = mali_jm_wait_ex(MALI_JM_COMPUTE_SLOT, 500000, &wd);
    uart_puts(&console, "[MATMUL] wait res=");
    uart_put_hex(&console, (u32)res);
    uart_puts(&console, " done=");
    uart_put_hex(&console, wd.saw_done);
    uart_puts(&console, " fail=");
    uart_put_hex(&console, wd.saw_fail);
    uart_puts(&console, " js=0x");
    uart_put_hex(&console, wd.js_status);
    uart_puts(&console, " irq=0x");
    uart_put_hex(&console, wd.irq_raw);
    uart_puts(&console, "\r\n");
    mali_invalidate_range(g_mm_ts, sizeof(g_mm_ts));
    uart_puts(&console, "[MATMUL] tls w0=0x");
    uart_put_hex(&console, g_mm_ts[0]);
    uart_puts(&console, " w1=0x");
    uart_put_hex(&console, g_mm_ts[1]);
    uart_puts(&console, "\r\n");
    uart_puts(&console, "[MATMUL] shader: ");
    for (u32 i = 0; i < MATMUL_SHADER_WORDS && i < 96; i++) {
        uart_put_hex(&console, g_mm_shader[i]);
        uart_puts(&console, " ");
    }
    uart_puts(&console, "\r\n");
    u64 mm_mov_lo_ok = (u64)g_mm_shader[0] | ((u64)g_mm_shader[1] << 32);
    u64 mm_mov_word1_ok = (mm_mov_lo_ok >> 8) & 0x0FFFFFFFFFFFFFFFULL;
    u64 mm_ld_a_lo_ok = (u64)g_mm_shader[24] | ((u64)g_mm_shader[25] << 32);
    u64 mm_ld_a_word1_ok = (mm_ld_a_lo_ok >> 8) & 0x0FFFFFFFFFFFFFFFULL;
    u64 mm_ld_bt_lo_ok = (u64)g_mm_shader[48] | ((u64)g_mm_shader[49] << 32);
    u64 mm_ld_bt_word1_ok = (mm_ld_bt_lo_ok >> 8) & 0x0FFFFFFFFFFFFFFFULL;
    u64 mm_st_lo_ok = (u64)g_mm_shader[76] | ((u64)g_mm_shader[77] << 32);
    u64 mm_st_hi_ok = (u64)g_mm_shader[78] | ((u64)g_mm_shader[79] << 32);
    u64 mm_st_word1_ok = (mm_st_lo_ok >> 8) & 0x0FFFFFFFFFFFFFFFULL;
    u64 mm_st_word2_ok = ((mm_st_hi_ok >> 4) & 0x0FFFFFFFFFFFFFFFULL);
    uart_puts(&console, "[MATMUL] ldst_mov TID decode: ");
    mali_decode_st32_word(mm_mov_word1_ok);
    uart_puts(&console, "[MATMUL] load A decode: ");
    mali_decode_st32_word(mm_ld_a_word1_ok);
    uart_puts(&console, "[MATMUL] load BT decode: ");
    mali_decode_st32_word(mm_ld_bt_word1_ok);
    uart_puts(&console, "[MATMUL] store decode: ");
    mali_decode_st32_word(mm_st_word1_ok);
    uart_puts(&console, "[MATMUL] store nop decode: ");
    mali_decode_st32_word(mm_st_word2_ok);
    uart_puts(&console, "[MATMUL] job w4=0x");
    uart_put_hex(&console, g_mm_job[4]);
    uart_puts(&console, " w8=0x");
    uart_put_hex(&console, g_mm_job[8]);
    uart_puts(&console, " w9=0x");
    uart_put_hex(&console, g_mm_job[9]);
    uart_puts(&console, " w10=0x");
    uart_put_hex(&console, g_mm_job[10]);
    uart_puts(&console, "\r\n[MATMUL] draw w16=0x");
    uart_put_hex(&console, g_mm_job[16]);
    uart_puts(&console, " w20=0x");
    uart_put_hex(&console, g_mm_job[20]);
    uart_puts(&console, " w30=0x");
    uart_put_hex(&console, g_mm_job[30]);
    uart_puts(&console, " w44=0x");
    uart_put_hex(&console, g_mm_job[44]);
    uart_puts(&console, "\r\n[MATMUL] rsd w0=0x");
    uart_put_hex(&console, g_mm_rsd[0]);
    uart_puts(&console, " w1=0x");
    uart_put_hex(&console, g_mm_rsd[1]);
    uart_puts(&console, " w4=0x");
    uart_put_hex(&console, g_mm_rsd[4]);
    uart_puts(&console, "\r\n[MATMUL] addrs shader=0x");
    uart_put_hex(&console, (u32)(uintptr_t)g_mm_shader);
    uart_puts(&console, " A=0x");
    uart_put_hex(&console, (u32)(uintptr_t)g_mm_A);
    uart_puts(&console, " BT=0x");
    uart_put_hex(&console, (u32)(uintptr_t)g_mm_BT);
    uart_puts(&console, " C=0x");
    uart_put_hex(&console, (u32)(uintptr_t)g_mm_C);
    uart_puts(&console, "\r\n");
    if (res != OK) {
        // Invalidate CPU cache to see GPU-written job header
        mali_invalidate_range(g_mm_job, sizeof(g_mm_job));
        // Fault pointer (w2-w3) tells us which address caused the bus fault
        uart_puts(&console, "[MATMUL] fault_ptr=0x");
        uart_put_hex(&console, g_mm_job[3]);
        uart_put_hex(&console, g_mm_job[2]);
        uart_puts(&console, " exc=0x");
        uart_put_hex(&console, g_mm_job[0]);
        uart_puts(&console, " task=0x");
        uart_put_hex(&console, g_mm_job[1]);
        uart_puts(&console, "\r\n");
        mali_mmu_dump(0);
        // TLS descriptor verification
        mali_invalidate_range(g_mm_ts, sizeof(g_mm_ts));
        uart_puts(&console, "[MATMUL] tls w0=0x");
        uart_put_hex(&console, g_mm_ts[0]);
        uart_puts(&console, " w1=0x");
        uart_put_hex(&console, g_mm_ts[1]);
        uart_puts(&console, "\r\n");
        // Shader words
        uart_puts(&console, "[MATMUL] shader: ");
        for (u32 i = 0; i < MATMUL_SHADER_WORDS && i < 96; i++) {
            uart_put_hex(&console, g_mm_shader[i]);
            uart_puts(&console, " ");
        }
        uart_puts(&console, "\r\n");
        u64 mm_mov_lo = (u64)g_mm_shader[0] | ((u64)g_mm_shader[1] << 32);
        u64 mm_mov_word1 = (mm_mov_lo >> 8) & 0x0FFFFFFFFFFFFFFFULL;
        u64 mm_ld_a_lo = (u64)g_mm_shader[24] | ((u64)g_mm_shader[25] << 32);
        u64 mm_ld_a_word1 = (mm_ld_a_lo >> 8) & 0x0FFFFFFFFFFFFFFFULL;
        u64 mm_ld_bt_lo = (u64)g_mm_shader[48] | ((u64)g_mm_shader[49] << 32);
        u64 mm_ld_bt_word1 = (mm_ld_bt_lo >> 8) & 0x0FFFFFFFFFFFFFFFULL;
        u64 mm_st_lo = (u64)g_mm_shader[76] | ((u64)g_mm_shader[77] << 32);
        u64 mm_st_hi = (u64)g_mm_shader[78] | ((u64)g_mm_shader[79] << 32);
        u64 mm_st_word1 = (mm_st_lo >> 8) & 0x0FFFFFFFFFFFFFFFULL;
        u64 mm_st_word2 = ((mm_st_hi >> 4) & 0x0FFFFFFFFFFFFFFFULL);
        uart_puts(&console, "[MATMUL] ldst_mov TID decode: ");
        mali_decode_st32_word(mm_mov_word1);
        uart_puts(&console, "[MATMUL] load A decode: ");
        mali_decode_st32_word(mm_ld_a_word1);
        uart_puts(&console, "[MATMUL] load BT decode: ");
        mali_decode_st32_word(mm_ld_bt_word1);
        uart_puts(&console, "[MATMUL] store decode: ");
        mali_decode_st32_word(mm_st_word1);
        uart_puts(&console, "[MATMUL] store nop decode: ");
        mali_decode_st32_word(mm_st_word2);
        // Job descriptor key words
        uart_puts(&console, "[MATMUL] job w4=0x");
        uart_put_hex(&console, g_mm_job[4]);
        uart_puts(&console, " w8=0x");
        uart_put_hex(&console, g_mm_job[8]);
        uart_puts(&console, " w9=0x");
        uart_put_hex(&console, g_mm_job[9]);
        uart_puts(&console, " w10=0x");
        uart_put_hex(&console, g_mm_job[10]);
        uart_puts(&console, "\r\n[MATMUL] draw w16=0x");
        uart_put_hex(&console, g_mm_job[16]);
        uart_puts(&console, " w30=0x");
        uart_put_hex(&console, g_mm_job[30]);
        uart_puts(&console, " w44=0x");
        uart_put_hex(&console, g_mm_job[44]);
        uart_puts(&console, "\r\n[MATMUL] rsd w0=0x");
        uart_put_hex(&console, g_mm_rsd[0]);
        uart_puts(&console, " w1=0x");
        uart_put_hex(&console, g_mm_rsd[1]);
        uart_puts(&console, " w4=0x");
        uart_put_hex(&console, g_mm_rsd[4]);
        uart_puts(&console, "\r\n[MATMUL] addrs shader=0x");
        uart_put_hex(&console, (u32)(uintptr_t)g_mm_shader);
        uart_puts(&console, " A=0x");
        uart_put_hex(&console, (u32)(uintptr_t)g_mm_A);
        uart_puts(&console, " BT=0x");
        uart_put_hex(&console, (u32)(uintptr_t)g_mm_BT);
        uart_puts(&console, " C=0x");
        uart_put_hex(&console, (u32)(uintptr_t)g_mm_C);
        uart_puts(&console, "\r\n");
        return res;
    }

    {
        build_cache_flush_job(&g_cache_flush_job);
        u64 cf_addr = (u64)(uintptr_t)&g_cache_flush_job;
        mali_mmu_clear_irqs();
        result_t cf_res = mali_jm_submit(MALI_JM_COMPUTE_SLOT, cf_addr, 0);
        if (cf_res == OK) {
            cf_res = mali_jm_wait(MALI_JM_COMPUTE_SLOT, 100000);
        }
        uart_puts(&console, "[MATMUL] post-compute L2 flush ");
        uart_puts(&console, cf_res == OK ? "OK\r\n" : "FAIL\r\n");
        if (cf_res != OK) return cf_res;
    }

    // Flush GPU caches and invalidate CPU view
    mali_invalidate_range((void*)g_mm_C, sizeof(g_mm_C));

    u32 errors = 0;
    for (u32 i = 0; i < 16; i++) {
        u32 row = i >> 2;
        u32 col = i & 3;
        u32 expect = (row == col) ? F32_ONE : F32_ZERO;
        if (g_mm_C[i] != expect) errors++;
    }

    uart_puts(&console, "[MATMUL] full 4x4 dot4 result: ");
    if (errors == 0) {
        uart_puts(&console, "*** PASS *** (C=I)\r\n");
    } else {
        uart_puts(&console, "FAIL errors=");
        uart_put_hex(&console, errors);
        uart_puts(&console, "\r\n  C[0..15] = ");
        for (u32 i = 0; i < 16; i++) {
            uart_put_hex(&console, g_mm_C[i]);
            uart_puts(&console, (i < 15) ? "," : "\r\n");
        }
    }

    return (errors == 0) ? OK : ERR_INVALID_PARAM;
}

// High-level API entry: run a single inference via GPU compute (async)
result_t mali_infer_gpu_async(const telemetry_t* in, inference_result_t* out) {
    (void)in; (void)out;
    return ERR_INVALID_PARAM;
}
