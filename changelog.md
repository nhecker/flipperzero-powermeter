## v1.0

First release.

- Counts the pulse-output LED on a utility meter and shows instantaneous demand
  in watts, rolling 2 / 30 / 60 minute averages, session energy and runtime.
- Graph pages over 2 min, 30 min, 60 min and 24 h, with selectable linear or
  logarithmic scale and per-window min/avg/max.
- Pulse source is any usable GPIO pin, the onboard IR receiver, or a built-in
  demo generator that needs no hardware.
- Free numeric entry for pulses/kWh, showing the derived watt-hours per pulse
  so a meter labelled either way can be matched without arithmetic.
- Configurable pulse level, internal pull, and minimum/maximum pulse width to
  reject mains flicker and contact chatter.
- Diagnostics page with raw pulse, discard, interval and IR counters.

Notes:

- Pins whose interrupt line the firmware already owns are omitted from the
  source list rather than offered and refused; on stock firmware that leaves
  PA4, PB2, PC0 and PC1.
- The IR source is experimental. The onboard receiver is a 38 kHz demodulator,
  so an unmodulated meter LED is invisible to it in steady state by design.
  Whether its illumination edges disturb the receiver enough to count is an
  open question the diagnostics page exists to answer.
