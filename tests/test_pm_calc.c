/* Host-side tests for the pure math in pm_calc.c. No furi, no hardware. */
#include "pm_calc.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        checks++;                                                  \
        if(!(cond)) {                                              \
            failures++;                                            \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while(0)

#define CHECK_EQ(actual, expected)                                                                \
    do {                                                                                          \
        checks++;                                                                                 \
        unsigned long a_ = (unsigned long)(actual);                                               \
        unsigned long e_ = (unsigned long)(expected);                                             \
        if(a_ != e_) {                                                                            \
            failures++;                                                                           \
            printf("FAIL %s:%d  %s == %lu, expected %lu\n", __FILE__, __LINE__, #actual, a_, e_); \
        }                                                                                         \
    } while(0)

#define CHECK_STR(actual, expected)                            \
    do {                                                       \
        checks++;                                              \
        if(strcmp((actual), (expected)) != 0) {                \
            failures++;                                        \
            printf(                                            \
                "FAIL %s:%d  %s == \"%s\", expected \"%s\"\n", \
                __FILE__,                                      \
                __LINE__,                                      \
                #actual,                                       \
                (actual),                                      \
                (expected));                                   \
        }                                                      \
    } while(0)

static PmRing ring;
static PmDayRing day;

static void test_watts_from_interval(void) {
    /* 1000 imp/kWh is exactly 1 Wh per pulse; 1 Wh in 1 s is 3600 W. */
    CHECK_EQ(pm_watts_from_interval(1000, 1000), 3600);
    CHECK_EQ(pm_watts_from_interval(1000, 3600), 1000);
    CHECK_EQ(pm_watts_from_interval(1000, 36000), 100);
    /* 3200 imp/kWh -> 0.3125 Wh per pulse. */
    CHECK_EQ(pm_watts_from_interval(3200, 1125), 1000);
    CHECK_EQ(pm_watts_from_interval(800, 1000), 4500);
    /* Degenerate inputs must not divide by zero. */
    CHECK_EQ(pm_watts_from_interval(0, 1000), 0);
    CHECK_EQ(pm_watts_from_interval(1000, 0), 0);
    /* Absurdly short intervals saturate rather than wrap. */
    CHECK_EQ(pm_watts_from_interval(1000, 1), PM_WATTS_MAX);
}

static void test_watts_from_pulses(void) {
    CHECK_EQ(pm_watts_from_milli(1000, 60000, 60), 3600);
    CHECK_EQ(pm_watts_from_milli(1000, 1000, 3600), 1);
    CHECK_EQ(pm_watts_from_milli(800, 100000, 600), 750);
    CHECK_EQ(pm_watts_from_milli(1000, 0, 60), 0);
    CHECK_EQ(pm_watts_from_milli(1000, 60000, 0), 0);
    CHECK_EQ(pm_watts_from_milli(0, 60000, 60), 0);
    /* A full hour at 1 pulse/s on a 1 Wh meter is 3600 W. */
    CHECK_EQ(pm_watts_from_milli(1000, 3600000, 3600), 3600);
}

static void test_nice_ceiling(void) {
    CHECK_EQ(pm_nice_ceiling(0), 1);
    CHECK_EQ(pm_nice_ceiling(1), 1);
    CHECK_EQ(pm_nice_ceiling(3), 5);
    CHECK_EQ(pm_nice_ceiling(6), 10);
    CHECK_EQ(pm_nice_ceiling(99), 100);
    CHECK_EQ(pm_nice_ceiling(100), 100);
    CHECK_EQ(pm_nice_ceiling(101), 200);
    CHECK_EQ(pm_nice_ceiling(1500), 2000);
    CHECK_EQ(pm_nice_ceiling(2001), 5000);
    CHECK_EQ(pm_nice_ceiling(50000), 50000);
}

static void test_ring_basics(void) {
    uint32_t used = 12345;
    pm_ring_reset(&ring);

    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 0);
    CHECK_EQ(used, 0);
    /* Adding before the ring is primed must be a no-op, not a stray write. */
    pm_ring_add_interval(&ring, 7000, 0, 0, 0);
    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 0);

    pm_ring_advance(&ring, 100);
    pm_ring_add_interval(&ring, 5000, 0, 0, 0);
    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 5000);
    CHECK_EQ(used, 1);

    pm_ring_advance(&ring, 101);
    pm_ring_add_interval(&ring, 3000, 0, 0, 0);
    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 8000);
    CHECK_EQ(used, 2);

    /* Newest bucket first: offset 0 is the second we are still filling. */
    CHECK_EQ(pm_ring_sum_at(&ring, 0, 1), 3000);
    CHECK_EQ(pm_ring_sum_at(&ring, 1, 1), 5000);
    CHECK_EQ(pm_ring_sum_at(&ring, 2, 1), 0);

    /* A ten second gap zero-fills rather than smearing the old value. */
    pm_ring_advance(&ring, 111);
    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 8000);
    CHECK_EQ(used, 12);
    CHECK_EQ(pm_ring_sum_at(&ring, 0, 1), 0);
    CHECK_EQ(pm_ring_sum_at(&ring, 10, 1), 3000);
}

static void test_ring_wrap(void) {
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 0);

    for(uint32_t s = 1; s <= 5000; s++) {
        pm_ring_advance(&ring, s);
        pm_ring_add_interval(&ring, 1000, 0, 0, 0);
    }

    uint32_t used = 0;
    /* History is capped at one hour even after 5000 s of input. */
    CHECK_EQ(pm_ring_sum(&ring, PM_RING_SECONDS, &used), PM_RING_SECONDS * PM_MILLI);
    CHECK_EQ(used, PM_RING_SECONDS);
    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 60000);
    CHECK_EQ(used, 60);
    /* Asking for more than we keep clamps to what we have. */
    CHECK_EQ(pm_ring_sum(&ring, 99999, &used), PM_RING_SECONDS * PM_MILLI);
    CHECK_EQ(used, PM_RING_SECONDS);
    /* One pulse per second on a 1 Wh meter is 3600 W. */
    CHECK_EQ(pm_watts_from_milli(1000, pm_ring_sum(&ring, 60, NULL), 60), 3600);
}

static void test_ring_stale_jump(void) {
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 10);
    pm_ring_add_interval(&ring, 42000, 0, 0, 0);

    /* Jumping past the whole window discards everything instead of aliasing. */
    pm_ring_advance(&ring, 10 + PM_RING_SECONDS + 5);
    uint32_t used = 0;
    CHECK_EQ(pm_ring_sum(&ring, PM_RING_SECONDS, &used), 0);
    CHECK_EQ(used, 1);

    pm_ring_add_interval(&ring, 2000, 0, 0, 0);
    CHECK_EQ(pm_ring_sum(&ring, PM_RING_SECONDS, NULL), 2000);
}

static void test_ring_saturation(void) {
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 1);
    pm_ring_add_interval(&ring, 60000, 0, 0, 0);
    pm_ring_add_interval(&ring, 60000, 0, 0, 0);
    CHECK_EQ(pm_ring_sum_at(&ring, 0, 1), 65535);
}

static void test_ring_interval(void) {
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 0);
    for(uint32_t s = 1; s <= 10; s++) {
        pm_ring_advance(&ring, s);
    }

    /* 2 s interval landing exactly on a second boundary: one full second into
     * the previous bucket, nothing into the partial current one. */
    pm_ring_add_interval(&ring, 1000, 2000, 0, 0);
    CHECK_EQ(pm_ring_sum(&ring, 60, NULL), 1000);
    CHECK_EQ(pm_ring_sum_at(&ring, 1, 1), 500);
    CHECK_EQ(pm_ring_sum_at(&ring, 2, 1), 500);

    /* Energy is conserved whatever the alignment. */
    for(uint32_t frac = 0; frac < 1000; frac += 137) {
        pm_ring_reset(&ring);
        pm_ring_advance(&ring, 0);
        for(uint32_t s = 1; s <= 20; s++) {
            pm_ring_advance(&ring, s);
        }
        pm_ring_add_interval(&ring, 1000, 3600, 0, frac);
        CHECK_EQ(pm_ring_sum(&ring, 60, NULL), 1000);
    }

    /* An interval longer than the retained history must not corrupt the ring. */
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 0);
    pm_ring_advance(&ring, 1);
    pm_ring_add_interval(&ring, 1000, 9999999, 0, 0);
    CHECK(pm_ring_sum(&ring, PM_RING_SECONDS, NULL) <= 1000);
}

/* The regression this whole milli-pulse scheme exists for: a steady 1 kW load
 * on a 1000 imp/kWh meter pulses every 3.6 s. Bucketing each pulse into its
 * arrival second read as a 3600 W spike between zeros; spreading it across the
 * interval should read as a level ~1 kW. */
static void test_steady_load_is_not_spiky(void) {
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 0);

    uint32_t ms = 0;
    for(uint32_t pulse = 0; pulse < 200; pulse++) {
        ms += 3600;
        pm_ring_advance(&ring, ms / 1000);
        pm_ring_add_interval(&ring, PM_MILLI, 3600, 0, ms % 1000);
    }

    /* Every individual second in the settled middle should read near 1 kW,
     * rather than alternating between 3600 W and 0 W. */
    uint32_t hi = 0, lo = PM_WATTS_MAX;
    for(uint32_t i = 5; i < 60; i++) {
        uint32_t w = pm_watts_from_milli(1000, pm_ring_sum_at(&ring, i, 1), 1);
        if(w > hi) hi = w;
        if(w < lo) lo = w;
    }
    CHECK(lo > 850);
    CHECK(hi < 1150);

    /* And the one-minute average lands on the true load. */
    uint32_t avg = pm_watts_from_milli(1000, pm_ring_sum(&ring, 60, NULL), 60);
    CHECK(avg > 950 && avg < 1050);
}

/* Regression for a per-pulse spike: rounding each bucket down and parking the
 * leftover in one of them put a visible tooth in the 1 s/px plot once per
 * pulse. At ~370 W the interval is ~9.7 s, so the leftover was a tenth of a
 * bucket's true share -- small in energy, obvious on screen, and it inflated
 * max. Every settled bucket should now sit within a percent of the true load. */
static void test_no_per_pulse_spike(void) {
    const uint32_t interval = 9730; /* ~370 W at 1000 imp/kWh */
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 0);

    uint32_t ms = 0;
    for(uint32_t pulse = 0; pulse < 400; pulse++) {
        ms += interval;
        pm_ring_advance(&ring, ms / 1000);
        pm_ring_add_interval(&ring, PM_MILLI, interval, 0, ms % 1000);
    }

    uint32_t hi = 0, lo = PM_WATTS_MAX;
    for(uint32_t i = 12; i < 240; i++) {
        uint32_t w = pm_watts_from_milli(1000, pm_ring_sum_at(&ring, i, 1), 1);
        if(w > hi) hi = w;
        if(w < lo) lo = w;
    }
    CHECK(lo > 366);
    CHECK(hi < 374);
    /* Which also means max is not reading high off a rounding artefact. */
    CHECK(hi - lo < 8);
}

static void test_bar_height(void) {
    /* Linear is a plain proportion, and the top of the scale fills the plot. */
    CHECK_EQ(pm_bar_height(0, 0, 1000, 40, false), 0);
    CHECK_EQ(pm_bar_height(500, 0, 1000, 40, false), 20);
    CHECK_EQ(pm_bar_height(1000, 0, 1000, 40, false), 40);
    /* Over-scale values clamp instead of overflowing the plot. */
    CHECK_EQ(pm_bar_height(4000, 0, 1000, 40, false), 40);

    /* Log: the floor draws as nothing and the ceiling still fills the plot. */
    CHECK_EQ(pm_bar_height(PM_LOG_FLOOR, 0, 10000, 40, true), 0);
    CHECK_EQ(pm_bar_height(5, 0, 10000, 40, true), 0);
    CHECK_EQ(pm_bar_height(10000, 0, 10000, 40, true), 40);

    /* Each decade above the floor should occupy an equal third of the plot
     * across 10 W -> 10 kW, within the fixed-point approximation. */
    uint32_t d1 = pm_bar_height(100, 0, 10000, 39, true);
    uint32_t d2 = pm_bar_height(1000, 0, 10000, 39, true);
    CHECK(d1 > 9 && d1 < 17);
    CHECK(d2 > 22 && d2 < 30);
    /* Monotonic, and small values are lifted clear of the axis. */
    CHECK(pm_bar_height(50, 0, 10000, 39, true) < d1);
    CHECK(pm_bar_height(50, 0, 10000, 39, true) > 0);
    /* The whole point: 100 W is a third of the plot on log, a hundredth on
     * linear, so quiet periods stay legible next to a big peak. */
    CHECK(pm_bar_height(100, 0, 10000, 39, true) > pm_bar_height(100, 0, 10000, 39, false));
}

static void test_interval_round_trip(void) {
    /* The demo derives its schedule from this, so it has to be the exact
     * inverse of the reading the app then computes back. */
    const uint32_t loads[] = {100, 250, 1000, 3600, 12000, 48000};
    for(size_t i = 0; i < sizeof(loads) / sizeof(loads[0]); i++) {
        uint32_t ms = pm_interval_from_watts(1000, loads[i]);
        uint32_t back = pm_watts_from_interval(1000, ms);
        /* Within the rounding of a whole-millisecond interval. */
        uint32_t tol = loads[i] / 50 + 1;
        CHECK(back + tol >= loads[i] && loads[i] + tol >= back);
    }
    CHECK_EQ(pm_interval_from_watts(1000, 3600), 1000);
    CHECK_EQ(pm_interval_from_watts(1000, 12000), 300);
    CHECK_EQ(pm_interval_from_watts(0, 1000), 0);
    CHECK_EQ(pm_interval_from_watts(1000, 0), 0);
}

static void test_ring_age(void) {
    /* A pulse drained a tick late must land in the seconds it actually spanned,
     * not the ones ending at the drain. */
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 0);
    for(uint32_t sec = 1; sec <= 10; sec++) {
        pm_ring_advance(&ring, sec);
    }
    /* 1 s interval that ended 2 s ago, sampled at a second boundary. */
    pm_ring_add_interval(&ring, 1000, 1000, 2000, 0);
    CHECK_EQ(pm_ring_sum(&ring, 60, NULL), 1000);
    CHECK_EQ(pm_ring_sum_at(&ring, 0, 1), 0);
    CHECK_EQ(pm_ring_sum_at(&ring, 1, 1), 0);
    CHECK_EQ(pm_ring_sum_at(&ring, 3, 1), 1000);
}

static void test_triple_and_wh(void) {
    char buf[40];
    pm_fmt_triple(buf, sizeof(buf), 277, 304, 517);
    CHECK_STR(buf, "277/304/517 W");
    /* One unit for all three once any of them is large. */
    pm_fmt_triple(buf, sizeof(buf), 300, 1200, 12300);
    CHECK_STR(buf, "0.3/1.2/12.3 kW");

    pm_fmt_wh_per_pulse(buf, sizeof(buf), 1000);
    CHECK_STR(buf, "1.00Wh");
    /* A high-demand meter labelled 200 Wh/pulse is 5 imp/kWh. */
    pm_fmt_wh_per_pulse(buf, sizeof(buf), 5);
    CHECK_STR(buf, "200Wh");
    pm_fmt_wh_per_pulse(buf, sizeof(buf), 3200);
    CHECK_STR(buf, "0.313Wh");
    pm_fmt_wh_per_pulse(buf, sizeof(buf), 800);
    CHECK_STR(buf, "1.25Wh");
    pm_fmt_wh_per_pulse(buf, sizeof(buf), 0);
    CHECK_STR(buf, "-");
}

static void test_day_ring(void) {
    pm_day_reset(&day);
    CHECK_EQ(pm_day_sum_at(&day, 0, 60), 0);

    pm_day_advance(&day, 100);
    pm_day_set_at(&day, 0, 5000);
    CHECK_EQ(pm_day_sum_at(&day, 0, 1), 5000);

    pm_day_advance(&day, 101);
    pm_day_set_at(&day, 1, 7000);
    CHECK_EQ(pm_day_sum_at(&day, 1, 1), 7000);
    CHECK_EQ(pm_day_sum_at(&day, 0, 2), 7000);

    /* Minute buckets hold far more than a uint16 could: 12 kW is ~200 pulses
     * per minute, i.e. 200000 milli-pulses. */
    pm_day_reset(&day);
    pm_day_advance(&day, 0);
    pm_day_set_at(&day, 0, 200000);
    CHECK_EQ(pm_day_sum_at(&day, 0, 1), 200000);
    CHECK_EQ(pm_watts_from_milli(1000, pm_day_sum_at(&day, 0, 1), 60), 12000);

    /* A full day wraps and caps rather than growing without bound. */
    pm_day_reset(&day);
    pm_day_advance(&day, 0);
    for(uint32_t m = 1; m <= 2000; m++) {
        pm_day_advance(&day, m);
        pm_day_set_at(&day, 0, 1000);
    }
    CHECK_EQ(day.filled, PM_DAY_MINUTES);
    CHECK_EQ(pm_day_sum_at(&day, 0, PM_DAY_MINUTES), PM_DAY_MINUTES * 1000);

    /* Jumping past the whole window discards rather than aliasing. */
    pm_day_advance(&day, 2000 + PM_DAY_MINUTES + 3);
    CHECK_EQ(pm_day_sum_at(&day, 0, PM_DAY_MINUTES), 0);
    CHECK_EQ(day.filled, 1);
}

static void test_axis_range(void) {
    uint32_t lo, hi;

    pm_axis_range(0, 3390, true, &lo, &hi);
    CHECK_EQ(lo, 0);
    CHECK_EQ(hi, 5000);
    /* An idle meter still gets a sane axis rather than a degenerate one. */
    pm_axis_range(0, 0, true, &lo, &hi);
    CHECK_EQ(lo, 0);
    CHECK_EQ(hi, 100);

    /* The point of the feature: a steady 3370-3390 W load must fill the plot.
     * Nice-rounding the endpoints directly would give 2000..5000 and stay the
     * flat line it already was. */
    pm_axis_range(3370, 3390, false, &lo, &hi);
    CHECK(lo >= 3350 && lo <= 3370);
    CHECK(hi >= 3390 && hi <= 3410);
    CHECK(hi - lo <= 60);

    /* A wide-ranging load still gets a sensible axis. */
    pm_axis_range(100, 3000, false, &lo, &hi);
    CHECK(lo <= 100);
    CHECK(hi >= 3000);

    /* Perfectly flat data must not produce a zero-width axis. */
    pm_axis_range(1500, 1500, false, &lo, &hi);
    CHECK(hi > lo);
}

static void test_fitted_axis_resolves_detail(void) {
    /* 3370 and 3390 W should land at clearly different heights on a fitted
     * axis, and at the same height on a zero-based one -- which is the whole
     * reason the setting exists. */
    uint32_t lo, hi;
    pm_axis_range(3370, 3390, false, &lo, &hi);
    uint32_t a = pm_bar_height(3370, lo, hi, 40, false);
    uint32_t b = pm_bar_height(3390, lo, hi, 40, false);
    CHECK(b > a + 20);

    pm_axis_range(3370, 3390, true, &lo, &hi);
    uint32_t za = pm_bar_height(3370, lo, hi, 40, false);
    uint32_t zb = pm_bar_height(3390, lo, hi, 40, false);
    CHECK(zb - za < 2);
}

static void test_nice_floor(void) {
    CHECK_EQ(pm_nice_floor(0), 0);
    CHECK_EQ(pm_nice_floor(1), 1);
    CHECK_EQ(pm_nice_floor(9), 5);
    CHECK_EQ(pm_nice_floor(100), 100);
    CHECK_EQ(pm_nice_floor(3390), 2000);
    CHECK_EQ(pm_nice_floor(5000), 5000);
}

static void test_formatting(void) {
    char buf[24];

    pm_fmt_watts(buf, sizeof(buf), 0);
    CHECK_STR(buf, "0W");
    pm_fmt_watts(buf, sizeof(buf), 1234);
    CHECK_STR(buf, "1234W");
    pm_fmt_watts(buf, sizeof(buf), 9999);
    CHECK_STR(buf, "9999W");
    pm_fmt_watts(buf, sizeof(buf), 12345);
    CHECK_STR(buf, "12.3kW");

    pm_fmt_kwh(buf, sizeof(buf), 1000, 1234);
    CHECK_STR(buf, "1.234");
    pm_fmt_kwh(buf, sizeof(buf), 3200, 3200);
    CHECK_STR(buf, "1.000");
    pm_fmt_kwh(buf, sizeof(buf), 1000, 0);
    CHECK_STR(buf, "0.000");
    pm_fmt_kwh(buf, sizeof(buf), 0, 500);
    CHECK_STR(buf, "0.000");

    pm_fmt_hms(buf, sizeof(buf), 59);
    CHECK_STR(buf, "0m59s");
    pm_fmt_hms(buf, sizeof(buf), 61);
    CHECK_STR(buf, "1m01s");
    pm_fmt_hms(buf, sizeof(buf), 3661);
    CHECK_STR(buf, "1h01m01s");
    /* 1h02m03s cannot be misread as a day, which 1:02:03 can. */
    pm_fmt_hms(buf, sizeof(buf), 3723);
    CHECK_STR(buf, "1h02m03s");
}

int main(void) {
    test_watts_from_interval();
    test_watts_from_pulses();
    test_nice_ceiling();
    test_ring_basics();
    test_ring_wrap();
    test_ring_stale_jump();
    test_ring_saturation();
    test_ring_interval();
    test_steady_load_is_not_spiky();
    test_no_per_pulse_spike();
    test_bar_height();
    test_nice_floor();
    test_axis_range();
    test_fitted_axis_resolves_detail();
    test_interval_round_trip();
    test_ring_age();
    test_triple_and_wh();
    test_day_ring();
    test_formatting();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
