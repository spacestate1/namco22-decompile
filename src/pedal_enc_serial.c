/* pedal_enc_serial.c -- see include/pedal_enc_serial.h */
#include "pedal_enc_serial.h"
#include <stdio.h>
#include <string.h>

static char g_err[160];
const char *penc_serial_error(void) { return g_err; }

#ifdef _WIN32
/* ---------------------------------------------------------------- Windows */
#include <windows.h>

static void win_err(const char *what, const char *path)
{
    snprintf(g_err, sizeof g_err, "%s %s (Windows error %lu)", what, path, (unsigned long)GetLastError());
}

int penc_serial_open(penc_serial_t *s, const char *path, int baud)
{
    char full[128];
    /* COM10 and above only open through the \\.\ namespace; it works for all. */
    if (strncmp(path, "\\\\.\\", 4) == 0) snprintf(full, sizeof full, "%s", path);
    else snprintf(full, sizeof full, "\\\\.\\%s", path);

    HANDLE h = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { win_err("cannot open", path); return -1; }

    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) { win_err("GetCommState failed for", path); CloseHandle(h); return -1; }
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;
    dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl  = DTR_CONTROL_ENABLE;
    dcb.fRtsControl  = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE; dcb.fInX = FALSE;
    dcb.fNull = FALSE; dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) { win_err("SetCommState failed for", path); CloseHandle(h); return -1; }

    /* MAXDWORD interval with zero totals = ReadFile returns at once with
     * whatever is already buffered. */
    COMMTIMEOUTS to;
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout = MAXDWORD;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    s->h = h;
    return 0;
}

int penc_serial_read(penc_serial_t *s, uint8_t *buf, int n)
{
    DWORD got = 0;
    if (!s->h) return -1;
    if (!ReadFile((HANDLE)s->h, buf, (DWORD)n, &got, NULL)) return -1;
    return (int)got;
}

int penc_serial_write(penc_serial_t *s, const uint8_t *buf, int n)
{
    DWORD put = 0;
    if (!s->h) return -1;
    if (!WriteFile((HANDLE)s->h, buf, (DWORD)n, &put, NULL)) return -1;
    return (int)put;
}

void penc_serial_close(penc_serial_t *s)
{
    if (s->h) CloseHandle((HANDLE)s->h);
    s->h = NULL;
}

void penc_serial_list(void)
{
    int any = 0;
    for (int i = 1; i <= 32; i++) {
        char name[32], full[40];
        snprintf(name, sizeof name, "COM%d", i);
        snprintf(full, sizeof full, "\\\\.\\%s", name);
        HANDLE h = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); printf("  %s\n", name); any = 1; }
        else if (GetLastError() == ERROR_ACCESS_DENIED) { printf("  %s (in use)\n", name); any = 1; }
    }
    if (!any) printf("  (no COM ports found)\n");
}

#else
/* ------------------------------------------------------------------ POSIX */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

static int baud_const(int baud)
{
    switch (baud) {
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return -1;
    }
}

int penc_serial_open(penc_serial_t *s, const char *path, int baud)
{
    int speed = baud_const(baud);
    if (speed < 0) { snprintf(g_err, sizeof g_err, "unsupported baud rate %d", baud); return -1; }

    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { snprintf(g_err, sizeof g_err, "cannot open %s: %s", path, strerror(errno)); return -1; }

    struct termios t;
    if (tcgetattr(fd, &t) != 0) {
        snprintf(g_err, sizeof g_err, "%s is not a serial port: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&t);                          /* 8 bits, no parity, no translation */
    cfsetispeed(&t, (speed_t)speed);
    cfsetospeed(&t, (speed_t)speed);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(tcflag_t)(CSTOPB | PARENB | CRTSCTS);
    t.c_cc[VMIN] = 0;                       /* read() returns at once ...        */
    t.c_cc[VTIME] = 0;                      /* ... with whatever is buffered     */
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        snprintf(g_err, sizeof g_err, "cannot configure %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    s->fd = fd;
    return 0;
}

int penc_serial_read(penc_serial_t *s, uint8_t *buf, int n)
{
    if (s->fd < 0) return -1;
    ssize_t r = read(s->fd, buf, (size_t)n);
    if (r > 0) return (int)r;
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        return -1;                          /* EIO / ENXIO: the adapter left */
    /* Nothing to read. A tty that has been HUNG UP -- a USB adapter pulled out,
     * a pty whose other end closed -- reads 0 bytes forever, exactly like an
     * idle line, so ask poll() which of the two this is. */
    struct pollfd p = { s->fd, POLLIN, 0 };
    if (poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL))) return -1;
    return 0;
}

int penc_serial_write(penc_serial_t *s, const uint8_t *buf, int n)
{
    if (s->fd < 0) return -1;
    ssize_t w = write(s->fd, buf, (size_t)n);
    if (w >= 0) return (int)w;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

void penc_serial_close(penc_serial_t *s)
{
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
}

void penc_serial_list(void)
{
    static const char *pats[] = {
        "/dev/ttyUSB*", "/dev/ttyACM*", "/dev/cu.usb*", "/dev/serial/by-id/*", NULL
    };
    int any = 0;
    for (int p = 0; pats[p]; p++) {
        glob_t g;
        if (glob(pats[p], 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) { printf("  %s\n", g.gl_pathv[i]); any = 1; }
            globfree(&g);
        }
    }
    if (!any) printf("  (no USB serial ports found)\n");
}
#endif
