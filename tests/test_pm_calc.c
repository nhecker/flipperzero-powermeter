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
    pm_ring_add_interval(&ring, 7000, 0, 0);
    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 0);

    pm_ring_advance(&ring, 100);
    pm_ring_add_interval(&ring, 5000, 0, 0);
    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 5000);
    CHECK_EQ(used, 1);

    pm_ring_advance(&ring, 101);
    pm_ring_add_interval(&ring, 3000, 0, 0);
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
        pm_ring_add_interval(&ring, 1000, 0, 0);
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
    pm_ring_add_interval(&ring, 42000, 0, 0);

    /* Jumping past the whole window discards everything instead of aliasing. */
    pm_ring_advance(&ring, 10 + PM_RING_SECONDS + 5);
    uint32_t used = 0;
    CHECK_EQ(pm_ring_sum(&ring, PM_RING_SECONDS, &used), 0);
    CHECK_EQ(used, 1);

    pm_ring_add_interval(&ring, 2000, 0, 0);
    CHECK_EQ(pm_ring_sum(&ring, PM_RING_SECONDS, NULL), 2000);
}

static void test_ring_saturation(void) {
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 1);
    pm_ring_add_interval(&ring, 60000, 0, 0);
    pm_ring_add_interval(&ring, 60000, 0, 0);
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
    pm_ring_add_interval(&ring, 1000, 2000, 0);
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
        pm_ring_add_interval(&ring, 1000, 3600, frac);
        CHECK_EQ(pm_ring_sum(&ring, 60, NULL), 1000);
    }

    /* An interval longer than the retained history must not corrupt the ring. */
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 0);
    pm_ring_advance(&ring, 1);
    pm_ring_add_interval(&ring, 1000, 9999999, 0);
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
        pm_ring_add_interval(&ring, PM_MILLI, 3600, ms % 1000);
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
    CHECK_STR(buf, "0:59");
    pm_fmt_hms(buf, sizeof(buf), 61);
    CHECK_STR(buf, "1:01");
    pm_fmt_hms(buf, sizeof(buf), 3661);
    CHECK_STR(buf, "1:01:01");
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
    test_formatting();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
