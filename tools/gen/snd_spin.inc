/* ---- POLLING-LOOP FAST-FORWARD (tools/gen/snd_spin.inc, emitted by tools/gen/snd_translate.py) ----
 *
 * The sound driver spends ~93% of its instructions in `LDA dp ; BEQ self`: the main loop waiting for a timer
 * interrupt handler to set a flag (0xC12B `LDA $83` in Prop Cycle / Tokyo Wars, 0xC0DD / 0xC0FF in Rave Racer's BIOS).
 * Every pass is the same two instructions with the same cost, and nothing can change the flag until the next
 * interrupt: timers are absolute-time (t_next), so nothing happens before m37710_next_event(). snd_spin_fast() lets the
 * host jump a whole run of passes at once -- cycles += n * C -- instead of executing them one instruction at a time.
 *
 * It is EXACT, not approximate: it executes one full pass itself (through run_chunk, the real translation) to
 * measure the cost C and to see that the loop really repeats (PC back at the head, no interrupt taken), skips whole
 * passes only, and stops 2 passes short of `limit` (the next event / pin / slice end), so the boundary the
 * original crosses is crossed by the same instruction. The register state after n passes equals the state after one:
 * LDA re-reads the same byte of internal RAM, which no other code runs to change. The test oracle never skips
 * (its snd_spin_fast is a stub), so the translated-vs-oracle gates compare skipping against not skipping. */
typedef struct { uint32_t head; uint8_t mx, off; } spin_site_t;
static const spin_site_t spin_sites[] = { @SITES@ };
static int g_in_spin;

uint64_t snd_spin_fast(m37710_t *c, uint64_t limit)
{
    if (c->pending || c->unimpl_hit || c->stopped || g_in_spin) return 0;
    const uint32_t pc0 = ((uint32_t)c->pg << 16) | c->pc;
    const unsigned mx = (c->ps >> 4) & 3u;
    const spin_site_t *s = NULL;
    for (unsigned i = 0; i < sizeof spin_sites / sizeof spin_sites[0]; i++)
        if (spin_sites[i].mx == mx && (spin_sites[i].head == pc0 || spin_sites[i].head + 2 == pc0)) { s = &spin_sites[i]; break; }
    if (!s) return 0;
    const uint64_t ev = m37710_next_event(c);
    if (ev < limit) limit = ev;
    const uint64_t t_in = c->cycles;
    if (limit <= t_in + 96) return 0;                 /* not worth it -- and never close to an event or the end */
    const uint32_t ea = (uint32_t)c->dpr + s->off;
    if (ea < 0x80 || ea >= 0x400) return 0;           /* internal RAM only: plain memory, unlike the peripheral block */
    const uint32_t head = s->head;
    const uint64_t i0 = c->irq_taken;
    g_in_spin = 1;
    if (pc0 != head) run_chunk(c, c->cycles + 1);     /* mid-pass (at the branch): finish the pass first */
    if (!c->unimpl_hit && c->irq_taken == i0 && (((uint32_t)c->pg << 16) | c->pc) == head) {
        const uint64_t t0 = c->cycles;
        run_chunk(c, t0 + 1);                         /* the load */
        run_chunk(c, c->cycles + 1);                  /* the branch */
        if (!c->unimpl_hit && c->irq_taken == i0 && (((uint32_t)c->pg << 16) | c->pc) == head) {
            const uint64_t C = c->cycles - t0;        /* one whole pass */
            if (C > 0 && limit > c->cycles + 3 * C) c->cycles += ((limit - c->cycles) / C - 2) * C;
        }
    }
    g_in_spin = 0;
    return c->cycles - t_in;
}
