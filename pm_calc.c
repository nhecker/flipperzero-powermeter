#include "pm_calc.h"

#include <stdio.h>
#include <string.h>

void pm_ring_reset(PmRing* r) {
    memset(r, 0, sizeof(*r));
}

void pm_ring_advance(PmRing* r, uint32_t now_sec) {
    if(!r->primed) {
        r->primed = true;
        r->head_sec = now_sec;
        r->filled = 1;
        r->bucket[now_sec % PM_RING_SECONDS] = 0;
        return;
    }
    if(now_sec <= r->head_sec) return;
    if(now_sec - r->head_sec >= PM_RING_SECONDS) {
        memset(r->bucket, 0, sizeof(r->bucket));
        r->head_sec = now_sec;
        r->filled = 1;
        return;
    }
    while(r->head_sec < now_sec) {
        r->head_sec++;
        r->bucket[r->head_sec % PM_RING_SECONDS] = 0;
        if(r->filled < PM_RING_SECONDS) r->filled++;
    }
}

void pm_ring_add(PmRing* r, uint32_t pulses) {
    if(!r->primed || pulses == 0) return;
    uint16_t* b = &r->bucket[r->head_sec % PM_RING_SECONDS];
    uint32_t v = (uint32_t)*b + pulses;
    *b = (v > UINT16_MAX) ? UINT16_MAX : (uint16_t)v;
}

uint32_t pm_ring_sum_at(const PmRing* r, uint32_t offset_sec, uint32_t span_sec) {
    if(!r->primed) return 0;
    uint32_t sum = 0;
    for(uint32_t i = 0; i < span_sec; i++) {
        uint32_t back = offset_sec + i;
        if(back >= r->filled) break;
        sum += r->bucket[(r->head_sec - back) % PM_RING_SECONDS];
    }
    return sum;
}

uint32_t pm_ring_sum(const PmRing* r, uint32_t span_sec, uint32_t* used_sec) {
    uint32_t n = r->primed ? (span_sec < r->filled ? span_sec : r->filled) : 0;
    if(used_sec) *used_sec = n;
    return pm_ring_sum_at(r, 0, n);
}

/* A meter emitting one pulse per (1000/imp) Wh, seen `interval_ms` apart, is
 * drawing 3.6e9 / (imp * interval_ms) watts. */
uint32_t pm_watts_from_interval(uint32_t imp_per_kwh, uint32_t interval_ms) {
    if(imp_per_kwh == 0 || interval_ms == 0) return 0;
    uint64_t d = (uint64_t)imp_per_kwh * interval_ms;
    uint64_t w = (3600000000ULL + d / 2) / d;
    return w > PM_WATTS_MAX ? PM_WATTS_MAX : (uint32_t)w;
}

uint32_t pm_watts_from_pulses(uint32_t imp_per_kwh, uint32_t pulses, uint32_t seconds) {
    if(imp_per_kwh == 0 || seconds == 0) return 0;
    uint64_t d = (uint64_t)imp_per_kwh * seconds;
    uint64_t w = (3600000ULL * pulses + d / 2) / d;
    return w > PM_WATTS_MAX ? PM_WATTS_MAX : (uint32_t)w;
}

/* Smallest 1/2/5 x 10^n at or above `value`, for graph autoscaling. */
uint32_t pm_nice_ceiling(uint32_t value) {
    if(value == 0) return 1;
    uint32_t mag = 1;
    while(mag <= value / 10 && mag <= 100000000u)
        mag *= 10;
    if(value <= mag) return mag;
    if(value <= 2 * mag) return 2 * mag;
    if(value <= 5 * mag) return 5 * mag;
    return 10 * mag;
}

void pm_fmt_watts(char* out, size_t len, uint32_t watts) {
    if(watts < 10000) {
        snprintf(out, len, "%luW", (unsigned long)watts);
    } else {
        snprintf(
            out,
            len,
            "%lu.%lukW",
            (unsigned long)(watts / 1000),
            (unsigned long)((watts % 1000) / 100));
    }
}

void pm_fmt_kwh(char* out, size_t len, uint32_t imp_per_kwh, uint32_t pulses) {
    if(imp_per_kwh == 0) {
        snprintf(out, len, "0.000");
        return;
    }
    uint64_t milli = ((uint64_t)pulses * 1000ULL + imp_per_kwh / 2) / imp_per_kwh;
    snprintf(out, len, "%lu.%03lu", (unsigned long)(milli / 1000), (unsigned long)(milli % 1000));
}

void pm_fmt_hms(char* out, size_t len, uint32_t seconds) {
    unsigned long h = seconds / 3600;
    unsigned long m = (seconds / 60) % 60;
    unsigned long s = seconds % 60;
    if(h) {
        snprintf(out, len, "%lu:%02lu:%02lu", h, m, s);
    } else {
        snprintf(out, len, "%lu:%02lu", m, s);
    }
}
