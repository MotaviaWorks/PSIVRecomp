/*
 * Sound: the Z80 (running the game's own DAC driver), YM2612 and PSG.
 *
 * Time base: Z80 cycles. NTSC master clock 53.693175 MHz; Z80 = /15
 * (3,579,545 Hz); YM2612 = /7 with one sample per 144 clocks (53,267 Hz);
 * PSG = Z80 / 16. One frame (59.922751 Hz) is ~59,736.6 Z80 cycles.
 *
 * The 68000 runs "instantly" in this port, so the Z80 is advanced in two
 * places: by sound_end_frame() for the rest of each frame, and by a few cycles
 * whenever the 68000 releases the Z80 bus -- the 68000 sound driver's StopZ80
 * releases and retries while the Z80 flags it is busy ($A01FFD bit 7), and on
 * hardware the Z80 runs during those retries.
 */
#include "sound.h"
#include "m68k.h"
#include "ym2612.h"

#include "z80.h"                 /* implementation compiled in z80_impl.c */

#include <math.h>
#include <string.h>

#define Z80_CLOCK       3579545.0
#define FRAME_CYCLES    (Z80_CLOCK / 59.922751)
#define CYCLES_PER_SAMPLE (Z80_CLOCK / SOUND_RATE)      /* ~67.2 */
#define PSG_PER_SAMPLE  (Z80_CLOCK / 16.0 / SOUND_RATE)  /* ~4.2 */
#define RELEASE_CYCLES  32                               /* Z80 time per 68000 bus release */
#define VINT_IRQ_CYCLES 171                              /* Z80 INT held at vertical blank */
#define MAX_SAMPLES     2048

static z80_t z80;
static uint64_t pins;
static uint8_t z80_ram[0x2000];
static int busreq, reset_held = 1;
static uint32_t bank;                  /* 9-bit bank register: 68000 address bits 15-23 */

static double frame_pos;               /* Z80 cycles run so far this frame */
static double sample_phase;            /* Z80 cycles toward the next output sample */
static double psg_phase;
static int irq_cycles;
static int16_t buffer[MAX_SAMPLES * 2];
static int nsamples;
static uint64_t total_samples;
static FILE *write_log;

/* ------------------------------------------------------------------ PSG (SN76489 in the VDP) */
static struct {
    uint16_t period[4];                /* tone 0-2; [3] unused */
    int16_t  count[4];
    uint8_t  out[4];
    uint8_t  vol[4];                   /* attenuation, 15 = off */
    uint8_t  noise;                    /* noise control */
    uint16_t lfsr;
    uint8_t  latch;
} psg;
static int16_t psg_volume[16];

static void psg_init(void) {
    memset(&psg, 0, sizeof psg);
    for (int i = 0; i < 4; i++) { psg.vol[i] = 15; psg.count[i] = 1; }
    psg.lfsr = 0x8000;
    for (int i = 0; i < 15; i++) psg_volume[i] = (int16_t)(2600.0 * pow(10.0, -0.1 * i));   /* 2 dB steps */
    psg_volume[15] = 0;
}

/* Log lines: "sample chip reg value tick" ("sample psg value tick" for the
 * PSG). chip is ym0/ym1/psg for the 68000's writes and zym0/zym1/zpsg for the
 * Z80's; tick counts runs of the 68000 sound driver (psiv_sound_tick). */
static uint64_t sound_ticks;

void psiv_sound_tick(void) { sound_ticks++; }

static void psg_write(uint8_t v, int from_z80) {
    if (write_log)
        fprintf(write_log, "%llu %spsg %02X %llu\n", (unsigned long long)(total_samples + (uint64_t)nsamples),
                from_z80 ? "z" : "", v, (unsigned long long)sound_ticks);
    if (v & 0x80) {
        psg.latch = (v >> 4) & 7;
        int ch = psg.latch >> 1;
        if (psg.latch & 1) psg.vol[ch] = v & 0x0F;
        else if (ch < 3) psg.period[ch] = (uint16_t)((psg.period[ch] & 0x3F0) | (v & 0x0F));
        else { psg.noise = v & 7; psg.lfsr = 0x8000; }
    } else {
        int ch = psg.latch >> 1;
        if (psg.latch & 1) psg.vol[ch] = v & 0x0F;
        else if (ch < 3) psg.period[ch] = (uint16_t)((psg.period[ch] & 0x0F) | (v & 0x3F) << 4);
        else { psg.noise = v & 7; psg.lfsr = 0x8000; }
    }
}

void sound_psg_write(uint8_t v) { psg_write(v, 0); }

static void psg_clock(void) {
    for (int ch = 0; ch < 3; ch++) {
        if (--psg.count[ch] <= 0) {
            psg.count[ch] = (int16_t)(psg.period[ch] ? psg.period[ch] : 1);
            psg.out[ch] ^= 1;
            if (psg.period[ch] <= 1) psg.out[ch] = 1;      /* period 0/1: held high (used for samples) */
        }
    }
    if (--psg.count[3] <= 0) {
        static const int16_t rate[3] = {0x10, 0x20, 0x40};
        psg.count[3] = (psg.noise & 3) == 3 ? (int16_t)(psg.period[2] ? psg.period[2] : 1) : rate[psg.noise & 3];
        psg.out[3] ^= 1;
        if (psg.out[3]) {                                  /* shift on each rising edge */
            unsigned fb = (psg.noise & 4) ? ((psg.lfsr ^ (psg.lfsr >> 3)) & 1)   /* white: taps 0 and 3 */
                                          : (psg.lfsr & 1);                       /* periodic */
            psg.lfsr = (uint16_t)((psg.lfsr >> 1) | (fb << 15));
        }
    }
}

static int32_t psg_level(void) {
    int32_t s = 0;
    for (int ch = 0; ch < 3; ch++) s += psg.out[ch] ? psg_volume[psg.vol[ch]] : -psg_volume[psg.vol[ch]];
    s += (psg.lfsr & 1) ? psg_volume[psg.vol[3]] : -psg_volume[psg.vol[3]];
    return s;
}

/* ------------------------------------------------------------------ Z80 bus */
static uint8_t z80_mem_read(uint16_t a) {
    if (a < 0x4000) return z80_ram[a & 0x1FFF];
    if (a < 0x6000) return ym_read_status();
    if (a >= 0x8000) return rd8(bank << 15 | (a & 0x7FFF));   /* 32 KB window into 68000 space */
    return 0xFF;
}

static void ym_logged_write(int port, uint8_t v, int from_z80) {
    static uint8_t addr[2];                /* one chip: the address latch is shared by both CPUs */
    if (!(port & 1)) addr[port >> 1] = v;
    else if (write_log)
        fprintf(write_log, "%llu %sym%d %02X %02X %llu\n", (unsigned long long)(total_samples + (uint64_t)nsamples),
                from_z80 ? "z" : "", port >> 1, addr[port >> 1], v, (unsigned long long)sound_ticks);
    ym_write(port, v);
}

static void z80_mem_write(uint16_t a, uint8_t v) {
    if (a < 0x4000) { z80_ram[a & 0x1FFF] = v; return; }
    if (a < 0x6000) { ym_logged_write(a & 3, v, 1); return; }
    if (a < 0x6100) { bank = (bank >> 1 | (uint32_t)(v & 1) << 8) & 0x1FF; return; }
    if (a == 0x7F11) { psg_write(v, 1); return; }
    if (a >= 0x8000) wr8(bank << 15 | (a & 0x7FFF), v);
}

static void z80_step(void) {
    if (irq_cycles > 0) { pins |= Z80_INT; irq_cycles--; } else pins &= ~Z80_INT;
    pins = z80_tick(&z80, pins);
    if (pins & Z80_MREQ) {
        uint16_t a = Z80_GET_ADDR(pins);
        /* Z80_SET_DATA expands to a braced block: keep the braces */
        if (pins & Z80_RD) {
            Z80_SET_DATA(pins, z80_mem_read(a));
        } else if (pins & Z80_WR) {
            z80_mem_write(a, Z80_GET_DATA(pins));
        }
    } else if (pins & Z80_IORQ) {
        if ((pins & Z80_M1) || (pins & Z80_RD)) {           /* no I/O devices; IM1 */
            Z80_SET_DATA(pins, 0xFF);
        }
    }
}

/* advance everything by n Z80 cycles */
static void advance(double n) {
    int run_z80 = !reset_held && !busreq;
    double end = frame_pos + n;
    while (frame_pos + 1 <= end) {
        if (run_z80) z80_step();
        frame_pos += 1;
        sample_phase += 1;
        if (sample_phase >= CYCLES_PER_SAMPLE) {
            sample_phase -= CYCLES_PER_SAMPLE;
            int32_t l, r, p = 0;
            int ticks = 0;
            for (psg_phase += PSG_PER_SAMPLE; psg_phase >= 1; psg_phase -= 1, ticks++) {
                psg_clock();
                p += psg_level();
            }
            if (ticks) p /= ticks;
            ym_generate(&l, &r);
            /* master gain: the loudest song peaks near 11,500 raw; x2.25 puts it
             * around -2 dBFS with headroom for effects over music */
            l = (l + p) * 9 / 4;
            r = (r + p) * 9 / 4;
            if (nsamples < MAX_SAMPLES) {
                buffer[nsamples * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
                buffer[nsamples * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
                nsamples++;
            }
        }
    }
}

/* ------------------------------------------------------------------ 68000 side */
void sound_init(void) {
    memset(z80_ram, 0, sizeof z80_ram);
    pins = z80_init(&z80);
    busreq = 0;
    reset_held = 1;
    bank = 0;
    frame_pos = sample_phase = psg_phase = 0;
    nsamples = 0;
    ym_init();
    psg_init();
}

uint8_t sound_68k_read(uint32_t a) {
    a &= 0xFFFF;
    if (a < 0x4000) return z80_ram[a & 0x1FFF];
    if (a < 0x6000) return ym_read_status();
    return 0xFF;
}

void sound_68k_write(uint32_t a, uint8_t v) {
    a &= 0xFFFF;
    if (a < 0x4000) { z80_ram[a & 0x1FFF] = v; return; }
    if (a < 0x6000) { ym_logged_write(a & 3, v, 0); return; }
    if (a < 0x6100) { bank = (bank >> 1 | (uint32_t)(v & 1) << 8) & 0x1FF; return; }
}

uint16_t sound_busreq_read(void) {
    /* bit 0 clear = the 68000 has the bus (granted at once here) */
    return busreq && !reset_held ? 0x0000 : 0x0100;
}

void sound_busreq_write(uint16_t v) {
    int want = (v & 0x100) != 0;
    if (busreq && !want) {
        busreq = 0;
        advance(RELEASE_CYCLES);        /* the Z80 runs while the 68000 waits to retry */
    }
    busreq = want;
}

void sound_reset_write(uint16_t v) {
    int hold = !(v & 0x100);
    if (reset_held && !hold) pins = z80_reset(&z80);
    reset_held = hold;
}

int sound_end_frame(const int16_t **out) {
    irq_cycles = VINT_IRQ_CYCLES;       /* the VDP raises the Z80 interrupt at vertical blank */
    if (frame_pos < FRAME_CYCLES) advance(FRAME_CYCLES - frame_pos);
    frame_pos -= FRAME_CYCLES;
    *out = buffer;
    int n = nsamples;
    total_samples += (uint64_t)n;
    nsamples = 0;
    return n;
}

void sound_set_log(FILE *f) { write_log = f; }
