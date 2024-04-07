#ifndef FARPATCH_UART_H__
#define FARPATCH_UART_H__

#include <stdarg.h>

void uart_dbg_install(void);
void uart_init(void);

#define TARGET_UART_DEV    UART1
#define TARGET_UART_IDX    1
#define PERIPH_UART_MODULE PERIPH_UART1_MODULE
#define PERIPH_UART_IRQ    ETS_UART1_INTR_SOURCE

#endif /* FARPATCH_UART_H__ */
