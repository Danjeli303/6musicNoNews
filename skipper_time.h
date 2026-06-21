////////////////////////////////////////////////////////////////////////////
//                            **** SKIPPER ****                           //
//                  Selective Audio Detection and Filter                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

#ifndef SKIPPER_TIME_H
#define SKIPPER_TIME_H

#include <stdint.h>
#include <stddef.h>

int parse_iso8601_timestamp_ms (const char *time_text, int64_t *epoch_ms_out, int *utc_offset_minutes_out);
int parse_utc_offset_minutes (const char *offset_text, int *utc_offset_minutes_out, const char **end_ptr_out);
int format_epoch_ms_with_utc_offset (int64_t epoch_ms, int utc_offset_minutes, char *buffer, size_t buffer_size);
int is_leap_year (int year);
int days_in_month (int year, int month);
int64_t days_from_civil (int year, int month, int day);
int is_time_restricted_window_active (int stream_time_enabled, int64_t stream_start_epoch_ms,
                                      int stream_time_utc_offset_minutes, int64_t sample_index,
                                      int sample_rate);

#endif
