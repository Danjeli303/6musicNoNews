////////////////////////////////////////////////////////////////////////////
//                            **** SKIPPER ****                           //
//                  Selective Audio Detection and Filter                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "skipper_time.h"

#define MINUTES_PER_DAY 1440
#define MAX_UTC_OFFSET_MINUTES (23 * 60 + 59)

static int parse_fixed_digits (const char **text_ptr, int digit_count, int *value_out);
static int minute_is_valid (int minute);
static int value_in_wrapping_range (int value, int start, int end, int modulus);
static int parse_nonnegative_minutes (const char *text, int *value_out);
static int parse_time_of_day_minutes (const char *text, int *minute_of_day_out);
static int parse_schedule_times (const char *text, int *times, int *time_count);
static int ascii_equals_ignore_case (const char *left, const char *right);
static char *trim_spaces (char *text);
static void strip_inline_comment (char *text);
static int is_news_schedule_near (const TimeRestrictionWindow *window, const struct tm *time_info, int margin_seconds);

void init_default_time_restriction_window (TimeRestrictionWindow *window)
{
    memset (window, 0, sizeof (*window));
    window->range_count = 2;
    window->ranges [0].start_minute = 58;
    window->ranges [0].end_minute = 5;
    window->ranges [1].start_minute = 28;
    window->ranges [1].end_minute = 35;
}

int parse_iso8601_timestamp_ms (const char *time_text, int64_t *epoch_ms_out, int *utc_offset_minutes_out)
{
    const char *cursor = time_text;
    int year, month, day, hour, minute, second;
    int millisecond = 0, utc_offset_minutes;

    if (!parse_fixed_digits (&cursor, 4, &year) || *cursor++ != '-' ||
        !parse_fixed_digits (&cursor, 2, &month) || *cursor++ != '-' ||
        !parse_fixed_digits (&cursor, 2, &day))
        return 0;

    if (*cursor != 'T' && *cursor != 't' && *cursor != ' ')
        return 0;

    cursor++;

    if (!parse_fixed_digits (&cursor, 2, &hour) || *cursor++ != ':' ||
        !parse_fixed_digits (&cursor, 2, &minute) || *cursor++ != ':' ||
        !parse_fixed_digits (&cursor, 2, &second))
        return 0;

    if (*cursor == '.') {
        int digits_seen = 0;

        cursor++;

        while (isdigit ((unsigned char)*cursor)) {
            if (digits_seen < 3)
                millisecond = millisecond * 10 + (*cursor - '0');

            digits_seen++;
            cursor++;
        }

        if (!digits_seen)
            return 0;

        while (digits_seen++ < 3)
            millisecond *= 10;
    }

    if (!parse_utc_offset_minutes (cursor, &utc_offset_minutes, &cursor))
        return 0;

    while (isspace ((unsigned char)*cursor))
        cursor++;

    if (*cursor || month < 1 || month > 12 || day < 1 || day > days_in_month (year, month) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60)
        return 0;

    int64_t epoch_seconds = days_from_civil (year, month, day) * 86400LL + hour * 3600LL + minute * 60LL + second;
    epoch_seconds -= utc_offset_minutes * 60LL;

    *epoch_ms_out = epoch_seconds * 1000LL + millisecond;
    *utc_offset_minutes_out = utc_offset_minutes;
    return 1;
}

int parse_utc_offset_minutes (const char *offset_text, int *utc_offset_minutes_out, const char **end_ptr_out)
{
    const char *cursor = offset_text;
    int sign = 1, hour, minute = 0;

    if (*cursor == 'Z' || *cursor == 'z') {
        cursor++;
        *utc_offset_minutes_out = 0;

        if (end_ptr_out) {
            *end_ptr_out = cursor;
            return 1;
        }

        while (isspace ((unsigned char)*cursor))
            cursor++;

        return *cursor == '\0';
    }

    if (*cursor == '+')
        cursor++;
    else if (*cursor == '-') {
        sign = -1;
        cursor++;
    }
    else
        return 0;

    if (!parse_fixed_digits (&cursor, 2, &hour))
        return 0;

    if (*cursor == ':') {
        cursor++;

        if (!parse_fixed_digits (&cursor, 2, &minute))
            return 0;
    }
    else if (isdigit ((unsigned char)cursor [0]) && isdigit ((unsigned char)cursor [1]))
        if (!parse_fixed_digits (&cursor, 2, &minute))
            return 0;

    if (minute > 59 || hour * 60 + minute > MAX_UTC_OFFSET_MINUTES)
        return 0;

    *utc_offset_minutes_out = sign * (hour * 60 + minute);

    if (end_ptr_out) {
        *end_ptr_out = cursor;
        return 1;
    }

    while (isspace ((unsigned char)*cursor))
        cursor++;

    return *cursor == '\0';
}

int parse_time_restriction_window (const char *window_text, TimeRestrictionWindow *window_out, const char **end_ptr_out)
{
    TimeRestrictionWindow parsed_window = { 0 };
    const char *cursor = window_text;

    while (1) {
        char *end_ptr;
        long start_minute;
        long end_minute;

        if (parsed_window.range_count == TIME_RESTRICTION_MAX_RANGES)
            return 0;

        while (isspace ((unsigned char)*cursor))
            cursor++;

        start_minute = strtol (cursor, &end_ptr, 10);

        if (end_ptr == cursor || !minute_is_valid ((int) start_minute))
            return 0;

        cursor = end_ptr;

        while (isspace ((unsigned char)*cursor))
            cursor++;

        if (*cursor != '-')
            return 0;

        cursor++;

        while (isspace ((unsigned char)*cursor))
            cursor++;

        end_minute = strtol (cursor, &end_ptr, 10);

        if (end_ptr == cursor || !minute_is_valid ((int) end_minute) || start_minute == end_minute)
            return 0;

        parsed_window.ranges [parsed_window.range_count].start_minute = (int) start_minute;
        parsed_window.ranges [parsed_window.range_count].end_minute = (int) end_minute;
        parsed_window.range_count++;
        cursor = end_ptr;

        while (isspace ((unsigned char)*cursor))
            cursor++;

        if (*cursor != ',')
            break;

        cursor++;
    }

    if (!parsed_window.range_count)
        return 0;

    *window_out = parsed_window;

    if (end_ptr_out) {
        *end_ptr_out = cursor;
        return 1;
    }

    while (isspace ((unsigned char)*cursor))
        cursor++;

    return *cursor == '\0';
}

int parse_news_schedule_file (const char *path, TimeRestrictionWindow *window_out)
{
    enum { SECTION_NONE, SECTION_WINDOW, SECTION_WEEKDAY, SECTION_WEEKEND };
    TimeRestrictionWindow parsed_window = { 0 };
    char line [2048];
    int section = SECTION_NONE, saw_before = 0, saw_after = 0;
    FILE *file = fopen (path, "r");

    if (!file)
        return 0;

    parsed_window.schedule_enabled = 1;

    while (fgets (line, sizeof (line), file)) {
        char *cursor, *equals;
        size_t length = strlen (line);

        if (length == sizeof (line) - 1 && line [length - 1] != '\n' && !feof (file)) {
            fclose (file);
            return 0;
        }

        strip_inline_comment (line);
        cursor = trim_spaces (line);

        if (!*cursor)
            continue;

        length = strlen (cursor);

        if (cursor [0] == '[') {
            if (length < 3 || cursor [length - 1] != ']') {
                fclose (file);
                return 0;
            }

            cursor [length - 1] = '\0';
            cursor = trim_spaces (cursor + 1);

            if (ascii_equals_ignore_case (cursor, "window"))
                section = SECTION_WINDOW;
            else if (ascii_equals_ignore_case (cursor, "weekday"))
                section = SECTION_WEEKDAY;
            else if (ascii_equals_ignore_case (cursor, "weekend"))
                section = SECTION_WEEKEND;
            else {
                fclose (file);
                return 0;
            }

            continue;
        }

        equals = strchr (cursor, '=');

        if (!equals) {
            fclose (file);
            return 0;
        }

        *equals = '\0';
        char *key = trim_spaces (cursor);
        char *value = trim_spaces (equals + 1);

        if (section == SECTION_WINDOW) {
            if (ascii_equals_ignore_case (key, "before_minutes") || ascii_equals_ignore_case (key, "before")) {
                if (!parse_nonnegative_minutes (value, &parsed_window.before_minutes)) {
                    fclose (file);
                    return 0;
                }
                saw_before = 1;
            }
            else if (ascii_equals_ignore_case (key, "after_minutes") || ascii_equals_ignore_case (key, "after")) {
                if (!parse_nonnegative_minutes (value, &parsed_window.after_minutes)) {
                    fclose (file);
                    return 0;
                }
                saw_after = 1;
            }
            else {
                fclose (file);
                return 0;
            }
        }
        else if (section == SECTION_WEEKDAY || section == SECTION_WEEKEND) {
            int *times = section == SECTION_WEEKDAY ? parsed_window.weekday_times : parsed_window.weekend_times;
            int *time_count = section == SECTION_WEEKDAY ? &parsed_window.weekday_time_count : &parsed_window.weekend_time_count;

            if (!ascii_equals_ignore_case (key, "times") && !ascii_equals_ignore_case (key, "time")) {
                fclose (file);
                return 0;
            }

            if (!parse_schedule_times (value, times, time_count)) {
                fclose (file);
                return 0;
            }
        }
        else {
            fclose (file);
            return 0;
        }
    }

    fclose (file);

    if (!saw_before || !saw_after || parsed_window.before_minutes + parsed_window.after_minutes <= 0 ||
        !parsed_window.weekday_time_count || !parsed_window.weekend_time_count)
        return 0;

    *window_out = parsed_window;
    return 1;
}

int parse_time_restriction_argument (const char *window_text, TimeRestrictionWindow *window_out)
{
    if (parse_time_restriction_window (window_text, window_out, NULL))
        return 1;

    return parse_news_schedule_file (window_text, window_out);
}

int format_epoch_ms_with_utc_offset (int64_t epoch_ms, int utc_offset_minutes, char *buffer, size_t buffer_size)
{
    int64_t shifted_epoch_ms = epoch_ms + utc_offset_minutes * 60LL * 1000LL;
    int64_t shifted_epoch_seconds = shifted_epoch_ms / 1000LL;
    int millisecond = (int)(shifted_epoch_ms % 1000LL);
    char offset_sign = '+';
    int absolute_offset_minutes = utc_offset_minutes;
    char offset_text [8];

    if (utc_offset_minutes < -MAX_UTC_OFFSET_MINUTES || utc_offset_minutes > MAX_UTC_OFFSET_MINUTES)
        return 0;

    if (millisecond < 0) {
        millisecond += 1000;
        shifted_epoch_seconds--;
    }

    time_t shifted_time = (time_t)shifted_epoch_seconds;
    struct tm *time_info = gmtime (&shifted_time);

    if (!time_info)
        return 0;

    if (absolute_offset_minutes < 0) {
        offset_sign = '-';
        absolute_offset_minutes = -absolute_offset_minutes;
    }

    snprintf (offset_text, sizeof (offset_text), "%c%02d:%02d", offset_sign,
              absolute_offset_minutes / 60, absolute_offset_minutes % 60);

    return snprintf (buffer, buffer_size, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s",
                     time_info->tm_year + 1900, time_info->tm_mon + 1, time_info->tm_mday,
                     time_info->tm_hour, time_info->tm_min, time_info->tm_sec,
                     millisecond, offset_text) > 0;
}

int is_time_restricted_window_active (int stream_time_enabled, int64_t stream_start_epoch_ms,
                                      int stream_time_utc_offset_minutes, int64_t sample_index,
                                      int sample_rate)
{
    TimeRestrictionWindow window;

    init_default_time_restriction_window (&window);
    return is_time_restricted_window_active_with_config (stream_time_enabled, stream_start_epoch_ms,
                                                        stream_time_utc_offset_minutes, sample_index,
                                                        sample_rate, &window);
}

int is_time_restricted_window_active_with_config (int stream_time_enabled, int64_t stream_start_epoch_ms,
                                                 int stream_time_utc_offset_minutes, int64_t sample_index,
                                                 int sample_rate, const TimeRestrictionWindow *window)
{
    return is_time_restricted_window_near_with_config (stream_time_enabled, stream_start_epoch_ms,
                                                       stream_time_utc_offset_minutes, sample_index,
                                                       sample_rate, window, 0);
}

int is_time_restricted_window_near_with_config (int stream_time_enabled, int64_t stream_start_epoch_ms,
                                               int stream_time_utc_offset_minutes, int64_t sample_index,
                                               int sample_rate, const TimeRestrictionWindow *window,
                                               int margin_seconds)
{
    time_t time_for_restriction;
    struct tm *time_info;

    if (margin_seconds < 0)
        margin_seconds = 0;

    if (stream_time_enabled) {
        int64_t stream_epoch_ms = stream_start_epoch_ms + sample_index * 1000LL / sample_rate;
        stream_epoch_ms += stream_time_utc_offset_minutes * 60LL * 1000LL;
        time_for_restriction = (time_t)(stream_epoch_ms / 1000LL);
        time_info = gmtime (&time_for_restriction);
    }
    else {
        time (&time_for_restriction);
        time_info = localtime (&time_for_restriction);
    }

    if (!time_info)
        return 1;

    int hour = time_info->tm_hour;
    int min = time_info->tm_min;

    if (window->schedule_enabled)
        return is_news_schedule_near (window, time_info, margin_seconds);

    int margin_minutes = (margin_seconds + 59) / 60;

    if (hour < 6 || hour > 21)
        return 0;

    for (int i = 0; i < window->range_count; ++i) {
        int start_minute = window->ranges [i].start_minute - margin_minutes;
        int end_minute = window->ranges [i].end_minute + margin_minutes;

        if (value_in_wrapping_range (min, start_minute, end_minute, 60))
            return 1;
    }

    return 0;
}

static int parse_fixed_digits (const char **text_ptr, int digit_count, int *value_out)
{
    const char *cursor = *text_ptr;
    int value = 0;

    for (int i = 0; i < digit_count; ++i) {
        if (!isdigit ((unsigned char)cursor [i]))
            return 0;

        value = value * 10 + (cursor [i] - '0');
    }

    *value_out = value;
    *text_ptr = cursor + digit_count;
    return 1;
}

static int minute_is_valid (int minute)
{
    return minute >= 0 && minute <= 59;
}

static int value_in_wrapping_range (int value, int start, int end, int modulus)
{
    start %= modulus;
    end %= modulus;

    if (start < 0)
        start += modulus;

    if (end < 0)
        end += modulus;

    if (start <= end)
        return value >= start && value < end;

    return value >= start || value < end;
}

static int parse_nonnegative_minutes (const char *text, int *value_out)
{
    char *end_ptr;
    long value = strtol (text, &end_ptr, 10);

    if (end_ptr == text || value < 0 || value >= MINUTES_PER_DAY)
        return 0;

    end_ptr = trim_spaces (end_ptr);

    if (*end_ptr)
        return 0;

    *value_out = (int) value;
    return 1;
}

static int parse_time_of_day_minutes (const char *text, int *minute_of_day_out)
{
    char *end_ptr;
    long hour = strtol (text, &end_ptr, 10);
    long minute;

    if (end_ptr == text || *end_ptr != ':')
        return 0;

    text = end_ptr + 1;
    minute = strtol (text, &end_ptr, 10);

    if (end_ptr == text || hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return 0;

    end_ptr = trim_spaces (end_ptr);

    if (*end_ptr)
        return 0;

    *minute_of_day_out = (int)(hour * 60 + minute);
    return 1;
}

static int parse_schedule_times (const char *text, int *times, int *time_count)
{
    char buffer [2048];
    char *cursor = buffer;

    if (strlen (text) >= sizeof (buffer))
        return 0;

    strcpy (buffer, text);

    while (1) {
        char *comma = strchr (cursor, ',');
        char *item = cursor;
        int minute_of_day;

        if (comma)
            *comma = '\0';

        item = trim_spaces (item);

        if (!*item || *time_count == TIME_RESTRICTION_MAX_SCHEDULE_TIMES ||
            !parse_time_of_day_minutes (item, &minute_of_day))
            return 0;

        times [(*time_count)++] = minute_of_day;

        if (!comma)
            return 1;

        cursor = comma + 1;
    }
}

static int ascii_equals_ignore_case (const char *left, const char *right)
{
    while (*left && *right) {
        if (tolower ((unsigned char)*left) != tolower ((unsigned char)*right))
            return 0;

        left++;
        right++;
    }

    return *left == *right;
}

static char *trim_spaces (char *text)
{
    char *end;

    while (isspace ((unsigned char)*text))
        text++;

    end = text + strlen (text);

    while (end > text && isspace ((unsigned char)end [-1]))
        *--end = '\0';

    return text;
}

static void strip_inline_comment (char *text)
{
    while (*text) {
        if (*text == '#' || *text == ';') {
            *text = '\0';
            return;
        }

        text++;
    }
}

static int is_news_schedule_near (const TimeRestrictionWindow *window, const struct tm *time_info, int margin_seconds)
{
    int minute_of_day = time_info->tm_hour * 60 + time_info->tm_min;
    int second_of_day = minute_of_day * 60 + time_info->tm_sec;
    int is_weekend = time_info->tm_wday == 0 || time_info->tm_wday == 6;
    const int *times = is_weekend ? window->weekend_times : window->weekday_times;
    int time_count = is_weekend ? window->weekend_time_count : window->weekday_time_count;

    for (int i = 0; i < time_count; ++i) {
        int start_second = (times [i] - window->before_minutes) * 60 - margin_seconds;
        int end_second = (times [i] + window->after_minutes) * 60 + margin_seconds;

        if (value_in_wrapping_range (second_of_day, start_second, end_second, MINUTES_PER_DAY * 60))
            return 1;
    }

    return 0;
}

int is_leap_year (int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month (int year, int month)
{
    static const int days_per_month [] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (month == 2 && is_leap_year (year))
        return 29;

    if (month < 1 || month > 12)
        return 0;

    return days_per_month [month - 1];
}

int64_t days_from_civil (int year, int month, int day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned month_prime = (unsigned)(month + (month > 2 ? -3 : 9));
    const unsigned day_of_year = (153 * month_prime + 2) / 5 + (unsigned)day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

    return (int64_t)era * 146097LL + (int64_t)day_of_era - 719468LL;
}
