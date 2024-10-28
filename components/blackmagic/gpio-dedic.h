#ifndef GPIO_DEDIC_H_
#define GPIO_DEDIC_H_

#include "general.h"

// SWDIO must be pin 0 to simplify the masking and shifting when reading values in
// and writing values out.
#define SWDIO_TMS_DEDIC_PIN     0
#define SWCLK_DEDIC_PIN         1
#define JTAG_TDO_DEDIC_PIN      2
#define JTAG_TDI_DEDIC_PIN      3
#define SWDIO_TMS_DIR_DEDIC_PIN 4
#define SWCLK_DIR_DEDIC_PIN     5

#define SWDIO_TMS_DEDIC_MASK     (1 << SWDIO_TMS_DEDIC_PIN)
#define SWCLK_DEDIC_MASK         (1 << SWCLK_DEDIC_PIN)
#define JTAG_TDO_DEDIC_MASK      (1 << JTAG_TDO_DEDIC_PIN)
#define JTAG_TDI_DEDIC_MASK      (1 << JTAG_TDI_DEDIC_PIN)
#define SWDIO_TMS_DIR_DEDIC_MASK (1 << SWDIO_TMS_DIR_DEDIC_PIN)
#define SWCLK_DIR_DEDIC_MASK     (1 << SWCLK_DIR_DEDIC_PIN)

/* Omit TMS/SWDIO because that isn't strictly an output since it changes */
#if CONFIG_TCK_TDI_DIR_GPIO == -1
#if CONFIG_TMS_SWDIO_DIR_GPIO == -1
#define ALL_OUTPUT_MASK (SWCLK_DEDIC_MASK | JTAG_TDI_PIN)
#else /* CONFIG_TMS_SWDIO_DIR_GPIO != 0 */
#define ALL_OUTPUT_MASK (SWCLK_DEDIC_MASK | JTAG_TDI_PIN | SWDIO_TMS_DIR_DEDIC_MASK)
#endif
#else /* CONFIG_TCK_TDI_DIR_GPIO != 0 */
#define ALL_OUTPUT_MASK (SWCLK_DEDIC_MASK | JTAG_TDI_PIN | SWDIO_TMS_DIR_DEDIC_MASK | SWCLK_DIR_DEDIC_MASK)
#endif

void gpio_dedic_init(void);

#endif /* GPIO_DEDIC_H_ */
