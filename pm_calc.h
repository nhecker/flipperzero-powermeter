#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PM_RING_SECONDS 3600u
#define PM_WATTS_MAX    999999u
#define PM_MILLI        1000u
#define PM_LOG_FLOOR    10u

/* One-second buckets covering the last hour, holding milli-pulses rather than
 * whole pulses.
 *
 * A pulse means "one quantum of energy has flowed since the previous pulse",
 * not "energy arrived at this instant". Crediting it to the arrival second
 * alone makes a steady load look like isolated spikes whenever the pulse
 * interval is longer than a bucket. Spreading it back across the interval it
 * covers is both smoother and physically honest, and it needs sub-pulse
 * resolution per bucket -- hence milli-pulses.
 *
 * Freestanding of furi so the host test suite can link it directly. */
typedef struct {
    uint16_t bucket[PM_RING_SECONDS];
    uint32_t head_sec;
    uint32_t filled;
    bool primed;
} PmRing;

void pm_ring_reset(PmRing* r);
void pm_ring_advance(PmRing* r, uint32_t now_sec);

/** Credit `milli` milli-pulses back across the interval they represent.
 *
 *  `interval_ms` is the gap since the previous pulse, `age_ms` how long ago the
 *  pulse itself landed, and `frac_ms` how far the present moment sits into the
 *  newest bucket. Each bucket receives the share of the interval that actually
 *  overlaps it, so the rate comes out right even when the interval does not
 *  divide into whole seconds. `interval_ms` of 0 lands everything in the
 *  current bucket. */
void pm_ring_add_interval(
    PmRing* r,
    uint32_t milli,
    uint32_t interval_ms,
    uint32_t age_ms,
    uint32_t frac_ms);

uint32_t pm_ring_sum(const PmRing* r, uint32_t span_sec, uint32_t* used_sec);

/* A day of history at one-second resolution would be 172 KB, so the long view
 * gets its own coarse ring: one bucket per minute, filled by rolling up the
 * second ring as each minute closes. 1440 x uint32 is under 6 KB, and holding
 * milli-pulses keeps a 100 W load (1.67 pulses/min) from quantising to nothing
 * the way whole pulses would. */
#define PM_DAY_MINUTES 1440u

typedef struct {
    uint32_t bucket[PM_DAY_MINUTES];
    uint32_t head_min;
    uint32_t filled;
    bool primed;
} PmDayRing;

void pm_day_reset(PmDayRing* d);
void pm_day_advance(PmDayRing* d, uint32_t now_min);
void pm_day_set_at(PmDayRing* d, uint32_t offset_min, uint32_t milli);
uint32_t pm_day_sum_at(const PmDayRing* d, uint32_t offset_min, uint32_t span_min);
uint32_t pm_ring_sum_at(const PmRing* r, uint32_t offset_sec, uint32_t span_sec);

uint32_t pm_watts_from_interval(uint32_t imp_per_kwh, uint32_t interval_ms);
uint32_t pm_watts_from_milli(uint32_t imp_per_kwh, uint32_t milli, uint32_t seconds);
uint32_t pm_interval_from_watts(uint32_t imp_per_kwh, uint32_t watts);
uint32_t pm_nice_ceiling(uint32_t value);

uint32_t pm_nice_floor(uint32_t value);

/** Choose the y range to plot.
 *
 *  Zero-based gives [0, nice ceiling]. Fitted picks a tick step from the span
 *  of the data and snaps outward to it -- nice-rounding the min and max
 *  directly is far too coarse to zoom with, since 3370..3390 W would round out
 *  to 2000..5000 and stay a flat line. */
void pm_axis_range(
    uint32_t data_lo,
    uint32_t data_hi,
    bool zero_based,
    uint32_t* out_lo,
    uint32_t* out_hi);

/** Height in pixels of `value` on an axis spanning [lo, hi]. Log mode uses the
 *  greater of `lo` and PM_LOG_FLOOR as its floor; anything at or below the
 *  floor draws at zero height. */
uint32_t pm_log2_fx(uint32_t v);
uint32_t pm_bar_height(uint32_t value, uint32_t lo, uint32_t hi, uint32_t height, bool log_scale);

void pm_fmt_range(char* out, size_t len, uint32_t lo, uint32_t hi);

void pm_fmt_watts(char* out, size_t len, uint32_t watts);
void pm_fmt_kwh(char* out, size_t len, uint32_t imp_per_kwh, uint32_t pulses);
void pm_fmt_hms(char* out, size_t len, uint32_t seconds);
void pm_fmt_triple(char* out, size_t len, uint32_t lo, uint32_t avg, uint32_t hi);
void pm_fmt_wh_per_pulse(char* out, size_t len, uint32_t imp_per_kwh);
