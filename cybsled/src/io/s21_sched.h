/* DEV: MAME's scheduler, as far as it changes when a device timer armed by a CPU fires. MAME arms a timer from the
 * start of the current timeslice (machine().time()), not from the instruction that wrote the register, and a slice
 * ends at every timer event and otherwise after the maximum quantum (namcos21: 1/60000 s). This keeps the grid of
 * slice starts so the ADC (C68) and C140 (6809) timers fire at MAME's instruction. Time unit: 2.048 MHz cycles. */
#ifndef S21_SCHED_H
#define S21_SCHED_H
#include <stdint.h>
void     s21_sched_event(uint64_t t);         /* a timer fired at t: a new slice starts there */
uint64_t s21_sched_slice_start(uint64_t now); /* start of the slice containing now */
#endif
