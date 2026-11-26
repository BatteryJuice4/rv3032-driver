/*
 * test_rv3032.c — the RV-3032-C7 driver (src/rv3032.c) against a fake chip.
 * https://github.com/BatteryJuice4/rv3032-driver — MIT licence.
 *
 * The fake follows the application manual (Rev. 1.3): address auto-increment, status
 * flags that clear on 0 and ignore 1 (and THF/TLF cleared by any status write, §3.7),
 * EEPROM commands 21h/22h through EEADDR/EEDATA/EECMD with EEbusy for a few reads and
 * EEF on a failed write (§4.6), and the configuration RAM mirror. It also records the
 * conditions §4.6 asks for at each EEPROM command (EERD = 1, EEbusy = 0, switchover off).
 */
#include <string.h>

#include "rv3032.h"
#include "test.h"

static uint8_t g_reg[256];
static uint8_t g_ee[256];
static bool g_present = true;
static int g_busy;           /* reads of 0Eh left with EEbusy set */
static bool g_fail_write;    /* the next EEPROM write fails (EEF) */
static int g_ee_writes;      /* EEPROM bytes written */
static bool g_bad_ee_access; /* an EEPROM command without the §4.6 conditions */
static int g_tick_on_read;   /* the time ticks once after this many burst reads (-1 off) */
static unsigned g_delay_ms;

static void tick(void)
{
    /* Only what the tests need: 59.99 s -> next minute. */
    g_reg[0] = 0x00;
    g_reg[1] = 0x00;
    g_reg[2] = (uint8_t)(g_reg[2] + 1);
}

static void reg_write(uint8_t a, uint8_t v)
{
    switch (a) {
    case RV3032_REG_100TH:
    case RV3032_REG_TEMP_MSB:
        return; /* read only */
    case RV3032_REG_SECONDS:
        g_reg[RV3032_REG_100TH] = 0; /* §4.5.1 */
        break;
    case RV3032_REG_STATUS:
        g_reg[a] &= v; /* clear on 0 */
        g_reg[a] &= (uint8_t)~(RV3032_ST_THF | RV3032_ST_TLF);
        return;
    case RV3032_REG_TEMP_LSB: {
        uint8_t keep = (uint8_t)(g_reg[a] & 0xF4); /* TEMP and EEbusy read only */
        uint8_t fl = (uint8_t)(g_reg[a] & v & 0x0B);
        g_reg[a] = (uint8_t)(keep | fl);
        return;
    }
    case RV3032_REG_EECMD: {
        uint8_t bsm = (uint8_t)((g_reg[RV3032_REG_PMU] & RV3032_PMU_BSM_MASK) >> 4);
        if (!(g_reg[RV3032_REG_CTRL1] & RV3032_C1_EERD) || g_busy ||
            bsm == RV3032_BSM_DSM || bsm == RV3032_BSM_LSM) {
            g_bad_ee_access = true;
        }
        uint8_t ea = g_reg[RV3032_REG_EEADDR];
        if (v == RV3032_EECMD_WRITE) {
            if (g_fail_write) {
                g_reg[RV3032_REG_TEMP_LSB] |= RV3032_TL_EEF;
                g_fail_write = false;
            } else {
                g_ee[ea] = g_reg[RV3032_REG_EEDATA];
                g_ee_writes++;
            }
            g_busy = 3;
        } else if (v == RV3032_EECMD_READ) {
            g_reg[RV3032_REG_EEDATA] = g_ee[ea];
            g_busy = 1;
        }
        return; /* write only */
    }
    default:
        break;
    }
    g_reg[a] = v;
}

static uint8_t reg_read(uint8_t a)
{
    if (a == RV3032_REG_TEMP_LSB) {
        uint8_t v = (uint8_t)(g_reg[a] & ~RV3032_TL_EEBUSY);
        if (g_busy > 0) {
            g_busy--;
            v |= RV3032_TL_EEBUSY;
        }
        return v;
    }
    if (a == RV3032_REG_EECMD) {
        return 0;
    }
    return g_reg[a];
}

static int f_xfer(void *ctx, uint8_t addr, const uint8_t *tx, size_t ntx, uint8_t *rx, size_t nrx)
{
    (void)ctx;
    if (!g_present || addr != RV3032_I2C_ADDR || ntx < 1) {
        return -1;
    }
    uint8_t p = tx[0];
    for (size_t i = 1; i < ntx; i++) {
        reg_write(p++, tx[i]);
    }
    if (nrx && p == RV3032_REG_100TH && g_tick_on_read >= 0 && g_tick_on_read-- == 0) {
        tick();
    }
    for (size_t i = 0; i < nrx; i++) {
        rx[i] = reg_read(p++);
    }
    return 0;
}

static void f_delay(void *ctx, unsigned ms)
{
    (void)ctx;
    g_delay_ms += ms;
}

static const rv3032_bus_t g_bus = {NULL, f_xfer, f_delay};

static void chip_reset(void)
{
    memset(g_reg, 0, sizeof(g_reg));
    memset(g_ee, 0, sizeof(g_ee));
    g_reg[RV3032_REG_DATE] = 0x01; /* reset values (§3.22) */
    g_reg[RV3032_REG_MONTH] = 0x01;
    g_reg[RV3032_REG_STATUS] = RV3032_ST_PORF;
    g_reg[RV3032_REG_EEADDR] = 0xC0;
    g_busy = 0;
    g_ee_writes = 0;
    g_bad_ee_access = false;
    g_tick_on_read = -1;
}

int main(void)
{
    rv3032_t d = {&g_bus};
    chip_reset();

    /* ---- probe ---- */
    CHECK_EQ(rv3032_probe(&d), RV3032_OK);
    g_present = false;
    CHECK_EQ(rv3032_probe(&d), RV3032_E_BUS);
    g_present = true;

    /* ---- after power-on: time lost, until cleared ---- */
    bool lost = false;
    CHECK_EQ(rv3032_time_lost(&d, &lost), RV3032_OK);
    CHECK(lost);

    /* ---- set and read the time (one burst from 01h, BCD) ---- */
    rv3032_time_t t = {.year = 2026, .month = 11, .day = 25, .hour = 23, .minute = 59,
                       .second = 58, .weekday = 3};
    g_reg[RV3032_REG_100TH] = 0x42;
    CHECK_EQ(rv3032_set_time(&d, &t), RV3032_OK);
    CHECK_EQ(g_reg[RV3032_REG_SECONDS], 0x58);
    CHECK_EQ(g_reg[RV3032_REG_MINUTES], 0x59);
    CHECK_EQ(g_reg[RV3032_REG_HOURS], 0x23);
    CHECK_EQ(g_reg[RV3032_REG_WEEKDAY], 3);
    CHECK_EQ(g_reg[RV3032_REG_DATE], 0x25);
    CHECK_EQ(g_reg[RV3032_REG_MONTH], 0x11);
    CHECK_EQ(g_reg[RV3032_REG_YEAR], 0x26);
    CHECK_EQ(g_reg[RV3032_REG_100TH], 0); /* writing seconds restarts the second */
    g_reg[RV3032_REG_100TH] = 0x37;
    rv3032_time_t r;
    CHECK_EQ(rv3032_get_time(&d, &r), RV3032_OK);
    CHECK_EQ(r.year, 2026);
    CHECK_EQ(r.month, 11);
    CHECK_EQ(r.day, 25);
    CHECK_EQ(r.hour, 23);
    CHECK_EQ(r.minute, 59);
    CHECK_EQ(r.second, 58);
    CHECK_EQ(r.hundredths, 37);

    /* bad values are refused */
    rv3032_time_t bad = t;
    bad.day = 31; /* November has 30 */
    CHECK_EQ(rv3032_set_time(&d, &bad), RV3032_E_ARG);
    bad = t;
    bad.year = 2100;
    CHECK_EQ(rv3032_set_time(&d, &bad), RV3032_E_ARG);
    bad = t;
    bad.month = 2;
    bad.day = 29; /* 2026 is not a leap year */
    CHECK_EQ(rv3032_set_time(&d, &bad), RV3032_E_ARG);
    bad.year = 2028;
    CHECK_EQ(rv3032_set_time(&d, &bad), RV3032_OK);
    CHECK_EQ(rv3032_set_time(&d, &t), RV3032_OK);

    /* garbage in the registers is reported, not returned as a time */
    g_reg[RV3032_REG_MINUTES] = 0x5A;
    CHECK_EQ(rv3032_get_time(&d, &r), RV3032_E_INVALID);
    CHECK_EQ(rv3032_probe(&d), RV3032_E_INVALID);
    g_reg[RV3032_REG_MINUTES] = 0x59;

    /* ---- hundredths at 99: read again; a tick between the reads is caught ---- */
    g_reg[RV3032_REG_SECONDS] = 0x59;
    g_reg[RV3032_REG_MINUTES] = 0x10;
    g_reg[RV3032_REG_HOURS] = 0x08;
    g_reg[RV3032_REG_100TH] = 0x99;
    g_tick_on_read = 1; /* the second read sees the next minute */
    CHECK_EQ(rv3032_get_time(&d, &r), RV3032_OK);
    CHECK_EQ(r.hour, 8);
    CHECK_EQ(r.minute, 11);
    CHECK_EQ(r.second, 0);
    g_tick_on_read = -1;

    /* ---- Unix time <-> calendar ---- */
    rv3032_time_t u;
    CHECK_EQ(rv3032_from_unix(946684800, &u), RV3032_OK); /* 2000-01-01 00:00:00, Sat */
    CHECK_EQ(u.year, 2000);
    CHECK_EQ(u.month, 1);
    CHECK_EQ(u.day, 1);
    CHECK_EQ(u.weekday, 6);
    CHECK_EQ(rv3032_from_unix(1795564800 + 3600 * 13 + 60 * 5 + 9, &u), RV3032_OK);
    CHECK_EQ(u.year, 2026); /* 2026-11-25 13:05:09, a Wednesday */
    CHECK_EQ(u.month, 11);
    CHECK_EQ(u.day, 25);
    CHECK_EQ(u.hour, 13);
    CHECK_EQ(u.minute, 5);
    CHECK_EQ(u.second, 9);
    CHECK_EQ(u.weekday, 3);
    CHECK_EQ(rv3032_to_unix(&u), 1795564800 + 3600 * 13 + 60 * 5 + 9);
    CHECK_EQ(rv3032_from_unix(951782400, &u), RV3032_OK); /* 2000-02-29 (leap) */
    CHECK_EQ(u.month, 2);
    CHECK_EQ(u.day, 29);
    CHECK_EQ(rv3032_from_unix(4102444799LL, &u), RV3032_OK); /* 2099-12-31 23:59:59 */
    CHECK_EQ(u.year, 2099);
    CHECK_EQ(rv3032_from_unix(4102444800LL, &u), RV3032_E_ARG); /* 2100 */
    CHECK_EQ(rv3032_from_unix(946684799, &u), RV3032_E_ARG);     /* 1999 */
    bool round_trip = true;
    for (int64_t s = 946684800; s < 4102444800LL; s += 86400 * 37 + 3601) {
        rv3032_time_t x;
        round_trip &= rv3032_from_unix(s, &x) == RV3032_OK && rv3032_to_unix(&x) == s;
    }
    CHECK(round_trip);

    /* ---- status: clearing one flag leaves the others ---- */
    g_reg[RV3032_REG_STATUS] = RV3032_ST_PORF | RV3032_ST_VLF | RV3032_ST_AF;
    CHECK_EQ(rv3032_clear_status(&d, RV3032_ST_PORF | RV3032_ST_VLF), RV3032_OK);
    CHECK_EQ(g_reg[RV3032_REG_STATUS], RV3032_ST_AF);
    CHECK_EQ(rv3032_time_lost(&d, &lost), RV3032_OK);
    CHECK(!lost);

    /* ---- alarm: AIE off, AF cleared, registers with AE = 0, then AIE ---- */
    CHECK_EQ(rv3032_set_alarm(&d, 26, 7, 30, true), RV3032_OK);
    CHECK_EQ(g_reg[RV3032_REG_ALM_MIN], 0x30);
    CHECK_EQ(g_reg[RV3032_REG_ALM_HOUR], 0x07);
    CHECK_EQ(g_reg[RV3032_REG_ALM_DATE], 0x26);
    CHECK_EQ(g_reg[RV3032_REG_STATUS] & RV3032_ST_AF, 0);
    CHECK(g_reg[RV3032_REG_CTRL2] & RV3032_C2_AIE);
    CHECK_EQ(rv3032_set_alarm(&d, 0, 7, 30, true), RV3032_E_ARG);
    CHECK_EQ(rv3032_set_alarm(&d, 1, 24, 0, true), RV3032_E_ARG);
    bool fired = true;
    CHECK_EQ(rv3032_alarm_fired(&d, &fired), RV3032_OK);
    CHECK(!fired);
    g_reg[RV3032_REG_STATUS] |= RV3032_ST_AF;
    CHECK_EQ(rv3032_alarm_fired(&d, &fired), RV3032_OK);
    CHECK(fired);
    CHECK_EQ(g_reg[RV3032_REG_STATUS] & RV3032_ST_AF, 0);
    CHECK_EQ(rv3032_set_alarm(&d, 1, 0, 0, false), RV3032_OK);
    CHECK(!(g_reg[RV3032_REG_CTRL2] & RV3032_C2_AIE));
    /* off: AIE 0, and never "every minute" (all AE bits set) */
    CHECK_EQ(rv3032_alarm_off(&d), RV3032_OK);
    CHECK(!(g_reg[RV3032_REG_CTRL2] & RV3032_C2_AIE));
    CHECK(!((g_reg[8] & g_reg[9] & g_reg[10]) & RV3032_AE_OFF));
    CHECK_EQ(g_reg[RV3032_REG_ALM_DATE], 0x00); /* invalid date: never matches */

    /* ---- temperature: 12-bit two's complement, 1/16 °C ---- */
    int16_t temp;
    g_reg[RV3032_REG_TEMP_MSB] = 0x19;
    g_reg[RV3032_REG_TEMP_LSB] = 0x40; /* 0x194 = 404 -> 25.25 °C */
    CHECK_EQ(rv3032_temperature(&d, &temp), RV3032_OK);
    CHECK_EQ(temp, 404);
    g_reg[RV3032_REG_TEMP_MSB] = 0xD8; /* 0xD80 = -640 -> -40 °C (§3.8 table) */
    g_reg[RV3032_REG_TEMP_LSB] = 0x00;
    CHECK_EQ(rv3032_temperature(&d, &temp), RV3032_OK);
    CHECK_EQ(temp, -640);
    g_reg[RV3032_REG_TEMP_MSB] = 0xFF;
    g_reg[RV3032_REG_TEMP_LSB] = 0xF3; /* -0.0625 °C, with flag bits set below */
    CHECK_EQ(rv3032_temperature(&d, &temp), RV3032_OK);
    CHECK_EQ(temp, -1);
    g_reg[RV3032_REG_TEMP_LSB] = 0x00;

    /* ---- EEPROM byte access, with the §4.6 conditions ---- */
    g_reg[RV3032_REG_PMU] = (uint8_t)(RV3032_BSM_LSM << RV3032_PMU_BSM_SHIFT); /* on */
    g_reg[RV3032_REG_CTRL1] = 0;                                             /* EERD 0 */
    g_busy = 2; /* still busy from before: must wait */
    CHECK_EQ(rv3032_eeprom_write(&d, RV3032_EE_USER, 0xA5), RV3032_OK);
    CHECK_EQ(g_ee[RV3032_EE_USER], 0xA5);
    CHECK(!g_bad_ee_access);
    CHECK_EQ(g_reg[RV3032_REG_PMU], RV3032_BSM_LSM << RV3032_PMU_BSM_SHIFT); /* restored */
    CHECK_EQ(g_reg[RV3032_REG_CTRL1] & RV3032_C1_EERD, 0);                 /* restored */
    CHECK(g_delay_ms >= 10);
    uint8_t v = 0;
    CHECK_EQ(rv3032_eeprom_read(&d, RV3032_EE_USER, &v), RV3032_OK);
    CHECK_EQ(v, 0xA5);
    CHECK(!g_bad_ee_access);
    CHECK_EQ(rv3032_eeprom_write(&d, 0x40, 1), RV3032_E_ARG); /* RAM, not EEPROM */
    CHECK_EQ(rv3032_eeprom_write(&d, 0xEB, 1), RV3032_E_ARG);
    g_fail_write = true;
    CHECK_EQ(rv3032_eeprom_write(&d, RV3032_EE_USER + 1, 0x11), RV3032_E_EEPROM);
    CHECK_EQ(g_reg[RV3032_REG_PMU], RV3032_BSM_LSM << RV3032_PMU_BSM_SHIFT); /* still */
    CHECK_EQ(rv3032_eeprom_write(&d, RV3032_EE_USER + 1, 0x11), RV3032_OK); /* EEF cleared */
    g_busy = 1000; /* stuck */
    CHECK_EQ(rv3032_eeprom_write(&d, RV3032_EE_USER, 0), RV3032_E_BUSY);
    g_busy = 0;

    /* ---- configure: the RAM mirror now, the EEPROM only where it differs ---- */
    chip_reset();
    g_reg[RV3032_REG_CLKOUT2] = 0x05; /* some HFD bits, kept */
    g_ee[RV3032_REG_CLKOUT2] = 0x05;
    rv3032_config_t c = {
        .backup = RV3032_BSM_DSM,
        .trickle = RV3032_TCM_OFF,
        .trickle_r = RV3032_TCR_7K,
        .clkout_freq = RV3032_CLKOUT_1HZ,
        .clkout_at_por = false,
        .offset = -3,
        .por_int = false,
        .vlow_int = true,
        .auto_refresh = false,
    };
    CHECK_EQ(rv3032_configure(&d, &c, true), RV3032_OK);
    uint8_t pmu = (uint8_t)(RV3032_PMU_NCLKE | (RV3032_BSM_DSM << 4) | (RV3032_TCR_7K << 2));
    CHECK_EQ(g_reg[RV3032_REG_PMU], pmu);
    CHECK_EQ(g_ee[RV3032_REG_PMU], pmu);
    CHECK_EQ(g_reg[RV3032_REG_OFFSET], 0x40 | 0x3D); /* VLIE, -3 in 6 bits */
    CHECK_EQ(g_ee[RV3032_REG_OFFSET], 0x40 | 0x3D);
    CHECK_EQ(g_reg[RV3032_REG_CLKOUT2], 0x60 | 0x05); /* FD = 1 Hz, HFD kept, OS = 0 */
    CHECK_EQ(g_ee[RV3032_REG_CLKOUT2], 0x60 | 0x05);
    CHECK(g_reg[RV3032_REG_CTRL1] & RV3032_C1_EERD);
    CHECK_EQ(g_ee_writes, 3);
    CHECK(!g_bad_ee_access);
    /* again: nothing to write */
    CHECK_EQ(rv3032_configure(&d, &c, true), RV3032_OK);
    CHECK_EQ(g_ee_writes, 3);
    CHECK(!g_bad_ee_access);
    /* RAM only */
    c.offset = 5;
    CHECK_EQ(rv3032_configure(&d, &c, false), RV3032_OK);
    CHECK_EQ(g_reg[RV3032_REG_OFFSET] & 0x3F, 5);
    CHECK_EQ(g_ee[RV3032_REG_OFFSET] & 0x3F, 0x3D);
    CHECK_EQ(g_ee_writes, 3);
    /* impossible combinations */
    rv3032_config_t cb = c;
    cb.trickle = RV3032_TCM_3V0; /* charge pump needs LSM */
    CHECK_EQ(rv3032_configure(&d, &cb, false), RV3032_E_ARG);
    cb = c;
    cb.backup = RV3032_BSM_OFF;
    cb.trickle = RV3032_TCM_VDD; /* no charging without switchover */
    CHECK_EQ(rv3032_configure(&d, &cb, false), RV3032_E_ARG);
    cb = c;
    cb.offset = 32;
    CHECK_EQ(rv3032_configure(&d, &cb, false), RV3032_E_ARG);
    cb = c;
    cb.backup = RV3032_BSM_LSM;
    cb.trickle = RV3032_TCM_4V5;
    CHECK_EQ(rv3032_configure(&d, &cb, false), RV3032_OK);
    CHECK_EQ(g_reg[RV3032_REG_PMU] & 0x33, (RV3032_BSM_LSM << 4) | RV3032_TCM_4V5);
    /* the default (as delivered) */
    CHECK_EQ(rv3032_configure(&d, &rv3032_config_default, false), RV3032_OK);
    CHECK_EQ(g_reg[RV3032_REG_PMU], 0x00);
    CHECK_EQ(g_reg[RV3032_REG_CLKOUT2], 0x05);
    CHECK_EQ(g_reg[RV3032_REG_CTRL1] & RV3032_C1_EERD, 0);

    /* ---- CLKOUT on/off: NCLKE in the RAM mirror only ---- */
    CHECK_EQ(rv3032_configure(&d, &c, false), RV3032_OK);
    int w = g_ee_writes;
    CHECK_EQ(rv3032_clkout(&d, true), RV3032_OK);
    CHECK_EQ(g_reg[RV3032_REG_PMU] & RV3032_PMU_NCLKE, 0);
    CHECK_EQ(g_reg[RV3032_REG_PMU] & RV3032_PMU_BSM_MASK, RV3032_BSM_DSM << 4); /* kept */
    CHECK_EQ(rv3032_clkout(&d, false), RV3032_OK);
    CHECK(g_reg[RV3032_REG_PMU] & RV3032_PMU_NCLKE);
    CHECK_EQ(g_ee_writes, w);

    TEST_MAIN_END();
}
