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
    CHECK_EQ(pm_watts_from_pulses(1000, 60, 60), 3600);
    CHECK_EQ(pm_watts_from_pulses(1000, 1, 3600), 1);
    CHECK_EQ(pm_watts_from_pulses(800, 100, 600), 750);
    CHECK_EQ(pm_watts_from_pulses(1000, 0, 60), 0);
    CHECK_EQ(pm_watts_from_pulses(1000, 60, 0), 0);
    CHECK_EQ(pm_watts_from_pulses(0, 60, 60), 0);
    /* A full hour at 1 pulse/s on a 1 Wh meter is 3600 W. */
    CHECK_EQ(pm_watts_from_pulses(1000, 3600, 3600), 3600);
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
    pm_ring_add(&ring, 7);
    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 0);

    pm_ring_advance(&ring, 100);
    pm_ring_add(&ring, 5);
    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 5);
    CHECK_EQ(used, 1);

    pm_ring_advance(&ring, 101);
    pm_ring_add(&ring, 3);
    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 8);
    CHECK_EQ(used, 2);

    /* Newest bucket first: offset 0 is the second we are still filling. */
    CHECK_EQ(pm_ring_sum_at(&ring, 0, 1), 3);
    CHECK_EQ(pm_ring_sum_at(&ring, 1, 1), 5);
    CHECK_EQ(pm_ring_sum_at(&ring, 2, 1), 0);

    /* A ten second gap zero-fills rather than smearing the old value. */
    pm_ring_advance(&ring, 111);
    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 8);
    CHECK_EQ(used, 12);
    CHECK_EQ(pm_ring_sum_at(&ring, 0, 1), 0);
    CHECK_EQ(pm_ring_sum_at(&ring, 10, 1), 3);
}

static void test_ring_wrap(void) {
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 0);

    for(uint32_t s = 1; s <= 5000; s++) {
        pm_ring_advance(&ring, s);
        pm_ring_add(&ring, 1);
    }

    uint32_t used = 0;
    /* History is capped at one hour even after 5000 s of input. */
    CHECK_EQ(pm_ring_sum(&ring, PM_RING_SECONDS, &used), PM_RING_SECONDS);
    CHECK_EQ(used, PM_RING_SECONDS);
    CHECK_EQ(pm_ring_sum(&ring, 60, &used), 60);
    CHECK_EQ(used, 60);
    /* Asking for more than we keep clamps to what we have. */
    CHECK_EQ(pm_ring_sum(&ring, 99999, &used), PM_RING_SECONDS);
    CHECK_EQ(used, PM_RING_SECONDS);
    /* One pulse per second on a 1 Wh meter is 3600 W. */
    CHECK_EQ(pm_watts_from_pulses(1000, pm_ring_sum(&ring, 60, NULL), 60), 3600);
}

static void test_ring_stale_jump(void) {
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 10);
    pm_ring_add(&ring, 42);

    /* Jumping past the whole window discards everything instead of aliasing. */
    pm_ring_advance(&ring, 10 + PM_RING_SECONDS + 5);
    uint32_t used = 0;
    CHECK_EQ(pm_ring_sum(&ring, PM_RING_SECONDS, &used), 0);
    CHECK_EQ(used, 1);

    pm_ring_add(&ring, 2);
    CHECK_EQ(pm_ring_sum(&ring, PM_RING_SECONDS, NULL), 2);
}

static void test_ring_saturation(void) {
    pm_ring_reset(&ring);
    pm_ring_advance(&ring, 1);
    pm_ring_add(&ring, 60000);
    pm_ring_add(&ring, 60000);
    CHECK_EQ(pm_ring_sum_at(&ring, 0, 1), 65535);
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
    test_formatting();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
