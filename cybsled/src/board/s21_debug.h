/* s21_debug.h -- the developer test mode left in the ROM (s21_debug.c, PLAN.md Module I). */
#ifndef S21_DEBUG_H
#define S21_DEBUG_H
void s21_debug_enable(int on);        /* on: the test-mode loop runs the developer test mode (master 0x2BB0 / slave 0x1C24) */
int  s21_debug_enabled(void);
int  s21_debug_dev_entered(void);     /* the master has entered 0x2BB0 since it was enabled */
#endif
