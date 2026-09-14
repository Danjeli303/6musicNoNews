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

    EXPECT_EQ_INT(EVENT_NONE,
                  parse_event("NEWS_EVENT schedule_off sample=96000\n").event);

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

static void test_latest_news_event_is_authoritative(void)
{
    EXPECT_EQ_INT(1, state_for_event(EVENT_NEWS_ON));
    EXPECT_EQ_INT(1, state_for_event(EVENT_NEWS_ON));
    EXPECT_EQ_INT(0, state_for_event(EVENT_NEWS_OFF));
    EXPECT_EQ_INT(0, state_for_event(EVENT_NEWS_OFF));
    EXPECT_EQ_INT(-1, state_for_event(EVENT_NONE));
}

static void test_transition_status_is_published(void)
{
    char path[] = "/tmp/news-mixer-status.XXXXXX";
    char contents[256] = { 0 };
    int descriptor = mkstemp(path);
    FILE *file;

    EXPECT_TRUE(descriptor >= 0);
    if (descriptor < 0)
        return;
    close(descriptor);
    EXPECT_TRUE(write_status(path, 0, 123, 1));
    file = fopen(path, "r");
    EXPECT_TRUE(file != NULL);
    if (file) {
        EXPECT_TRUE(fgets(contents, sizeof(contents), file) != NULL);
        fclose(file);
    }
    EXPECT_TRUE(strstr(contents, "\"news_active\":false") != NULL);
    EXPECT_TRUE(strstr(contents, "\"transitioning\":true") != NULL);
    EXPECT_TRUE(strstr(contents, "\"sample\":123") != NULL);
    remove(path);
}

int main(void)
{
    test_parse_events();
    test_crossfade_levels();
    test_latest_news_event_is_authoritative();
    test_transition_status_is_published();
    if (tests_failed) {
        fprintf(stderr, "%d of %d mixer control tests failed\n", tests_failed, tests_run);
        return 1;
    }
    printf("All %d mixer control tests passed\n", tests_run);
    return 0;
}
