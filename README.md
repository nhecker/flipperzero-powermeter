# flipperzero-powermeter

A Flipper Zero app that counts the pulse-output LED on a utility meter and turns it into
live demand in watts, rolling averages, and scrolling graphs.

Built against official firmware **1.4.3 / API 87.1**.

## Pulse sources

Three, selectable in Settings.

**GPIO** — the reliable one. You supply a photodetector on a GPIO pin. See *Hardware*. The
pin is chosen directly in the Source setting rather than as a separate option, so what is
being read is never ambiguous.

**IR** — the onboard receiver. Worth understanding before you trust it. The Flipper's IR
receiver is a TSOP-type demodulating module tuned to a ~38 kHz *carrier*. That figure is
not being compared against the meter's flash rate; it is the frequency the module expects
the light to be chopped at. Its AGC and bandpass filter exist to discard steady light so a
remote works in a sunlit room, and a meter's pulse LED is DC-driven, so in steady state it
is invisible to the module by design.

The interesting part is the edges. A sharp illumination step is broadband and does contain
energy at 38 kHz, and TSOPs are notorious for glitching on fast light-level changes. So the
plausible outcome is not silence but a **transient at the start of each flash** — countable
if it turns out to be consistent. Whether your meter's LED has fast enough edges is an
empirical question, which is why the source exists and why there is a diagnostics page.

In this mode the app counts one pulse per burst of receiver activity and uses **Max pulse**
as a refractory window, because TSOP output on an unmodulated edge tends to chatter rather
than produce one clean mark.

**Demo** — synthesises pulses at a configurable load. Drives the whole UI with no hardware.

## Hardware

Minimum viable sensor: one phototransistor, no other parts, using the STM32's internal
pull-up.

```
  Flipper GPIO header

  pin 4  (PA4) ──────┬───────────┐
                     │           │
                     │      ┌────┴────┐
              internal      │  photo- │   <- window facing the meter LED
              pull-up       │  trans. │
              (~40k)        └────┬────┘
                     │           │
  pin 8  (GND) ──────┴───────────┘
```

Light on the phototransistor pulls PA4 low, so the defaults are **Internal pull = On** and
**Pulse level = Low**. The pull direction is not a separate setting: it has to oppose the
pulse level or the sensor has nothing to pull against, so it is derived from Pulse level
and the only choice is whether to use it at all. 3V3 is on pin 9 if you use a powered sensor instead.

GPIO logic is 3.3 V — don't feed 5 V logic into it. If you want an external pull-up rather
than the internal one, 10 kΩ from pin 9 (3V3) to the signal pin is a stiffer, less
noise-prone choice; then set **Internal pull = Off**.

### Why some pins are marked busy

Not every header pin can take an interrupt. The STM32's EXTI controller has one callback
slot per pin *number*, shared across ports — PA3, PB3, PC3 and PH3 all contend for line 3.
The firmware claims six of those lines from boot for the buttons alone:

| EXTI line | Owner                  | Blocks header pin |
| --------- | ---------------------- | ----------------- |
| 3         | OK button (PH3)        | PC3, PB3          |
| 6         | Down button (PC6)      | PA6               |
| 7         | Expansion module (PB7) | PA7               |
| 10–13     | Up/Left/Right/Back     | —                 |

Measured on stock 1.4.3 that leaves **PA4, PB2, PC0 and PC1** usable. Lines 0, 1 and 2 are
shared with the IR receiver, CC1101 and NFC respectively, but none of those hold an EXTI
callback while idle, so the pins are free in practice.

Claiming an occupied line does not fail gracefully; `furi_hal_gpio_add_int_callback`
asserts and the device resets. So at startup the app reads which EXTI lines are already
unmasked and marks those pins **busy** in the Source list, refusing to arm them. A saved
config naming a busy pin falls forward to the first free one rather than wedging the app.

This is read from the hardware rather than hardcoded, so it stays correct if a firmware
version frees or claims a different line. Which pins are free depends on what else is
running — check the settings list on your own device.

### Choosing a phototransistor

Check what colour your meter emits before you buy. Many meters use a **visible red** LED,
and an IR phototransistor with a daylight-blocking filter (the black-epoxy kind) will not
see it at all. A clear-package phototransistor sensitive across red and near-IR covers
both cases.

Shroud the sensor — black heatshrink or a short piece of black tube — and tape it over
the meter's LED window. Ambient light rejection matters more than sensitivity.

### If the count is unstable

The internal pull-up is weak, so edges can be slow enough for the STM32's EXTI to fire
twice. The pulse-width filter (Min/Max pulse) catches most of that. If it isn't enough,
put a 74HC14 Schmitt inverter between the sensor and the pin and flip **Pulse level**.

## Configuring for your meter

The one setting you must get right is **Pulses/kWh**. Look on the meter faceplate for
`imp/kWh` (or `Imp/kWh`). Older meters print `Kh`, which is watt-hours per revolution
rather than per pulse, and high-demand meters often print a watt-hours-per-pulse figure
instead — 200 Wh/pulse, say, which is 5 imp/kWh.

Rather than force that arithmetic on you, the setting is a free numeric entry and shows
the derived watt-hours per pulse beside it (`1000  1.00Wh`). If your meter is labelled in
Wh/pulse, adjust until that second figure matches the faceplate.

| imp/kWh | Wh per pulse |
| ------- | ------------ |
| 5       | 200          |
| 800     | 1.25         |
| 1000    | 1.0          |
| 1600    | 0.625        |
| 3200    | 0.3125       |
| 10000   | 0.1          |

1000 imp/kWh is the common case and gives exactly 1 Wh per pulse. It is the default.

Sanity check the result against a known load — a kettle or a resistive space heater with
a rating on the label is ideal.

## Controls

| Key         | Action                                       |
| ----------- | -------------------------------------------- |
| ◀ ▶         | Change page                                  |
| OK          | Settings                                     |
| OK (hold)   | Reset session counters and history           |
| Back        | Exit                                         |

Pages:

1. **Live** — instantaneous demand, plus 2 / 30 / 60 minute averages, pulse count and
   session energy. Those three spans are exactly the three graph windows, so a figure
   here and the `avg` on the matching chart always describe the same period. Averages
   over a window longer than the app has been running are taken over the history that
   exists — which is why, under a steady load, all three can read the same. That is the
   correct answer, not a stuck value.
2. **2 min** graph, 1 s per pixel
3. **30 min** graph, 15 s per pixel
4. **60 min** graph, 30 s per pixel
5. **24 h** graph, 12 min per pixel
6. **Diag** — raw counters for bring-up: accepted pulses, rejected pulses, last interval,
   raw IR edge count, last IR mark duration, and live pin level.

Graphs autoscale to a 1/2/5 ceiling shown in the header, with a dotted mid-height
reference line. The bottom row gives min/avg/max **of the columns actually plotted**, so
the numbers always describe that window rather than the whole ring. It reads
`min/avg/max 277/304/517 W` — one label and one unit, dropping to the bare triple when
the labelled form will not fit.

**Chart scale** switches between linear and logarithmic. Log compresses toward a 10 W
floor, which keeps a 100 W standby load legible on the same axis as a 10 kW peak instead
of flattening it against the baseline. Values below the floor draw as nothing.

The Diag page is the one to watch when testing the IR source. `IR edges` counts every
transition the receiver reports, before any filtering. If it stays at zero while the meter
is flashing, the TSOP is not reacting at all and the theory is dead. If it climbs but
`pulses` does not track the flashes, the refractory window needs tuning.

## How the numbers are derived

Each pulse is `1000 / imp_per_kwh` watt-hours.

**Instantaneous** demand comes from the gap between the last two pulses:

```
W = 3.6e9 / (imp_per_kwh * interval_ms)
```

Once the time since the last pulse exceeds that interval, the elapsed time is substituted
instead, so a load dropping to zero decays toward zero rather than freezing at its last
value. This is the honest behaviour for a pulse meter: with no pulse, all you know is that
demand is *below* some bound.

The **first pulse after a reset is not counted**. It represents energy that accumulated
over an interval starting before the app was watching, so treating it as a measurement
would invent a reading from an unknown duration. It sets the baseline timestamp and
nothing else; the second pulse produces the first real interval.

**Averages and graphs** come from a ring of 3600 one-second buckets (7.2 KB) holding
milli-pulses:

```
W = 3600 * milli_pulses / (imp_per_kwh * seconds)
```

A pulse is not credited to the second it arrived in. It means "one quantum of energy has
flowed since the previous pulse", so it is spread back across the seconds its interval
overlaps, weighted by how much of each second it covers. Without that, a 1 kW load on a
1000 imp/kWh meter — one pulse every 3.6 s — draws as a 3600 W spike between zeros on the
1 s/px graph instead of a level 1 kW. Spreading over whole seconds instead of milliseconds
is not good enough either: it reads 900 W for a true 1 kW, because 3.6 s of energy gets
smeared over 4 buckets.

Sub-pulse resolution per bucket is what the milli-pulse unit buys. Energy is preserved:
the remainder from integer division is kept rather than dropped.

Seconds of history are capped at one hour. A day at that resolution would be 172 KB, so
the 24 h page has its own coarse ring: one bucket per minute, filled by rolling up the
second ring as each minute closes. 1440 uint32 buckets is under 6 KB, and holding
milli-pulses there matters — a 100 W load is 1.67 pulses per minute, which whole-pulse
counting would quantise into nonsense.

**Un-credited trailing time is excluded.** Because energy is credited backwards when a
pulse closes an interval, the seconds since the last pulse hold nothing yet. Counting them
would read as zero power for up to a full interval — visible as `min` flickering to zero
between pulses, and a downward bias on `avg`. Those buckets are skipped, but only up to
one expected interval: past that, the absence of pulses genuinely means the load dropped
and the zeros are the truth.

Pulses are timestamped in a GPIO interrupt on the leading edge and committed on the
trailing edge, so the width check rejects mains-frequency flicker and contact chatter
before it reaches the statistics. Timing resolution is the 1 ms kernel tick.

## Building

Requires [ufbt](https://github.com/flipperdevices/flipperzero-ufbt).

```bash
pip install --upgrade ufbt && ufbt update --channel=release
```

Build the `.fap`:

```bash
ufbt
```

Build and launch on a connected Flipper:

```bash
ufbt launch
```

Run the host-side unit tests (pure math, no hardware or SDK needed):

```bash
make -C tests check
```

Check formatting against the Flipper clang-format style:

```bash
ufbt lint
```

## Layout

| File             | Contents                                                    |
| ---------------- | ----------------------------------------------------------- |
| `powermeter.c`   | App lifecycle, view dispatcher wiring                        |
| `pm_meter.c`     | GPIO interrupt capture, demo generator, tick, derived stats  |
| `pm_view.c`      | Canvas drawing for all pages, input handling                 |
| `pm_settings.c`  | Config load/save, settings menu                              |
| `pm_calc.c`      | Ring buffer and power math — no furi deps, host-testable     |
| `tests/`         | Host unit tests for `pm_calc.c`                              |

CI builds the `.fap` on every push and attaches it to a GitHub Release when a `v*` tag is
pushed.

## Status

Runs on official firmware 1.4.3 (API 87.1). Verified on hardware: launches, pages render,
Demo mode drives the display, and all eight header pins — including the four whose EXTI
lines are contended — start without resetting the device.

**Not yet validated against a real meter.** The pulse path has only been exercised with
synthetic pulses, so expect to tune Min/Max pulse width and Pulse level on first contact
with a sensor. Start in Demo mode to confirm the UI, then switch to GPIO.

The Diag page and the raw IR counters are bring-up scaffolding, not permanent features;
they should be trimmed once the pulse path is confirmed against a meter.
