#include "opl.h"
#include "sched.h"

/* Output sample rate of the mixer; phase increments are scaled from the chip's native
 * 49716 Hz so pitch stays exact (envelope clocks run at the mixer rate, ~3% slow — inaudible). */
#define OUT_RATE 48000

/* ---------------- tables ---------------- */
static u16 logsin_tab[256]; /* -log2(sin((i+0.5)/256 * pi/2)) * 256 */
static u16 exp_tab[256];    /* (2^(i/256) - 1) * 1024 */
static int tables_ready;

/* sin(x) for x in [0, pi/2], Q30 in/out, via odd polynomial (max err ~1e-7 with 4 terms) */
static s64 sin_q30(s64 x) {
  s64 x2 = (x * x) >> 30;
  s64 t = x;
  s64 sum = x;
  t = (t * x2) >> 30; sum -= t / 6;
  t = (t * x2) >> 30; sum += t / 120;
  t = (t * x2) >> 30; sum -= t / 5040;
  return sum;
}

/* log2(v / 2^30) * 256 for v in (0, 2^30], returns a NEGATIVE-or-zero value negated */
static s64 neglog2_q8(s64 v) {
  if (v <= 0) return 0xFFF;
  int lg = 0;
  while (v < (s64)1 << 29) { v <<= 1; lg++; }
  /* v in [2^29, 2^30) → x = v*2 in [1,2) Q30; frac = log2(x) in Q9 by squaring */
  s64 x = v << 1;
  s64 frac = 0;
  for (int b = 0; b < 9; b++) {
    x = (x * x) >> 30;
    frac <<= 1;
    if (x >= (s64)2 << 30) { frac |= 1; x >>= 1; }
  }
  /* v was shifted lg times into [2^29, 2^30), so value = 2^-(lg+1) * x0 with x0 = x/2^30 ∈ [1,2)
   * and log2(x0) = frac/512 → -log2(value) = (lg+1) - frac/512 */
  s64 att = ((s64)(lg + 1) << 8) - ((frac << 8) >> 9);
  if (att < 0) att = 0;
  if (att > 0xFFF) att = 0xFFF;
  return att;
}

static void build_tables(void) {
  const s64 PI_2_Q30 = 1686629713LL; /* pi/2 * 2^30 */
  for (int i = 0; i < 256; i++) {
    s64 x = PI_2_Q30 * (2 * i + 1) / 512;
    s64 s = sin_q30(x);
    logsin_tab[i] = (u16)neglog2_q8(s);
  }
  s64 r = 1 << 30;
  exp_tab[0] = 0;
  for (int i = 1; i < 256; i++) {
    r = (r * 1076646725LL) >> 30; /* × 2^(1/256) */
    exp_tab[i] = (u16)(((r - ((s64)1 << 30)) * 1024) >> 30);
  }
  tables_ready = 1;
}

/* attenuation in 1/256-octave units → linear (about 13 bits peak) */
static int att_to_lin(u32 att) {
  if (att > 0x1FFF) return 0;
  u32 frac = att & 0xFF;
  u32 shift = att >> 8;
  int v = (int)((exp_tab[(255 - frac) & 0xFF] + 1024) << 1);
  return shift > 30 ? 0 : v >> shift;
}

/* ---------------- state ---------------- */
typedef struct {
  u32 phase, inc;   /* 10.10 fixed-point phase */
  u8 am, vib, egt, ksr, mult;
  u8 ksl, tl;
  u8 ar, dr, sl, rr;
  u8 ws;
  u8 state;         /* 0 off, 1 attack, 2 decay, 3 sustain, 4 release */
  s32 env;          /* attenuation 0..511 */
  u8 key;
  int fb1, fb2;
  u8 ch;
} Op;

typedef struct {
  u16 fnum;
  u8 block, fb, cnt, pan, keyon;
} Chan;

static struct {
  Op op[36];
  Chan ch[18];
  u8 cur_bank, cur_addr;
  u8 opl3_mode, rhythm, tre_depth, vib_depth, nts;
  u32 lfo_am_cnt, lfo_vib_cnt, noise;
  u32 eg_counter;
  u8 t1_reload, t2_reload, timer_ctl, t1_on, t2_on;
  s64 t1_start, t2_start;
} opl;

/* slot ↔ channel mapping within one bank of 18 slots */
static const u8 slot_ch9[18] = {0, 1, 2, 0, 1, 2, 3, 4, 5, 3, 4, 5, 6, 7, 8, 6, 7, 8};
static const u8 slot_which[18] = {0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1};
static const s8 off_slot[32] = {0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1,
                                12, 13, 14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static const u8 mult_x2[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
static const u8 ksl_shift_tab[4] = {31, 1, 2, 0};
static const u8 ksl_tab[16] = {0, 24, 32, 37, 40, 43, 45, 47, 48, 50, 51, 52, 53, 54, 55, 56};

static Op *ops_of(int chi, int which) {
  int bank = chi >= 9;
  int base = bank * 18;
  for (int s = 0; s < 18; s++)
    if (slot_ch9[s] == chi % 9 && slot_which[s] == which) return &opl.op[base + s];
  return &opl.op[0];
}

static void update_inc(int chi) {
  Chan *c = &opl.ch[chi];
  for (int w = 0; w < 2; w++) {
    Op *o = ops_of(chi, w);
    u64 inc = (((u64)c->fnum << c->block) * mult_x2[o->mult]) / 2;
    o->inc = (u32)(inc * 49716 / OUT_RATE);
  }
}

static u8 rate_hi(const Op *o, const Chan *c) {
  int rof = (c->block << 1) | ((c->fnum >> (opl.nts ? 8 : 9)) & 1);
  return (u8)(o->ksr ? rof : rof >> 2);
}

static void env_keyon(Op *o) { o->key = 1; o->state = 1; o->phase = 0; }
static void env_keyoff(Op *o) { o->key = 0; if (o->state) o->state = 4; }

static void keyon_ch(int chi, int on) {
  Chan *c = &opl.ch[chi];
  if ((int)c->keyon == on) return;
  c->keyon = (u8)on;
  /* rhythm mode owns the key state of channels 7-8 operators (and shares 6) */
  if ((opl.rhythm & 0x20) && chi % 9 >= 6 && chi < 9) return;
  for (int w = 0; w < 2; w++) {
    Op *o = ops_of(chi, w);
    if (on) env_keyon(o); else env_keyoff(o);
  }
}

/* ---------------- register writes ---------------- */
void opl_addr(int bank, u8 v) { opl.cur_bank = (u8)(bank & 1); opl.cur_addr = v; }

static void rhythm_write(u8 old, u8 v) {
  opl.tre_depth = (v >> 7) & 1;
  opl.vib_depth = (v >> 6) & 1;
  opl.rhythm = v & 0x3F;
  int mode_on = v & 0x20;
  struct { u8 bit; u8 slot_a; s8 slot_b; } rk[5] = {
    {0x10, 12, 15}, /* bass drum: ch6 both ops */
    {0x01, 13, -1}, /* hi-hat: ch7 op1 */
    {0x08, 16, -1}, /* snare: ch7 op2 */
    {0x04, 14, -1}, /* tom: ch8 op1 */
    {0x02, 17, -1}, /* cymbal: ch8 op2 */
  };
  for (int i = 0; i < 5; i++) {
    int was = (old & rk[i].bit) && (old & 0x20);
    int now = (v & rk[i].bit) && mode_on;
    if (was == now) continue;
    if (now) { env_keyon(&opl.op[rk[i].slot_a]); if (rk[i].slot_b >= 0) env_keyon(&opl.op[rk[i].slot_b]); }
    else { env_keyoff(&opl.op[rk[i].slot_a]); if (rk[i].slot_b >= 0) env_keyoff(&opl.op[rk[i].slot_b]); }
  }
}

static int wr_count;
static void write_reg(int bank, u8 idx, u8 v) {
  extern int cpu_trace_faults;
  if (cpu_trace_faults >= 2 && wr_count < 24) { dm_log("OPL wr b%d %02x=%02x", bank, idx, v); wr_count++; }
  int base = idx & 0xE0;
  if (bank == 1) {
    if (idx == 0x05) { opl.opl3_mode = v & 1; return; }
    if (idx == 0x04) return; /* 4-op enable: pairs render as two 2-op channels for now */
  }
  if (bank == 0) {
    switch (idx) {
      case 0x01: return;
      case 0x02: opl.t1_reload = v; return;
      case 0x03: opl.t2_reload = v; return;
      case 0x04:
        if (v & 0x80) { opl.timer_ctl &= (u8)~0x60; return; }
        opl.timer_ctl = v;
        if (v & 1) { opl.t1_on = 1; opl.t1_start = emu_now_ns(); } else opl.t1_on = 0;
        if (v & 2) { opl.t2_on = 1; opl.t2_start = emu_now_ns(); } else opl.t2_on = 0;
        return;
      case 0x08: opl.nts = (v >> 6) & 1; return;
      case 0xBD: { u8 old = (u8)(opl.rhythm | (opl.tre_depth << 7) | (opl.vib_depth << 6)); rhythm_write(old, v); return; }
      default: break;
    }
  }
  if (base == 0x20 || base == 0x40 || base == 0x60 || base == 0x80 || base == 0xE0) {
    s8 s = off_slot[idx & 0x1F];
    if (s < 0) return;
    Op *o = &opl.op[bank * 18 + s];
    switch (base) {
      case 0x20:
        o->am = (v >> 7) & 1; o->vib = (v >> 6) & 1; o->egt = (v >> 5) & 1; o->ksr = (v >> 4) & 1;
        o->mult = v & 15;
        update_inc(o->ch);
        break;
      case 0x40: o->ksl = (v >> 6) & 3; o->tl = v & 63; break;
      case 0x60: o->ar = v >> 4; o->dr = v & 15; break;
      case 0x80: o->sl = v >> 4; o->rr = v & 15; break;
      case 0xE0: o->ws = (u8)(v & (opl.opl3_mode ? 7 : 3)); break;
    }
    return;
  }
  /* A0/B0/C0 are 0x10-wide register blocks — idx & 0xE0 would fold B0 into A0 and
   * silently eat every key-on write, so these dispatch on idx & 0xF0. */
  int hi = idx & 0xF0;
  if (hi == 0xA0 || hi == 0xB0 || hi == 0xC0) {
    int chn = idx & 0x0F;
    if (chn > 8) return;
    int chi = bank * 9 + chn;
    Chan *c = &opl.ch[chi];
    switch (hi) {
      case 0xA0: c->fnum = (u16)((c->fnum & 0x300) | v); update_inc(chi); break;
      case 0xB0: {
        extern int cpu_trace_faults;
        c->fnum = (u16)((c->fnum & 0xFF) | ((v & 3) << 8));
        c->block = (v >> 2) & 7;
        update_inc(chi);
        if (cpu_trace_faults >= 2 && ((v >> 5) & 1) && !c->keyon) dm_log("OPL keyon ch%d fnum=%d block=%d", chi, c->fnum, c->block);
        keyon_ch(chi, (v >> 5) & 1);
        break;
      }
      case 0xC0:
        c->fb = (v >> 1) & 7;
        c->cnt = v & 1;
        c->pan = opl.opl3_mode ? (u8)((v >> 4) & 3) : 3;
        if (!c->pan) c->pan = 3;
        break;
    }
  }
}

void opl_data(u8 v) { write_reg(opl.cur_bank, opl.cur_addr, v); }

u8 opl_status(void) {
  extern int cpu_trace_faults;
  u8 s = 0;
  if (cpu_trace_faults >= 3) dm_log("OPL status t1on=%d ctl=%02x", opl.t1_on, opl.timer_ctl);
  if (opl.t1_on && !(opl.timer_ctl & 0x40)) {
    if (emu_now_ns() - opl.t1_start >= (s64)(256 - opl.t1_reload) * 80000) s |= 0x40;
  }
  if (opl.t2_on && !(opl.timer_ctl & 0x20)) {
    if (emu_now_ns() - opl.t2_start >= (s64)(256 - opl.t2_reload) * 320000) s |= 0x20;
  }
  if (s) s |= 0x80;
  return s;
}

/* ---------------- rendering ---------------- */
static int op_output(const Op *o, u32 phase10, u32 mod, u32 extra_att) {
  u32 p = (phase10 + mod) & 0x3FF;
  u32 att;
  int sign = 0;
  u32 q = p & 0xFF;
  switch (o->ws) {
    case 0: if (p & 0x100) q ^= 0xFF; sign = (p & 0x200) != 0; att = logsin_tab[q]; break;
    case 1: if (p & 0x200) return 0; if (p & 0x100) q ^= 0xFF; att = logsin_tab[q]; break;
    case 2: if (p & 0x100) q ^= 0xFF; att = logsin_tab[q]; break;
    case 3: if (p & 0x100) return 0; att = logsin_tab[q]; break;
    case 4:
      if (p & 0x200) return 0;
      sign = (p & 0x100) != 0;
      q = (p << 1) & 0xFF;
      if (p & 0x80) q ^= 0xFF;
      att = logsin_tab[q];
      break;
    case 5:
      if (p & 0x200) return 0;
      q = (p << 1) & 0xFF;
      if (p & 0x80) q ^= 0xFF;
      att = logsin_tab[q];
      break;
    case 6: sign = (p & 0x200) != 0; att = 0; break;
    default: sign = (p & 0x200) != 0; att = (p & 0x1FF) << 3; break;
  }
  int lin = att_to_lin(att + extra_att);
  return sign ? -lin : lin;
}

static const u8 eg_inc_tab[4][8] = {
  {0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1}};

static void env_step(Op *o, const Chan *c) {
  int rate;
  switch (o->state) {
    case 1: rate = o->ar; break;
    case 2: rate = o->dr; break;
    case 3: rate = o->egt ? 0 : o->rr; break;
    case 4: rate = o->rr; break;
    default: return;
  }
  if (rate == 0) return;
  u32 r = (u32)(rate * 4 + rate_hi(o, c));
  if (r > 63) r = 63;
  int inc = 0;
  if (r < 48) {
    u32 shift = 12 - (r >> 2);
    if ((opl.eg_counter & ((1u << shift) - 1)) == 0)
      inc = eg_inc_tab[r & 3][(opl.eg_counter >> shift) & 7];
  } else {
    inc = 1 << ((r >> 2) - 12);
    inc += eg_inc_tab[r & 3][opl.eg_counter & 7] << ((r >> 2) - 12);
    if (inc > 8) inc = 8;
  }
  if (!inc) return;
  if (o->state == 1) {
    if (r >= 60) o->env = 0;
    else {
      s32 d = ((~o->env) * inc) >> 3; /* exponential attack toward 0 */
      if (d >= 0) d = -1;
      o->env += d;
    }
    if (o->env <= 0) { o->env = 0; o->state = 2; }
  } else {
    o->env += inc;
    if (o->state == 2 && (o->env >> 4) >= (s32)o->sl) o->state = 3;
    if (o->env >= 511) { o->env = 511; o->state = 0; }
  }
}

static u32 op_total_att(const Op *o, const Chan *c, u32 lfo_am) {
  u32 att = (u32)o->env << 3;
  att += (u32)o->tl << 5;
  if (o->ksl) {
    int k = ksl_tab[c->fnum >> 6] - 8 * (7 - c->block);
    if (k > 0) att += ((u32)k << 5) >> ksl_shift_tab[o->ksl];
  }
  if (o->am) att += lfo_am << 4;
  return att;
}

void opl_render(s32 *dst, u32 frames) {
  if (!tables_ready) return;
  for (u32 f = 0; f < frames; f++) {
    opl.eg_counter++;
    opl.lfo_am_cnt++;
    opl.lfo_vib_cnt++;
    u32 am_pos = (opl.lfo_am_cnt / 256) % 52; /* ~3.6 Hz triangle */
    u32 lfo_am = am_pos < 26 ? am_pos : 51 - am_pos;
    if (!opl.tre_depth) lfo_am >>= 2;
    u32 vib_pos = (opl.lfo_vib_cnt / 1024) & 7; /* ~5.9 Hz */
    opl.noise = (opl.noise >> 1) | (((opl.noise ^ (opl.noise >> 14) ^ (opl.noise >> 15) ^ (opl.noise >> 22)) & 1) << 22);

    s32 out_l = 0, out_r = 0;
    int rhythm_on = (opl.rhythm & 0x20) != 0;
    for (int chi = 0; chi < 18; chi++) {
      Chan *c = &opl.ch[chi];
      Op *o1 = ops_of(chi, 0), *o2 = ops_of(chi, 1);
      int is_rhythm = rhythm_on && chi >= 6 && chi <= 8;
      if (!is_rhythm && !c->keyon && o1->state == 0 && o2->state == 0) continue;
      u32 vib = 0;
      int vib_neg = 0;
      if (vib_pos & 3) {
        u32 delta = (u32)(c->fnum >> 7);
        if ((vib_pos & 3) == 2) delta = c->fnum >> 6 >> 1; /* peak */
        if (!opl.vib_depth) delta >>= 1;
        vib = delta;
        vib_neg = vib_pos >= 4;
      }
      env_step(o1, c);
      env_step(o2, c);
      u32 v1 = o1->vib && vib ? (u32)((((u64)vib << c->block) * mult_x2[o1->mult] / 2) * 49716 / OUT_RATE) : 0;
      u32 v2 = o2->vib && vib ? (u32)((((u64)vib << c->block) * mult_x2[o2->mult] / 2) * 49716 / OUT_RATE) : 0;
      o1->phase += vib_neg ? o1->inc - v1 : o1->inc + v1;
      o2->phase += vib_neg ? o2->inc - v2 : o2->inc + v2;
      s32 sample;
      u32 fbmod = c->fb ? (u32)((o1->fb1 + o1->fb2) >> (10 - c->fb)) : 0;
      if (is_rhythm) {
        u32 hp1 = (o1->phase >> 10) & 0x3FF;
        u32 nbit = opl.noise & 1;
        if (chi == 6) {
          int m = op_output(o1, o1->phase >> 10 << 0, fbmod, op_total_att(o1, c, lfo_am));
          o1->fb2 = o1->fb1; o1->fb1 = m;
          sample = 2 * op_output(o2, o2->phase >> 10, (u32)(m >> 1), op_total_att(o2, c, lfo_am));
        } else if (chi == 7) {
          u32 hh = (u32)(((hp1 & 0x08) ? 0x200 : 0) | 0x80 | (nbit ? 0x34 : 0xD0));
          sample = 2 * op_output(o1, hh, 0, op_total_att(o1, c, lfo_am));
          u32 sd = (u32)(0x100 | (nbit << 8) | (((o2->phase >> 19) & 1) << 9));
          sample += 2 * op_output(o2, sd, 0, op_total_att(o2, c, lfo_am));
        } else {
          sample = 2 * op_output(o1, o1->phase >> 10, 0, op_total_att(o1, c, lfo_am));
          u32 cy = (u32)(0x100 | (nbit << 9));
          sample += 2 * op_output(o2, cy, 0, op_total_att(o2, c, lfo_am));
        }
      } else if (c->cnt) {
        int m = op_output(o1, o1->phase >> 10, fbmod, op_total_att(o1, c, lfo_am));
        o1->fb2 = o1->fb1; o1->fb1 = m;
        sample = m + op_output(o2, o2->phase >> 10, 0, op_total_att(o2, c, lfo_am));
      } else {
        int m = op_output(o1, o1->phase >> 10, fbmod, op_total_att(o1, c, lfo_am));
        o1->fb2 = o1->fb1; o1->fb1 = m;
        sample = op_output(o2, o2->phase >> 10, (u32)(m >> 1), op_total_att(o2, c, lfo_am));
      }
      if (c->pan & 1) out_l += sample;
      if (c->pan & 2) out_r += sample;
    }
    dst[f * 2] += out_l << 2;
    dst[f * 2 + 1] += out_r << 2;
  }
}

void opl_init(void) {
  dm_memset(&opl, 0, sizeof opl);
  opl.noise = 1;
  for (int i = 0; i < 36; i++) {
    opl.op[i].env = 511;
    opl.op[i].ch = (u8)(slot_ch9[i % 18] + (i >= 18 ? 9 : 0));
  }
  for (int i = 0; i < 18; i++) opl.ch[i].pan = 3;
  if (!tables_ready) build_tables();
}

void opl_register_ports(void) { /* wired by sb.c together with 388h-38Bh */ }
