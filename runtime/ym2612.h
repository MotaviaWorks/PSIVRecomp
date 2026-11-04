#ifndef PSIV_YM2612_H
#define PSIV_YM2612_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The YM2612 makes one sample every 144 input clocks (7.67 MHz / 144 = 53,267 Hz). */
#define YM_CLOCKS_PER_SAMPLE 144

void    ym_init(void);
void    ym_write(int port, uint8_t data);       /* port 0-3: addr/data part I, addr/data part II */
uint8_t ym_read_status(void);
void    ym_generate(int32_t *left, int32_t *right);

#ifdef __cplusplus
}
#endif

#endif
