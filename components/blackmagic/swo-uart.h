#ifndef SWO_UART_H
#define SWO_UART_H

#include <stdint.h>

void swo_uart_deinit(void);
void swo_uart_init(const uint32_t baudrate);
uint32_t swo_uart_get_baudrate(void);
void swo_uart_set_baudrate(unsigned int baud);

#endif /* SWO_UART_H */
