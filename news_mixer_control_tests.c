#include <math.h>
#include <stdio.h>
#include <string.h>

#define main news_mixer_control_program_main
#include "news_mixer_control.c"
#undef main

static int tests_run;
static int tests_failed;

#define EXPECT_TRUE(value) do { tests_run++; if (!(value)) { \
    fprintf(stderr, "%s:%d: expected true: %s\n", __FILE__, __LINE__, #value); \
    tests_failed++; } } while (0)
#define EXPECT_EQ_INT(expected, actual) do { tests_run++; if ((expected) != (actual)) { \
    fprintf(stderr, "%s:%d: expected %d, got %d\n", __FILE__, __LINE__, \
            (expected), (actual)); tests_failed++; } } while (0)
#define EXPECT_NEAR(expected, actual) do { tests_run++; if (fabs((expected) - (actual)) > 0.000001) { \
    fprintf(stderr, "%s:%d: expected %.6f, got %.6f\n", __FILE__, __LINE__, \
            (double)(expected), (double)(actual)); tests_failed++; } } while (0)

static void test_parse_events(void)
{
    ParsedEvent parsed = parse_event("NEWS_EVENT news_on sample=48000 delay_ms=7250\n");
    EXPECT_EQ_INT(EVENT_NEWS_ON, parsed.event);
    EXPECT_TRUE(parsed.sample == 48000);
    EXPECT_EQ_INT(7250, parsed.delay_milliseconds);

    parsed = parse_event("NEWS_EVENT schedule_off sample=96000\n");
    EXPECT_EQ_INT(EVENT_SCHEDULE_OFF, parsed.event);
    EXPECT_TRUE(parsed.sample == 96000);

    EXPECT_EQ_INT(EVENT_NONE, parse_event("NEWS_EVENT news_on sample=-1\n").event);
    EXPECT_EQ_INT(EVENT_NONE, parse_event("unrelated diagnostic\n").event);
}

static void test_crossfade_levels(void)
{
    double bbc, fip;

    mix_levels(0.0, 1, 0.8, &bbc, &fip);
    EXPECT_NEAR(1.0, bbc);
    EXPECT_NEAR(0.0, fip);
    mix_levels(0.5, 1, 0.8, &bbc, &fip);
    EXPECT_NEAR(0.5, bbc);
    EXPECT_NEAR(0.4, fip);
    mix_levels(1.0, 1, 0.8, &bbc, &fip);
    EXPECT_NEAR(0.0, bbc);
    EXPECT_NEAR(0.8, fip);
    mix_levels(1.0, 0, 0.8, &bbc, &fip);
    EXPECT_NEAR(1.0, bbc);
    EXPECT_NEAR(0.0, fip);

    interpolate_levels(0.5, 0.8, 0.2, 0.0, 0.8, &bbc, &fip);
    EXPECT_NEAR(0.4, bbc);
    EXPECT_NEAR(0.5, fip);
}

static void test_news_off_controls_return_to_bbc(void)
{
    int schedule_active = 0;
    int news_active = 0;

    EXPECT_EQ_INT(-1, transition_for_event(EVENT_SCHEDULE_ON,
                                           &schedule_active, &news_active));
    EXPECT_TRUE(schedule_active);
    EXPECT_EQ_INT(1, transition_for_event(EVENT_NEWS_ON,
                                          &schedule_active, &news_active));
    EXPECT_TRUE(news_active);
    EXPECT_EQ_INT(0, transition_for_event(EVENT_NEWS_OFF,
                                          &schedule_active, &news_active));
    EXPECT_TRUE(!news_active);
    EXPECT_EQ_INT(-1, transition_for_event(EVENT_SCHEDULE_OFF,
                                           &schedule_active, &news_active));
    EXPECT_TRUE(!schedule_active);
    EXPECT_TRUE(!news_active);
}

int main(void)
{
    test_parse_events();
    test_crossfade_levels();
    test_news_off_controls_return_to_bbc();
    if (tests_failed) {
        fprintf(stderr, "%d of %d mixer control tests failed\n", tests_failed, tests_run);
        return 1;
    }
    printf("All %d mixer control tests passed\n", tests_run);
    return 0;
}
