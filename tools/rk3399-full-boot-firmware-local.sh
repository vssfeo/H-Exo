#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OUTPUT_DIR="${2:-${REPO_ROOT}/artifacts/rk3399-full-boot-local}"
TFA_TAG="${3:-v2.14.0}"
UBOOT_REPO="${4:-https://github.com/u-boot/u-boot.git}"
UBOOT_BRANCH="${5:-v2026.04}"
DDR_MHZ="${6:-800}"

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

require_cmd git
require_cmd make
require_cmd sed
require_cmd grep
require_cmd awk
require_cmd sha256sum
require_cmd aarch64-linux-gnu-gcc
require_cmd aarch64-linux-gnu-objcopy
require_cmd arm-none-eabi-gcc

PYTHON_BIN=""
for c in python3 python py py.exe; do
  if command -v "$c" >/dev/null 2>&1; then
    PYTHON_BIN="$c"
    break
  fi
done

if [[ -z "${PYTHON_BIN}" ]]; then
  echo "Missing required command: python3/python/py" >&2
  exit 1
fi

echo "[LOCAL PIPELINE] python=${PYTHON_BIN}"

WORK_DIR="$(mktemp -d)"
cleanup() {
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

echo "[LOCAL PIPELINE] repo=${REPO_ROOT}"
echo "[LOCAL PIPELINE] work=${WORK_DIR}"
echo "[LOCAL PIPELINE] out=${OUTPUT_DIR}"

git clone --depth 1 --branch "${TFA_TAG}" \
  https://github.com/TrustedFirmware-A/trusted-firmware-a.git \
  "${WORK_DIR}/tfa"

# PMUSRAM_RSIZE 8 KiB -> 16 KiB
sed -i '/PMUSRAM_RSIZE/s/SIZE_K(8)/SIZE_K(16)/' \
  "${WORK_DIR}/tfa/plat/rockchip/rk3399/include/shared/addressmap_shared.h"

# Patch plat_rockchip_pmu_init to keep debug domain powered (inline, idempotent)
# Same logic as CI workflow awk-based insertion.
echo "[LOCAL PIPELINE] patch: PMU keep-debug-powered (inline awk)"
F="${WORK_DIR}/tfa/plat/rockchip/rk3399/drivers/pmu/pmu.c"
if grep -q DBG_NOPWERDWN_L0_EN "$F"; then
  echo "Already patched."
else
  awk '
    /^void plat_rockchip_pmu_init/ { in_fn = 1 }
    { print }
    in_fn && /init_pmu_counts\(\);/ {
      print ""
      print "\t/*"
      print "\t * H-Exo: Keep per-core debug power domain alive so PMCCNTR_EL0"
      print "\t * and the PMU event counters keep ticking when non-secure"
      print "\t * software (EL2/EL1) reads them. Without this, the PMU regs"
      print "\t * are read/writeable but their input clock is gated together"
      print "\t * with the core power domain, freezing the cycle counter."
      print "\t */"
      print "\tmmio_setbits_32(PMU_BASE + PMU_SFT_CON,"
      print "\t\t\tBIT(DBG_NOPWERDWN_L0_EN) |"
      print "\t\t\tBIT(DBG_NOPWERDWN_L1_EN) |"
      print "\t\t\tBIT(DBG_NOPWERDWN_L2_EN) |"
      print "\t\t\tBIT(DBG_NOPWERDWN_L3_EN) |"
      print "\t\t\tBIT(DBG_NO_PWERDWN_B0_EN) |"
      print "\t\t\tBIT(DBG_NO_PWERDWN_B1_EN));"
      in_fn = 0
    }
  ' "$F" > "${F}.new"
  mv "${F}.new" "$F"
fi
grep -n DBG_NOPWERDWN_L0_EN "$F"
grep -n DBG_NO_PWERDWN_B0_EN "$F"

# SMPEN + H-Exo diagnostic SiP patch (inline, idempotent)
cat > "${WORK_DIR}/h_exo_patch_sip.py" <<'PY'
from pathlib import Path
import textwrap

p = Path("plat/rockchip/rk3399/plat_sip_calls.c")
if not p.exists():
    raise SystemExit(f"missing {p}")

s = p.read_text()
marker = "H_EXO_SIP_FW_INFO"
if marker in s:
    print("H-Exo SiP patch already present")
    raise SystemExit(0)

helper = textwrap.dedent(r'''

#define RK_SIP_SMPEN_GET            U(0x82000099)
#define RK_SIP_SMPEN_SET            U(0x8200009A)
#define RK_SIP_SMPEN_GET_64         U(0xC2000099)
#define RK_SIP_SMPEN_SET_64         U(0xC200009A)
#define H_EXO_SIP_FW_INFO           U(0xC20000A0)
#define H_EXO_SIP_CPU_DIAG          U(0xC20000A1)
#define H_EXO_FW_MAGIC              0x4845584F524B3339ULL
#define H_EXO_FW_VERSION            0x0000000000000001ULL
#define A72_CPUECTLR_SMP_BIT        (1ULL << 6)

static uint64_t rk_smpen_get(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, S3_1_C15_C2_1" : "=r"(v));
	return (v & A72_CPUECTLR_SMP_BIT) ? 1ULL : 0ULL;
}

static uint64_t rk_smpen_set(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, S3_1_C15_C2_1" : "=r"(v));
	if ((v & A72_CPUECTLR_SMP_BIT) == 0ULL) {
		v |= A72_CPUECTLR_SMP_BIT;
		__asm__ volatile ("msr S3_1_C15_C2_1, %0\n\tisb" :: "r"(v));
	}
	return rk_smpen_get();
}

static uint64_t h_exo_read_mpidr(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, mpidr_el1" : "=r"(v));
	return v;
}

static uint64_t h_exo_read_midr(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, midr_el1" : "=r"(v));
	return v;
}

static uint64_t h_exo_read_currentel(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, CurrentEL" : "=r"(v));
	return v;
}
''')

anchor = "uint64_t sip_smc_dram("
if anchor in s:
    s = s.replace(anchor, helper + "\n" + anchor, 1)
else:
    include_pos = s.rfind("#include")
    if include_pos < 0:
        raise SystemExit("cannot find include/helper insertion point")
    line_end = s.find("\n", include_pos)
    s = s[:line_end + 1] + helper + s[line_end + 1:]

cases = textwrap.dedent(r'''
	case RK_SIP_SMPEN_GET:
	case RK_SIP_SMPEN_GET_64:
		SMC_RET1(handle, rk_smpen_get());
	case RK_SIP_SMPEN_SET:
	case RK_SIP_SMPEN_SET_64:
		SMC_RET1(handle, rk_smpen_set());
	case H_EXO_SIP_FW_INFO:
		SMC_RET2(handle, H_EXO_FW_MAGIC, H_EXO_FW_VERSION);
	case H_EXO_SIP_CPU_DIAG:
		SMC_RET4(handle, h_exo_read_mpidr(), h_exo_read_midr(),
			 h_exo_read_currentel(), rk_smpen_get());
''')

switch_anchor = "switch (smc_fid) {"
if switch_anchor not in s:
    raise SystemExit("cannot find SiP switch (smc_fid)")
s = s.replace(switch_anchor, switch_anchor + cases, 1)

p.write_text(s)
print("H-Exo SiP patch applied")
PY

(
  cd "${WORK_DIR}/tfa"
  "${PYTHON_BIN}" "${WORK_DIR}/h_exo_patch_sip.py"
)

# Patch RK3399 SiP GICR WAKER diagnostic calls (inline Python, idempotent)
# Same logic as CI workflow h_exo_patch_gicr_sip.py.
echo "[LOCAL PIPELINE] patch: GICR WAKER SiP calls (inline python)"
cat > "${WORK_DIR}/h_exo_patch_gicr_sip.py" <<'PY'
from pathlib import Path
import textwrap

p = Path("plat/rockchip/rk3399/plat_sip_calls.c")
if not p.exists():
    raise SystemExit(f"missing {p}")

s = p.read_text()
marker = "RK_SIP_GICR_WAKER_GET"
if marker in s:
    print("H-Exo GICR SiP patch already present")
    raise SystemExit(0)

helper = textwrap.dedent(r'''

#define RK_SIP_GICR_WAKER_GET      U(0x820000A2)
#define RK_SIP_GICR_WAKE_TRY       U(0x820000A3)
#define RK_SIP_GICR_WAKER_GET_64   U(0xC20000A2)
#define RK_SIP_GICR_WAKE_TRY_64    U(0xC20000A3)

#define RK3399_GICR_BASE_PHYS      ULL(0xFEF00000)
#define RK3399_GICR_STRIDE         ULL(0x20000)
#define RK3399_GICR_WAKER_OFF      ULL(0x14)
#define RK3399_CORE_COUNT          U(6)
#define RK_GICR_WAKER_PS           U(2)   /* bit 1: ProcessorSleep */
#define RK_GICR_WAKER_CA           U(4)   /* bit 2: ChildrenAsleep */

#define RK_GICR_WAKE_OK            U(0)
#define RK_GICR_WAKE_ERR_CORE      U(1)
#define RK_GICR_WAKE_ERR_TIMEOUT   U(2)
#define RK_GICR_WAKE_ERR_WRITE_IGN U(4)

static uintptr_t rk3399_gicr_waker_addr(uint64_t core)
{
	if (core >= RK3399_CORE_COUNT)
		return (uintptr_t)0;

	return (uintptr_t)(RK3399_GICR_BASE_PHYS +
			(core * RK3399_GICR_STRIDE) +
			RK3399_GICR_WAKER_OFF);
}

static uint32_t rk3399_gicr_waker_read(uint64_t core, uint32_t *status)
{
	uintptr_t a = rk3399_gicr_waker_addr(core);

	if (a == 0U) {
		if (status != NULL)
			*status = RK_GICR_WAKE_ERR_CORE;
		return 0U;
	}

	if (status != NULL)
		*status = RK_GICR_WAKE_OK;
	return *((volatile uint32_t *)a);
}

static uint64_t rk3399_gicr_wake_try(uint64_t core, uint32_t *before,
					      uint32_t *after)
{
	uintptr_t a = rk3399_gicr_waker_addr(core);
	uint32_t v, vr, st = RK_GICR_WAKE_OK;
	int i;

	if (a == 0U) {
		if (before != NULL)
			*before = 0U;
		if (after != NULL)
			*after = 0U;
		return RK_GICR_WAKE_ERR_CORE;
	}

	v = *((volatile uint32_t *)a);
	if (before != NULL)
		*before = v;

	/* Phase A: force ProcessorSleep=1 and wait for ChildrenAsleep=1 */
	v |= RK_GICR_WAKER_PS;
	*((volatile uint32_t *)a) = v;
	vr = *((volatile uint32_t *)a);
	if ((vr & RK_GICR_WAKER_PS) == 0U)
		st |= RK_GICR_WAKE_ERR_WRITE_IGN;

	for (i = 0; i < 10000; i++) {
		vr = *((volatile uint32_t *)a);
		if ((vr & RK_GICR_WAKER_CA) != 0U)
			break;
	}
	if (i == 10000)
		st |= RK_GICR_WAKE_ERR_TIMEOUT;

	/* Phase B: clear ProcessorSleep and wait for ChildrenAsleep=0 */
	v = vr & ~RK_GICR_WAKER_PS;
	*((volatile uint32_t *)a) = v;
	vr = *((volatile uint32_t *)a);
	if ((vr & RK_GICR_WAKER_PS) != 0U)
		st |= RK_GICR_WAKE_ERR_WRITE_IGN;

	for (i = 0; i < 10000; i++) {
		vr = *((volatile uint32_t *)a);
		if ((vr & RK_GICR_WAKER_CA) == 0U)
			break;
	}
	if (i == 10000)
		st |= RK_GICR_WAKE_ERR_TIMEOUT;

	if (after != NULL)
		*after = vr;
	return st;
}
''')

anchor = "uint64_t sip_smc_dram("
if anchor in s:
    s = s.replace(anchor, helper + "\n" + anchor, 1)
else:
    include_pos = s.rfind("#include")
    if include_pos < 0:
        raise SystemExit("cannot find include/helper insertion point")
    line_end = s.find("\n", include_pos)
    s = s[:line_end + 1] + helper + s[line_end + 1:]

cases = textwrap.dedent(r'''
	case RK_SIP_GICR_WAKER_GET:
	case RK_SIP_GICR_WAKER_GET_64: {
		uint32_t st;
		uint32_t w = rk3399_gicr_waker_read(x1, &st);
		SMC_RET2(handle, st, w);
		}

	case RK_SIP_GICR_WAKE_TRY:
	case RK_SIP_GICR_WAKE_TRY_64: {
		uint32_t b = 0U, a = 0U;
		uint64_t st = rk3399_gicr_wake_try(x1, &b, &a);
		SMC_RET3(handle, st, b, a);
		}
''')

switch_anchor = "switch (smc_fid) {"
if switch_anchor not in s:
    raise SystemExit("cannot find SiP switch (smc_fid)")
s = s.replace(switch_anchor, switch_anchor + cases, 1)

p.write_text(s)
print("H-Exo GICR SiP patch applied")
PY

(
  cd "${WORK_DIR}/tfa"
  "${PYTHON_BIN}" "${WORK_DIR}/h_exo_patch_gicr_sip.py"
)

(
  cd "${WORK_DIR}/tfa"
  make -j"$(nproc)" \
    CROSS_COMPILE=aarch64-linux-gnu- \
    PLAT=rk3399 \
    DEBUG=0 \
    LOG_LEVEL=20 \
    ENABLE_ASSERTIONS=0 \
    RK3399_BAUDRATE=1500000 \
    bl31
)

BL31_PATH="$(find "${WORK_DIR}/tfa/build" -path '*/bl31/bl31.elf' -type f | head -1)"
if [[ -z "${BL31_PATH}" || ! -s "${BL31_PATH}" ]]; then
  echo "BL31 build output not found" >&2
  exit 1
fi

git clone --depth 1 --branch "${UBOOT_BRANCH}" \
  "${UBOOT_REPO}" \
  "${WORK_DIR}/u-boot"

# U-Boot Makefile rejects source dirs with colons (Windows C:/...).
# Remove the check so Git Bash builds work.
sed -i '/source directory cannot contain spaces or colons/d' \
  "${WORK_DIR}/u-boot/Makefile"
sed -i '/ifneq ($$(findstring :,$$(CURDIR)),)/,/endif/d' \
  "${WORK_DIR}/u-boot/Makefile"

(
  cd "${WORK_DIR}/u-boot"

  DTSI_M4="$(find . -path '*/rk3399-nanopi-m4-u-boot.dtsi' -type f | head -1)"
  DTSI_NP4="$(find . -path '*/rk3399-nanopi4.dtsi' -type f | head -1)"
  test -n "${DTSI_M4}"
  test -n "${DTSI_NP4}"

  sed -i 's|rk3399-sdram-lpddr3-samsung-4GB-1866|rk3399-sdram-ddr3-1600|' "${DTSI_M4}"

  export DTSI_NP4
  cat > "${WORK_DIR}/patch_u_boot_dtsi.py" <<'PY'
import os
from pathlib import Path

p = Path(os.environ["DTSI_NP4"])
s = p.read_text()
block = '''
&i2c0 {
        clock-frequency = <400000>;
        status = "okay";

        rk808@1b {
                compatible = "rockchip,rk808";
                reg = <0x1b>;
                #clock-cells = <1>;
                clock-output-names = "xin32k", "rk808-clkout2";
                interrupt-parent = <&gpio1>;
                interrupts = <21 8>;
                pinctrl-names = "default";
                pinctrl-0 = <&pmic_int_l>;
                rockchip,system-power-controller;
                wakeup-source;

                vdd_cpu_l: rk808-dcdc1 {
                        regulator-name = "vdd_cpu_l";
                        regulator-always-on;
                        regulator-boot-on;
                        regulator-min-microvolt = <750000>;
                        regulator-max-microvolt = <1350000>;
                };

                vcc1v8_dvp: rk808-ldo1 { regulator-name = "vcc1v8_dvp"; };
                vcc3v0_touch: rk808-ldo2 { regulator-name = "vcc3v0_touch"; };
                vcc1v8_pmupll: rk808-ldo3 { regulator-name = "vcc1v8_pmupll"; };
                vcc_sd: rk808-ldo4 {
                        regulator-name = "vcc_sd";
                        regulator-min-microvolt = <3300000>;
                        regulator-max-microvolt = <3300000>;
                };
                vcc5v0_usb: rk808-ldo5 { regulator-name = "vcc5v0_usb"; };
                vcc1v8_codec: rk808-ldo6 { regulator-name = "vcc1v8_codec"; };
                vcc_1v8: rk808-ldo7 { regulator-name = "vcc_1v8"; };
                vcc_3v0: rk808-ldo8 { regulator-name = "vcc_3v0"; };
                vcca_1v8: rk808-dcdc2 { regulator-name = "vcca_1v8"; };
                vcc_sdio: rk808-dcdc3 { regulator-name = "vcc_sdio"; };
                vcc5v0_host: rk808-dcdc4 { regulator-name = "vcc5v0_host"; };
        };

        vdd_cpu_b: syr827@40 {
                compatible = "silergy,syr827";
                reg = <0x40>;
                fcs,suspend-voltage-selector = <1>;
                regulator-name = "vdd_cpu_b";
                regulator-min-microvolt = <712500>;
                regulator-max-microvolt = <1500000>;
                regulator-always-on;
                regulator-boot-on;
        };
};
'''

if "silergy,syr827" not in s:
    s = s.rstrip() + "\n" + block

if "cpu-supply" not in s:
    s = s.rstrip() + "\n" + '''
&cpu_b0 { cpu-supply = <&vdd_cpu_b>; };
&cpu_b1 { cpu-supply = <&vdd_cpu_b>; };
&cpu_l0 { cpu-supply = <&vdd_cpu_l>; };
&cpu_l1 { cpu-supply = <&vdd_cpu_l>; };
&cpu_l2 { cpu-supply = <&vdd_cpu_l>; };
&cpu_l3 { cpu-supply = <&vdd_cpu_l>; };
'''

if "&io_domains" not in s:
    s = s.rstrip() + "\n" + '''
&io_domains {
        status = "okay";
        bt656-supply = <&vcc1v8_dvp>;
        audio-supply = <&vcc1v8_dvp>;
        sdmmc-supply = <&vcc_sd>;
        gpio1830-supply = <&vcc_3v0>;
};
'''

p.write_text(s)
PY
  "${PYTHON_BIN}" "${WORK_DIR}/patch_u_boot_dtsi.py"

  make nanopi-m4-rk3399_defconfig

  scripts/config --disable SPL_FIT_SIGNATURE
  scripts/config --enable CMD_I2C
  scripts/config --enable CMD_REGULATOR
  scripts/config --enable DM_REGULATOR
  scripts/config --enable DM_PMIC_FAN53555
  scripts/config --enable DM_REGULATOR_FAN53555
  scripts/config --enable DM_PMIC_RK8XX
  scripts/config --enable DM_REGULATOR_RK8XX
  scripts/config --enable CMD_CLK
  scripts/config --enable CMD_NET
  scripts/config --enable CMD_TFTPBOOT
  scripts/config --enable CMD_MII
  scripts/config --enable CMD_MDIO
  scripts/config --enable PHY_GIGE
  scripts/config --enable CMD_PCAP
  scripts/config --enable NETCONSOLE
  scripts/config --enable CMD_SNTP
  scripts/config --enable BOOTP_NTPSERVER
  scripts/config --enable CMD_TFTPPUT
  scripts/config --enable CMD_TFTPSRV
  scripts/config --enable NET_TFTP_VARS
  scripts/config --set-val TFTP_WINDOWSIZE 16
  scripts/config --enable CMD_WDT
  scripts/config --enable CMD_CONFIG
  scripts/config --enable USE_PREBOOT
  scripts/config --set-str PREBOOT 'regulator dev vdd_cpu_b; regulator value 1200000 -f; regulator dev vdd_cpu_l; regulator value 1000000 -f'

  make olddefconfig
  make -j"$(nproc)" CROSS_COMPILE=aarch64-linux-gnu- BL31="${BL31_PATH}"

  test -s idbloader.img
  test -s u-boot.itb
)

mkdir -p "${OUTPUT_DIR}"
cp -f "${WORK_DIR}/u-boot/idbloader.img" "${OUTPUT_DIR}/idbloader.img"
cp -f "${WORK_DIR}/u-boot/u-boot.itb" "${OUTPUT_DIR}/u-boot.itb"
cp -f "${BL31_PATH}" "${OUTPUT_DIR}/bl31.elf"

(
  cd "${OUTPUT_DIR}"
  sha256sum idbloader.img u-boot.itb bl31.elf > SHA256SUMS
)

cat > "${OUTPUT_DIR}/README_LOCAL_BUILD.txt" <<EOF
RK3399 NanoPi M4 local full-boot build
======================================
TF-A tag: ${TFA_TAG}
U-Boot: ${UBOOT_BRANCH}
DDR profile: ${DDR_MHZ}

Artifacts:
- idbloader.img  (flash LBA 0x40)
- u-boot.itb     (flash LBA 0x4000)
- bl31.elf

Patch into Armbian image (PowerShell):
  .\\tools\\patch_etcher_image.ps1 -ArmbianPath C:\\path\\Armbian.img -IdbLoaderPath ${OUTPUT_DIR}/idbloader.img -UBootItbPath ${OUTPUT_DIR}/u-boot.itb -OutputPath C:\\path\\Armbian_patched.img
EOF

echo "[LOCAL PIPELINE] done"
ls -lh "${OUTPUT_DIR}/idbloader.img" "${OUTPUT_DIR}/u-boot.itb" "${OUTPUT_DIR}/bl31.elf"
