#ifndef FARPATCH_UART_H__
#define FARPATCH_UART_H__

#include <stdarg.h>

void uart_dbg_install(void);
void uart_init(void);

#define TARGET_UART_IDX 1

#endif /* FARPATCH_UART_H__ */
