#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skipper_time.h"

#define SAMPLE_RATE 1000
#define SEARCH_MINUTES (8 * 24 * 60)

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s --from-iso window iso-time utc-offset\n", program);
    fprintf(stderr, "       %s --local window local-epoch-seconds\n", program);
}

static int parse_i64(const char *text, int64_t *value_out)
{
    char *end = NULL;
    errno = 0;
    long long value = strtoll(text, &end, 10);

    if (errno || end == text || *end)
        return 0;

    *value_out = (int64_t)value;
    return 1;
}

static int active_at(const TimeRestrictionWindow *window, int64_t local_epoch_seconds)
{
    return is_time_restricted_window_active_with_config(
        1, local_epoch_seconds * 1000LL, 0, 0, SAMPLE_RATE, window);
}

static int next_duration_seconds(const TimeRestrictionWindow *window, int64_t local_epoch_seconds)
{
    int active = active_at(window, local_epoch_seconds);
    int64_t next_minute = (local_epoch_seconds / 60 + 1) * 60;

    for (int i = 0; i < SEARCH_MINUTES; ++i) {
        int64_t candidate = next_minute + i * 60LL;

        if (active_at(window, candidate) != active)
            return (int)(candidate - local_epoch_seconds);
    }

    return 3600;
}

static int load_window(const char *text, TimeRestrictionWindow *window)
{
    if (parse_time_restriction_argument(text, window))
        return 1;

    fprintf(stderr, "Error: invalid schedule/window: %s\n", text);
    return 0;
}

static int print_state(const TimeRestrictionWindow *window, int64_t local_epoch_seconds, int include_epoch)
{
    int active = active_at(window, local_epoch_seconds);
    int duration = next_duration_seconds(window, local_epoch_seconds);

    if (include_epoch)
        printf("%lld ", (long long)local_epoch_seconds);

    printf("%s %d\n", active ? "active" : "inactive", duration);
    return 0;
}

int main(int argc, char **argv)
{
    TimeRestrictionWindow window;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (!strcmp(argv[1], "--from-iso")) {
        int64_t epoch_ms;
        int parsed_offset_minutes;
        int requested_offset_minutes;

        if (argc != 5 || !load_window(argv[2], &window) ||
            !parse_iso8601_timestamp_ms(argv[3], &epoch_ms, &parsed_offset_minutes) ||
            !parse_utc_offset_minutes(argv[4], &requested_offset_minutes, NULL)) {
            usage(argv[0]);
            return 1;
        }

        (void)parsed_offset_minutes;
        return print_state(&window, epoch_ms / 1000LL + requested_offset_minutes * 60LL, 1);
    }

    if (!strcmp(argv[1], "--local")) {
        int64_t local_epoch_seconds;

        if (argc != 4 || !load_window(argv[2], &window) || !parse_i64(argv[3], &local_epoch_seconds)) {
            usage(argv[0]);
            return 1;
        }

        return print_state(&window, local_epoch_seconds, 0);
    }

    usage(argv[0]);
    return 1;
}
