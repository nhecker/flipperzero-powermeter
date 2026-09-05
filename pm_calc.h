#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PM_RING_SECONDS 3600u
#define PM_WATTS_MAX    999999u

/* One-second pulse-count buckets covering the last hour. Freestanding of furi so
 * the host test suite can link it directly. */
typedef struct {
    uint16_t bucket[PM_RING_SECONDS];
    uint32_t head_sec;
    uint32_t filled;
    bool primed;
} PmRing;

void pm_ring_reset(PmRing* r);
void pm_ring_advance(PmRing* r, uint32_t now_sec);
void pm_ring_add(PmRing* r, uint32_t pulses);
uint32_t pm_ring_sum(const PmRing* r, uint32_t span_sec, uint32_t* used_sec);
uint32_t pm_ring_sum_at(const PmRing* r, uint32_t offset_sec, uint32_t span_sec);

uint32_t pm_watts_from_interval(uint32_t imp_per_kwh, uint32_t interval_ms);
uint32_t pm_watts_from_pulses(uint32_t imp_per_kwh, uint32_t pulses, uint32_t seconds);
uint32_t pm_nice_ceiling(uint32_t value);

void pm_fmt_watts(char* out, size_t len, uint32_t watts);
void pm_fmt_kwh(char* out, size_t len, uint32_t imp_per_kwh, uint32_t pulses);
void pm_fmt_hms(char* out, size_t len, uint32_t seconds);
