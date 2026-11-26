/*
 * rv3032.h — Micro Crystal RV-3032-C7 real-time clock, MCU-independent.
 * https://github.com/BatteryJuice4/rv3032-driver — MIT licence.
 *
 * Written from the RV-3032-C7 Application Manual, Rev. 1.3 (May 2023); section numbers
 * below refer to it. The chip is a temperature-compensated RTC (±2.5 ppm, 160 nA) on I2C
 * at address 0x51 (§6.6).
 *
 * Covered: time and date (one burst, §4.5), the alarm (§4.10), status flags (§3.7),
 * temperature (§3.8), the clock output (§4.4), backup switchover and trickle charger
 * (§4.2, §4.3), aging offset (§3.20.2) and EEPROM access (§4.6). Not yet: the countdown
 * timer, the periodic update interrupt, time stamps, the EVI input, the password.
 *
 * The chip's configuration (C0h-CAh) lives in EEPROM and is mirrored in RAM: the RAM copy
 * is what is in effect, the EEPROM copy is loaded at power-on and, unless turned off
 * (`auto_refresh = false`), again every day just before midnight (§4.6.1, §4.6.2).
 *
 * As delivered, the chip outputs 32.768 kHz on CLKOUT from power-on (§4.4). If CLKOUT
 * feeds something that must not see that (a memory LCD's EXTCOMIN, an unpowered part),
 * store a configuration with `clkout_at_por = false` once: rv3032_configure(..., true).
 *
 * You provide one I2C callback (and a millisecond delay). No allocation, no platform code.
 *
 * UNTESTED ON HARDWARE: verified against the manual with a host unit test only.
 */
#ifndef RV3032_H
#define RV3032_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RV3032_I2C_ADDR 0x51 /* 7-bit (§6.6) */

/* ---- Registers (§3.2) ------------------------------------------------------------------ */
#define RV3032_REG_100TH    0x00
#define RV3032_REG_SECONDS  0x01
#define RV3032_REG_MINUTES  0x02
#define RV3032_REG_HOURS    0x03
#define RV3032_REG_WEEKDAY  0x04
#define RV3032_REG_DATE     0x05
#define RV3032_REG_MONTH    0x06
#define RV3032_REG_YEAR     0x07
#define RV3032_REG_ALM_MIN  0x08
#define RV3032_REG_ALM_HOUR 0x09
#define RV3032_REG_ALM_DATE 0x0A
#define RV3032_REG_TIMER0   0x0B
#define RV3032_REG_TIMER1   0x0C
#define RV3032_REG_STATUS   0x0D
#define RV3032_REG_TEMP_LSB 0x0E
#define RV3032_REG_TEMP_MSB 0x0F
#define RV3032_REG_CTRL1    0x10
#define RV3032_REG_CTRL2    0x11
#define RV3032_REG_CTRL3    0x12
#define RV3032_REG_TS_CTRL  0x13
#define RV3032_REG_CLK_MASK 0x14
#define RV3032_REG_EVI_CTRL 0x15
#define RV3032_REG_EEADDR   0x3D
#define RV3032_REG_EEDATA   0x3E
#define RV3032_REG_EECMD    0x3F
#define RV3032_REG_USER_RAM 0x40 /* 16 bytes, 40h-4Fh */
#define RV3032_REG_PMU      0xC0 /* configuration (EEPROM + RAM mirror) */
#define RV3032_REG_OFFSET   0xC1
#define RV3032_REG_CLKOUT1  0xC2
#define RV3032_REG_CLKOUT2  0xC3
#define RV3032_REG_TREF0    0xC4
#define RV3032_REG_TREF1    0xC5
#define RV3032_EE_USER      0xCB /* 32 bytes of user EEPROM, CBh-EAh */
#define RV3032_EE_USER_END  0xEA

/* Alarm registers: bit 7 is the enable bit, and 0 means ENABLED (§3.5). */
#define RV3032_AE_OFF 0x80u

/* Status 0Dh (§3.7). Flags are cleared by writing 0; writing 1 leaves them. */
#define RV3032_ST_THF  0x80u
#define RV3032_ST_TLF  0x40u
#define RV3032_ST_UF   0x20u
#define RV3032_ST_TF   0x10u
#define RV3032_ST_AF   0x08u /* alarm */
#define RV3032_ST_EVF  0x04u
#define RV3032_ST_PORF 0x02u /* power-on reset: time and registers not valid */
#define RV3032_ST_VLF  0x01u /* voltage dropped below VLOW: data may not be valid */

/* Temperature LSBs 0Eh (§3.8): low nibble holds flags. */
#define RV3032_TL_EEF    0x08u /* EEPROM write failed */
#define RV3032_TL_EEBUSY 0x04u /* EEPROM busy (read only) */
#define RV3032_TL_CLKF   0x02u
#define RV3032_TL_BSF    0x01u /* switched to backup power */

/* Control 1 10h, Control 2 11h (§3.9). */
#define RV3032_C1_EERD 0x04u /* 1 = daily EEPROM refresh off */
#define RV3032_C2_AIE  0x08u /* alarm drives INT low */
#define RV3032_C2_STOP 0x01u

/* EEPROM PMU C0h (§3.20.1). */
#define RV3032_PMU_NCLKE     0x40u /* 1 = CLKOUT off (low) */
#define RV3032_PMU_BSM_SHIFT 4
#define RV3032_PMU_BSM_MASK  0x30u
#define RV3032_PMU_TCR_SHIFT 2
#define RV3032_PMU_TCR_MASK  0x0Cu
#define RV3032_PMU_TCM_MASK  0x03u

/* EEPROM Clkout 2 C3h (§3.20.3). */
#define RV3032_CO2_OS       0x80u /* 1 = HF mode */
#define RV3032_CO2_FD_SHIFT 5
#define RV3032_CO2_FD_MASK  0x60u

/* EE Command 3Fh (§3.18). */
#define RV3032_EECMD_UPDATE  0x11
#define RV3032_EECMD_REFRESH 0x12
#define RV3032_EECMD_WRITE   0x21
#define RV3032_EECMD_READ    0x22

/* ---- Bus ------------------------------------------------------------------------------- */
typedef struct {
    void *ctx;
    /* One I2C transaction to 7-bit `addr`: write tx[0..ntx), then, if nrx > 0, a repeated
     * start and read rx[0..nrx). Return 0 on success, negative on a NACK or timeout. The
     * whole transaction must finish within 950 ms (§4.5; the chip resets its bus then). */
    int (*xfer)(void *ctx, uint8_t addr, const uint8_t *tx, size_t ntx, uint8_t *rx, size_t nrx);
    /* Sleep at least `ms` milliseconds (used while the EEPROM is busy). */
    void (*delay_ms)(void *ctx, unsigned ms);
} rv3032_bus_t;

typedef struct {
    const rv3032_bus_t *bus;
} rv3032_t;

enum {
    RV3032_OK = 0,
    RV3032_E_BUS = -1,     /* the bus callback failed: no chip, or wiring */
    RV3032_E_ARG = -2,     /* a value out of range */
    RV3032_E_BUSY = -3,    /* the EEPROM stayed busy */
    RV3032_E_EEPROM = -4,  /* EEPROM write failed (EEF: supply too low) */
    RV3032_E_INVALID = -5, /* the chip's registers hold no valid time */
};

/* ---- Registers ------------------------------------------------------------------------- */
int rv3032_read(rv3032_t *d, uint8_t reg, uint8_t *buf, size_t n);
int rv3032_write(rv3032_t *d, uint8_t reg, const uint8_t *buf, size_t n);
/* Read-modify-write: reg = (reg & ~mask) | (value & mask). */
int rv3032_update(rv3032_t *d, uint8_t reg, uint8_t mask, uint8_t value);

/* Is the chip there? (Reads the clock registers and checks they are valid BCD.) */
int rv3032_probe(rv3032_t *d);

/* ---- Time ------------------------------------------------------------------------------ */
typedef struct {
    uint16_t year;      /* 2000-2099 */
    uint8_t month;      /* 1-12 */
    uint8_t day;        /* 1-31 */
    uint8_t hour;       /* 0-23 */
    uint8_t minute;     /* 0-59 */
    uint8_t second;     /* 0-59 */
    uint8_t weekday;    /* 0-6; the chip only counts it. rv3032_from_unix: 0 = Sunday */
    uint8_t hundredths; /* 0-99, read only (writing the time clears it) */
} rv3032_time_t;

/* Read the time in one burst (registers stay coherent during it, §4.5). Re-reads when the
 * hundredths are at 00 or 99, as the manual advises. */
int rv3032_get_time(rv3032_t *d, rv3032_time_t *t);

/* Set the time in one burst. This also restarts the second (hundredths to 00, §4.5.1):
 * call it at the start of the second you are writing. */
int rv3032_set_time(rv3032_t *d, const rv3032_time_t *t);

/* Seconds since 1970-01-01 00:00:00 <-> calendar time (proleptic Gregorian, no time
 * zones). rv3032_from_unix fails outside 2000-2099, the chip's range. */
int64_t rv3032_to_unix(const rv3032_time_t *t);
int rv3032_from_unix(int64_t secs, rv3032_time_t *t);

/* Status flags (RV3032_ST_*). */
int rv3032_get_status(rv3032_t *d, uint8_t *status);
/* Clear the given RV3032_ST_* flags, leaving the others. Note (§3.7): any write to the
 * status register also clears THF and TLF. */
int rv3032_clear_status(rv3032_t *d, uint8_t flags);

/* PORF or VLF set: the time cannot be trusted until it is set again. */
int rv3032_time_lost(rv3032_t *d, bool *lost);

/* ---- Alarm (§4.10) --------------------------------------------------------------------- */
/* Fire when minute, hour and date all match (once a month at most): the alarm has no
 * seconds or month, so an alarm more than a month away fires a month early. `int_pin`
 * also drives INT low until the alarm flag is cleared. Clears AF first. */
int rv3032_set_alarm(rv3032_t *d, uint8_t day, uint8_t hour, uint8_t minute, bool int_pin);
/* No more alarms (and AF cleared). */
int rv3032_alarm_off(rv3032_t *d);
/* Has the alarm fired since it was set? Clears AF (and releases INT) if so. */
int rv3032_alarm_fired(rv3032_t *d, bool *fired);

/* ---- Temperature (§3.8) ---------------------------------------------------------------- */
/* Last measurement, in 1/16 °C (two's complement, -40 to +105 °C usable). */
int rv3032_temperature(rv3032_t *d, int16_t *sixteenths);

/* ---- Clock output (§4.4) --------------------------------------------------------------- */
typedef enum {
    RV3032_CLKOUT_32768HZ = 0, /* the delivered default; not temperature compensated */
    RV3032_CLKOUT_1024HZ = 1,
    RV3032_CLKOUT_64HZ = 2,
    RV3032_CLKOUT_1HZ = 3,
} rv3032_clkout_t;

/* Start or stop CLKOUT now (the RAM mirror's NCLKE bit). The frequency is the one set by
 * rv3032_configure. With the daily refresh on, the stored setting comes back at midnight;
 * configure with auto_refresh = false if the output must stay as set here. */
int rv3032_clkout(rv3032_t *d, bool on);

/* ---- Configuration (EEPROM with RAM mirror) ------------------------------------------- */
typedef enum {
    RV3032_BSM_OFF = 0, /* no backup supply (VBACKUP to VSS through 10 kΩ) */
    RV3032_BSM_DSM = 1, /* switch when VDD < VBACKUP */
    RV3032_BSM_LSM = 2, /* switch when VDD < 2.0 V and VBACKUP > 2.0 V */
} rv3032_bsm_t;

typedef enum {
    RV3032_TCM_OFF = 0,
    RV3032_TCM_VDD = 1,  /* charge from VDD through a Schottky diode (DSM) */
    RV3032_TCM_1V75 = 1, /* LSM: 1.75 V regulated (same code as VDD; depends on BSM) */
    RV3032_TCM_3V0 = 2,  /* LSM only: charge pump 3.0 V */
    RV3032_TCM_4V5 = 3,  /* LSM only: charge pump 4.5 V */
} rv3032_tcm_t;

typedef enum {
    RV3032_TCR_600 = 0, /* series resistance 0.6 kΩ */
    RV3032_TCR_2K = 1,
    RV3032_TCR_7K = 2,
    RV3032_TCR_12K = 3,
} rv3032_tcr_t;

typedef struct {
    uint8_t backup;        /* rv3032_bsm_t */
    uint8_t trickle;       /* rv3032_tcm_t: check your backup cell's voltage rating! */
    uint8_t trickle_r;     /* rv3032_tcr_t */
    uint8_t clkout_freq;   /* rv3032_clkout_t (XTAL mode) */
    bool clkout_at_por;    /* CLKOUT running from power-on (the delivered default) */
    int8_t offset;         /* aging correction, -32..31 steps of 0.2384 ppm (§5.3) */
    bool por_int;          /* PORIE: INT low after a power-on reset */
    bool vlow_int;         /* VLIE: INT low after a voltage drop */
    bool auto_refresh;     /* daily EEPROM -> RAM refresh (the delivered default) */
} rv3032_config_t;

/* The chip as delivered: no switchover, no trickle, 32.768 kHz on CLKOUT. */
extern const rv3032_config_t rv3032_config_default;

/*
 * Put `c` in effect (the RAM mirrors of C0h, C1h, C3h, and EERD). With `store`, also
 * write it to the EEPROM, so it holds from the next power-on: only bytes that differ are
 * written (EEPROM wear; each takes ~5 ms), following §4.6 (auto refresh off, wait for
 * EEbusy, backup switchover off during the write, EEF checked). The supply must stay
 * above 1.6 V until it returns. Fails with RV3032_E_ARG for an impossible combination
 * (charge-pump voltages without LSM, a trickle charger without switchover).
 */
int rv3032_configure(rv3032_t *d, const rv3032_config_t *c, bool store);

/* Single EEPROM bytes (configuration C0h-CAh, user CBh-EAh), with the §4.6 procedure.
 * These change the EEPROM only, not the RAM mirror. */
int rv3032_eeprom_read(rv3032_t *d, uint8_t addr, uint8_t *value);
int rv3032_eeprom_write(rv3032_t *d, uint8_t addr, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* RV3032_H */
