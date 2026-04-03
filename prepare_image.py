# prepare_image.py
import os
import sys


def create_stage2_image(input_bin, output_img):
    if not os.path.exists(input_bin):
        print(f"Error: {input_bin} not found!")
        return 1

    with open(input_bin, "rb") as f:
        kernel_data = f.read()

    padding_needed = (512 - (len(kernel_data) % 512)) % 512
    stage2_data = kernel_data + (b"\x00" * padding_needed)

    with open(output_img, "wb") as f:
        f.write(stage2_data)

    print(f"[OK] Stage2 image created: {output_img}")
    print(f"[INFO] Input size: {len(kernel_data)} bytes")
    print(f"[INFO] Output size: {len(stage2_data)} bytes (512-byte aligned)")
    print("[IMPORTANT] RK3399 BootROM needs a real idbloader.img at sector 64.")
    print("[NEXT] Flash idbloader.img -> sector 64, this stage2 image -> sector 16384.")
    return 0


if __name__ == "__main__":
    in_file = "kernel.bin"
    out_file = "H_Exo_Aleph.img"

    if len(sys.argv) >= 2:
        in_file = sys.argv[1]
    if len(sys.argv) >= 3:
        out_file = sys.argv[2]

    raise SystemExit(create_stage2_image(in_file, out_file))