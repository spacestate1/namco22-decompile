/* pedal_enc_serial.h -- a minimal NON-BLOCKING serial port, for an exercise
 * bike's cadence line (a Peloton's RS-232 jack through a USB adapter, or a
 * microcontroller streaming encoder counts). POSIX termios and Win32 behind
 * one four-call surface; nothing here knows about the game. */
#ifndef PEDAL_ENC_SERIAL_H
#define PEDAL_ENC_SERIAL_H

#include <stdint.h>

typedef struct {
#ifdef _WIN32
    void *h;            /* HANDLE */
#else
    int   fd;
#endif
} penc_serial_t;

/* Open `path` (/dev/ttyUSB0, COM3, \\.\COM12) as `baud` 8N1, raw, no flow
 * control. 0 on success, -1 on failure (see penc_serial_error). */
int  penc_serial_open(penc_serial_t *s, const char *path, int baud);
/* Read up to n bytes without waiting. Returns the count, 0 if nothing is
 * waiting, -1 if the port is gone (unplugged): close it and retry later. */
int  penc_serial_read(penc_serial_t *s, uint8_t *buf, int n);
int  penc_serial_write(penc_serial_t *s, const uint8_t *buf, int n);
void penc_serial_close(penc_serial_t *s);
/* Text of the last open failure. */
const char *penc_serial_error(void);
/* Print the serial ports this machine has, one per line. */
void penc_serial_list(void);

#endif
