# flipperzero-powermeter

A Flipper Zero app that counts the pulse-output LED on a utility meter and turns it into
live demand in watts, rolling averages, and scrolling graphs.

Built against official firmware **1.4.3 / API 87.1**.

## Pulse sources

Three, selectable in Settings.

**GPIO** — the reliable one. You supply a photodetector on a GPIO pin. See *Hardware*.

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

Light on the phototransistor pulls PA4 low, so the defaults are **Pull = Up** and
**Active level = Low**. 3V3 is on pin 9 if you use a powered sensor instead.

GPIO logic is 3.3 V — don't feed 5 V logic into it. If you want an external pull-up rather
than the internal one, 10 kΩ from pin 9 (3V3) to the signal pin is a stiffer, less
noise-prone choice; then set **Pull = None**.

### Why some pins are marked busy

Not every header pin can take an interrupt. The STM32's EXTI controller has one callback
slot per pin *number*, shared across ports — PA3, PB3, PC3 and PH3 all contend for line 3.
The firmware claims six of those lines from boot for the buttons alone:

| EXTI line | Owner            | Blocks header pin |
| --------- | ---------------- | ----------------- |
| 3         | OK button (PH3)  | PC3, PB3          |
| 6         | Down button (PC6)| PA6               |
| 7         | Expansion module (PB7) | PA7         |
| 10–13     | Up/Left/Right/Back | —               |

Claiming an occupied line does not fail gracefully; `furi_hal_gpio_add_int_callback`
asserts and the device resets. So at startup the app reads which EXTI lines are already
unmasked and marks those pins **busy** in the settings list, refusing to arm them. A saved
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
put a 74HC14 Schmitt inverter between the sensor and the pin and flip **Active level**.

## Configuring for your meter

The one setting you must get right is **Pulses/kWh**. Look on the meter faceplate for
`imp/kWh` (or `Imp/kWh`). Older meters print `Kh`, which is watt-hours per revolution
rather than per pulse.

| imp/kWh | Wh per pulse |
| ------- | ------------ |
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

1. **Live** — instantaneous demand, plus 1 / 15 / 60 minute averages, pulse count and
   session energy. A `~` after a window label means less history has accumulated than the
   window covers, so the figure is over a shorter span.
2. **2 min** graph, 1 s per pixel
3. **30 min** graph, 15 s per pixel
4. **60 min** graph, 30 s per pixel
5. **Diag** — raw counters for bring-up: accepted pulses, rejected pulses, last interval,
   raw IR edge count, last IR mark duration, and live pin level.

Graphs autoscale to a 1/2/5 ceiling shown in the header, with a dotted half-scale line.

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

**Averages** come from a ring of 3600 one-second pulse-count buckets (7.2 KB):

```
W = 3600000 * pulses / (imp_per_kwh * seconds)
```

History is one hour; that is the hard limit on the longest graph.

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
synthetic pulses, so expect to tune Min/Max pulse width and Active level on first contact
with a sensor. Start in Demo mode to confirm the UI, then switch to GPIO.

The Diag page and the raw IR counters are bring-up scaffolding, not permanent features;
they should be trimmed once the pulse path is confirmed against a meter.
