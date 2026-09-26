/* pedal_enc.h -- an EXTERNAL PEDAL for Prop Cycle: a rotary encoder on an
 * exercise bike's crank or flywheel, or a Peloton's own cadence.
 *
 * The cabinet's pedal is a one-bit optical sensor whose pulse RATE is speed;
 * src/input.c turns a 0..127 "pedal level" into that pulse train (the MAME
 * curve, register row 54). This module supplies the level from a real
 * cadence, and input.c takes the strongest of keyboard / pad trigger / this.
 *
 * Sources (propcycl_controls.cfg, or PROPCYCL_ENC for headless runs):
 *
 *   enc_source=joy_buttons   quadrature A/B on two joystick buttons (an arcade
 *                            encoder board, a Pico/Arduino HID device)
 *   enc_source=joy_axis      a wrapping counter on a joystick axis
 *   enc_source=serial        a serial line: a Peloton Gen-1 bike (RS-232, 19200)
 *                            or a microcontroller streaming counts / RPM
 *
 * `./build/propcycl --enctest` shows what the game is receiving, live, and is
 * how to find enc_counts_per_rev. Not wired into the Escape menu yet. */
#ifndef PEDAL_ENC_H
#define PEDAL_ENC_H

#include <SDL2/SDL.h>
#include <stdio.h>

/* propcycl_controls.cfg hooks (called from input_joy_cfg / _save). */
int    pedal_enc_cfg(const char *key, const char *val);   /* 1 if the key was ours */
void   pedal_enc_cfg_save(FILE *f);

/* `interactive` = a real run. A headless run ignores the saved config (like
 * the saved free-play setting) unless PROPCYCL_ENC names a source, so a
 * player's bike can never change what a gate measures. Reads the environment
 * once, here (getenv in the frame loop faults: register row 40). */
void   pedal_enc_init(int interactive);

void   pedal_enc_event(const SDL_Event *e);   /* joystick button/axis events */
void   pedal_enc_poll(void);                  /* serial I/O; every input_poll, menu open or not */
int    pedal_enc_level(void);                 /* 0..127, 0 = stopped or no source */
double pedal_enc_rpm(void);
int    pedal_enc_active(void);                /* a source is configured */
int    pedal_enctest(void);                   /* --enctest */

#endif
