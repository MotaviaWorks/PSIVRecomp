/*
 * Diagnostics for crashes and hangs: name the 68000 routine at an address and
 * walk the emulated stack for the call chain. The port pushes real 68000
 * return addresses (see CALL in m68k.h), so the stack reads like the
 * original game's.
 */
#ifndef PSIV_DIAG_H
#define PSIV_DIAG_H

#include <stddef.h>
#include <stdint.h>

/* Routine containing addr (the nearest function entry at or below it), and
 * addr's offset from that entry. */
const char *diag_function_name(uint32_t addr, uint32_t *offset);

/* Probable return addresses on the 68000 stack, innermost first: values that
 * point just after a jsr/bsr/trap in ROM. Returns how many were found. */
int diag_callstack(uint32_t *out, int max);

/* Human-readable report: what went wrong, registers, call chain. */
void diag_report(char *buf, size_t size, const char *what, uint32_t addr, uint32_t value);

/* Top of the 68000 stack (the reset SSP), set at boot. */
extern uint32_t diag_stack_top;

/* Implemented by each front end: show/log a fatal report, then exit. */
void platform_fatal(const char *report);

#endif
