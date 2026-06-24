#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lzwlib.h"

#define main skipper_program_main
#include "skipper.c"
#undef main

static int tests_run;
static int tests_failed;

#define EXPECT_TRUE(expr) do { \
    tests_run++; \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: expected true: %s\n", __FILE__, __LINE__, #expr); \
        tests_failed++; \
    } \
} while (0)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_EQ_INT(expected, actual) do { \
    int expected_value = (expected); \
    int actual_value = (actual); \
    tests_run++; \
    if (expected_value != actual_value) { \
        fprintf(stderr, "%s:%d: expected %d, got %d\n", __FILE__, __LINE__, expected_value, actual_value); \
        tests_failed++; \
    } \
} while (0)

#define EXPECT_EQ_I64(expected, actual) do { \
    int64_t expected_value = (expected); \
    int64_t actual_value = (actual); \
    tests_run++; \
    if (expected_value != actual_value) { \
        fprintf(stderr, "%s:%d: expected %lld, got %lld\n", __FILE__, __LINE__, \
                (long long)expected_value, (long long)actual_value); \
        tests_failed++; \
    } \
} while (0)

#define EXPECT_EQ_SIZE(expected, actual) do { \
    size_t expected_value = (expected); \
    size_t actual_value = (actual); \
    tests_run++; \
    if (expected_value != actual_value) { \
        fprintf(stderr, "%s:%d: expected %zu, got %zu\n", __FILE__, __LINE__, expected_value, actual_value); \
        tests_failed++; \
    } \
} while (0)

#define EXPECT_MEMEQ(expected, actual, size) do { \
    const void *expected_value = (expected); \
    const void *actual_value = (actual); \
    size_t compare_size = (size); \
    tests_run++; \
    if (memcmp(expected_value, actual_value, compare_size) != 0) { \
        fprintf(stderr, "%s:%d: buffers differ over %zu bytes\n", __FILE__, __LINE__, compare_size); \
        tests_failed++; \
    } \
} while (0)

static int64_t parse_epoch_ms_or_fail(const char *time_text, int *offset_minutes)
{
    int64_t epoch_ms = 0;

    EXPECT_TRUE(parse_iso8601_timestamp_ms(time_text, &epoch_ms, offset_minutes));
    return epoch_ms;
}

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t index;
} TestByteReader;

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} TestByteWriter;

typedef struct {
    int status;
    unsigned char *output;
    size_t output_size;
} ProgramRun;

static int test_read_byte(void *context)
{
    TestByteReader *reader = (TestByteReader *)context;

    if (reader->index == reader->size)
        return EOF;

    return reader->data[reader->index++];
}

static void test_write_byte(int value, void *context)
{
    TestByteWriter *writer = (TestByteWriter *)context;

    if (writer->size == writer->capacity) {
        size_t new_capacity = writer->capacity ? writer->capacity * 2 : 4096;
        unsigned char *new_data = realloc(writer->data, new_capacity);

        if (!new_data) {
            fprintf(stderr, "out of memory while building test tensor\n");
            abort();
        }

        writer->data = new_data;
        writer->capacity = new_capacity;
    }

    writer->data[writer->size++] = (unsigned char)value;
}

static uint32_t checksum_tensor_bytes(const tensor_array source)
{
    const unsigned char *bytes = (const unsigned char *)source;
    uint32_t checksum = 0;

    for (size_t i = 0; i < sizeof(tensor_array); ++i)
        checksum += bytes[i];

    return checksum;
}

static void fill_test_tensor(tensor_array target)
{
    unsigned char *bytes = (unsigned char *)target;

    for (size_t i = 0; i < sizeof(tensor_array); ++i)
        bytes[i] = (unsigned char)((i * 37u + i / 97u) & 0xffu);
}

static unsigned char *make_compressed_tensor_blob(tensor_array source, size_t *blob_size_out)
{
    TestByteReader reader = { (const unsigned char *)source, sizeof(tensor_array), 0 };
    TestByteWriter writer = { 0 };
    struct tensor_header header;
    unsigned char *blob;

    EXPECT_EQ_INT(0, lzw_compress(test_write_byte, &writer, test_read_byte, &reader, 12));

    header.version = TENSOR_VERSION;
    header.checksum = checksum_tensor_bytes(source);
    header.dimensions[0] = ARRAY_BINS_1;
    header.dimensions[1] = ARRAY_BINS_2;
    header.dimensions[2] = ARRAY_BINS_3;
    header.dimensions[3] = ARRAY_BINS_4;

    *blob_size_out = sizeof(header) + writer.size;
    blob = malloc(*blob_size_out);
    EXPECT_TRUE(blob != NULL);
    if (!blob) {
        free(writer.data);
        return NULL;
    }

    memcpy(blob, &header, sizeof(header));
    memcpy(blob + sizeof(header), writer.data, writer.size);
    free(writer.data);
    return blob;
}

static ProgramRun run_skipper_with_input(int argc, char **argv, const int16_t *input, size_t frames, int channels)
{
    ProgramRun run = { 0, NULL, 0 };
    FILE *input_file = tmpfile();
    FILE *output_file = tmpfile();
    FILE *stderr_file = tmpfile();
    int saved_stdin;
    int saved_stdout;
    int saved_stderr;
    long output_size;

    EXPECT_TRUE(input_file != NULL);
    EXPECT_TRUE(output_file != NULL);
    EXPECT_TRUE(stderr_file != NULL);
    if (!input_file || !output_file || !stderr_file)
        goto done;

    EXPECT_EQ_SIZE(frames, fwrite(input, sizeof(int16_t) * channels, frames, input_file));
    rewind(input_file);

    fflush(stdin);
    fflush(stdout);
    fflush(stderr);

    saved_stdin = dup(fileno(stdin));
    saved_stdout = dup(fileno(stdout));
    saved_stderr = dup(fileno(stderr));
    EXPECT_TRUE(saved_stdin >= 0 && saved_stdout >= 0 && saved_stderr >= 0);
    if (saved_stdin < 0 || saved_stdout < 0 || saved_stderr < 0)
        goto done;

    EXPECT_TRUE(dup2(fileno(input_file), fileno(stdin)) >= 0);
    EXPECT_TRUE(dup2(fileno(output_file), fileno(stdout)) >= 0);
    EXPECT_TRUE(dup2(fileno(stderr_file), fileno(stderr)) >= 0);
    clearerr(stdin);
    clearerr(stdout);
    clearerr(stderr);

    run.status = skipper_program_main(argc, argv);

    fflush(stdout);
    fflush(stderr);
    output_size = ftell(output_file);
    EXPECT_TRUE(output_size >= 0);
    if (output_size > 0) {
        run.output_size = (size_t)output_size;
        run.output = malloc(run.output_size);
        EXPECT_TRUE(run.output != NULL);
        if (run.output) {
            rewind(output_file);
            EXPECT_EQ_SIZE(run.output_size, fread(run.output, 1, run.output_size, output_file));
        }
    }

    EXPECT_TRUE(dup2(saved_stdin, fileno(stdin)) >= 0);
    EXPECT_TRUE(dup2(saved_stdout, fileno(stdout)) >= 0);
    EXPECT_TRUE(dup2(saved_stderr, fileno(stderr)) >= 0);
    clearerr(stdin);
    clearerr(stdout);
    clearerr(stderr);
    close(saved_stdin);
    close(saved_stdout);
    close(saved_stderr);

done:
    if (input_file) fclose(input_file);
    if (output_file) fclose(output_file);
    if (stderr_file) fclose(stderr_file);
    return run;
}

static void reset_analysis_histograms(void)
{
    memset(peak_to_trough_histogram, 0, sizeof(peak_to_trough_histogram));
    memset(cycles_histogram, 0, sizeof(cycles_histogram));
    memset(low_third_histogram, 0, sizeof(low_third_histogram));
    memset(mid_third_histogram, 0, sizeof(mid_third_histogram));
    memset(high_third_histogram, 0, sizeof(high_third_histogram));
    memset(attack_ratio_histogram, 0, sizeof(attack_ratio_histogram));
    memset(peak_jitter_histogram, 0, sizeof(peak_jitter_histogram));
}

static int local_tensor_file_suppressed(tensor_array target, unsigned char *blob, int blob_size)
{
    int result;
    FILE *stderr_file = tmpfile();
    int saved_stderr;

    EXPECT_TRUE(stderr_file != NULL);
    if (!stderr_file)
        return local_tensor_file(target, blob, blob_size);

    fflush(stderr);
    saved_stderr = dup(fileno(stderr));
    EXPECT_TRUE(saved_stderr >= 0);
    if (saved_stderr < 0) {
        fclose(stderr_file);
        return local_tensor_file(target, blob, blob_size);
    }

    EXPECT_TRUE(dup2(fileno(stderr_file), fileno(stderr)) >= 0);
    result = local_tensor_file(target, blob, blob_size);
    fflush(stderr);
    EXPECT_TRUE(dup2(saved_stderr, fileno(stderr)) >= 0);
    close(saved_stderr);
    fclose(stderr_file);
    return result;
}

static void test_calendar_helpers(void)
{
    EXPECT_TRUE(is_leap_year(2000));
    EXPECT_TRUE(is_leap_year(2024));
    EXPECT_FALSE(is_leap_year(1900));
    EXPECT_FALSE(is_leap_year(2023));

    EXPECT_EQ_INT(29, days_in_month(2024, 2));
    EXPECT_EQ_INT(28, days_in_month(2023, 2));
    EXPECT_EQ_INT(0, days_in_month(2023, 0));
    EXPECT_EQ_INT(0, days_in_month(2023, 13));

    EXPECT_EQ_I64(0, days_from_civil(1970, 1, 1));
    EXPECT_EQ_I64(1, days_from_civil(1970, 1, 2));
    EXPECT_EQ_I64(-1, days_from_civil(1969, 12, 31));
}

static void test_parse_utc_offset_minutes(void)
{
    int offset = 0;
    const char *end = NULL;

    EXPECT_TRUE(parse_utc_offset_minutes("Z", &offset, NULL));
    EXPECT_EQ_INT(0, offset);

    EXPECT_TRUE(parse_utc_offset_minutes("+01:30", &offset, NULL));
    EXPECT_EQ_INT(90, offset);

    EXPECT_TRUE(parse_utc_offset_minutes("+01", &offset, NULL));
    EXPECT_EQ_INT(60, offset);

    EXPECT_TRUE(parse_utc_offset_minutes("+0130", &offset, NULL));
    EXPECT_EQ_INT(90, offset);

    EXPECT_TRUE(parse_utc_offset_minutes("-0230", &offset, &end));
    EXPECT_EQ_INT(-150, offset);
    EXPECT_EQ_INT('\0', *end);

    EXPECT_TRUE(parse_utc_offset_minutes("+05:45tail", &offset, &end));
    EXPECT_EQ_INT(345, offset);
    EXPECT_EQ_INT('t', *end);

    EXPECT_FALSE(parse_utc_offset_minutes("+24:00", &offset, NULL));
    EXPECT_FALSE(parse_utc_offset_minutes("+01:60", &offset, NULL));
    EXPECT_FALSE(parse_utc_offset_minutes("01:00", &offset, NULL));
    EXPECT_FALSE(parse_utc_offset_minutes("+05:45tail", &offset, NULL));
}

static void test_parse_time_restriction_window(void)
{
    TimeRestrictionWindow window;
    const char *end = NULL;

    EXPECT_TRUE(parse_time_restriction_window("58-10,28-40", &window, NULL));
    EXPECT_EQ_INT(2, window.range_count);
    EXPECT_EQ_INT(58, window.ranges[0].start_minute);
    EXPECT_EQ_INT(10, window.ranges[0].end_minute);
    EXPECT_EQ_INT(28, window.ranges[1].start_minute);
    EXPECT_EQ_INT(40, window.ranges[1].end_minute);

    EXPECT_TRUE(parse_time_restriction_window("0-5,20-30,30-40tail", &window, &end));
    EXPECT_EQ_INT(3, window.range_count);
    EXPECT_EQ_INT(0, window.ranges[0].start_minute);
    EXPECT_EQ_INT(5, window.ranges[0].end_minute);
    EXPECT_EQ_INT('t', *end);

    EXPECT_FALSE(parse_time_restriction_window("0-5,20-30,30-40tail", &window, NULL));
    EXPECT_FALSE(parse_time_restriction_window("", &window, NULL));
    EXPECT_FALSE(parse_time_restriction_window("0", &window, NULL));
    EXPECT_FALSE(parse_time_restriction_window("0-60", &window, NULL));
    EXPECT_FALSE(parse_time_restriction_window("-1-5", &window, NULL));
    EXPECT_FALSE(parse_time_restriction_window("5-5", &window, NULL));
    EXPECT_FALSE(parse_time_restriction_window("0-5,", &window, NULL));
}

static void test_parse_iso8601_timestamp_ms(void)
{
    int offset = 123;
    int64_t epoch_ms = 0;

    epoch_ms = parse_epoch_ms_or_fail("1970-01-01T00:00:00Z", &offset);
    EXPECT_EQ_I64(0, epoch_ms);
    EXPECT_EQ_INT(0, offset);

    epoch_ms = parse_epoch_ms_or_fail("1970-01-01T01:30:00+01:30", &offset);
    EXPECT_EQ_I64(0, epoch_ms);
    EXPECT_EQ_INT(90, offset);

    epoch_ms = parse_epoch_ms_or_fail("1970-01-01 00:00:00.5Z", &offset);
    EXPECT_EQ_I64(500, epoch_ms);

    epoch_ms = parse_epoch_ms_or_fail("1970-01-01t00:00:00.050Z", &offset);
    EXPECT_EQ_I64(50, epoch_ms);

    epoch_ms = parse_epoch_ms_or_fail("1970-01-01T00:00:00.1239Z", &offset);
    EXPECT_EQ_I64(123, epoch_ms);

    epoch_ms = parse_epoch_ms_or_fail("2024-02-29T12:34:56+0130", &offset);
    EXPECT_EQ_INT(90, offset);
    EXPECT_TRUE(epoch_ms > 0);

    EXPECT_FALSE(parse_iso8601_timestamp_ms("2024-02-30T00:00:00Z", &epoch_ms, &offset));
    EXPECT_FALSE(parse_iso8601_timestamp_ms("2023-02-29T00:00:00Z", &epoch_ms, &offset));
    EXPECT_FALSE(parse_iso8601_timestamp_ms("2024-01-01T00:00Z", &epoch_ms, &offset));
    EXPECT_FALSE(parse_iso8601_timestamp_ms("2024-01-01T00:00:00", &epoch_ms, &offset));
    EXPECT_FALSE(parse_iso8601_timestamp_ms("2024-01-01T00:00:00.Z", &epoch_ms, &offset));
    EXPECT_FALSE(parse_iso8601_timestamp_ms("2024-13-01T00:00:00Z", &epoch_ms, &offset));
    EXPECT_FALSE(parse_iso8601_timestamp_ms("2024-01-01T24:00:00Z", &epoch_ms, &offset));
}

static void test_time_restricted_skip_window(void)
{
    int offset = 0;
    int sample_rate = 1000;
    TimeRestrictionWindow default_window;
    TimeRestrictionWindow custom_window;
    TimeRestrictionWindow extended_window;
    int64_t active_start = parse_epoch_ms_or_fail("2026-06-20T06:00:00Z", &offset);
    int64_t pre_news_start = parse_epoch_ms_or_fail("2026-06-20T05:58:00Z", &offset);
    int64_t half_hour_start = parse_epoch_ms_or_fail("2026-06-20T06:28:00Z", &offset);
    int64_t inactive_start = parse_epoch_ms_or_fail("2026-06-20T09:10:00Z", &offset);
    int64_t last_window_start = parse_epoch_ms_or_fail("2026-06-20T21:58:00Z", &offset);
    int64_t late_start = parse_epoch_ms_or_fail("2026-06-20T22:00:00Z", &offset);

    init_default_time_restriction_window(&default_window);
    EXPECT_TRUE(parse_time_restriction_window("0-5,20-30,30-40", &custom_window, NULL));
    EXPECT_TRUE(parse_time_restriction_window("58-10,28-40", &extended_window, NULL));

    EXPECT_FALSE(is_time_restricted_skip_active(1, pre_news_start, 0, 0, sample_rate, &default_window));
    EXPECT_TRUE(is_time_restricted_skip_active(1, active_start, 0, 0, sample_rate, &default_window));
    EXPECT_TRUE(is_time_restricted_skip_active(1, active_start, 0, 4 * 60 * sample_rate, sample_rate, &default_window));
    EXPECT_FALSE(is_time_restricted_skip_active(1, active_start, 0, 5 * 60 * sample_rate, sample_rate, &default_window));
    EXPECT_TRUE(is_time_restricted_skip_active(1, half_hour_start, 0, 0, sample_rate, &default_window));
    EXPECT_TRUE(is_time_restricted_skip_active(1, half_hour_start, 0, 6 * 60 * sample_rate, sample_rate, &default_window));
    EXPECT_FALSE(is_time_restricted_skip_active(1, half_hour_start, 0, 7 * 60 * sample_rate, sample_rate, &default_window));
    EXPECT_TRUE(is_time_restricted_skip_active(1, last_window_start, 0, 0, sample_rate, &default_window));
    EXPECT_FALSE(is_time_restricted_skip_active(1, inactive_start, 60, 0, sample_rate, &default_window));
    EXPECT_FALSE(is_time_restricted_skip_active(1, late_start, 0, 0, sample_rate, &default_window));

    EXPECT_TRUE(is_time_restricted_skip_active(1, active_start, 0, 4 * 60 * sample_rate, sample_rate, &custom_window));
    EXPECT_FALSE(is_time_restricted_skip_active(1, active_start, 0, 5 * 60 * sample_rate, sample_rate, &custom_window));
    EXPECT_TRUE(is_time_restricted_skip_active(1, active_start, 0, 20 * 60 * sample_rate, sample_rate, &custom_window));
    EXPECT_TRUE(is_time_restricted_skip_active(1, active_start, 0, 39 * 60 * sample_rate, sample_rate, &custom_window));
    EXPECT_FALSE(is_time_restricted_skip_active(1, active_start, 0, 40 * 60 * sample_rate, sample_rate, &custom_window));

    EXPECT_TRUE(is_time_restricted_skip_active(1, active_start, 0, 9 * 60 * sample_rate, sample_rate, &extended_window));
    EXPECT_FALSE(is_time_restricted_skip_active(1, active_start, 0, 10 * 60 * sample_rate, sample_rate, &extended_window));
    EXPECT_TRUE(is_time_restricted_skip_active(1, half_hour_start, 0, 11 * 60 * sample_rate, sample_rate, &extended_window));
    EXPECT_FALSE(is_time_restricted_skip_active(1, half_hour_start, 0, 12 * 60 * sample_rate, sample_rate, &extended_window));
}

static void test_should_skip_mode_at_time(void)
{
    int offset = 0;
    int sample_rate = 1000;
    TimeRestrictionWindow default_window;
    int64_t active_start = parse_epoch_ms_or_fail("2026-06-20T05:58:00Z", &offset);
    int64_t inactive_start = parse_epoch_ms_or_fail("2026-06-20T09:10:00Z", &offset);

    init_default_time_restriction_window(&default_window);

    EXPECT_TRUE(should_skip_mode_at_time(SKIP_EVERYTHING, MODE_MUSIC, 0, 1, active_start, 60, 0, sample_rate, &default_window));
    EXPECT_TRUE(should_skip_mode_at_time(SKIP_MUSIC, MODE_MUSIC, 0, 1, active_start, 60, 0, sample_rate, &default_window));
    EXPECT_FALSE(should_skip_mode_at_time(SKIP_MUSIC, MODE_TALK, 0, 1, active_start, 60, 0, sample_rate, &default_window));
    EXPECT_TRUE(should_skip_mode_at_time(SKIP_TALK, MODE_TALK, 1, 1, active_start, 60, 0, sample_rate, &default_window));
    EXPECT_FALSE(should_skip_mode_at_time(SKIP_TALK, MODE_TALK, 1, 1, inactive_start, 60, 0, sample_rate, &default_window));
    EXPECT_TRUE(should_skip_mode_at_time(SKIP_TALK, MODE_TALK, 0, 1, inactive_start, 60, 0, sample_rate, &default_window));
    EXPECT_FALSE(should_skip_mode_at_time(SKIP_TALK, MODE_MUSIC, 1, 1, active_start, 60, 0, sample_rate, &default_window));
    EXPECT_FALSE(should_skip_mode_at_time(SKIP_TALK, MODE_NOTHING, 1, 1, active_start, 60, 0, sample_rate, &default_window));
    EXPECT_FALSE(should_skip_mode_at_time(SKIP_NOTHING, MODE_TALK, 0, 1, active_start, 60, 0, sample_rate, &default_window));
}

static void test_fades(void)
{
    int16_t fade_out_samples[] = { 1000, 1000, 1000, 1000 };
    int16_t fade_in_samples[] = { 1000, 1000, 1000, 1000 };
    int16_t interleaved_samples[] = { 1000, 99, 1000, 99, 1000, 99, 1000, 99 };

    fade_out(fade_out_samples, 4, 1);
    EXPECT_EQ_INT(750, fade_out_samples[0]);
    EXPECT_EQ_INT(500, fade_out_samples[1]);
    EXPECT_EQ_INT(250, fade_out_samples[2]);
    EXPECT_EQ_INT(0, fade_out_samples[3]);

    fade_in(fade_in_samples, 4, 1);
    EXPECT_EQ_INT(250, fade_in_samples[0]);
    EXPECT_EQ_INT(500, fade_in_samples[1]);
    EXPECT_EQ_INT(750, fade_in_samples[2]);
    EXPECT_EQ_INT(1000, fade_in_samples[3]);

    fade_out(interleaved_samples, 4, 2);
    EXPECT_EQ_INT(750, interleaved_samples[0]);
    EXPECT_EQ_INT(99, interleaved_samples[1]);
    EXPECT_EQ_INT(500, interleaved_samples[2]);
    EXPECT_EQ_INT(99, interleaved_samples[3]);
    EXPECT_EQ_INT(250, interleaved_samples[4]);
    EXPECT_EQ_INT(99, interleaved_samples[5]);
    EXPECT_EQ_INT(0, interleaved_samples[6]);
    EXPECT_EQ_INT(99, interleaved_samples[7]);
}

static void test_analyze_window_constant_and_cyclic_levels(void)
{
    float constant_levels[8];
    float cyclic_levels[16];
    struct analysis_result result;
    int tensor_result;

    reset_analysis_histograms();
    memset(tensor, 42, sizeof(tensor));
    for (size_t i = 0; i < sizeof(constant_levels) / sizeof(constant_levels[0]); ++i)
        constant_levels[i] = 100.0f;

    analysis_output_file = tmpfile();
    EXPECT_TRUE(analysis_output_file != NULL);
    tensor_result = analyze_window(constant_levels, 8, 8, 1000);
    EXPECT_EQ_INT(42, tensor_result);
    fflush(analysis_output_file);
    rewind(analysis_output_file);
    EXPECT_EQ_SIZE(1, fread(&result, sizeof(result), 1, analysis_output_file));
    fclose(analysis_output_file);
    analysis_output_file = NULL;

    EXPECT_EQ_INT(0, result.range_dB);
    EXPECT_EQ_INT(0, result.cycles);
    EXPECT_EQ_INT(244, result.low_third);
    EXPECT_EQ_INT(0, result.mid_third);
    EXPECT_EQ_INT(0, result.high_third);
    EXPECT_EQ_INT(128, result.attack_ratio);
    EXPECT_EQ_INT(255, result.peak_jitter);
    EXPECT_EQ_INT(1, peak_to_trough_histogram[0]);
    EXPECT_EQ_INT(1, cycles_histogram[0]);

    reset_analysis_histograms();
    memset(tensor, -7, sizeof(tensor));
    for (size_t i = 0; i < sizeof(cyclic_levels) / sizeof(cyclic_levels[0]); ++i)
        cyclic_levels[i] = (i & 1) ? 400.0f : 100.0f;

    analysis_output_file = tmpfile();
    EXPECT_TRUE(analysis_output_file != NULL);
    tensor_result = analyze_window(cyclic_levels, 16, 16, 1000);
    EXPECT_EQ_INT(-7, tensor_result);
    fflush(analysis_output_file);
    rewind(analysis_output_file);
    EXPECT_EQ_SIZE(1, fread(&result, sizeof(result), 1, analysis_output_file));
    fclose(analysis_output_file);
    analysis_output_file = NULL;

    EXPECT_TRUE(result.cycles >= 6);
    EXPECT_TRUE(attack_ratio_histogram[result.attack_ratio] > 0);
    EXPECT_TRUE(peak_jitter_histogram[result.peak_jitter] > 0);
}

static void test_tensor_loading_round_trip(void)
{
    static tensor_array source;
    static tensor_array decoded;
    size_t blob_size = 0;
    unsigned char *blob;
    unsigned char *bad_blob;
    char path[] = "/tmp/skipper_tensor_test_XXXXXX";
    int fd;
    FILE *file;

    fill_test_tensor(source);
    memset(decoded, 0, sizeof(decoded));
    blob = make_compressed_tensor_blob(source, &blob_size);
    EXPECT_TRUE(blob != NULL);
    if (!blob)
        return;

    EXPECT_TRUE(local_tensor_file(decoded, blob, (int)blob_size));
    EXPECT_MEMEQ(source, decoded, sizeof(tensor_array));

    fd = mkstemp(path);
    EXPECT_TRUE(fd >= 0);
    file = fd >= 0 ? fdopen(fd, "wb") : NULL;
    EXPECT_TRUE(file != NULL);
    if (file) {
        EXPECT_EQ_SIZE(blob_size, fwrite(blob, 1, blob_size, file));
        fclose(file);
        memset(decoded, 0, sizeof(decoded));
        EXPECT_TRUE(read_tensor_file(decoded, path));
        EXPECT_MEMEQ(source, decoded, sizeof(tensor_array));
        unlink(path);
    } else if (fd >= 0) {
        close(fd);
        unlink(path);
    }

    EXPECT_FALSE(local_tensor_file_suppressed(decoded, blob, (int)sizeof(struct tensor_header) - 1));

    bad_blob = malloc(blob_size);
    EXPECT_TRUE(bad_blob != NULL);
    if (bad_blob) {
        memcpy(bad_blob, blob, blob_size);
        ((struct tensor_header *)bad_blob)->version = TENSOR_VERSION + 1;
        EXPECT_FALSE(local_tensor_file_suppressed(decoded, bad_blob, (int)blob_size));
        free(bad_blob);
    }

    free(blob);
}

static void test_program_main_pass_all_mono_input(void)
{
    int16_t input[] = { 1000, -1000, 3000, -3000 };
    int16_t expected[] = {
        1000, 1000,
        -1000, -1000,
        3000, 3000,
        -3000, -3000
    };
    char arg0[] = "skipper";
    char arg1[] = "-p";
    char arg2[] = "-q";
    char arg3[] = "-c1";
    char arg4[] = "-s11025";
    char *argv[] = { arg0, arg1, arg2, arg3, arg4 };
    ProgramRun run = run_skipper_with_input(5, argv, input, 4, 1);

    EXPECT_EQ_INT(0, run.status);
    EXPECT_EQ_SIZE(sizeof(expected), run.output_size);
    if (run.output)
        EXPECT_MEMEQ(expected, run.output, sizeof(expected));
    free(run.output);
}

static void test_program_main_skip_everything_and_debug_mono(void)
{
    int16_t mono_input[] = { 1000, -1000, 3000, -3000 };
    int16_t stereo_input[] = { 1000, 3000, -2000, 2000 };
    int16_t expected_mono_debug[] = {
        2000, 2000,
        0, 0
    };
    char skip_arg0[] = "skipper";
    char skip_arg1[] = "-n";
    char skip_arg2[] = "-q";
    char skip_arg3[] = "-c1";
    char skip_arg4[] = "-s11025";
    char *skip_argv[] = { skip_arg0, skip_arg1, skip_arg2, skip_arg3, skip_arg4 };
    char mono_arg0[] = "skipper";
    char mono_arg1[] = "-p";
    char mono_arg2[] = "-q";
    char mono_arg3[] = "-c2";
    char mono_arg4[] = "-s11025";
    char mono_arg5[] = "-l1";
    char mono_arg6[] = "-r1";
    char *mono_argv[] = { mono_arg0, mono_arg1, mono_arg2, mono_arg3, mono_arg4, mono_arg5, mono_arg6 };
    ProgramRun run;

    run = run_skipper_with_input(5, skip_argv, mono_input, 4, 1);
    EXPECT_EQ_INT(0, run.status);
    EXPECT_EQ_SIZE(0, run.output_size);
    free(run.output);

    run = run_skipper_with_input(7, mono_argv, stereo_input, 2, 2);
    EXPECT_EQ_INT(0, run.status);
    EXPECT_EQ_SIZE(sizeof(expected_mono_debug), run.output_size);
    if (run.output)
        EXPECT_MEMEQ(expected_mono_debug, run.output, sizeof(expected_mono_debug));
    free(run.output);
}

static void test_program_main_argument_validation(void)
{
    int16_t no_input = 0;
    char usage_arg0[] = "skipper";
    char *usage_argv[] = { usage_arg0 };
    char bad_channels0[] = "skipper";
    char bad_channels1[] = "-c3";
    char *bad_channels[] = { bad_channels0, bad_channels1 };
    char bad_rate0[] = "skipper";
    char bad_rate1[] = "-s96001";
    char *bad_rate[] = { bad_rate0, bad_rate1 };
    char bad_left0[] = "skipper";
    char bad_left1[] = "-l5";
    char *bad_left[] = { bad_left0, bad_left1 };
    char bad_right0[] = "skipper";
    char bad_right1[] = "-r5";
    char *bad_right[] = { bad_right0, bad_right1 };
    char bad_music0[] = "skipper";
    char bad_music1[] = "-m100";
    char *bad_music[] = { bad_music0, bad_music1 };
    char bad_talk0[] = "skipper";
    char bad_talk1[] = "-t100";
    char *bad_talk[] = { bad_talk0, bad_talk1 };
    char missing_analysis0[] = "skipper";
    char missing_analysis1[] = "-a";
    char *missing_analysis[] = { missing_analysis0, missing_analysis1 };
    char missing_tensor0[] = "skipper";
    char missing_tensor1[] = "-d";
    char *missing_tensor[] = { missing_tensor0, missing_tensor1 };
    char missing_time0[] = "skipper";
    char missing_time1[] = "-T";
    char *missing_time[] = { missing_time0, missing_time1 };
    char missing_zone0[] = "skipper";
    char missing_zone1[] = "-z";
    char *missing_zone[] = { missing_zone0, missing_zone1 };
    char missing_window0[] = "skipper";
    char missing_window1[] = "-w";
    char *missing_window[] = { missing_window0, missing_window1 };
    char bad_time0[] = "skipper";
    char bad_time1[] = "-T";
    char bad_time2[] = "2024-02-30T00:00:00Z";
    char *bad_time[] = { bad_time0, bad_time1, bad_time2 };
    char bad_zone0[] = "skipper";
    char bad_zone1[] = "-z+24:00";
    char *bad_zone[] = { bad_zone0, bad_zone1 };
    char bad_window0[] = "skipper";
    char bad_window1[] = "-w0-60";
    char *bad_window[] = { bad_window0, bad_window1 };
    char extra0[] = "skipper";
    char extra1[] = "extra";
    char *extra[] = { extra0, extra1 };
    ProgramRun run;

    run = run_skipper_with_input(1, usage_argv, &no_input, 0, 1);
    EXPECT_EQ_INT(0, run.status);
    EXPECT_EQ_SIZE(0, run.output_size);
    free(run.output);

    run = run_skipper_with_input(2, bad_channels, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, bad_rate, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, bad_left, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, bad_right, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, bad_music, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, bad_talk, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, missing_analysis, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, missing_tensor, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, missing_time, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, missing_zone, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, missing_window, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(3, bad_time, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, bad_zone, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, bad_window, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
    run = run_skipper_with_input(2, extra, &no_input, 0, 1);
    EXPECT_TRUE(run.status != 0);
    free(run.output);
}

int main(void)
{
    test_calendar_helpers();
    test_parse_utc_offset_minutes();
    test_parse_time_restriction_window();
    test_parse_iso8601_timestamp_ms();
    test_time_restricted_skip_window();
    test_should_skip_mode_at_time();
    test_fades();
    test_analyze_window_constant_and_cyclic_levels();
    test_tensor_loading_round_trip();
    test_program_main_pass_all_mono_input();
    test_program_main_skip_everything_and_debug_mono();
    test_program_main_argument_validation();

    if (tests_failed) {
        fprintf(stderr, "%d of %d skipper tests failed\n", tests_failed, tests_run);
        return 1;
    }

    printf("All %d skipper tests passed\n", tests_run);
    return 0;
}
