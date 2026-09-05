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

static void pm_bucket_add(PmRing* r, uint32_t offset, uint32_t add) {
    if(add == 0 || offset >= r->filled) return;
    uint16_t* b = &r->bucket[(r->head_sec - offset) % PM_RING_SECONDS];
    uint32_t v = (uint32_t)*b + add;
    *b = (v > UINT16_MAX) ? UINT16_MAX : (uint16_t)v;
}

void pm_ring_add_interval(
    PmRing* r,
    uint32_t milli,
    uint32_t interval_ms,
    uint32_t age_ms,
    uint32_t frac_ms) {
    if(!r->primed || milli == 0) return;
    if(interval_ms == 0) {
        pm_bucket_add(r, 0, milli);
        return;
    }

    /* Everything is measured as milliseconds-ago from the present moment. The
     * interval occupies [age, age + interval]; bucket 0 covers [0, frac) and
     * every older bucket a full second before that. Each bucket takes
     * milli * (its overlap with the interval) / interval. */
    const uint32_t lo = age_ms;
    const uint32_t hi = age_ms + interval_ms;

    /* Deposit the difference between successive *cumulative* shares rather
     * than rounding each bucket independently. Rounding each one down and
     * dumping the leftover somewhere put a visible spike in one bucket per
     * pulse; carrying the cumulative total keeps every bucket within one unit
     * of its true share, and the final bucket lands on exactly `milli` so no
     * energy is invented or lost. */
    uint32_t placed = 0;

    for(uint32_t i = 0; i < r->filled; i++) {
        uint32_t b_hi = (i == 0) ? frac_ms : frac_ms + i * 1000;

        uint32_t e = b_hi < hi ? b_hi : hi;
        if(e > lo) {
            uint32_t cum = (uint32_t)(((uint64_t)milli * (e - lo)) / interval_ms);
            if(cum > placed) {
                pm_bucket_add(r, i, cum - placed);
                placed = cum;
            }
        }
        if(b_hi >= hi) break;
    }
    /* If the ring ran out before the interval did, the remainder belongs to
     * time we no longer keep. Dropping it is correct; parking it in a bucket
     * would be the spike all over again. */
}

/* Inverse of pm_watts_from_interval: what gap between pulses a given load
 * implies. Used by the demo source to emit on an exact schedule. */
uint32_t pm_interval_from_watts(uint32_t imp_per_kwh, uint32_t watts) {
    if(imp_per_kwh == 0 || watts == 0) return 0;
    uint64_t d = (uint64_t)imp_per_kwh * watts;
    uint64_t ms = (3600000000ULL + d / 2) / d;
    return ms > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)ms;
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

uint32_t pm_watts_from_milli(uint32_t imp_per_kwh, uint32_t milli, uint32_t seconds) {
    if(imp_per_kwh == 0 || seconds == 0) return 0;
    uint64_t d = (uint64_t)imp_per_kwh * seconds;
    uint64_t w = (3600ULL * milli + d / 2) / d;
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

/* Unit suffixes rather than colons: 1h02m03s cannot be misread as a day and
 * two hours the way 1:02:03 can. Hours are dropped entirely below one. */
void pm_fmt_hms(char* out, size_t len, uint32_t seconds) {
    unsigned long h = seconds / 3600;
    unsigned long m = (seconds / 60) % 60;
    unsigned long s = seconds % 60;
    if(h) {
        snprintf(out, len, "%luh%02lum%02lus", h, m, s);
    } else {
        snprintf(out, len, "%lum%02lus", m, s);
    }
}

/* log2 in 8.8 fixed point, linearly interpolating the mantissa. Peak error is
 * ~0.09 of an octave, which is under a pixel on a 41 px plot -- cheaper and
 * more predictable here than pulling in libm. */
uint32_t pm_log2_fx(uint32_t v) {
    if(v == 0) return 0;
    uint32_t b = 31u - (uint32_t)__builtin_clz(v);
    uint32_t norm = (uint32_t)(((uint64_t)v << 16) >> b); /* 1.0..2.0 in 16.16 */
    return (b << 8) | ((norm - 65536u) >> 8);
}

uint32_t pm_bar_height(uint32_t value, uint32_t scale, uint32_t height, bool log_scale) {
    if(scale == 0 || height == 0 || value == 0) return 0;
    if(value > scale) value = scale;

    if(!log_scale) {
        return (uint32_t)(((uint64_t)value * height) / scale);
    }
    if(scale <= PM_LOG_FLOOR || value <= PM_LOG_FLOOR) return 0;

    uint32_t lo = pm_log2_fx(PM_LOG_FLOOR);
    uint32_t span = pm_log2_fx(scale) - lo;
    if(span == 0) return 0;
    return (uint32_t)(((uint64_t)(pm_log2_fx(value) - lo) * height) / span);
}

/* One label, one unit, fixed field order: "277/304/517 W" beats three
 * separately-labelled values that have to be kept from colliding. */
void pm_fmt_triple(char* out, size_t len, uint32_t lo, uint32_t avg, uint32_t hi) {
    if(hi < 10000) {
        snprintf(
            out, len, "%lu/%lu/%lu W", (unsigned long)lo, (unsigned long)avg, (unsigned long)hi);
    } else {
        snprintf(
            out,
            len,
            "%lu.%lu/%lu.%lu/%lu.%lu kW",
            (unsigned long)(lo / 1000),
            (unsigned long)((lo % 1000) / 100),
            (unsigned long)(avg / 1000),
            (unsigned long)((avg % 1000) / 100),
            (unsigned long)(hi / 1000),
            (unsigned long)((hi % 1000) / 100));
    }
}

/* Meters print either imp/kWh or Wh per pulse; showing the derived figure lets
 * one setting serve a faceplate labelled either way. */
void pm_fmt_wh_per_pulse(char* out, size_t len, uint32_t imp_per_kwh) {
    if(imp_per_kwh == 0) {
        snprintf(out, len, "-");
        return;
    }
    uint32_t milli = (uint32_t)((1000000ULL + imp_per_kwh / 2) / imp_per_kwh);
    if(milli >= 10000) {
        snprintf(out, len, "%luWh", (unsigned long)((milli + 500) / 1000));
    } else if(milli >= 1000) {
        snprintf(
            out,
            len,
            "%lu.%02luWh",
            (unsigned long)(milli / 1000),
            (unsigned long)((milli % 1000) / 10));
    } else {
        snprintf(out, len, "0.%03luWh", (unsigned long)milli);
    }
}

void pm_day_reset(PmDayRing* d) {
    memset(d, 0, sizeof(*d));
}

void pm_day_advance(PmDayRing* d, uint32_t now_min) {
    if(!d->primed) {
        d->primed = true;
        d->head_min = now_min;
        d->filled = 1;
        d->bucket[now_min % PM_DAY_MINUTES] = 0;
        return;
    }
    if(now_min <= d->head_min) return;
    if(now_min - d->head_min >= PM_DAY_MINUTES) {
        memset(d->bucket, 0, sizeof(d->bucket));
        d->head_min = now_min;
        d->filled = 1;
        return;
    }
    while(d->head_min < now_min) {
        d->head_min++;
        d->bucket[d->head_min % PM_DAY_MINUTES] = 0;
        if(d->filled < PM_DAY_MINUTES) d->filled++;
    }
}

void pm_day_set_at(PmDayRing* d, uint32_t offset_min, uint32_t milli) {
    if(!d->primed || offset_min >= d->filled) return;
    d->bucket[(d->head_min - offset_min) % PM_DAY_MINUTES] = milli;
}

uint32_t pm_day_sum_at(const PmDayRing* d, uint32_t offset_min, uint32_t span_min) {
    if(!d->primed) return 0;
    uint32_t sum = 0;
    for(uint32_t i = 0; i < span_min; i++) {
        uint32_t back = offset_min + i;
        if(back >= d->filled) break;
        sum += d->bucket[(d->head_min - back) % PM_DAY_MINUTES];
    }
    return sum;
}
