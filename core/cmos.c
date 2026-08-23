#include "cmos.h"
#include "io.h"
#include "sched.h"
#include "pic.h"

static u8 cmos[128];
static u8 index_reg;
static s64 epoch_secs;  /* seconds since 0000-03-01 proleptic (days*86400) at emu_ns = 0 */
static s64 set_ns;      /* emu_ns when the epoch was set */
static s64 periodic_ns;

/* days from civil (Howard Hinnant's algorithm), usable for 1980-2099 */
static s64 days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  s64 era = (y >= 0 ? y : y - 399) / 400;
  s64 yoe = y - era * 400;
  s64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  s64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
static void civil_from_days(s64 z, int *y, int *m, int *d) {
  z += 719468;
  s64 era = (z >= 0 ? z : z - 146096) / 146097;
  s64 doe = z - era * 146097;
  s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  s64 yy = yoe + era * 400;
  s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  s64 mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yy + (*m <= 2));
}

void cmos_set_time(int year, int month, int day, int hour, int min, int sec) {
  epoch_secs = days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
  set_ns = emu_ns;
}

void cmos_get_time(int *year, int *month, int *day, int *hour, int *min, int *sec, int *wday) {
  s64 secs = epoch_secs + (emu_ns - set_ns) / 1000000000LL;
  s64 days = secs / 86400;
  s64 rem = secs % 86400;
  if (rem < 0) { rem += 86400; days--; }
  civil_from_days(days, year, month, day);
  *hour = (int)(rem / 3600);
  *min = (int)(rem % 3600 / 60);
  *sec = (int)(rem % 60);
  s64 w = (days + 4) % 7; /* 1970-01-01 was a Thursday */
  if (w < 0) w += 7;
  *wday = (int)w + 1; /* RTC: 1 = Sunday */
}

static u8 to_bcd(int v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static u8 enc(int v) { return (cmos[0x0B] & 4) ? (u8)v : to_bcd(v); }

static u8 rtc_read(int reg) {
  int y, mo, d, h, mi, s, wd;
  cmos_get_time(&y, &mo, &d, &h, &mi, &s, &wd);
  switch (reg) {
    case 0x00: return enc(s);
    case 0x02: return enc(mi);
    case 0x04:
      if (cmos[0x0B] & 2) return enc(h);
      { int h12 = h % 12; if (h12 == 0) h12 = 12; return (u8)(enc(h12) | (h >= 12 ? 0x80 : 0)); }
    case 0x06: return enc(wd);
    case 0x07: return enc(d);
    case 0x08: return enc(mo);
    case 0x09: return enc(y % 100);
    case 0x32: return enc(y / 100);
    default: return cmos[reg];
  }
}

static void rtc_periodic(void) {
  cmos[0x0C] |= 0xC0; /* IRQF + PF */
  pic_raise_irq(8);
  sched_set(EV_RTC, emu_ns + periodic_ns, rtc_periodic);
}

static void update_periodic(void) {
  int rate = cmos[0x0A] & 0x0F;
  if ((cmos[0x0B] & 0x40) && rate) {
    if (rate < 3) rate += 7;
    periodic_ns = (s64)(1000000000LL >> (16 - rate));
    if (!sched_active(EV_RTC)) sched_set(EV_RTC, emu_ns + periodic_ns, rtc_periodic);
  } else {
    sched_cancel(EV_RTC);
  }
}

static void wr_cmos(u16 port, u8 v) {
  if (port == 0x70) { index_reg = v & 0x7F; return; }
  int r = index_reg;
  switch (r) {
    case 0x0A: cmos[r] = v & 0x7F; update_periodic(); break;
    case 0x0B: cmos[r] = v; update_periodic(); break;
    case 0x0C: case 0x0D: break;
    case 0x00: case 0x02: case 0x04: case 0x06: case 0x07: case 0x08: case 0x09: case 0x32:
      break; /* setting the clock from the guest: accepted silently, host time stays authoritative */
    default: cmos[r] = v; break;
  }
}

static u8 rd_cmos(u16 port) {
  if (port == 0x70) return index_reg;
  int r = index_reg;
  if (r <= 0x09 || r == 0x32) return rtc_read(r);
  if (r == 0x0A) return cmos[r] & 0x7F; /* UIP never set */
  if (r == 0x0C) { u8 v = cmos[r]; cmos[r] = 0; pic_lower_irq(8); return v; }
  if (r == 0x0D) return 0x80; /* battery good */
  return cmos[r];
}

void cmos_set_byte(int reg, u8 v) { cmos[reg & 0x7F] = v; }
u8 cmos_get_byte(int reg) { return cmos[reg & 0x7F]; }

void cmos_init(void) {
  dm_memset(cmos, 0, sizeof cmos);
  cmos[0x0A] = 0x26;
  cmos[0x0B] = 0x02; /* 24-hour, BCD */
  cmos[0x0D] = 0x80;
  cmos[0x0E] = 0x00;
  index_reg = 0;
  periodic_ns = 0;
  cmos_set_time(1994, 1, 1, 12, 0, 0);
}

void cmos_register_ports(void) { io_register(0x70, 2, rd_cmos, wr_cmos); }
