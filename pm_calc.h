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
 *  `interval_ms` is the gap since the previous pulse and `frac_ms` how far the
 *  present moment sits into the current second. Each bucket receives the share
 *  of the interval that actually overlaps it, so the rate comes out right even
 *  when the interval does not divide into whole seconds. `interval_ms` of 0
 *  lands everything in the current bucket. */
void pm_ring_add_interval(PmRing* r, uint32_t milli, uint32_t interval_ms, uint32_t frac_ms);

uint32_t pm_ring_sum(const PmRing* r, uint32_t span_sec, uint32_t* used_sec);
uint32_t pm_ring_sum_at(const PmRing* r, uint32_t offset_sec, uint32_t span_sec);

uint32_t pm_watts_from_interval(uint32_t imp_per_kwh, uint32_t interval_ms);
uint32_t pm_watts_from_milli(uint32_t imp_per_kwh, uint32_t milli, uint32_t seconds);
uint32_t pm_nice_ceiling(uint32_t value);

/** Bar height in pixels. Log mode compresses toward PM_LOG_FLOOR watts, below
 *  which a value draws as nothing. */
uint32_t pm_log2_fx(uint32_t v);
uint32_t pm_bar_height(uint32_t value, uint32_t scale, uint32_t height, bool log_scale);

void pm_fmt_watts(char* out, size_t len, uint32_t watts);
void pm_fmt_kwh(char* out, size_t len, uint32_t imp_per_kwh, uint32_t pulses);
void pm_fmt_hms(char* out, size_t len, uint32_t seconds);
