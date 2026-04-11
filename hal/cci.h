#ifndef HAL_CCI_H
#define HAL_CCI_H

#include "../core/types.h"

// Enable RK3399 CCI-500 snoop + DVM on both clusters.
// Returns OK when CCI reports enabled, ERR_TIMEOUT otherwise.
result_t cci500_enable(void);

#endif // HAL_CCI_H
