////////////////////////////////////////////////////////////////////////////
//                            **** SKIPPER ****                           //
//                  Selective Audio Detection and Filter                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

#include <ctype.h>
#include <stdio.h>
#include <time.h>

#include "skipper_time.h"

#define MAX_UTC_OFFSET_MINUTES (23 * 60 + 59)

static int parse_fixed_digits (const char **text_ptr, int digit_count, int *value_out);

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
    time_t time_for_restriction;
    struct tm *time_info;

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

    return hour >= 6 && hour <= 21 && ((min >= 58 || min < 5) || (min >= 28 && min < 35));
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
