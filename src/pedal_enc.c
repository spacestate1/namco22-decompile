/* pedal_enc.c -- SDL / serial glue for an external pedal. The logic is in
 * pedal_enc_core.c (unit-tested); this file only moves bytes and events into
 * it and reads the answer back. See include/pedal_enc.h for the sources. */
#include "pedal_enc.h"
#include "pedal_enc_core.h"
#include "pedal_enc_serial.h"
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

enum { SRC_NONE, SRC_JOY_BUTTONS, SRC_JOY_AXIS, SRC_SERIAL };
enum { PROTO_PELOTON, PROTO_PELOTON_SNIFF, PROTO_TICKS, PROTO_RPM };

static struct {
    int    src, proto;
    int    joy_a, joy_b, joy_axis, axis_bits;
    int    invert;
    double cpr;          /* decoded counts per crank revolution */
    double full_rpm;     /* the cadence that means a full 127 pedal level */
    char   serial[128];
    int    baud;         /* 0 = the protocol's own (19200 Peloton, 115200 else) */
} cfg = { SRC_NONE, PROTO_PELOTON, -1, -1, -1, 16, 0, 96.0, 100.0, "", 0 };

static int  interactive = 1;
static int  inited;
static char env_override[512];
static int  hexdump;                    /* --enctest: show raw serial bytes */

static penc_cad_t  cad;
static penc_qdec_t qd;
static int         a_state, b_state;
static SDL_JoystickID enc_dev = -1;     /* the one joystick that drives us */
static int         axis_have;
static uint32_t    axis_last_raw;
static long long   total_counts;        /* --enctest calibration aid */
static int         alive_said;

static penc_serial_t ser;
static int         ser_ok;
static uint32_t    ser_next_try, last_req;
static int         ser_err_said;
static penc_pel_t  pel;
static penc_line_t line;
static int64_t     tick_last;
static int         tick_have;

/* ---- config ------------------------------------------------------------- */
static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static const char *src_name(void)
{
    switch (cfg.src) {
    case SRC_JOY_BUTTONS: return "joy_buttons";
    case SRC_JOY_AXIS:    return "joy_axis";
    case SRC_SERIAL:      return "serial";
    default:              return "none";
    }
}
static const char *proto_name(void)
{
    switch (cfg.proto) {
    case PROTO_PELOTON_SNIFF: return "peloton_sniff";
    case PROTO_TICKS:         return "ticks";
    case PROTO_RPM:           return "rpm";
    default:                  return "peloton";
    }
}

int pedal_enc_cfg(const char *key_in, const char *val_in)
{
    if (strncmp(key_in, "enc_", 4) != 0) return 0;
    char key[64], val[160];
    snprintf(key, sizeof key, "%s", key_in);
    snprintf(val, sizeof val, "%s", val_in);
    trim(key);
    trim(val);

    if (!strcmp(key, "enc_source")) {
        cfg.src = !strcmp(val, "joy_buttons") ? SRC_JOY_BUTTONS
                : !strcmp(val, "joy_axis")    ? SRC_JOY_AXIS
                : !strcmp(val, "serial")      ? SRC_SERIAL : SRC_NONE;
    } else if (!strcmp(key, "enc_joy_a"))         cfg.joy_a = atoi(val);
    else if (!strcmp(key, "enc_joy_b"))           cfg.joy_b = atoi(val);
    else if (!strcmp(key, "enc_joy_axis"))        cfg.joy_axis = atoi(val);
    else if (!strcmp(key, "enc_axis_bits"))       { int b = atoi(val); cfg.axis_bits = (b >= 2 && b <= 16) ? b : 16; }
    else if (!strcmp(key, "enc_invert"))          cfg.invert = atoi(val) ? 1 : 0;
    else if (!strcmp(key, "enc_counts_per_rev"))  { double d = atof(val); if (d > 0.0) cfg.cpr = d; cad.cpr = cfg.cpr; }
    else if (!strcmp(key, "enc_full_rpm"))        { double d = atof(val); if (d > 0.0) cfg.full_rpm = d; }
    else if (!strcmp(key, "enc_serial"))          snprintf(cfg.serial, sizeof cfg.serial, "%.*s", (int)sizeof cfg.serial - 1, val);
    else if (!strcmp(key, "enc_serial_baud"))     cfg.baud = atoi(val);
    else if (!strcmp(key, "enc_serial_proto")) {
        cfg.proto = !strcmp(val, "peloton_sniff") ? PROTO_PELOTON_SNIFF
                  : !strcmp(val, "ticks")         ? PROTO_TICKS
                  : !strcmp(val, "rpm")           ? PROTO_RPM : PROTO_PELOTON;
    } else {
        return 0;                       /* an enc_* key we do not know */
    }
    return 1;
}

void pedal_enc_cfg_save(FILE *f)
{
    fprintf(f, "# external pedal: a rotary encoder / exercise bike (see --enctest)\n");
    fprintf(f, "# enc_source: none | joy_buttons (A/B on two buttons) | joy_axis (a wrapping counter axis) | serial\n");
    fprintf(f, "enc_source=%s\n", src_name());
    fprintf(f, "enc_joy_a=%d\nenc_joy_b=%d\nenc_joy_axis=%d\nenc_axis_bits=%d\n",
            cfg.joy_a, cfg.joy_b, cfg.joy_axis, cfg.axis_bits);
    fprintf(f, "enc_invert=%d\n", cfg.invert);
    fprintf(f, "# decoded counts per crank turn (A/B x4 counts) -- turn the crank once under --enctest and read it off\n");
    fprintf(f, "enc_counts_per_rev=%g\n", cfg.cpr);
    fprintf(f, "# the cadence that counts as flat out\nenc_full_rpm=%g\n", cfg.full_rpm);
    fprintf(f, "# enc_serial_proto: peloton (we poll the bike, 19200) | peloton_sniff (listen only) | ticks | rpm\n");
    fprintf(f, "enc_serial=%s\nenc_serial_proto=%s\nenc_serial_baud=%d\n", cfg.serial, proto_name(), cfg.baud);
}

void pedal_enc_init(int is_interactive)
{
    interactive = is_interactive;
    const char *e = getenv("PROPCYCL_ENC");
    if (e) snprintf(env_override, sizeof env_override, "%s", e);
}

/* PROPCYCL_ENC="enc_source=serial,enc_serial=/dev/pts/5,enc_serial_proto=peloton"
 * -- the same keys as the cfg file, comma separated. Applied on first use, so it
 * wins over the file, and it is what a headless run needs. */
static void ensure_init(void)
{
    if (inited) return;
    inited = 1;
    if (env_override[0]) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s", env_override);
        for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = '\0';
            if (!pedal_enc_cfg(tok, eq + 1)) fprintf(stderr, "  [ENC] PROPCYCL_ENC: unknown key '%s'\n", tok);
        }
    } else if (!interactive) {
        cfg.src = SRC_NONE;              /* headless: the saved config is ignored */
    }
    penc_cad_init(&cad, cfg.cpr);
    memset(&ser, 0, sizeof ser);
#ifndef _WIN32
    ser.fd = -1;
#endif
    if (cfg.src != SRC_NONE)
        printf("  [ENC] external pedal: %s%s%s, %g counts/rev, %g rpm = full\n", src_name(),
               cfg.src == SRC_SERIAL ? " / " : "", cfg.src == SRC_SERIAL ? proto_name() : "",
               cfg.cpr, cfg.full_rpm);
}

int pedal_enc_active(void) { ensure_init(); return cfg.src != SRC_NONE; }

/* ---- counts in ----------------------------------------------------------- */
static void counts_in(uint32_t t, int d)
{
    if (!d) return;
    if (cfg.invert) d = -d;
    total_counts += d;
    penc_cad_ticks(&cad, t, d);
    if (!alive_said) { alive_said = 1; printf("  [ENC] first encoder counts received\n"); }
}

void pedal_enc_event(const SDL_Event *e)
{
    ensure_init();
    if (cfg.src != SRC_JOY_BUTTONS && cfg.src != SRC_JOY_AXIS) return;

    if (e->type == SDL_JOYDEVICEREMOVED) {
        if (e->jdevice.which == enc_dev) { enc_dev = -1; axis_have = 0; printf("  [ENC] encoder device removed\n"); }
        return;
    }

    if (cfg.src == SRC_JOY_BUTTONS) {
        if (e->type != SDL_JOYBUTTONDOWN && e->type != SDL_JOYBUTTONUP) return;
        int btn = e->jbutton.button, down = (e->type == SDL_JOYBUTTONDOWN);
        if (btn != cfg.joy_a && btn != cfg.joy_b) return;
        if (enc_dev < 0) {
            /* First edge from the encoder: take both lines' real levels so the
             * decoder does not start from an assumed 00. This edge itself is
             * absorbed into that state. */
            SDL_Joystick *j = SDL_JoystickFromInstanceID(e->jbutton.which);
            if (!j) return;
            enc_dev = e->jbutton.which;
            a_state = cfg.joy_a >= 0 ? SDL_JoystickGetButton(j, cfg.joy_a) : 0;
            b_state = cfg.joy_b >= 0 ? SDL_JoystickGetButton(j, cfg.joy_b) : 0;
            penc_qdec_init(&qd, a_state, b_state);
            return;
        }
        if (e->jbutton.which != enc_dev) return;
        if (btn == cfg.joy_a) a_state = down; else b_state = down;
        counts_in(e->jbutton.timestamp, penc_qdec_step(&qd, a_state, b_state));
        return;
    }

    /* joy_axis: a free-running counter carried on an axis. SDL scales the
     * device's logical range to -32768..32767, so undo that to the counter's
     * real bit width and difference it modulo 2^bits. */
    if (e->type != SDL_JOYAXISMOTION || e->jaxis.axis != cfg.joy_axis) return;
    if (enc_dev < 0) { enc_dev = e->jaxis.which; axis_have = 0; }
    if (e->jaxis.which != enc_dev) return;
    uint32_t mod = 1u << cfg.axis_bits;
    uint32_t raw = (uint32_t)((((uint64_t)(e->jaxis.value + 32768)) * (mod - 1) + 32767) / 65535);
    if (!axis_have) { axis_last_raw = raw; axis_have = 1; return; }
    int32_t d = (int32_t)((raw - axis_last_raw) & (mod - 1));
    if (d >= (int32_t)(mod / 2)) d -= (int32_t)mod;
    axis_last_raw = raw;
    /* No crank moves a quarter of the counter's range between two reports. SDL
     * does exactly that when the device is unplugged or loses focus: it
     * "recentres" every axis to its value at open, which reads as a huge jump.
     * Take the new position as the baseline and count nothing. */
    if (d > (int32_t)(mod / 4) || d < -(int32_t)(mod / 4)) return;
    counts_in(e->jaxis.timestamp, d);
}

/* ---- serial -------------------------------------------------------------- */
static void serial_byte(uint32_t now, uint8_t b)
{
    if (hexdump) printf("%02X ", b);
    if (cfg.proto == PROTO_PELOTON || cfg.proto == PROTO_PELOTON_SNIFF) {
        penc_pel_msg_t m;
        if (penc_pel_feed(&pel, b, &m)) {
            if (hexdump) printf("\n  [PELOTON] type %02X = %ld%s\n", m.type, m.value,
                                m.type == PENC_PEL_CADENCE ? " rpm" : m.type == PENC_PEL_OUTPUT ? " (watts x10)" : "");
            if (m.type == PENC_PEL_CADENCE) {
                penc_cad_set_rpm(&cad, now, (double)m.value);
                if (!alive_said) { alive_said = 1; printf("  [ENC] first cadence reply from the bike\n"); }
            }
        }
        return;
    }
    double v;
    if (!penc_line_feed(&line, b, &v) || !isfinite(v)) return;
    if (hexdump) printf("\n  [LINE] %g\n", v);
    if (cfg.proto == PROTO_RPM) {
        penc_cad_set_rpm(&cad, now, v);
        if (!alive_said) { alive_said = 1; printf("  [ENC] first RPM line received\n"); }
        return;
    }
    /* ticks: every line is the encoder's running position; the counts are its
     * difference. The first line only sets the baseline, and a jump larger than
     * any real crank could make in one line is a microcontroller reset. */
    int64_t pos = (int64_t)llround(v);
    if (!tick_have) { tick_last = pos; tick_have = 1; return; }
    int64_t d = pos - tick_last;
    tick_last = pos;
    if (d > 100000 || d < -100000) return;
    counts_in(now, (int)d);
}

static void serial_lost(uint32_t now)
{
    penc_serial_close(&ser);
    ser_ok = 0;
    ser_next_try = now + 1000;
    printf("  [ENC] serial %s lost, retrying\n", cfg.serial);
}

static void serial_step(uint32_t now)
{
    if (!ser_ok) {
        if ((int32_t)(now - ser_next_try) < 0) return;
        ser_next_try = now + 2000;
        if (!cfg.serial[0]) {
            if (!ser_err_said) { ser_err_said = 1; fprintf(stderr, "  [ENC] enc_source=serial but enc_serial is empty (try --enctest)\n"); }
            return;
        }
        int baud = cfg.baud > 0 ? cfg.baud
                 : (cfg.proto == PROTO_PELOTON || cfg.proto == PROTO_PELOTON_SNIFF) ? 19200 : 115200;
        if (penc_serial_open(&ser, cfg.serial, baud) == 0) {
            ser_ok = 1; ser_err_said = 0;
            memset(&pel, 0, sizeof pel);
            memset(&line, 0, sizeof line);
            tick_have = 0;
            last_req = now;
            printf("  [ENC] serial %s open, %d baud, %s\n", cfg.serial, baud, proto_name());
        } else if (!ser_err_said) {
            ser_err_said = 1;
            fprintf(stderr, "  [ENC] %s (will keep retrying)\n", penc_serial_error());
        }
        return;
    }

    /* The head unit polls; the bike only ever answers. Peloton's own tablet
     * asks every 100 ms. In peloton_sniff mode something else is asking and
     * we only listen (writing would collide with it). */
    if (cfg.proto == PROTO_PELOTON && (uint32_t)(now - last_req) >= 100) {
        uint8_t rq[4];
        penc_pel_request(PENC_PEL_CADENCE, rq);
        last_req = now;
        if (penc_serial_write(&ser, rq, 4) < 0) { serial_lost(now); return; }   /* EIO: adapter gone */
    }

    uint8_t buf[256];
    for (;;) {
        int n = penc_serial_read(&ser, buf, (int)sizeof buf);
        if (n < 0) { serial_lost(now); return; }
        for (int i = 0; i < n; i++) serial_byte(now, buf[i]);
        if (n < (int)sizeof buf) break;
    }
}

void pedal_enc_poll(void)
{
    ensure_init();
    if (cfg.src == SRC_SERIAL) serial_step(SDL_GetTicks());
}

double pedal_enc_rpm(void)
{
    ensure_init();
    if (cfg.src == SRC_NONE) return 0.0;
    return penc_cad_rpm(&cad, SDL_GetTicks());
}

int pedal_enc_level(void)
{
    ensure_init();
    if (cfg.src == SRC_NONE) return 0;
    return penc_level_from_rpm(penc_cad_rpm(&cad, SDL_GetTicks()), cfg.full_rpm);
}

/* ---- --enctest -------------------------------------------------------------
 * The bike-side counterpart of --joytest: shows what the game is being given.
 * Turn the crank exactly one revolution, note `counts`, put it in
 * enc_counts_per_rev. */
extern int ui_controls_load(void);

/* main() installs a handler that only clears ITS run flag, which this loop
 * cannot see -- so Ctrl-C would do nothing here without our own. */
static volatile sig_atomic_t enctest_stop;
static void enctest_on_signal(int sig) { (void)sig; enctest_stop = 1; }

int pedal_enctest(void)
{
    signal(SIGINT,  enctest_on_signal);
    signal(SIGTERM, enctest_on_signal);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    ui_controls_load();
    pedal_enc_init(1);
    ensure_init();
    hexdump = 1;

    printf("\nexternal pedal source: %s", src_name());
    if (cfg.src == SRC_SERIAL) printf(" (%s on %s)", proto_name(), cfg.serial[0] ? cfg.serial : "<enc_serial not set>");
    printf("\n  edit propcycl_controls.cfg, or set PROPCYCL_ENC=\"enc_source=...,...\"\n");

    printf("\nserial ports:\n");
    penc_serial_list();

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_Delay(200); SDL_PumpEvents();
    printf("\njoysticks: %d\n", SDL_NumJoysticks());
    for (int i = 0; i < SDL_NumJoysticks() && i < 16; i++) {
        SDL_Joystick *j = SDL_JoystickOpen(i);
        if (j) printf("  %d: %s, %d axes, %d buttons\n", i, SDL_JoystickName(j), SDL_JoystickNumAxes(j), SDL_JoystickNumButtons(j));
    }
    if (cfg.src == SRC_NONE)
        printf("\nno source configured; showing joystick events so you can find the button/axis numbers.\n");
    printf("\nCtrl-C to stop. counts = net encoder counts since start (turn the crank once to calibrate).\n\n");
    fflush(stdout);

    uint32_t next = 0;
    while (!enctest_stop) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) enctest_stop = 1;
            pedal_enc_event(&e);
            if (cfg.src == SRC_NONE || cfg.src == SRC_JOY_BUTTONS) {
                if (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP)
                    printf("  button %d %s\n", e.jbutton.button, e.type == SDL_JOYBUTTONDOWN ? "down" : "up");
            }
            if (cfg.src == SRC_NONE || cfg.src == SRC_JOY_AXIS) {
                if (e.type == SDL_JOYAXISMOTION) printf("  axis %d = %d\n", e.jaxis.axis, e.jaxis.value);
            }
        }
        pedal_enc_poll();
        uint32_t now = SDL_GetTicks();
        if ((int32_t)(now - next) >= 0) {
            next = now + 250;
            if (cfg.src != SRC_NONE) {
                printf("\r  counts=%-8lld rpm=%6.1f level=%3d/127%s      ", total_counts, pedal_enc_rpm(), pedal_enc_level(),
                       cfg.src == SRC_SERIAL && !ser_ok ? "  (serial port not open)" : "");
                fflush(stdout);
            }
        }
        SDL_Delay(5);
    }
    printf("\n");
    penc_serial_close(&ser);
    SDL_Quit();
    return 0;
}
