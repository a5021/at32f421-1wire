# Hardware notes

## Clocks and dividers

- SCLK 120 MHz from 8 MHz HEXT (PLL ×15). HICK/2 (4 MHz, PLL ×30) fallback on
  crystal-less boards; both produce a 1 µs TMR1 tick.
- `clock_config()` decodes the PLL multiplier field per the vendor encoding
  (multiplier−2 up to ×15, multiplier−1 from ×16). The HICK/2 fallback
  previously silently produced ×29 (116 MHz) instead of ×30 (120 MHz).
- The clock tree is read back from CRM after the switch and published as
  `system_core_clock`; `clock_ahb_hz()` / `clock_apb1_hz()` /
  `clock_apb2_hz()` derive frequencies from the actual register values.

### Verified on AT32F421F8P7 (OpenOCD register read)

| Path   | Register | Value | Meaning |
|--------|----------|-------|---------|
| CRM_CTRL | 0x3038F83 | HEXTEN=1, HEXTSTBL=1, PLLEN=1, PLLSTBL=1 | HEXT running, PLL locked |
| CRM_CFG  | 0x035000A | PLLMULT=13 (×15), SCLKSEL=PLL | 8 MHz × 15 = 120 MHz |
| TMR1_DIV | 119 | APB2/120 = 1 MHz tick | verified |
| I2C2_CTRL2 | 120 | CLKFREQ=120 MHz | verified |
| I2C2_CLKCTRL | 600 (100 kHz) / 100 (400 kHz) | standard/fast prescaler | verified |

## Timer engine

- TMR1_CH3 (PA10, MUX 2) drives the 1-Wire line in PWM mode B.
- TMR1_CH2 compare (DMA1 ch3) feeds the next C3DT for multi-slot writes.
- TMR1_CH4 capture (DMA1 ch4) stores the counter at each rising edge.
- One-cycle mode (OCMEN): each pass stops itself at its update event
  (RPR sets how many periods away). CH3 compare is preloaded with `0` so the
  line is released HIGH when the counter halts.
- No interrupts: `_start` arms one hardware pass, `_poll` waits for the
  timer update flag.

## Strong pull-up (parasite power)

`ow_hal_strong_pullup(true)` switches PA10 to push-pull and forces the timer
output HIGH. `ow_hal_strong_pullup(false)` restores open-drain and stops the
timer (disables C3EN/TMREN).

## Verified configurations

- 9 devices on the bus (7× DS18B20, 1× DS2413, 1× DS2431).
- Both I2C speeds (100 kHz and 400 kHz) read all devices correctly.
- 8 consecutive rescans without stalling; cycle counter advances each cycle.
