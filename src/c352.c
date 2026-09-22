/* Namco C352 PCM chip -- ported from MAME's c352.cpp. See include/c352.h. */
#include "c352.h"
#include <string.h>

static uint8_t rom_read_byte(const c352_t *c, uint32_t addr)
{
    addr &= 0xFFFFFF; /* device_rom_interface<24> */
    if (addr >= c->rom_size) return 0;
    return c->rom[addr];
}

void c352_init(c352_t *c, const uint8_t *rom, uint32_t rom_size)
{
    memset(c, 0, sizeof(*c));
    c->rom = rom;
    c->rom_size = rom_size;

    /* Same construction as MAME's device_start -- output similar to
     * Namco's own VC emulator. */
    int j = 0;
    for (int i = 0; i < 128; i++) {
        c->mulawtab[i] = (int16_t)(j << 5);
        if (i < 16)      j += 1;
        else if (i < 24) j += 2;
        else if (i < 48) j += 4;
        else if (i < 100) j += 8;
        else              j += 16;
    }
    for (int i = 0; i < 128; i++)
        c->mulawtab[i + 128] = (int16_t)((~c->mulawtab[i]) & 0xffe0);

    c352_reset(c);
}

void c352_reset(c352_t *c)
{
    memset(c->voice, 0, sizeof(c->voice));
    c->random_state = 0x1234;
    c->control = 0;
}

/* reg_map order -- NOT struct layout, MAME's own external register
 * numbering (c352_device::read/write). */
uint16_t c352_read(const c352_t *c, uint32_t offset)
{
    if (offset < 0x100) {
        const c352_voice_t *v = &c->voice[offset / 8];
        switch (offset % 8) {
            case 0: return v->vol_f;
            case 1: return v->vol_r;
            case 2: return v->freq;
            case 3: return v->flags;
            case 4: return v->wave_bank;
            case 5: return v->wave_start;
            case 6: return v->wave_end;
            case 7: return v->wave_loop;
        }
    } else if (offset == 0x200) {
        return c->control;
    }
    return 0;
}

static void reg_store(c352_voice_t *v, uint32_t reg, uint16_t val)
{
    switch (reg) {
        case 0: v->vol_f = val; break;
        case 1: v->vol_r = val; break;
        case 2: v->freq = val; break;
        case 3: v->flags = val; break;
        case 4: v->wave_bank = val; break;
        case 5: v->wave_start = val; break;
        case 6: v->wave_end = val; break;
        case 7: v->wave_loop = val; break;
    }
}

void c352_write(c352_t *c, uint32_t offset, uint16_t data, uint16_t mem_mask)
{
    if (offset < 0x100) {
        /* COMBINE_DATA: *var = (*var & ~mem_mask) | (data & mem_mask) */
        uint16_t cur = c352_read(c, offset);
        uint16_t newval = (uint16_t)((cur & ~mem_mask) | (data & mem_mask));
        reg_store(&c->voice[offset / 8], offset % 8, newval);
    } else if (offset == 0x200) {
        c->control = (uint16_t)((c->control & ~mem_mask) | (data & mem_mask));
    } else if (offset == 0x202) {
        if (mem_mask != 0xffff) return; /* 16-bit-only exec, as MAME requires */

        for (int i = 0; i < 32; i++) {
            c352_voice_t *v = &c->voice[i];
            if (v->flags & C352_FLG_KEYON) {
                v->pos = ((uint32_t)v->wave_bank << 16) | v->wave_start;

                v->sample = 0;
                v->last_sample = 0;
                v->counter = 0xffff;

                v->flags |= C352_FLG_BUSY;
                v->flags &= (uint16_t)~(C352_FLG_KEYON | C352_FLG_LOOPHIST);

                v->curr_vol[0] = v->curr_vol[1] = 0;
                v->curr_vol[2] = v->curr_vol[3] = 0;
            }
            if (v->flags & C352_FLG_KEYOFF) {
                v->flags &= (uint16_t)~(C352_FLG_BUSY | C352_FLG_KEYOFF);
                v->counter = 0xffff;
            }
        }
    }
}

static void fetch_sample(c352_t *c, c352_voice_t *v)
{
    v->last_sample = v->sample;

    if (v->flags & C352_FLG_NOISE) {
        c->random_state = (uint16_t)((c->random_state >> 1) ^
                                      ((-(int16_t)(c->random_state & 1)) & 0xfff6));
        v->sample = (int16_t)c->random_state;
    } else {
        int8_t s = (int8_t)rom_read_byte(c, v->pos);

        if (v->flags & C352_FLG_MULAW)
            v->sample = c->mulawtab[(uint8_t)s];
        else
            v->sample = (int16_t)(s << 8);

        uint16_t pos = (uint16_t)(v->pos & 0xffff);

        if ((v->flags & C352_FLG_LOOP) && (v->flags & C352_FLG_REVERSE)) {
            if ((v->flags & C352_FLG_LDIR) && pos == v->wave_loop)
                v->flags &= (uint16_t)~C352_FLG_LDIR;
            else if (!(v->flags & C352_FLG_LDIR) && pos == v->wave_end)
                v->flags |= C352_FLG_LDIR;

            v->pos += (v->flags & C352_FLG_LDIR) ? (uint32_t)-1 : 1u;
        } else if (pos == v->wave_end) {
            if ((v->flags & C352_FLG_LINK) && (v->flags & C352_FLG_LOOP)) {
                v->pos = ((uint32_t)v->wave_start << 16) | v->wave_loop;
                v->flags |= C352_FLG_LOOPHIST;
            } else if (v->flags & C352_FLG_LOOP) {
                v->pos = (v->pos & 0xff0000) | v->wave_loop;
                v->flags |= C352_FLG_LOOPHIST;
            } else {
                v->flags |= C352_FLG_KEYOFF;
                v->flags &= (uint16_t)~C352_FLG_BUSY;
                v->sample = 0;
            }
        } else {
            v->pos += (v->flags & C352_FLG_REVERSE) ? (uint32_t)-1 : 1u;
        }
    }
}

static void ramp_volume(c352_voice_t *v, int ch, uint8_t val)
{
    int16_t delta = (int16_t)(v->curr_vol[ch] - val);
    if (delta != 0)
        v->curr_vol[ch] = (uint8_t)(v->curr_vol[ch] + ((delta > 0) ? -1 : 1));
}

static int16_t clamp_s16(int32_t v)
{
    /* MAME truncates via s16(out[ch] >> 3) -- an implicit narrowing
     * conversion, not a clamp. Mirror that with an explicit cast so the
     * wrap-around behaviour (should the mix ever exceed 16 bits) matches
     * bit for bit rather than silently clipping instead. */
    return (int16_t)v;
}

void c352_generate(c352_t *c, int16_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        int32_t outv[4] = {0, 0, 0, 0};

        for (int j = 0; j < 32; j++) {
            c352_voice_t *v = &c->voice[j];
            int16_t s = 0;

            if (v->flags & C352_FLG_BUSY) {
                uint32_t next_counter = v->counter + v->freq;

                if (next_counter & 0x10000)
                    fetch_sample(c, v);

                if ((next_counter ^ v->counter) & 0x18000) {
                    ramp_volume(v, 0, (uint8_t)(v->vol_f >> 8));
                    ramp_volume(v, 1, (uint8_t)(v->vol_f & 0xff));
                    ramp_volume(v, 2, (uint8_t)(v->vol_r >> 8));
                    ramp_volume(v, 3, (uint8_t)(v->vol_r & 0xff));
                }

                v->counter = next_counter & 0xffff;

                s = v->sample;

                if ((v->flags & C352_FLG_FILTER) == 0) {
                    int32_t diff = (int32_t)v->sample - (int32_t)v->last_sample;
                    uint32_t interp = (v->counter * (uint32_t)diff) >> 16;
                    s = (int16_t)((int32_t)v->last_sample + (int32_t)interp);
                }
            }

            outv[0] += (((v->flags & C352_FLG_PHASEFL) ? -s : s) * v->curr_vol[0]) >> 8;
            outv[2] += (((v->flags & C352_FLG_PHASERL) ? -s : s) * v->curr_vol[2]) >> 8;
            outv[1] += (((v->flags & C352_FLG_PHASEFR) ? -s : s) * v->curr_vol[1]) >> 8;
            outv[3] += (((v->flags & C352_FLG_PHASEFR) ? -s : s) * v->curr_vol[3]) >> 8;
        }

        out[i * 4 + 0] = clamp_s16(outv[0] >> 3);
        out[i * 4 + 1] = clamp_s16(outv[1] >> 3);
        out[i * 4 + 2] = clamp_s16(outv[2] >> 3);
        out[i * 4 + 3] = clamp_s16(outv[3] >> 3);
    }
}
