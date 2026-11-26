# rv3032-driver

A small, MCU-independent C driver for the **Micro Crystal RV-3032-C7**, a
temperature-compensated real-time clock (±2.5 ppm over −40…+85 °C, 160 nA, I²C at
address `0x51`). Written from the RV-3032-C7 Application Manual, Rev. 1.3 (May 2023).

- **No platform code.** You give it one I²C callback (write, then optionally read with a
  repeated start) and a millisecond delay; the same code runs on a PC (the unit test) and
  any microcontroller.
- **Follows the manual's procedures:** the time is read and written in one burst so it
  stays coherent (§4.5), with a re-read when the hundredths sit at 00/99; the alarm is
  set in the documented order (§4.10.2); EEPROM access disables the daily refresh,
  waits for `EEbusy`, turns backup switchover off during the access as §3.20.1 asks,
  and checks `EEF` afterwards (§4.6).
- **Kind to the EEPROM:** `rv3032_configure(..., true)` reads each configuration byte
  first and writes only the ones that differ.
- No allocation, C11, MIT licence.

## Status

> ⚠️ **Untested on real hardware.** The author does not have the chip yet. The driver is
> checked against a fake chip written from the manual (and the test itself was checked
> by breaking the driver on purpose in 14 ways).

**Covered:** time/date, Unix-time conversion (2000–2099), status flags, the alarm,
temperature, CLKOUT frequency and on/off, backup switchover, trickle charger, aging
offset, POR/voltage-low interrupts, single EEPROM bytes (configuration and the 32 bytes
of user EEPROM). **Not yet:** countdown timer, periodic update interrupt, time stamps,
the EVI input, the write-protection password. Pull requests welcome.

## Using it

```c
#include "rv3032.h"

static int my_xfer(void *ctx, uint8_t addr, const uint8_t *tx, size_t ntx,
                   uint8_t *rx, size_t nrx);      /* your I2C: return 0 or negative */
static void my_delay(void *ctx, unsigned ms);

static const rv3032_bus_t bus = {NULL, my_xfer, my_delay};
rv3032_t rtc = {&bus};

rv3032_time_t t;
if (rv3032_get_time(&rtc, &t) == RV3032_OK)
    printf("%04u-%02u-%02u %02u:%02u:%02u\n", t.year, t.month, t.day,
           t.hour, t.minute, t.second);
```

`examples/pico_example.c` is a complete Raspberry Pi Pico program.

### Things the chip does that may surprise you

- **CLKOUT runs at 32.768 kHz from power-on** as delivered (§4.4). If CLKOUT goes
  somewhere that must not see that, store a configuration with `clkout_at_por = false`
  once. Until then, every fresh chip starts with the clock running.
- **The configuration is in EEPROM, mirrored in RAM.** The RAM copy is what is in
  effect. With `auto_refresh` on (the default), the EEPROM copy is loaded again every
  day just before midnight, which undoes RAM-only changes such as `rv3032_clkout()`.
  Turn `auto_refresh` off if you switch CLKOUT at run time.
- **The alarm-enable bits are active low** (0 = compare this field), and the alarm has
  only minute, hour and date: no seconds, no month. An alarm more than a month away
  fires a month early, so re-arm it when it fires. `rv3032_alarm_off()` uses the invalid
  date 00, because setting all three disable bits means "every minute" (§4.10.2).
- **Writing the status register always clears THF and TLF** (§3.7).
  `rv3032_clear_status()` writes 0 only to the flags you name, so it never loses a flag
  that is set while it runs.
- **The trickle charger can reach 4.5 V** with its charge pump (LSM only). Check your
  supercapacitor's or cell's rating before turning it on.

## Building the test

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## Licence

MIT; see `LICENSE`. The application manual is Micro Crystal's and is not included; it is
available from their website and the usual distributors.
