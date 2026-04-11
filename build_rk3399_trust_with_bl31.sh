#!/usr/bin/env bash
set -euo pipefail

# Build a legacy RK3399 trust.img (BL3X slot) using rkbin and a supplied BL31.
# Intended to run on Linux / WSL / VDS where tools/trust_merger is executable.
#
# Same layout as GitHub Actions workflow: .github/workflows/rk3399-bl31-trust.yml
# (download the artifact if you do not have rkbin locally).

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RKBIN_DIR="${ROOT_DIR}/third_party/rkbin"
BL31_SRC="${1:-}"
OUT_DIR="${2:-${ROOT_DIR}/artifacts/rk3399-trust}"
BUILD_FLAVOR="${3:-default}"

if [[ -z "${BL31_SRC}" ]]; then
  echo "usage: $0 /path/to/bl31.elf [output-dir] [build_flavor]" >&2
  echo "  build_flavor: default | bl31_only (no BL32; uses patches/rktrust/RK3399TRUST-BL31ONLY.ini)" >&2
  exit 1
fi

if [[ ! -f "${BL31_SRC}" ]]; then
  echo "BL31 not found: ${BL31_SRC}" >&2
  exit 1
fi

if [[ ! -d "${RKBIN_DIR}" ]]; then
  echo "rkbin directory not found: ${RKBIN_DIR}" >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"

TMP_DIR="$(mktemp -d)"
cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

cp -a "${RKBIN_DIR}/." "${TMP_DIR}/"
cp -f "${BL31_SRC}" "${TMP_DIR}/bin/rk33/rk3399_bl31_v1.36.elf"

INI="RKTRUST/RK3399TRUST.ini"
if [[ "${BUILD_FLAVOR}" == "bl31_only" ]] || [[ "${BUILD_FLAVOR}" == "armbian_sd" ]]; then
  cp -f "${ROOT_DIR}/patches/rktrust/RK3399TRUST-BL31ONLY.ini" "${TMP_DIR}/RKTRUST/RK3399TRUST-BL31ONLY.ini"
  INI="RKTRUST/RK3399TRUST-BL31ONLY.ini"
fi

pushd "${TMP_DIR}" >/dev/null
chmod +x tools/trust_merger
./tools/trust_merger "$INI"
popd >/dev/null

if [[ ! -f "${TMP_DIR}/trust.img" ]]; then
  echo "trust_merger did not produce trust.img" >&2
  exit 1
fi

cp -f "${TMP_DIR}/trust.img" "${OUT_DIR}/trust.img"
cp -f "${TMP_DIR}/bin/rk33/rk3399_bl31_v1.36.elf" "${OUT_DIR}/bl31.elf"
printf '%s\n' "${BUILD_FLAVOR}" > "${OUT_DIR}/build-flavor.txt"

echo "Generated:"
ls -lh "${OUT_DIR}/trust.img" "${OUT_DIR}/bl31.elf"
sha256sum "${OUT_DIR}/trust.img" "${OUT_DIR}/bl31.elf"
echo "Flavor: ${BUILD_FLAVOR}"
