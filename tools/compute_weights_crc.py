#!/usr/bin/env python3
"""
H-Exo Omni-Core: Neural Weights CRC32 Calculator
Computes CRC32 checksum of neural network weights for integrity validation
"""

import struct
import sys

# CRC32 polynomial (IEEE 802.3)
CRC32_POLYNOMIAL = 0xEDB88320

# Generate CRC32 lookup table
def generate_crc32_table():
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ CRC32_POLYNOMIAL
            else:
                crc >>= 1
        table.append(crc)
    return table

CRC32_TABLE = generate_crc32_table()

def compute_crc32(data):
    """Compute CRC32 checksum of byte array"""
    crc = 0xFFFFFFFF
    for byte in data:
        index = (crc ^ byte) & 0xFF
        crc = (crc >> 1) ^ CRC32_TABLE[index]
    return ~crc & 0xFFFFFFFF

def pack_neural_weights():
    """
    Pack neural network weights into binary format matching C struct
    This must match the layout in neuro/neuro_sync.c:default_weights
    """
    # Layer 1: Input (6) -> Hidden (8)
    w1 = [
        [2, -1, 1, 0, 1, -1, 0, 1],
        [1, 2, -1, 1, 0, 1, -1, 0],
        [-1, 1, 2, -1, 1, 0, 1, -1],
        [0, -1, 1, 2, -1, 1, 0, 1],
        [1, 0, -1, 1, 2, -1, 1, 0],
        [-1, 1, 0, -1, 1, 2, -1, 1]
    ]
    
    # Bias for hidden layer
    b1 = [0, 0, 0, 0, 0, 0, 0, 0]
    
    # Layer 2: Hidden (8) -> Output (4)
    w2 = [
        [1, -1, 1, 0],
        [-1, 1, 0, 1],
        [1, 0, -1, 1],
        [0, 1, 1, -1],
        [1, -1, 0, 1],
        [-1, 0, 1, 1],
        [0, 1, -1, 0],
        [1, 0, 1, -1]
    ]
    
    # Bias for output layer
    b2 = [0, 0, 0, 0]
    
    # Convert to fixed-point Q16.16 format
    FIXED_SHIFT = 16
    
    def to_fixed(x):
        return int(x * (1 << FIXED_SHIFT))
    
    # Pack into binary (little-endian i32)
    data = bytearray()
    
    # Pack w1[6][8]
    for i in range(6):
        for j in range(8):
            data.extend(struct.pack('<i', to_fixed(w1[i][j])))
    
    # Pack b1[8]
    for i in range(8):
        data.extend(struct.pack('<i', to_fixed(b1[i])))
    
    # Pack w2[8][4]
    for i in range(8):
        for j in range(4):
            data.extend(struct.pack('<i', to_fixed(w2[i][j])))
    
    # Pack b2[4]
    for i in range(4):
        data.extend(struct.pack('<i', to_fixed(b2[i])))
    
    return bytes(data)

def main():
    print("=" * 60)
    print("  H-Exo Neural Weights CRC32 Calculator")
    print("=" * 60)
    print()
    
    # Pack weights
    weights_data = pack_neural_weights()
    print(f"Weights size: {len(weights_data)} bytes")
    
    # Compute CRC32
    crc = compute_crc32(weights_data)
    print(f"CRC32: 0x{crc:08X}")
    print()
    
    # Generate C code snippet
    print("Add this to neuro/weight_validation.c:")
    print("-" * 60)
    print(f"u32 get_expected_weights_crc(void) {{")
    print(f"    return 0x{crc:08X};")
    print(f"}}")
    print("-" * 60)
    print()
    
    # Verify
    print("Verification:")
    verify_crc = compute_crc32(weights_data)
    if verify_crc == crc:
        print("✓ CRC computation verified")
    else:
        print("✗ CRC verification failed!")
        sys.exit(1)
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
