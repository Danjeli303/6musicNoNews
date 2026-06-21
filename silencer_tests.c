#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lzwlib.h"

#define main silencer_program_main
#include "silencer.c"
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

#define EXPECT_STREQ(expected, actual) do { \
    const char *expected_value = (expected); \
    const char *actual_value = (actual); \
    tests_run++; \
    if (strcmp(expected_value, actual_value) != 0) { \
        fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, expected_value, actual_value); \
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
} CapturedRun;

typedef void (*CapturedCall)(void *context);

typedef struct {
    const ProgramConfig *config;
    AudioBuffers *buffers;
    ProgramState *state;
} AudioWriteCall;

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

static ProgramConfig config_with_stream_time(const char *time_text, int display_offset_minutes)
{
    ProgramConfig config;
    int parsed_offset = 0;

    initialize_program_config(&config);
    config.stream_start_epoch_ms = parse_epoch_ms_or_fail(time_text, &parsed_offset);
    config.stream_time_enabled = 1;
    config.stream_time_utc_offset_minutes = display_offset_minutes;
    config.stream_time_utc_offset_is_set = 1;
    return config;
}

static CapturedRun capture_stdout_from_call(CapturedCall call, void *context)
{
    CapturedRun run = { 0, NULL, 0 };
    FILE *output_file = tmpfile();
    FILE *stderr_file = tmpfile();
    int saved_stdout;
    int saved_stderr;
    long output_size;

    EXPECT_TRUE(output_file != NULL);
    EXPECT_TRUE(stderr_file != NULL);
    if (!output_file || !stderr_file)
        goto done;

    fflush(stdout);
    fflush(stderr);
    saved_stdout = dup(fileno(stdout));
    saved_stderr = dup(fileno(stderr));
    EXPECT_TRUE(saved_stdout >= 0 && saved_stderr >= 0);
    if (saved_stdout < 0 || saved_stderr < 0)
        goto done;

    EXPECT_TRUE(dup2(fileno(output_file), fileno(stdout)) >= 0);
    EXPECT_TRUE(dup2(fileno(stderr_file), fileno(stderr)) >= 0);
    call(context);
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

    EXPECT_TRUE(dup2(saved_stdout, fileno(stdout)) >= 0);
    EXPECT_TRUE(dup2(saved_stderr, fileno(stderr)) >= 0);
    close(saved_stdout);
    close(saved_stderr);

done:
    if (output_file) fclose(output_file);
    if (stderr_file) fclose(stderr_file);
    return run;
}

static CapturedRun run_silencer_with_input(int argc, char **argv, const int16_t *input, size_t frames, int channels)
{
    CapturedRun run = { 0, NULL, 0 };
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

    run.status = silencer_program_main(argc, argv);

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

static int parse_args_suppressed(int argc, char **argv, ProgramConfig *config)
{
    int result;
    FILE *stderr_file = tmpfile();
    int saved_stderr;

    EXPECT_TRUE(stderr_file != NULL);
    if (!stderr_file)
        return parse_command_line_arguments(argc, argv, config);

    fflush(stderr);
    saved_stderr = dup(fileno(stderr));
    EXPECT_TRUE(saved_stderr >= 0);
    if (saved_stderr < 0) {
        fclose(stderr_file);
        return parse_command_line_arguments(argc, argv, config);
    }

    EXPECT_TRUE(dup2(fileno(stderr_file), fileno(stderr)) >= 0);
    result = parse_command_line_arguments(argc, argv, config);
    fflush(stderr);
    EXPECT_TRUE(dup2(saved_stderr, fileno(stderr)) >= 0);
    close(saved_stderr);
    fclose(stderr_file);
    return result;
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

static void call_write_confirmed(void *context)
{
    AudioWriteCall *call = (AudioWriteCall *)context;
    write_confirmed_audio_to_stdout(call->config, call->buffers, call->state);
}

static void call_flush_remaining(void *context)
{
    AudioWriteCall *call = (AudioWriteCall *)context;
    ProgramConfig config = *call->config;
    flush_remaining_audio(&config, call->buffers, call->state);
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

static void test_default_config(void)
{
    ProgramConfig config;

    initialize_program_config(&config);

    EXPECT_EQ_INT(DEFAULT_CHANNELS, config.input_channels);
    EXPECT_EQ_INT(DEFAULT_SAMPLE_RATE, config.sample_rate);
    EXPECT_EQ_INT(PROCESSING_MODE_PASS_ALL, config.processing_mode);
    EXPECT_EQ_INT(OUTPUT_AUDIO, config.left_debug_output_mode);
    EXPECT_EQ_INT(OUTPUT_AUDIO, config.right_debug_output_mode);
    EXPECT_EQ_INT(0, config.time_restricted_silence_enabled);
    EXPECT_EQ_INT(0, config.stream_time_enabled);
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

static void test_format_epoch_ms_with_utc_offset(void)
{
    char buffer[64];

    EXPECT_TRUE(format_epoch_ms_with_utc_offset(0, 0, buffer, sizeof(buffer)));
    EXPECT_STREQ("1970-01-01 00:00:00.000 +00:00", buffer);

    EXPECT_TRUE(format_epoch_ms_with_utc_offset(0, 90, buffer, sizeof(buffer)));
    EXPECT_STREQ("1970-01-01 01:30:00.000 +01:30", buffer);

    EXPECT_TRUE(format_epoch_ms_with_utc_offset(-1, 0, buffer, sizeof(buffer)));
    EXPECT_STREQ("1969-12-31 23:59:59.999 +00:00", buffer);

    EXPECT_FALSE(format_epoch_ms_with_utc_offset(0, 24 * 60, buffer, sizeof(buffer)));
}

static void test_command_line_parsing(void)
{
    ProgramConfig config;
    char *argv[] = {
        "silencer",
        "-t12",
        "-x",
        "-s48000",
        "-c1",
        "-l3",
        "-r4",
        "-T",
        "2026-06-20T18:38:11.200Z",
        "-z+01:00",
        "-q"
    };

    initialize_program_config(&config);
    EXPECT_TRUE(parse_command_line_arguments((int)(sizeof(argv) / sizeof(argv[0])), argv, &config));
    EXPECT_EQ_INT(PROCESSING_MODE_SILENCE_TALK, config.processing_mode);
    EXPECT_EQ_INT(-12, config.detection_threshold);
    EXPECT_EQ_INT(1, config.time_restricted_silence_enabled);
    EXPECT_EQ_INT(48000, config.sample_rate);
    EXPECT_EQ_INT(1, config.input_channels);
    EXPECT_EQ_INT(OUTPUT_LEVEL, config.left_debug_output_mode);
    EXPECT_EQ_INT(OUTPUT_TENSOR, config.right_debug_output_mode);
    EXPECT_EQ_INT(1, config.stream_time_enabled);
    EXPECT_EQ_INT(60, config.stream_time_utc_offset_minutes);
    EXPECT_EQ_INT(1, config.quiet_mode);
}

static void test_command_line_parsing_rejects_invalid_inputs(void)
{
    ProgramConfig config;
    char *bad_channels[] = { "silencer", "-c3" };
    char *bad_rate[] = { "silencer", "-s96001" };
    char *bad_left[] = { "silencer", "-l5" };
    char *bad_right[] = { "silencer", "-r5" };
    char *bad_music_threshold[] = { "silencer", "-m100" };
    char *bad_talk_threshold[] = { "silencer", "-t100" };
    char *missing_analysis[] = { "silencer", "-a" };
    char *missing_tensor[] = { "silencer", "-d" };
    char *missing_time[] = { "silencer", "-T" };
    char *missing_zone[] = { "silencer", "-z" };
    char *bad_time[] = { "silencer", "-T", "2024-02-30T00:00:00Z" };
    char *bad_zone[] = { "silencer", "-z+24:00" };
    char *extra[] = { "silencer", "extra" };

    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(2, bad_channels, &config));
    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(2, bad_rate, &config));
    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(2, bad_left, &config));
    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(2, bad_right, &config));
    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(2, bad_music_threshold, &config));
    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(2, bad_talk_threshold, &config));
    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(2, missing_analysis, &config));
    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(2, missing_tensor, &config));
    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(2, missing_time, &config));
    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(2, missing_zone, &config));
    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(3, bad_time, &config));
    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(2, bad_zone, &config));
    initialize_program_config(&config);
    EXPECT_FALSE(parse_args_suppressed(2, extra, &config));
}

static void test_time_restricted_silence_window(void)
{
    ProgramConfig pre_news_config = config_with_stream_time("2026-06-20T05:58:00Z", 0);
    ProgramConfig active_config = config_with_stream_time("2026-06-20T06:00:00Z", 0);
    ProgramConfig half_hour_config = config_with_stream_time("2026-06-20T06:28:00Z", 0);
    ProgramConfig inactive_config = config_with_stream_time("2026-06-20T09:10:00Z", 60);
    ProgramConfig last_window_config = config_with_stream_time("2026-06-20T21:58:00Z", 0);
    ProgramConfig late_config = config_with_stream_time("2026-06-20T22:00:00Z", 0);

    pre_news_config.sample_rate = 1000;
    active_config.sample_rate = 1000;
    half_hour_config.sample_rate = 1000;
    inactive_config.sample_rate = 1000;
    last_window_config.sample_rate = 1000;
    late_config.sample_rate = 1000;

    EXPECT_FALSE(is_time_restricted_silence_active_at_sample(&pre_news_config, 0));
    EXPECT_TRUE(is_time_restricted_silence_active_at_sample(&active_config, 0));
    EXPECT_TRUE(is_time_restricted_silence_active_at_sample(&active_config, 4 * 60 * active_config.sample_rate));
    EXPECT_FALSE(is_time_restricted_silence_active_at_sample(&active_config, 5 * 60 * active_config.sample_rate));
    EXPECT_TRUE(is_time_restricted_silence_active_at_sample(&half_hour_config, 0));
    EXPECT_TRUE(is_time_restricted_silence_active_at_sample(&half_hour_config, 6 * 60 * half_hour_config.sample_rate));
    EXPECT_FALSE(is_time_restricted_silence_active_at_sample(&half_hour_config, 7 * 60 * half_hour_config.sample_rate));
    EXPECT_TRUE(is_time_restricted_silence_active_at_sample(&last_window_config, 0));
    EXPECT_FALSE(is_time_restricted_silence_active_at_sample(&inactive_config, 0));
    EXPECT_FALSE(is_time_restricted_silence_active_at_sample(&late_config, 0));
}

static void test_should_silence_audio_mode_at_sample(void)
{
    ProgramConfig config = config_with_stream_time("2026-06-20T05:58:00Z", 60);
    ProgramConfig inactive_config = config_with_stream_time("2026-06-20T09:10:00Z", 60);

    config.sample_rate = 1000;
    inactive_config.sample_rate = 1000;

    config.processing_mode = PROCESSING_MODE_SILENCE_ALL;
    EXPECT_TRUE(should_silence_audio_mode_at_sample(&config, AUDIO_MODE_MUSIC, 0));

    config.processing_mode = PROCESSING_MODE_SILENCE_MUSIC;
    EXPECT_TRUE(should_silence_audio_mode_at_sample(&config, AUDIO_MODE_MUSIC, 0));
    EXPECT_FALSE(should_silence_audio_mode_at_sample(&config, AUDIO_MODE_TALK, 0));

    config.processing_mode = PROCESSING_MODE_SILENCE_TALK;
    config.time_restricted_silence_enabled = 1;
    EXPECT_TRUE(should_silence_audio_mode_at_sample(&config, AUDIO_MODE_TALK, 0));
    EXPECT_FALSE(should_silence_audio_mode_at_sample(&config, AUDIO_MODE_MUSIC, 0));
    EXPECT_FALSE(should_silence_audio_mode_at_sample(&config, AUDIO_MODE_NOTHING, 0));

    inactive_config.processing_mode = PROCESSING_MODE_SILENCE_TALK;
    inactive_config.time_restricted_silence_enabled = 1;
    EXPECT_FALSE(should_silence_audio_mode_at_sample(&inactive_config, AUDIO_MODE_TALK, 0));

    inactive_config.time_restricted_silence_enabled = 0;
    EXPECT_TRUE(should_silence_audio_mode_at_sample(&inactive_config, AUDIO_MODE_TALK, 0));
}

static void test_bypass_talk_silencing(void)
{
    ProgramConfig config = config_with_stream_time("2026-06-20T09:10:00Z", 60);
    ProgramState state = { 0 };

    config.sample_rate = 1000;
    config.processing_mode = PROCESSING_MODE_SILENCE_TALK;
    config.time_restricted_silence_enabled = 1;

    EXPECT_TRUE(should_bypass_talk_silencing_due_to_time_restriction(&config, &state));

    config.stream_start_epoch_ms = parse_epoch_ms_or_fail("2026-06-20T05:58:00Z", &config.stream_time_utc_offset_minutes);
    config.stream_time_utc_offset_minutes = 60;
    EXPECT_FALSE(should_bypass_talk_silencing_due_to_time_restriction(&config, &state));

    config.processing_mode = PROCESSING_MODE_SILENCE_MUSIC;
    EXPECT_FALSE(should_bypass_talk_silencing_due_to_time_restriction(&config, &state));
}

static void test_fades(void)
{
    int16_t fade_out_samples[] = { 1000, -1000, 1000, -1000, 1000, -1000, 1000, -1000 };
    int16_t fade_in_samples[] = { 1000, -1000, 1000, -1000, 1000, -1000, 1000, -1000 };

    fade_out(fade_out_samples, 4, 2);
    EXPECT_EQ_INT(1000, fade_out_samples[0]);
    EXPECT_EQ_INT(-1000, fade_out_samples[1]);
    EXPECT_EQ_INT(666, fade_out_samples[2]);
    EXPECT_EQ_INT(-666, fade_out_samples[3]);
    EXPECT_EQ_INT(333, fade_out_samples[4]);
    EXPECT_EQ_INT(-333, fade_out_samples[5]);
    EXPECT_EQ_INT(0, fade_out_samples[6]);
    EXPECT_EQ_INT(0, fade_out_samples[7]);

    fade_in(fade_in_samples, 4, 2);
    EXPECT_EQ_INT(0, fade_in_samples[0]);
    EXPECT_EQ_INT(0, fade_in_samples[1]);
    EXPECT_EQ_INT(333, fade_in_samples[2]);
    EXPECT_EQ_INT(-333, fade_in_samples[3]);
    EXPECT_EQ_INT(666, fade_in_samples[4]);
    EXPECT_EQ_INT(-666, fade_in_samples[5]);
    EXPECT_EQ_INT(1000, fade_in_samples[6]);
    EXPECT_EQ_INT(-1000, fade_in_samples[7]);
}

static void test_buffer_allocation_and_filter_initialization(void)
{
    ProgramConfig config;
    AudioBuffers buffers = { 0 };
    ProgramState state = { 0 };
    int nonzero_values = 0;

    initialize_program_config(&config);
    config.sample_rate = 11025;
    config.input_channels = 1;

    EXPECT_TRUE(allocate_audio_buffers(&config, &buffers, &state));
    EXPECT_TRUE(buffers.input_buffer != NULL);
    EXPECT_TRUE(buffers.mono_float_samples != NULL);
    EXPECT_TRUE(buffers.rms_level_ring_buffer != NULL);
    EXPECT_TRUE(buffers.analysis_level_buffer != NULL);
    EXPECT_TRUE(buffers.main_output_buffer != NULL);
    EXPECT_TRUE(buffers.crossfade_buffer != NULL);
    EXPECT_EQ_INT(ANALYSIS_STEP_MSECS * config.sample_rate / 1000, state.analysis_step_samples);
    EXPECT_EQ_INT((config.sample_rate * LEVEL_WINDOW_MS + 500) / 1000, state.rms_ring_buffer_len);
    EXPECT_EQ_INT(ANALYSIS_WINDOW_SECONDS * config.sample_rate, state.analysis_level_buffer_len);
    EXPECT_EQ_INT(OUTPUT_BUFFER_DURATION_SECS * config.sample_rate, state.main_output_buffer_len);
    EXPECT_EQ_INT(CROSSFADE_DURATION_SECS * config.sample_rate, state.crossfade_buffer_len_samples);

    initialize_audio_filters(&config, &buffers, &state);
    EXPECT_EQ_INT(0x31415926, (int)state.dither_rng_state);
    prime_rms_ring_buffer(&buffers, &state);
    for (int i = 0; i < state.rms_ring_buffer_len; ++i)
        if (buffers.rms_level_ring_buffer[i] != 0.0f)
            nonzero_values++;
    EXPECT_TRUE(nonzero_values > 0);

    cleanup_resources(&buffers);
}

static void test_populate_main_output_buffer_sample_modes(void)
{
    ProgramConfig config;
    AudioBuffers buffers = { 0 };
    ProgramState state = { 0 };
    int16_t main_output[12] = { 0 };
    float analysis_levels[4] = { 32768.0f * 32767.0f * 0.5f, 0.0f, 0.0f, 0.0f };
    int16_t stereo_frame[] = { 1000, 3000 };
    int16_t mono_frame[] = { -2000 };

    initialize_program_config(&config);
    buffers.main_output_buffer = main_output;
    buffers.analysis_level_buffer = analysis_levels;
    state.rms_ring_buffer_len = 2;
    state.analysis_level_buffer_idx = 1;

    config.input_channels = 2;
    config.left_debug_output_mode = OUTPUT_AUDIO;
    config.right_debug_output_mode = OUTPUT_MONO;
    populate_main_output_buffer_sample(&config, &buffers, &state, stereo_frame, 123.0f);
    EXPECT_EQ_INT(1000, main_output[0]);
    EXPECT_EQ_INT(2000, main_output[1]);

    memset(main_output, 0, sizeof(main_output));
    config.left_debug_output_mode = OUTPUT_FILTERED;
    config.right_debug_output_mode = OUTPUT_AUDIO;
    populate_main_output_buffer_sample(&config, &buffers, &state, stereo_frame, -123.0f);
    EXPECT_EQ_INT(-123, main_output[0]);
    EXPECT_EQ_INT(3000, main_output[1]);

    memset(main_output, 0, sizeof(main_output));
    config.input_channels = 1;
    config.left_debug_output_mode = OUTPUT_MONO;
    config.right_debug_output_mode = OUTPUT_AUDIO;
    populate_main_output_buffer_sample(&config, &buffers, &state, mono_frame, 0.0f);
    EXPECT_EQ_INT(-2000, main_output[0]);
    EXPECT_EQ_INT(-2000, main_output[1]);

    memset(main_output, 0, sizeof(main_output));
    config.left_debug_output_mode = OUTPUT_LEVEL;
    config.right_debug_output_mode = OUTPUT_LEVEL;
    state.main_output_buffer_idx = 1;
    populate_main_output_buffer_sample(&config, &buffers, &state, mono_frame, 0.0f);
    EXPECT_TRUE(main_output[0] > 32000);
    EXPECT_TRUE(main_output[1] > 32000);

    memset(main_output, 1, sizeof(main_output));
    analysis_levels[0] = 0.0f;
    populate_main_output_buffer_sample(&config, &buffers, &state, mono_frame, 0.0f);
    EXPECT_EQ_INT(-32768, main_output[0]);
    EXPECT_EQ_INT(-32768, main_output[1]);
}

static void test_analyze_window_constant_and_cyclic_levels(void)
{
    float constant_levels[8];
    float cyclic_levels[16];
    struct analysis_result result;
    int tensor_result;

    reset_analysis_histograms();
    memset(loaded_tensor_data, 42, sizeof(loaded_tensor_data));
    for (size_t i = 0; i < sizeof(constant_levels) / sizeof(constant_levels[0]); ++i)
        constant_levels[i] = 100.0f;

    analysis_binary_output_file = tmpfile();
    EXPECT_TRUE(analysis_binary_output_file != NULL);
    tensor_result = analyze_window(constant_levels, 8, 8, 1000);
    EXPECT_EQ_INT(42, tensor_result);
    fflush(analysis_binary_output_file);
    rewind(analysis_binary_output_file);
    EXPECT_EQ_SIZE(1, fread(&result, sizeof(result), 1, analysis_binary_output_file));
    fclose(analysis_binary_output_file);
    analysis_binary_output_file = NULL;

    EXPECT_EQ_INT(0, result.range_dB);
    EXPECT_EQ_INT(0, result.cycles);
    EXPECT_EQ_INT(244, result.low_third);
    EXPECT_EQ_INT(0, result.mid_third);
    EXPECT_EQ_INT(0, result.high_third);
    EXPECT_EQ_INT(128, result.attack_ratio);
    EXPECT_EQ_INT(255, result.peak_jitter);
    EXPECT_EQ_INT(0, result.spare);
    EXPECT_EQ_INT(1, peak_to_trough_histogram[0]);
    EXPECT_EQ_INT(1, cycles_histogram[0]);

    reset_analysis_histograms();
    memset(loaded_tensor_data, -7, sizeof(loaded_tensor_data));
    for (size_t i = 0; i < sizeof(cyclic_levels) / sizeof(cyclic_levels[0]); ++i)
        cyclic_levels[i] = (i & 1) ? 400.0f : 100.0f;

    analysis_binary_output_file = tmpfile();
    EXPECT_TRUE(analysis_binary_output_file != NULL);
    tensor_result = analyze_window(cyclic_levels, 16, 16, 1000);
    EXPECT_EQ_INT(-7, tensor_result);
    fflush(analysis_binary_output_file);
    rewind(analysis_binary_output_file);
    EXPECT_EQ_SIZE(1, fread(&result, sizeof(result), 1, analysis_binary_output_file));
    fclose(analysis_binary_output_file);
    analysis_binary_output_file = NULL;

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
    char path[] = "/tmp/silencer_tensor_test_XXXXXX";
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

static void test_detection_confirmation_logic(void)
{
    ProgramConfig config;
    AudioBuffers buffers = { 0 };
    ProgramState state = { 0 };
    float levels[8];

    initialize_program_config(&config);
    config.sample_rate = 1000;
    config.quiet_mode = 1;
    state.analysis_level_buffer_len = 8;
    state.analysis_step_samples = 200;
    state.crossfade_buffer_len_samples = 2;
    buffers.analysis_level_buffer = levels;
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i)
        levels[i] = 100.0f;

    memset(loaded_tensor_data, 10, sizeof(loaded_tensor_data));
    for (int i = 0; i < AVERAGING_BUFFER_COUNT + MIN_MUSIC_DURATION_SECS * 1000 / ANALYSIS_STEP_MSECS; ++i) {
        state.total_samples_processed += state.analysis_step_samples;
        perform_detection_and_handle_transitions(&config, &buffers, &state);
    }
    EXPECT_EQ_INT(AUDIO_MODE_MUSIC, state.current_audio_mode);
    EXPECT_TRUE(state.raw_music_hits > 0);

    memset(loaded_tensor_data, -10, sizeof(loaded_tensor_data));
    for (int i = 0; i < AVERAGING_BUFFER_COUNT + MIN_TALK_DURATION_SECS * 1000 / ANALYSIS_STEP_MSECS; ++i) {
        state.total_samples_processed += state.analysis_step_samples;
        perform_detection_and_handle_transitions(&config, &buffers, &state);
    }
    EXPECT_EQ_INT(AUDIO_MODE_TALK, state.current_audio_mode);
    EXPECT_TRUE(state.raw_talk_hits > 0);
}

static void test_write_confirmed_audio_to_stdout_pass_and_silence(void)
{
    ProgramConfig config;
    AudioBuffers buffers = { 0 };
    ProgramState state = { 0 };
    int16_t main_output[] = { 1, 2, 3, 4, 5, 6 };
    int16_t crossfade[4] = { 0 };
    int16_t zeros[] = { 0, 0, 0, 0, 0, 0 };
    AudioWriteCall call;
    CapturedRun run;

    initialize_program_config(&config);
    config.sample_rate = 1000;
    buffers.main_output_buffer = main_output;
    buffers.crossfade_buffer = crossfade;
    state.main_output_buffer_idx = 3;
    state.main_output_buffer_len = 8;
    state.total_samples_processed = 3;
    state.last_confirmed_sample_point = 3;
    state.crossfade_buffer_len_samples = 2;
    state.current_audio_mode = AUDIO_MODE_MUSIC;
    call.config = &config;
    call.buffers = &buffers;
    call.state = &state;

    run = capture_stdout_from_call(call_write_confirmed, &call);
    EXPECT_EQ_SIZE(sizeof(main_output), run.output_size);
    if (run.output)
        EXPECT_MEMEQ(main_output, run.output, sizeof(main_output));
    EXPECT_EQ_INT(3, (int)state.samples_output_audible);
    EXPECT_EQ_INT(0, state.main_output_buffer_idx);
    free(run.output);

    memcpy(main_output, (int16_t[]){ 1, 2, 3, 4, 5, 6 }, sizeof(main_output));
    memset(&state, 0, sizeof(state));
    config.processing_mode = PROCESSING_MODE_SILENCE_ALL;
    state.main_output_buffer_idx = 3;
    state.main_output_buffer_len = 8;
    state.total_samples_processed = 3;
    state.last_confirmed_sample_point = 3;
    state.crossfade_buffer_len_samples = 2;
    state.current_audio_mode = AUDIO_MODE_MUSIC;
    run = capture_stdout_from_call(call_write_confirmed, &call);
    EXPECT_EQ_SIZE(sizeof(zeros), run.output_size);
    if (run.output)
        EXPECT_MEMEQ(zeros, run.output, sizeof(zeros));
    EXPECT_EQ_INT(3, (int)state.samples_output_silenced);
    EXPECT_EQ_INT(0, state.main_output_buffer_idx);
    free(run.output);
}

static void test_write_confirmed_audio_keepalive(void)
{
    ProgramConfig config;
    AudioBuffers buffers = { 0 };
    ProgramState state = { 0 };
    int16_t main_output[] = { 100, 100, 200, 200, 300, 300, 400, 400, 500, 500, 600, 600 };
    int16_t crossfade[4] = { 0 };
    AudioWriteCall call;
    CapturedRun run;

    initialize_program_config(&config);
    config.sample_rate = 1000;
    config.processing_mode = PROCESSING_MODE_SILENCE_ALL;
    config.keep_alive_enabled = 1;
    buffers.main_output_buffer = main_output;
    buffers.crossfade_buffer = crossfade;
    state.main_output_buffer_idx = 6;
    state.main_output_buffer_len = 8;
    state.total_samples_processed = 6;
    state.last_confirmed_sample_point = 6;
    state.crossfade_buffer_len_samples = 2;
    call.config = &config;
    call.buffers = &buffers;
    call.state = &state;

    run = capture_stdout_from_call(call_write_confirmed, &call);
    EXPECT_EQ_SIZE(sizeof(main_output), run.output_size);
    EXPECT_EQ_INT(2, (int)state.samples_output_audible);
    EXPECT_EQ_INT(4, (int)state.samples_output_silenced);
    EXPECT_EQ_INT(0, state.main_output_buffer_idx);
    free(run.output);
}

static void test_flush_remaining_audio(void)
{
    ProgramConfig config;
    AudioBuffers buffers = { 0 };
    ProgramState state = { 0 };
    int16_t main_output[] = { 7, 8, 9, 10 };
    int16_t zeros[] = { 0, 0, 0, 0 };
    AudioWriteCall call;
    CapturedRun run;

    initialize_program_config(&config);
    config.sample_rate = 1000;
    buffers.main_output_buffer = main_output;
    state.main_output_buffer_idx = 2;
    state.current_audio_mode = AUDIO_MODE_TALK;
    call.config = &config;
    call.buffers = &buffers;
    call.state = &state;

    run = capture_stdout_from_call(call_flush_remaining, &call);
    EXPECT_EQ_SIZE(sizeof(main_output), run.output_size);
    if (run.output)
        EXPECT_MEMEQ(main_output, run.output, sizeof(main_output));
    EXPECT_EQ_INT(2, (int)state.samples_output_audible);
    EXPECT_EQ_INT(0, state.main_output_buffer_idx);
    free(run.output);

    memcpy(main_output, (int16_t[]){ 7, 8, 9, 10 }, sizeof(main_output));
    memset(&state, 0, sizeof(state));
    config.processing_mode = PROCESSING_MODE_SILENCE_TALK;
    state.main_output_buffer_idx = 2;
    state.current_audio_mode = AUDIO_MODE_TALK;
    run = capture_stdout_from_call(call_flush_remaining, &call);
    EXPECT_EQ_SIZE(sizeof(zeros), run.output_size);
    if (run.output)
        EXPECT_MEMEQ(zeros, run.output, sizeof(zeros));
    EXPECT_EQ_INT(2, (int)state.samples_output_silenced);
    EXPECT_EQ_INT(0, state.main_output_buffer_idx);
    free(run.output);
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
    char arg0[] = "silencer";
    char arg1[] = "-p";
    char arg2[] = "-q";
    char arg3[] = "-c1";
    char arg4[] = "-s11025";
    char *argv[] = { arg0, arg1, arg2, arg3, arg4 };
    CapturedRun run = run_silencer_with_input(5, argv, input, 4, 1);

    EXPECT_EQ_INT(0, run.status);
    EXPECT_EQ_SIZE(sizeof(expected), run.output_size);
    if (run.output)
        EXPECT_MEMEQ(expected, run.output, sizeof(expected));
    free(run.output);
}

int main(void)
{
    test_calendar_helpers();
    test_default_config();
    test_parse_utc_offset_minutes();
    test_parse_iso8601_timestamp_ms();
    test_format_epoch_ms_with_utc_offset();
    test_command_line_parsing();
    test_command_line_parsing_rejects_invalid_inputs();
    test_time_restricted_silence_window();
    test_should_silence_audio_mode_at_sample();
    test_bypass_talk_silencing();
    test_fades();
    test_buffer_allocation_and_filter_initialization();
    test_populate_main_output_buffer_sample_modes();
    test_analyze_window_constant_and_cyclic_levels();
    test_tensor_loading_round_trip();
    test_detection_confirmation_logic();
    test_write_confirmed_audio_to_stdout_pass_and_silence();
    test_write_confirmed_audio_keepalive();
    test_flush_remaining_audio();
    test_program_main_pass_all_mono_input();

    if (tests_failed) {
        fprintf(stderr, "%d of %d silencer tests failed\n", tests_failed, tests_run);
        return 1;
    }

    printf("All %d silencer tests passed\n", tests_run);
    return 0;
}
