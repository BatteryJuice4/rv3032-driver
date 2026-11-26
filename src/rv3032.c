/*
 * rv3032.c — Micro Crystal RV-3032-C7 real-time clock (see rv3032.h). MIT licence.
 *
 * UNTESTED ON HARDWARE.
 */
#include "rv3032.h"

#include <string.h>

#define EE_TIMEOUT_MS 100 /* longest EEPROM operation: update all, ~46 ms (§4.6.3) */

const rv3032_config_t rv3032_config_default = {
    .backup = RV3032_BSM_OFF,
    .trickle = RV3032_TCM_OFF,
    .trickle_r = RV3032_TCR_600,
    .clkout_freq = RV3032_CLKOUT_32768HZ,
    .clkout_at_por = true,
    .offset = 0,
    .por_int = false,
    .vlow_int = false,
    .auto_refresh = true,
};

/* ---- Registers ------------------------------------------------------------------------- */

int rv3032_read(rv3032_t *d, uint8_t reg, uint8_t *buf, size_t n)
{
    return d->bus->xfer(d->bus->ctx, RV3032_I2C_ADDR, &reg, 1, buf, n) < 0 ? RV3032_E_BUS
                                                                          : RV3032_OK;
}

int rv3032_write(rv3032_t *d, uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tx[1 + 16];
    if (n > 16) {
        return RV3032_E_ARG;
    }
    tx[0] = reg;
    memcpy(tx + 1, buf, n);
    return d->bus->xfer(d->bus->ctx, RV3032_I2C_ADDR, tx, n + 1, NULL, 0) < 0 ? RV3032_E_BUS
                                                                             : RV3032_OK;
}

static int write1(rv3032_t *d, uint8_t reg, uint8_t v)
{
    return rv3032_write(d, reg, &v, 1);
}

int rv3032_update(rv3032_t *d, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t v;
    int r = rv3032_read(d, reg, &v, 1);
    if (r) {
        return r;
    }
    return write1(d, reg, (uint8_t)((v & ~mask) | (value & mask)));
}

/* ---- BCD and time ---------------------------------------------------------------------- */

static uint8_t to_bcd(unsigned v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

/* -1 if not valid BCD. */
static int from_bcd(uint8_t b)
{
    if ((b & 0x0Fu) > 9u || (b >> 4) > 9u) {
        return -1;
    }
    return (b >> 4) * 10 + (b & 0x0F);
}

static bool leap(unsigned y)
{
    return (y % 4u == 0 && y % 100u != 0) || y % 400u == 0;
}

static unsigned days_in_month(unsigned y, unsigned m)
{
    static const uint8_t dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29u : dim[m - 1];
}

static bool time_ok(const rv3032_time_t *t)
{
    return t->year >= 2000 && t->year <= 2099 && t->month >= 1 && t->month <= 12 &&
           t->day >= 1 && t->day <= days_in_month(t->year, t->month) && t->hour <= 23 &&
           t->minute <= 59 && t->second <= 59 && t->weekday <= 6;
}

/* Decode 01h..07h; false if any field is not a valid value. */
static bool decode(const uint8_t r[7], rv3032_time_t *t)
{
    int s = from_bcd(r[0] & 0x7F), mi = from_bcd(r[1] & 0x7F), h = from_bcd(r[2] & 0x3F);
    int dd = from_bcd(r[4] & 0x3F), mo = from_bcd(r[5] & 0x1F), y = from_bcd(r[6]);
    if (s < 0 || mi < 0 || h < 0 || dd < 0 || mo < 0 || y < 0) {
        return false;
    }
    t->second = (uint8_t)s;
    t->minute = (uint8_t)mi;
    t->hour = (uint8_t)h;
    t->weekday = r[3] & 0x07;
    t->day = (uint8_t)dd;
    t->month = (uint8_t)mo;
    t->year = (uint16_t)(2000 + y);
    return time_ok(t);
}

int rv3032_probe(rv3032_t *d)
{
    uint8_t r[7];
    int e = rv3032_read(d, RV3032_REG_SECONDS, r, sizeof(r));
    if (e) {
        return e;
    }
    rv3032_time_t t;
    return decode(r, &t) ? RV3032_OK : RV3032_E_INVALID;
}

int rv3032_get_time(rv3032_t *d, rv3032_time_t *t)
{
    /* One burst 00h-07h. The hundredths are not frozen during the burst (§4.5): at 00 or
     * 99 a second may have ticked mid-read, so read again until two reads agree. */
    uint8_t a[8], b[8];
    int e = rv3032_read(d, RV3032_REG_100TH, a, sizeof(a));
    for (int tries = 0; !e && (a[0] == 0x00 || a[0] == 0x99) && tries < 3; tries++) {
        e = rv3032_read(d, RV3032_REG_100TH, b, sizeof(b));
        if (!e && !memcmp(a + 1, b + 1, 7)) {
            break;
        }
        memcpy(a, b, sizeof(a));
    }
    if (e) {
        return e;
    }
    int hs = from_bcd(a[0]);
    if (hs < 0 || !decode(a + 1, t)) {
        return RV3032_E_INVALID;
    }
    t->hundredths = (uint8_t)hs;
    return RV3032_OK;
}

int rv3032_set_time(rv3032_t *d, const rv3032_time_t *t)
{
    if (!time_ok(t)) {
        return RV3032_E_ARG;
    }
    const uint8_t r[7] = {to_bcd(t->second), to_bcd(t->minute), to_bcd(t->hour),
                          (uint8_t)(t->weekday & 7u), to_bcd(t->day), to_bcd(t->month),
                          to_bcd(t->year - 2000u)};
    return rv3032_write(d, RV3032_REG_SECONDS, r, sizeof(r));
}

/* Days since 1970-01-01 for a civil date (H. Hinnant's days_from_civil). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned dd)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp = m > 2 ? m - 3u : m + 9u; /* March = 0 */
    unsigned doy = (153u * mp + 2u) / 5u + dd - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t rv3032_to_unix(const rv3032_time_t *t)
{
    return days_from_civil(t->year, t->month, t->day) * 86400 + t->hour * 3600 +
           t->minute * 60 + t->second;
}

int rv3032_from_unix(int64_t secs, rv3032_time_t *t)
{
    int64_t days = secs >= 0 ? secs / 86400 : (secs - 86399) / 86400;
    int64_t rem = secs - days * 86400;
    /* civil_from_days */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    unsigned dd = doy - (153u * mp + 2u) / 5u + 1u;
    unsigned m = mp < 10 ? mp + 3u : mp - 9u;
    y += m <= 2;
    if (y < 2000 || y > 2099) {
        return RV3032_E_ARG;
    }
    t->year = (uint16_t)y;
    t->month = (uint8_t)m;
    t->day = (uint8_t)dd;
    t->hour = (uint8_t)(rem / 3600);
    t->minute = (uint8_t)(rem / 60 % 60);
    t->second = (uint8_t)(rem % 60);
    t->weekday = (uint8_t)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday (4) */
    t->hundredths = 0;
    return RV3032_OK;
}

/* ---- Status ---------------------------------------------------------------------------- */

int rv3032_get_status(rv3032_t *d, uint8_t *status)
{
    return rv3032_read(d, RV3032_REG_STATUS, status, 1);
}

int rv3032_clear_status(rv3032_t *d, uint8_t flags)
{
    /* Flags clear on 0 and ignore 1 (§3.7), so no read-modify-write race with new events. */
    return write1(d, RV3032_REG_STATUS, (uint8_t)~flags);
}

int rv3032_time_lost(rv3032_t *d, bool *lost)
{
    uint8_t s;
    int e = rv3032_get_status(d, &s);
    if (!e) {
        *lost = (s & (RV3032_ST_PORF | RV3032_ST_VLF)) != 0;
    }
    return e;
}

/* ---- Alarm ----------------------------------------------------------------------------- */

int rv3032_set_alarm(rv3032_t *d, uint8_t day, uint8_t hour, uint8_t minute, bool int_pin)
{
    if (day < 1 || day > 31 || hour > 23 || minute > 59) {
        return RV3032_E_ARG;
    }
    /* §4.10.2: AIE and AF to 0, then the alarm, then AIE. */
    int e = rv3032_update(d, RV3032_REG_CTRL2, RV3032_C2_AIE, 0);
    if (!e) {
        e = rv3032_clear_status(d, RV3032_ST_AF);
    }
    if (!e) {
        const uint8_t a[3] = {to_bcd(minute), to_bcd(hour), to_bcd(day)}; /* AE_x = 0: on */
        e = rv3032_write(d, RV3032_REG_ALM_MIN, a, sizeof(a));
    }
    if (!e && int_pin) {
        e = rv3032_update(d, RV3032_REG_CTRL2, RV3032_C2_AIE, RV3032_C2_AIE);
    }
    return e;
}

int rv3032_alarm_off(rv3032_t *d)
{
    int e = rv3032_update(d, RV3032_REG_CTRL2, RV3032_C2_AIE, 0);
    if (!e) {
        /* All three AE bits set would mean "every minute" (§4.10.2 note 3); instead keep
         * them enabled with the invalid date 00, which never matches (note 2). */
        const uint8_t a[3] = {0x00, 0x00, 0x00};
        e = rv3032_write(d, RV3032_REG_ALM_MIN, a, sizeof(a));
    }
    if (!e) {
        e = rv3032_clear_status(d, RV3032_ST_AF);
    }
    return e;
}

int rv3032_alarm_fired(rv3032_t *d, bool *fired)
{
    uint8_t s;
    int e = rv3032_get_status(d, &s);
    if (e) {
        return e;
    }
    *fired = (s & RV3032_ST_AF) != 0;
    return *fired ? rv3032_clear_status(d, RV3032_ST_AF) : RV3032_OK;
}

/* ---- Temperature ----------------------------------------------------------------------- */

int rv3032_temperature(rv3032_t *d, int16_t *sixteenths)
{
    /* No shadowing between the two bytes (§3.8 note): read twice and compare. */
    uint8_t a[2], b[2];
    int e = rv3032_read(d, RV3032_REG_TEMP_LSB, a, 2);
    if (!e) {
        e = rv3032_read(d, RV3032_REG_TEMP_LSB, b, 2);
    }
    if (!e && (a[1] != b[1] || (a[0] & 0xF0) != (b[0] & 0xF0))) {
        e = rv3032_read(d, RV3032_REG_TEMP_LSB, a, 2);
    }
    if (e) {
        return e;
    }
    uint16_t raw = (uint16_t)(((unsigned)a[1] << 4) | (a[0] >> 4));
    *sixteenths = (int16_t)(raw & 0x800u ? (int)raw - 4096 : (int)raw);
    return RV3032_OK;
}

/* ---- Clock output ---------------------------------------------------------------------- */

int rv3032_clkout(rv3032_t *d, bool on)
{
    return rv3032_update(d, RV3032_REG_PMU, RV3032_PMU_NCLKE, on ? 0 : RV3032_PMU_NCLKE);
}

/* ---- EEPROM (§4.6) --------------------------------------------------------------------- */

/* Wait until EEbusy is 0, sleeping `first_ms` first and then 1 ms between checks. */
static int ee_wait(rv3032_t *d, unsigned first_ms)
{
    unsigned waited = 0;
    if (first_ms) {
        d->bus->delay_ms(d->bus->ctx, first_ms);
        waited = first_ms;
    }
    for (;;) {
        uint8_t tl;
        int e = rv3032_read(d, RV3032_REG_TEMP_LSB, &tl, 1);
        if (e) {
            return e;
        }
        if (!(tl & RV3032_TL_EEBUSY)) {
            return RV3032_OK;
        }
        if (waited >= EE_TIMEOUT_MS) {
            return RV3032_E_BUSY;
        }
        d->bus->delay_ms(d->bus->ctx, 1);
        waited++;
    }
}

/*
 * Run one EEPROM operation with the §4.6 procedure around it: daily refresh off (EERD),
 * backup switchover off in the RAM mirror (C0h; §3.20.1 asks for it during EEPROM access),
 * EEbusy idle, the command, wait, EEF checked; then C0h and EERD as they were.
 */
static int ee_op(rv3032_t *d, uint8_t addr, uint8_t cmd, uint8_t data, uint8_t *out)
{
    uint8_t c1, pmu;
    int e = rv3032_read(d, RV3032_REG_CTRL1, &c1, 1);
    if (!e) {
        e = rv3032_read(d, RV3032_REG_PMU, &pmu, 1);
    }
    if (e) {
        return e;
    }
    uint8_t bsm = (uint8_t)((pmu & RV3032_PMU_BSM_MASK) >> RV3032_PMU_BSM_SHIFT);
    bool bsm_on = bsm == RV3032_BSM_DSM || bsm == RV3032_BSM_LSM;

    e = write1(d, RV3032_REG_CTRL1, (uint8_t)(c1 | RV3032_C1_EERD));
    if (!e && bsm_on) {
        e = write1(d, RV3032_REG_PMU, (uint8_t)(pmu & ~RV3032_PMU_BSM_MASK));
    }
    if (!e) {
        e = ee_wait(d, 0);
    }
    if (!e && cmd == RV3032_EECMD_WRITE) {
        uint8_t tl = (uint8_t)~RV3032_TL_EEF; /* clear an old EEF; other flags stay */
        e = rv3032_write(d, RV3032_REG_TEMP_LSB, &tl, 1);
    }
    if (!e) {
        const uint8_t seq[3] = {addr, data, cmd}; /* 3Dh EEADDR, 3Eh EEDATA, 3Fh EECMD */
        e = rv3032_write(d, RV3032_REG_EEADDR, seq, cmd == RV3032_EECMD_WRITE ? 3 : 1);
        if (!e && cmd != RV3032_EECMD_WRITE) {
            e = write1(d, RV3032_REG_EECMD, cmd);
        }
    }
    if (!e) {
        e = ee_wait(d, cmd == RV3032_EECMD_WRITE ? 10u : 1u); /* §4.6.7 flowchart */
    }
    if (!e && cmd == RV3032_EECMD_WRITE) {
        uint8_t tl;
        e = rv3032_read(d, RV3032_REG_TEMP_LSB, &tl, 1);
        if (!e && (tl & RV3032_TL_EEF)) {
            e = RV3032_E_EEPROM;
        }
    }
    if (!e && out) {
        e = rv3032_read(d, RV3032_REG_EEDATA, out, 1);
    }
    /* Put back what we changed, even after an error. */
    int e2 = bsm_on ? write1(d, RV3032_REG_PMU, pmu) : RV3032_OK;
    int e3 = write1(d, RV3032_REG_CTRL1, c1);
    return e ? e : e2 ? e2 : e3;
}

static bool ee_addr_ok(uint8_t addr)
{
    return addr >= RV3032_REG_PMU && addr <= RV3032_EE_USER_END;
}

int rv3032_eeprom_read(rv3032_t *d, uint8_t addr, uint8_t *value)
{
    if (!ee_addr_ok(addr)) {
        return RV3032_E_ARG;
    }
    return ee_op(d, addr, RV3032_EECMD_READ, 0, value);
}

int rv3032_eeprom_write(rv3032_t *d, uint8_t addr, uint8_t value)
{
    if (!ee_addr_ok(addr)) {
        return RV3032_E_ARG;
    }
    return ee_op(d, addr, RV3032_EECMD_WRITE, value, NULL);
}

/* ---- Configuration --------------------------------------------------------------------- */

int rv3032_configure(rv3032_t *d, const rv3032_config_t *c, bool store)
{
    if (c->backup > RV3032_BSM_LSM || c->trickle > RV3032_TCM_4V5 ||
        c->trickle_r > RV3032_TCR_12K || c->clkout_freq > RV3032_CLKOUT_1HZ ||
        c->offset < -32 || c->offset > 31 ||
        (c->trickle != RV3032_TCM_OFF && c->backup == RV3032_BSM_OFF) ||
        (c->trickle >= RV3032_TCM_3V0 && c->backup != RV3032_BSM_LSM)) {
        return RV3032_E_ARG;
    }
    uint8_t pmu = (uint8_t)((c->clkout_at_por ? 0 : RV3032_PMU_NCLKE) |
                            (c->backup << RV3032_PMU_BSM_SHIFT) |
                            (c->trickle_r << RV3032_PMU_TCR_SHIFT) | c->trickle);
    uint8_t off = (uint8_t)((c->por_int ? 0x80u : 0) | (c->vlow_int ? 0x40u : 0) |
                            ((uint8_t)c->offset & 0x3Fu));
    uint8_t co2;
    int e = rv3032_read(d, RV3032_REG_CLKOUT2, &co2, 1);
    if (e) {
        return e;
    }
    /* XTAL mode with the chosen divider; HFD bits left as they are. */
    co2 = (uint8_t)((co2 & 0x1Fu) | (c->clkout_freq << RV3032_CO2_FD_SHIFT));

    if (store) {
        const uint8_t addr[3] = {RV3032_REG_PMU, RV3032_REG_OFFSET, RV3032_REG_CLKOUT2};
        const uint8_t want[3] = {pmu, off, co2};
        for (int i = 0; i < 3 && !e; i++) {
            uint8_t have;
            e = rv3032_eeprom_read(d, addr[i], &have);
            if (!e && have != want[i]) {
                e = rv3032_eeprom_write(d, addr[i], want[i]);
            }
        }
        if (e) {
            return e;
        }
    }
    /* In effect now: the RAM mirrors. C0h last, so switchover starts with the rest set. */
    e = write1(d, RV3032_REG_CLKOUT2, co2);
    if (!e) {
        e = write1(d, RV3032_REG_OFFSET, off);
    }
    if (!e) {
        e = write1(d, RV3032_REG_PMU, pmu);
    }
    if (!e) {
        e = rv3032_update(d, RV3032_REG_CTRL1, RV3032_C1_EERD,
                          c->auto_refresh ? 0 : RV3032_C1_EERD);
    }
    return e;
}
