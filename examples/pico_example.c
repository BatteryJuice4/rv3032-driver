/*
 * pico_example.c — the RV-3032-C7 on a Raspberry Pi Pico (RP2040 / RP2350) with the Pico
 * SDK's I2C. Link pico_stdlib and hardware_i2c. MIT licence.
 *
 * Wiring: SDA GP4, SCL GP5 (with pull-ups), VDD 3.3 V. CLKOUT and INT are optional.
 */
#include <stdio.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"
#include "rv3032.h"

static int pico_xfer(void *ctx, uint8_t addr, const uint8_t *tx, size_t ntx, uint8_t *rx,
                     size_t nrx)
{
    i2c_inst_t *i2c = ctx;
    /* nostop = true when a read follows: a repeated start, not a stop. */
    if (i2c_write_timeout_us(i2c, addr, tx, ntx, nrx > 0, 10000) != (int)ntx) {
        return -1;
    }
    if (nrx && i2c_read_timeout_us(i2c, addr, rx, nrx, false, 10000) != (int)nrx) {
        return -1;
    }
    return 0;
}

static void pico_delay(void *ctx, unsigned ms)
{
    (void)ctx;
    sleep_ms(ms);
}

int main(void)
{
    stdio_init_all();
    i2c_init(i2c0, 400000); /* the chip supports 400 kHz (§7.6) */
    gpio_set_function(4, GPIO_FUNC_I2C);
    gpio_set_function(5, GPIO_FUNC_I2C);

    static const rv3032_bus_t bus = {i2c0, pico_xfer, pico_delay};
    rv3032_t rtc = {&bus};

    if (rv3032_probe(&rtc) != RV3032_OK) {
        printf("no RV-3032 found\n");
        return 1;
    }

    /* Once per board: 1 Hz on CLKOUT but off at power-on, backup switchover when VDD
     * drops below VBACKUP, no trickle charging. Only changed EEPROM bytes are written. */
    rv3032_config_t cfg = rv3032_config_default;
    cfg.backup = RV3032_BSM_DSM;
    cfg.clkout_freq = RV3032_CLKOUT_1HZ;
    cfg.clkout_at_por = false;
    rv3032_configure(&rtc, &cfg, true);

    bool lost;
    rv3032_time_lost(&rtc, &lost);
    if (lost) {
        rv3032_time_t t;
        rv3032_from_unix(1795564800, &t); /* 2026-11-25 00:00:00 UTC */
        rv3032_set_time(&rtc, &t);
        rv3032_clear_status(&rtc, RV3032_ST_PORF | RV3032_ST_VLF);
    }

    rv3032_set_alarm(&rtc, 25, 7, 30, true); /* 07:30 on the 25th: INT goes low */

    for (;;) {
        rv3032_time_t t;
        int16_t temp;
        if (rv3032_get_time(&rtc, &t) == RV3032_OK && rv3032_temperature(&rtc, &temp) == 0) {
            printf("%04u-%02u-%02u %02u:%02u:%02u  %d.%02d C\n", t.year, t.month, t.day,
                   t.hour, t.minute, t.second, temp / 16, (temp % 16 < 0 ? -1 : 1) * (temp % 16) * 100 / 16);
        }
        sleep_ms(1000);
    }
}
