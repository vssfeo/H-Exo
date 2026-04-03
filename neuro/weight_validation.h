// H-Exo Omni-Core: Neural Weight Integrity Validation
// CRC32 checksum to detect weight corruption

#ifndef HEXO_WEIGHT_VALIDATION_H
#define HEXO_WEIGHT_VALIDATION_H

#include "../core/types.h"
#include "neuro_sync.h"

// CRC32 polynomial (IEEE 802.3)
#define CRC32_POLYNOMIAL 0xEDB88320

// Compute CRC32 checksum of neural network weights
u32 compute_weights_crc32(const neural_weights_t* weights);

// Validate weights integrity against expected checksum
bool validate_weights_integrity(const neural_weights_t* weights, u32 expected_crc);

// Get expected CRC (computed offline during build)
u32 get_expected_weights_crc(void);

#endif // HEXO_WEIGHT_VALIDATION_H
