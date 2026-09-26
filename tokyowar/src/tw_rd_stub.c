/*
 * tw_rd_stub.c -- the readable-C (Phase B) hooks the generated code and engine/lift_cpu.c call,
 * switched off. Tokyo Wars has no readable functions yet (T7); when it does, src/rd/rd_core.c
 * (shared with Rave Racer) replaces this file.
 */
#include <stdint.h>
int rd_on, rd_stop_on, rd_poll_rec, rd_quiet, rd_cov_on;
int rd_hook(uint32_t ep) { (void)ep; return 0; }
int rd_jump_stop(uint32_t t, uint32_t at) { (void)t; (void)at; return 0; }
void rd_poll_snap(void) {}
void rd_budget_out(void) {}
void rd_cov_ins(uint32_t pc) { (void)pc; }
