////////////////////////////////////////////////////////////////////////////
//                            **** SKIPPER **** //
//                  Selective Audio Detection and Filter                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// MODIFIED VERSION:
// - Silences talk/music instead of skipping.
// - Refactored for improved readability.
// - Added -x option for time-restricted talk silencing.
// - Corrected compiler errors and warnings.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h> // Added for time-based functionality

#ifdef _WIN32
#include <fcntl.h>
#endif

#include "4d-tensor.h" // Assumed to be in the same directory or include path
#include "skipper.h"   // Assumed to be in the same directory or include path
#include "biquad.h"    // Assumed to be in the same directory or include path
#include "skipper_time.h"
#include "skipper_tensor.h"

#define VERSION         0.1

// Output modes for debug views
#define OUTPUT_AUDIO    0 // Normal audio passthrough/silencing
#define OUTPUT_MONO     1 // Mono version of input
#define OUTPUT_FILTERED 2 // Filtered mono audio used for analysis
#define OUTPUT_LEVEL    3 // RMS level of the audio
#define OUTPUT_TENSOR   4 // Tensor discrimination value

// Processing modes define what to do with detected talk/music
#define PROCESSING_MODE_PASS_ALL     0 // Pass all audio
#define PROCESSING_MODE_SILENCE_TALK 1 // Silence talk, pass music
#define PROCESSING_MODE_SILENCE_MUSIC 2 // Silence music, pass talk
#define PROCESSING_MODE_SILENCE_ALL  3 // Silence all audio

// Internal states for detected audio type
#define AUDIO_MODE_NOTHING    0 // Initial or undetermined state
#define AUDIO_MODE_MUSIC      1 // Music is detected
#define AUDIO_MODE_TALK       -1 // Talk is detected

static const char *sign_on = "\n"
" SKIPPER  Selective Audio Detection and Filter (Silence Mod, Refactored, Time-Restricted) Version %.1f\n" // Modified
" Copyright (c) 2024 David Bryant. All Rights Reserved.\n\n"; //

static const char *usage =
" Usage:     SKIPPER [-options] < SourceAudio.pcm > StereoOutput.pcm\n\n"
" Operation: scan source audio (stdin) using tensor discrimination to filter\n"
"            output (stdout), silencing either music (-m) or talk (-t);\n"
"            or output raw scan analytics for use with TENSOR-GEN util (-a)\n\n"
" Options:  -a <file.bin>    = output analysis results to specified file\n" //
"           -c<n>            = override default channel count of 2\n" //
"           -d <file.tensor> = specify alternate discrimination tensor file\n" //
"           -k               = keep-alive crossfading for long silences\n" //
"           -l<n>            = left output override (for debug, n = 0-4:\n" //
"                            = 0=audio, 1=mono, 2=filtered, 3=level, 4=tensor)\n"
"           -m[<n>]          = silence music, with optional threshold offset\n" //
"                            = (raise or lower music threshold +/- 99 points)\n"
"           -n               = output only silence (silence everything)\n" //
"           -p               = pass all audio (no silencing, default)\n" //
"           -q               = no messaging except errors\n" //
"           -r<n>            = right output override (for debug, n = 0-4:\n" //
"                            = 0=audio, 1=mono, 2=filtered, 3=level, 4=tensor)\n"
"           -s<n>            = override default sample rate of 44.1 kHz\n" //
"           -t[<n>]          = silence talk, with optional threshold offset\n" //
"                            = (raise or lower talk threshold +/- 99 points)\n"
"           -T <iso-time>    = stream start time (e.g. HLS PROGRAM-DATE-TIME)\n" //
"           -x               = with -t, enables time-restricted talk silencing (6am-10pm, -2/+5 min around hour/half-hour)\n" // New option
"           -v[<n>]          = set verbosity + [rate in seconds]\n" //
"           -z<+/-HH:MM>     = UTC offset for stream debug time display (e.g. +01:00)\n\n" //
" Web:      Visit www.github.com/dbry/skipper for latest version and info\n\n"; //

// Default audio parameters
#define DEFAULT_CHANNELS        2 //
#define DEFAULT_SAMPLE_RATE     44100 //

// Analysis and processing parameters
#define LEVEL_WINDOW_MS    50     // RMS level calculation window in milliseconds
#define ANALYSIS_WINDOW_SECONDS  5 // Duration of audio window for feature analysis
#define AVERAGING_WINDOW_SECONDS 5 // Duration for averaging tensor results
#define ANALYSIS_STEP_MSECS      200    // How often to perform analysis (ms)
#define AVERAGING_BUFFER_COUNT   (AVERAGING_WINDOW_SECONDS*1000/ANALYSIS_STEP_MSECS) //

// Timing parameters for mode switching and crossfades
#define CROSSFADE_DURATION_SECS  2 //
#define MIN_TALK_DURATION_SECS   10 // Minimum duration to confirm talk mode
#define MIN_MUSIC_DURATION_SECS  20 // Minimum duration to confirm music mode
#define MAX_PENDING_STATE_SECS   60 // Max duration to wait for mode confirmation before cancelling
#define OUTPUT_BUFFER_DURATION_SECS  120 // Max duration of the main output buffer

// Filter parameters
#define LOWPASS_FILTER_FREQ    2000.0 //
#define HIGHPASS_FILTER_FREQ   250.0 //

#define MAX_AUDIO_CYCLES      128 // Max cycles to detect in analysis window for feature extraction

// --- Struct Definitions ---
typedef struct {
    int input_channels;
    int sample_rate;
    int keep_alive_enabled;
    int left_debug_output_mode;
    int right_debug_output_mode;
    int processing_mode; // e.g., PROCESSING_MODE_SILENCE_TALK
    int detection_threshold;
    char *analysis_output_filename;
    char *tensor_input_filename;
    int verbose_level; // 0 for none, otherwise rate in seconds
    int quiet_mode;
    int time_restricted_silence_enabled; // Flag for -x option
    int stream_time_enabled;
    int64_t stream_start_epoch_ms;
    int stream_time_utc_offset_minutes;
    int stream_time_utc_offset_is_set;
} ProgramConfig;

typedef struct {
    int16_t *input_buffer;    // Buffer for raw PCM input
    float   *mono_float_samples; // Buffer for mono, float, filtered samples for analysis
    float   *rms_level_ring_buffer; // Ring buffer for RMS level calculation
    float   *analysis_level_buffer; // Buffer storing levels over the analysis window duration
    int16_t *main_output_buffer;  // Main buffer for audio to be written to stdout
    int16_t *crossfade_buffer;  // Buffer used for crossfading audio segments
    Biquad  lowpass_filters[2]; // Lowpass filters (stereo for potential future use, currently mono applied)
    Biquad  highpass_filters[2]; // Highpass filters
} AudioBuffers;

typedef struct {
    int64_t total_samples_processed;
    int64_t samples_output_audible;
    int64_t samples_output_silenced;
    int64_t current_transition_sample_point; // Sample point where a potential mode change started
    int64_t last_confirmed_sample_point;   // Last sample point for which the mode is stable
    
    int current_audio_mode; // AUDIO_MODE_MUSIC, AUDIO_MODE_TALK, AUDIO_MODE_NOTHING
    int music_confirmation_counter; // Counts steps towards confirming music mode
    int talk_confirmation_counter;  // Counts steps towards confirming talk mode
    int pending_state_counter;      // Counts steps during a pending mode change

    int analysis_level_buffer_idx; // Current write index for analysis_level_buffer
    int main_output_buffer_idx;    // Current write index for main_output_buffer
    int rms_ring_buffer_len;       // Length of the RMS ring buffer
    int analysis_level_buffer_len; // Length of the analysis_level_buffer
    int main_output_buffer_len;    // Length of the main_output_buffer
    int crossfade_buffer_len_samples; // Length of crossfade buffer in samples per channel

    int analysis_step_samples;     // Number of samples per analysis step
    int num_analysis_windows_done;
    int raw_music_hits;
    int raw_talk_hits;

    signed char tensor_results_buffer[AVERAGING_BUFFER_COUNT]; // Stores recent tensor results for averaging
    int tensor_results_buffer_count;

    uint32_t dither_rng_state; // RNG state for dither
    double current_rms_level;  // Current calculated RMS level
    time_t next_debug_wall_clock_report; // Next fallback wall-clock time debug report
    int64_t next_debug_stream_sample_report; // Next stream sample index for debug reporting
} ProgramState;


// --- Global Variables ---
static tensor_array loaded_tensor_data; // Holds the discrimination tensor
static FILE *analysis_binary_output_file; // File for -a option
static int verbose_g; // Global verbose flag, set from ProgramConfig
static int quiet_g;   // Global quiet flag, set from ProgramConfig

// --- Function Prototypes (Refactored Main Logic) ---
static void initialize_program_config(ProgramConfig *config);
static int parse_command_line_arguments(int argc, char **argv, ProgramConfig *config);
static int parse_stream_start_time_option(ProgramConfig *config, const char *time_text);
static int parse_stream_time_utc_offset_option(ProgramConfig *config, const char *offset_text);
static int allocate_audio_buffers(const ProgramConfig *config, AudioBuffers *buffers, ProgramState *state);
static void initialize_audio_filters(const ProgramConfig *config, AudioBuffers *buffers, ProgramState *state);
static void prime_rms_ring_buffer(AudioBuffers *buffers, ProgramState *state);
static void process_audio_stream(ProgramConfig *config, AudioBuffers *buffers, ProgramState *state);
static void print_periodic_debug_time(const ProgramConfig *config, ProgramState *state);
static void process_input_chunk(const ProgramConfig *config, AudioBuffers *buffers, ProgramState *state, int16_t* pcm_input_chunk, int num_input_samples_in_chunk);
static void populate_main_output_buffer_sample(const ProgramConfig *config, AudioBuffers *buffers, ProgramState *state, const int16_t* current_input_sample_frame, float current_filtered_sample);
// Changed ProgramConfig to const ProgramConfig* for the next two prototypes
static void perform_detection_and_handle_transitions(const ProgramConfig *config, AudioBuffers *buffers, ProgramState *state);
static int should_bypass_talk_silencing_due_to_time_restriction(const ProgramConfig *config, const ProgramState *state);
static int is_time_restricted_silence_active_at_sample(const ProgramConfig *config, int64_t sample_index);
static int should_silence_audio_mode_at_sample(const ProgramConfig *config, int audio_mode, int64_t sample_index);
static void write_confirmed_audio_to_stdout(const ProgramConfig *config, AudioBuffers *buffers, ProgramState *state);
static void flush_remaining_audio(ProgramConfig *config, AudioBuffers *buffers, ProgramState *state);
static void print_summary_statistics(const ProgramConfig *config, const ProgramState *state);
static void cleanup_resources(AudioBuffers *buffers);

// --- Original Static Functions (Prototypes) ---
static void fade_out (int16_t *samples, int num_samples_per_channel, int stride); 
static void fade_in (int16_t *samples, int num_samples_per_channel, int stride); 
static int analyze_window (float *levels, long sample_index, int num_samples, int sample_rate); 
static void display_histogram (const char *name, int *histogram, int count); 
static void display_analysis_results (void); 


#define MINS(s,r) ((int)((s)/((r)*60))) 
#define SECS(s,r) ((int)(((s)/(r))%60)) 

int main (int argc, char **argv) {
    ProgramConfig config;
    AudioBuffers buffers;
    ProgramState state = {0}; // Initialize all fields to zero/NULL

    initialize_program_config(&config);

    if (argc == 1) {
        fprintf (stderr, sign_on, VERSION); 
        fprintf (stderr, "%s", usage); 
        return 0;
    }

#ifdef _WIN32
    // Set stdin and stdout to binary mode on Windows
    setmode (fileno (stdout), O_BINARY); 
    setmode (fileno (stdin), O_BINARY); 
#endif

    if (!parse_command_line_arguments(argc, argv, &config)) {
        return 1; // Error in parsing
    }
    
    verbose_g = config.verbose_level; 
    quiet_g = config.quiet_mode;     

    // Load tensor data (either from file or embedded)
    if (config.tensor_input_filename ? 
        !read_tensor_file (loaded_tensor_data, config.tensor_input_filename) : 
        !local_tensor_file (loaded_tensor_data, tensor_4d, sizeof (tensor_4d))) { 
        fprintf (stderr, "\nError: Failed to load tensor data, exiting!\n");
        return 1;
    }

    // Open analysis output file if specified
    if (config.analysis_output_filename) {
        analysis_binary_output_file = fopen (config.analysis_output_filename, "wb"); 
        if (!analysis_binary_output_file) {
            fprintf (stderr, "\nError: Can't open \"%s\" for writing!\n", config.analysis_output_filename); 
            return 1;
        }
    }

    if (!allocate_audio_buffers(&config, &buffers, &state)) {
        if (analysis_binary_output_file) fclose(analysis_binary_output_file);
        return 1; // Error in allocation
    }

    initialize_audio_filters(&config, &buffers, &state);
    prime_rms_ring_buffer(&buffers, &state);

    // Initialize ProgramState members not covered by {0} or allocate_audio_buffers
    state.dither_rng_state = 0x31415926; 
    state.current_audio_mode = AUDIO_MODE_NOTHING; 


    process_audio_stream(&config, &buffers, &state);

    flush_remaining_audio(&config, &buffers, &state);
    print_summary_statistics(&config, &state);
    cleanup_resources(&buffers);

    if (analysis_binary_output_file) {
        fclose (analysis_binary_output_file); 
    }

    return 0;
}

// Initializes the program configuration with default values.
static void initialize_program_config(ProgramConfig *config) {
    config->input_channels = DEFAULT_CHANNELS; 
    config->sample_rate = DEFAULT_SAMPLE_RATE; 
    config->keep_alive_enabled = 0; 
    config->left_debug_output_mode = OUTPUT_AUDIO; 
    config->right_debug_output_mode = OUTPUT_AUDIO; 
    config->processing_mode = PROCESSING_MODE_PASS_ALL; 
    config->detection_threshold = 0; 
    config->analysis_output_filename = NULL; 
    config->tensor_input_filename = NULL; 
    config->verbose_level = 0; 
    config->quiet_mode = 0; 
    config->time_restricted_silence_enabled = 0; // New: Default to disabled
    config->stream_time_enabled = 0;
    config->stream_start_epoch_ms = 0;
    config->stream_time_utc_offset_minutes = 0;
    config->stream_time_utc_offset_is_set = 0;
}

// Parses command-line arguments and populates the ProgramConfig struct.
// Returns 1 on success, 0 on failure.
static int parse_command_line_arguments(int argc, char **argv, ProgramConfig *config) {
    int analysis_output_file_follows = 0; 
    int tensor_input_file_follows = 0; 
    int stream_start_time_follows = 0;
    int stream_time_utc_offset_follows = 0;

    char **current_arg_ptr = argv + 1;
    int arg_count = argc - 1;

    while (arg_count > 0) {
        char *current_arg_str = *current_arg_ptr;
        if (analysis_output_file_follows) {
            config->analysis_output_filename = current_arg_str;
            analysis_output_file_follows = 0;
        } else if (tensor_input_file_follows) {
            config->tensor_input_filename = current_arg_str;
            tensor_input_file_follows = 0;
        } else if (stream_start_time_follows) {
            if (!parse_stream_start_time_option(config, current_arg_str)) return 0;
            stream_start_time_follows = 0;
        } else if (stream_time_utc_offset_follows) {
            if (!parse_stream_time_utc_offset_option(config, current_arg_str)) return 0;
            stream_time_utc_offset_follows = 0;
        }
#if defined (_WIN32)
        else if ((current_arg_str[0] == '-' || current_arg_str[0] == '/') && current_arg_str[1])
#else
        else if ((current_arg_str[0] == '-') && current_arg_str[1])
#endif
        { // Option argument
            char *option_char_ptr = &current_arg_str[1];
            while (*option_char_ptr) {
                char current_option = *option_char_ptr;
                char *next_char_in_option = option_char_ptr + 1; 

                switch (current_option) {
                    case 'a': analysis_output_file_follows = 1; break; 
                    case 'c': 
                        config->input_channels = strtol(next_char_in_option, &option_char_ptr, 10);
                        if (config->input_channels < 1 || config->input_channels > 2) { fprintf(stderr, "\nError: channels must be 1 or 2\n"); return 0; }
                        if (option_char_ptr == next_char_in_option -1 ) option_char_ptr++; 
                        else option_char_ptr--; 
                        break;
                    case 'd': tensor_input_file_follows = 1; break; 
                    case 'k': config->keep_alive_enabled = 1; break; 
                    case 'l': 
                        config->left_debug_output_mode = strtol(next_char_in_option, &option_char_ptr, 10);
                        if (config->left_debug_output_mode < 0 || config->left_debug_output_mode > 4) { fprintf(stderr, "\nError: left output spec must be 0 - 4\n"); return 0; }
                        if (option_char_ptr == next_char_in_option -1) option_char_ptr++; else option_char_ptr--;
                        break;
                    case 'm': // Silence Music
                        if (isdigit(*next_char_in_option) || *next_char_in_option == '-') {
                            config->detection_threshold = strtol(next_char_in_option, &option_char_ptr, 10);
                            option_char_ptr--; 
                        } 
                        if (config->detection_threshold < -99 || config->detection_threshold > 99) { fprintf(stderr, "\nError: music threshold is from -99 to 99\n"); return 0; }
                        config->processing_mode = PROCESSING_MODE_SILENCE_MUSIC;
                        break;
                    case 'n': config->processing_mode = PROCESSING_MODE_SILENCE_ALL; break; 
                    case 'p': config->processing_mode = PROCESSING_MODE_PASS_ALL; break; 
                    case 'q': config->quiet_mode = 1; break; 
                    case 'r': 
                        config->right_debug_output_mode = strtol(next_char_in_option, &option_char_ptr, 10);
                        if (config->right_debug_output_mode < 0 || config->right_debug_output_mode > 4) { fprintf(stderr, "\nError: right output spec must be 0 - 4\n"); return 0; }
                        if (option_char_ptr == next_char_in_option-1) option_char_ptr++; else option_char_ptr--;
                        break;
                    case 's': 
                        config->sample_rate = strtol(next_char_in_option, &option_char_ptr, 10);
                        if (config->sample_rate < 11025 || config->sample_rate > 96000) { fprintf(stderr, "\nError: invalid sample rate (11025-96000 Hz)\n"); return 0; }
                        if (option_char_ptr == next_char_in_option-1) option_char_ptr++; else option_char_ptr--;
                        break;
                    case 't': // Silence Talk
                        if (isdigit(*next_char_in_option) || *next_char_in_option == '-') {
                            config->detection_threshold = -strtol(next_char_in_option, &option_char_ptr, 10);
                             option_char_ptr--;
                        }
                        if (config->detection_threshold < -99 || config->detection_threshold > 99) { fprintf(stderr, "\nError: talk threshold is from -99 to 99\n"); return 0; }
                        config->processing_mode = PROCESSING_MODE_SILENCE_TALK;
                        break;
                    case 'T':
                        if (*next_char_in_option) {
                            if (!parse_stream_start_time_option(config, next_char_in_option)) return 0;
                            option_char_ptr = current_arg_str + strlen(current_arg_str) - 1;
                        } else {
                            stream_start_time_follows = 1;
                        }
                        break;
                    case 'x': // New option for time-restricted silencing
                        config->time_restricted_silence_enabled = 1;
                        break;
                    case 'v': 
                        if (isdigit(*next_char_in_option)) {
                            config->verbose_level = strtol(next_char_in_option, &option_char_ptr, 10);
                             option_char_ptr--;
                        } else {
                            config->verbose_level = 300; // Default verbose rate if no number
                        }
                        break;
                    case 'z':
                        if (*next_char_in_option) {
                            if (!parse_stream_time_utc_offset_option(config, next_char_in_option)) return 0;
                            option_char_ptr = current_arg_str + strlen(current_arg_str) - 1;
                        } else {
                            stream_time_utc_offset_follows = 1;
                        }
                        break;
                    default: fprintf(stderr, "\nIllegal option: %c !\n", *option_char_ptr); return 0;
                }
                option_char_ptr++; 
            }
        } else {
            fprintf(stderr, "\nExtra unknown argument: %s !\n", current_arg_str); 
            return 0;
        }
        current_arg_ptr++;
        arg_count--;
    }
    if (analysis_output_file_follows) { fprintf(stderr, "\nError: -a requires an output filename\n"); return 0; }
    if (tensor_input_file_follows) { fprintf(stderr, "\nError: -d requires a tensor filename\n"); return 0; }
    if (stream_start_time_follows) { fprintf(stderr, "\nError: -T requires an ISO-8601 stream start time\n"); return 0; }
    if (stream_time_utc_offset_follows) { fprintf(stderr, "\nError: -z requires a UTC offset like +01:00\n"); return 0; }
    return 1; // Success
}

static int parse_stream_start_time_option(ProgramConfig *config, const char *time_text) {
    int64_t stream_start_epoch_ms;
    int parsed_utc_offset_minutes;

    if (!parse_iso8601_timestamp_ms(time_text, &stream_start_epoch_ms, &parsed_utc_offset_minutes)) {
        fprintf(stderr, "\nError: invalid -T stream start time \"%s\" (expected ISO-8601, e.g. 2026-06-20T18:38:11.200000Z)\n", time_text);
        return 0;
    }

    config->stream_start_epoch_ms = stream_start_epoch_ms;
    config->stream_time_enabled = 1;
    if (!config->stream_time_utc_offset_is_set) {
        config->stream_time_utc_offset_minutes = parsed_utc_offset_minutes;
    }
    return 1;
}

static int parse_stream_time_utc_offset_option(ProgramConfig *config, const char *offset_text) {
    int utc_offset_minutes;

    if (!parse_utc_offset_minutes(offset_text, &utc_offset_minutes, NULL)) {
        fprintf(stderr, "\nError: invalid -z UTC offset \"%s\" (expected +HH:MM or -HH:MM)\n", offset_text);
        return 0;
    }

    config->stream_time_utc_offset_minutes = utc_offset_minutes;
    config->stream_time_utc_offset_is_set = 1;
    return 1;
}

// Allocates audio buffers based on program configuration.
// Returns 1 on success, 0 on failure.
static int allocate_audio_buffers(const ProgramConfig *config, AudioBuffers *buffers, ProgramState *state) {
    buffers->input_buffer = calloc (config->sample_rate, sizeof (int16_t) * config->input_channels); 
    buffers->mono_float_samples = calloc (config->sample_rate, sizeof (float)); 
    
    state->analysis_step_samples = ANALYSIS_STEP_MSECS * config->sample_rate / 1000; 
    state->rms_ring_buffer_len = (config->sample_rate * LEVEL_WINDOW_MS + 500) / 1000; 
    buffers->rms_level_ring_buffer = calloc (state->rms_ring_buffer_len, sizeof (float)); 
    
    state->analysis_level_buffer_len = ANALYSIS_WINDOW_SECONDS * config->sample_rate; 
    buffers->analysis_level_buffer = calloc (state->analysis_level_buffer_len, sizeof (float)); 
    
    state->main_output_buffer_len = OUTPUT_BUFFER_DURATION_SECS * config->sample_rate; 
    buffers->main_output_buffer = calloc (state->main_output_buffer_len, sizeof (int16_t) * 2);   // Always stereo output
    
    state->crossfade_buffer_len_samples = CROSSFADE_DURATION_SECS * config->sample_rate; 
    buffers->crossfade_buffer = calloc (state->crossfade_buffer_len_samples, sizeof (int16_t) * 2); 

    if (!buffers->input_buffer || !buffers->mono_float_samples || !buffers->rms_level_ring_buffer || 
        !buffers->analysis_level_buffer || !buffers->main_output_buffer || !buffers->crossfade_buffer) {
        fprintf(stderr, "Error: Could not allocate memory for audio buffers.\n");
        cleanup_resources(buffers); // Free any partially allocated buffers
        return 0;
    }
    return 1;
}

// Initializes Biquad audio filters.
static void initialize_audio_filters(const ProgramConfig *config, AudioBuffers *buffers, ProgramState *state) {
    BiquadCoefficients filter_coeffs; 
#ifdef HIGHPASS_FILTER_FREQ
    biquad_highpass (&filter_coeffs, HIGHPASS_FILTER_FREQ / config->sample_rate); 
    biquad_init(&buffers->highpass_filters[0], &filter_coeffs, 1.0); 
    biquad_init(&buffers->highpass_filters[1], &filter_coeffs, 1.0); 
#endif
#ifdef LOWPASS_FILTER_FREQ
    biquad_lowpass (&filter_coeffs, LOWPASS_FILTER_FREQ / config->sample_rate); 
    biquad_init(&buffers->lowpass_filters[0], &filter_coeffs, 1.0); 
    biquad_init(&buffers->lowpass_filters[1], &filter_coeffs, 1.0); 
#endif
    state->dither_rng_state = 0x31415926; // Initialize RNG for dither
    state->current_rms_level = 0.0; 
}

// Primes the RMS ring buffer with dither to avoid initial silence affecting RMS too much.
static void prime_rms_ring_buffer(AudioBuffers *buffers, ProgramState *state) {
    for (int i = 0; i < state->rms_ring_buffer_len; ++i) { 
        state->dither_rng_state = ((state->dither_rng_state << 4) - state->dither_rng_state) ^ 1; 
        buffers->rms_level_ring_buffer[i] = (int32_t)(state->dither_rng_state) >> 26; 
    }
#ifdef HIGHPASS_FILTER_FREQ
    biquad_apply_buffer(&buffers->highpass_filters[0], buffers->rms_level_ring_buffer, state->rms_ring_buffer_len, 1); 
#endif
#ifdef LOWPASS_FILTER_FREQ
    biquad_apply_buffer(&buffers->lowpass_filters[0], buffers->rms_level_ring_buffer, state->rms_ring_buffer_len, 1); 
#endif
}


// Main audio processing function, reads from stdin and processes in chunks.
static void process_audio_stream(ProgramConfig *config, AudioBuffers *buffers, ProgramState *state) {
    int samples_read_this_chunk;
    size_t samples_to_read_per_fread = config->sample_rate; 

    while ((samples_read_this_chunk = fread(buffers->input_buffer, sizeof(int16_t) * config->input_channels, samples_to_read_per_fread, stdin))) { 
        print_periodic_debug_time(config, state);
        process_input_chunk(config, buffers, state, buffers->input_buffer, samples_read_this_chunk);
    }
}

// Periodically prints stream time to stderr when available, otherwise the local wall-clock time.
static void print_periodic_debug_time(const ProgramConfig *config, ProgramState *state) {
    if (config->quiet_mode || config->verbose_level <= 0) {
        return;
    }

    if (config->stream_time_enabled) {
        int64_t report_interval_samples = (int64_t)config->verbose_level * config->sample_rate;
        if (report_interval_samples <= 0) {
            report_interval_samples = config->sample_rate;
        }

        if (state->next_debug_stream_sample_report != 0 && state->total_samples_processed < state->next_debug_stream_sample_report) {
            return;
        }

        // Raw PCM does not preserve HLS wall-clock metadata, so advance the
        // supplied stream start time by the number of decoded samples consumed.
        int64_t stream_epoch_ms = config->stream_start_epoch_ms + state->total_samples_processed * 1000LL / config->sample_rate;
        char formatted_time[40];
        if (!format_epoch_ms_with_utc_offset(stream_epoch_ms, config->stream_time_utc_offset_minutes, formatted_time, sizeof(formatted_time))) {
            return;
        }

        fprintf(stderr, "Debug time: %s\n", formatted_time);
        state->next_debug_stream_sample_report = state->total_samples_processed + report_interval_samples;
        return;
    }

    time_t current_epoch_time;
    time(&current_epoch_time);
    if (current_epoch_time == (time_t)-1) {
        return;
    }

    if (state->next_debug_wall_clock_report != 0 && current_epoch_time < state->next_debug_wall_clock_report) {
        return;
    }

    struct tm *local_time_info = localtime(&current_epoch_time);
    if (!local_time_info) {
        return;
    }

    char formatted_time[32];
    if (strftime(formatted_time, sizeof(formatted_time), "%Y-%m-%d %H:%M:%S %z", local_time_info) == 0) {
        return;
    }

    fprintf(stderr, "Debug time: %s\n", formatted_time);
    state->next_debug_wall_clock_report = current_epoch_time + config->verbose_level;
}

// Processes a single chunk of PCM input samples.
static void process_input_chunk(const ProgramConfig *config, AudioBuffers *buffers, ProgramState *state, int16_t* pcm_input_chunk, int num_input_samples_in_chunk) {
    // Convert input to mono float and apply filters
    for (int i = 0; i < num_input_samples_in_chunk; ++i) {
        // Generate mono float sample with dither
        if (config->input_channels == 2) { 
            buffers->mono_float_samples[i] = ((float)pcm_input_chunk[i * 2] + pcm_input_chunk[i * 2 + 1]) / 2.0f; 
        } else {
            buffers->mono_float_samples[i] = (float)pcm_input_chunk[i]; 
        }
        state->dither_rng_state = ((state->dither_rng_state << 4) - state->dither_rng_state) ^ 1; 
        buffers->mono_float_samples[i] += ((int32_t)state->dither_rng_state) >> 26; 

        // Apply filters (assumes filters operate on the mono_float_samples buffer)
#ifdef HIGHPASS_FILTER_FREQ
        buffers->mono_float_samples[i] = biquad_apply_sample(&buffers->highpass_filters[0], buffers->mono_float_samples[i]); 
#endif
#ifdef LOWPASS_FILTER_FREQ
        buffers->mono_float_samples[i] = biquad_apply_sample(&buffers->lowpass_filters[0], buffers->mono_float_samples[i]); 
#endif
    }

    // Process each sample from the filtered chunk
    for (int i = 0; i < num_input_samples_in_chunk; ++i) {
        // Calculate RMS level
        int rms_ring_idx = state->total_samples_processed % state->rms_ring_buffer_len; 
        if (state->rms_ring_buffer_len > 0) { // Ensure buffer is not zero-length
            if (rms_ring_idx == 0) { // Recalculate sum if wrapped or first sample in buffer segment
                buffers->rms_level_ring_buffer[0] = buffers->mono_float_samples[i]; 
                state->current_rms_level = buffers->mono_float_samples[i] * buffers->mono_float_samples[i]; 
                for (int k = 1; k < state->rms_ring_buffer_len; ++k) { 
                    state->current_rms_level += buffers->rms_level_ring_buffer[k] * buffers->rms_level_ring_buffer[k]; 
                }
            } else { // Update rolling RMS sum
                state->current_rms_level -= buffers->rms_level_ring_buffer[rms_ring_idx] * buffers->rms_level_ring_buffer[rms_ring_idx]; 
                buffers->rms_level_ring_buffer[rms_ring_idx] = buffers->mono_float_samples[i]; 
                state->current_rms_level += buffers->rms_level_ring_buffer[rms_ring_idx] * buffers->rms_level_ring_buffer[rms_ring_idx]; 
            }
            buffers->analysis_level_buffer[state->analysis_level_buffer_idx] = state->current_rms_level / state->rms_ring_buffer_len; 
        } else {
             buffers->analysis_level_buffer[state->analysis_level_buffer_idx] = 0; // Avoid division by zero if buffer length is 0
        }


        // Populate main_output_buffer with original stereo audio (or debug views)
        const int16_t* current_input_sample_frame = pcm_input_chunk + (i * config->input_channels);
        populate_main_output_buffer_sample(config, buffers, state, current_input_sample_frame, buffers->mono_float_samples[i]);
        
        state->analysis_level_buffer_idx++; 
        state->main_output_buffer_idx++;    
        state->total_samples_processed++;   

        // Time to analyze a window?
        if (state->analysis_level_buffer_idx == state->analysis_level_buffer_len) { 
            perform_detection_and_handle_transitions(config, buffers, state);
            
            // Shift analysis_level_buffer
            memmove(buffers->analysis_level_buffer, buffers->analysis_level_buffer + state->analysis_step_samples, 
                    (state->analysis_level_buffer_len - state->analysis_step_samples) * sizeof(float)); 
            state->analysis_level_buffer_idx -= state->analysis_step_samples; 
            state->num_analysis_windows_done++; 
        }
        
        // Write confirmed audio from main_output_buffer to stdout
        write_confirmed_audio_to_stdout(config, buffers, state);
    }
}


// Populates one stereo sample frame in the main_output_buffer.
static void populate_main_output_buffer_sample(const ProgramConfig *config, AudioBuffers *buffers, ProgramState *state,
                                               const int16_t* current_input_sample_frame, float current_filtered_sample) {
    double full_scale_rms_const = 32768.0 * 32767.0 * 0.5; // For OUTPUT_LEVEL debug

    // Left channel output
    if (config->left_debug_output_mode == OUTPUT_AUDIO) { 
        buffers->main_output_buffer[state->main_output_buffer_idx * 2] = current_input_sample_frame[0]; 
    } else if (config->left_debug_output_mode == OUTPUT_MONO) { 
        buffers->main_output_buffer[state->main_output_buffer_idx * 2] = (config->input_channels == 1) ? current_input_sample_frame[0] : (current_input_sample_frame[0] + current_input_sample_frame[1]) >> 1; 
    } else if (config->left_debug_output_mode == OUTPUT_FILTERED) { 
        buffers->main_output_buffer[state->main_output_buffer_idx * 2] = (int16_t)current_filtered_sample; 
    } else if (config->left_debug_output_mode == OUTPUT_LEVEL && state->main_output_buffer_idx >= state->rms_ring_buffer_len / 2) { 
        // Ensure index for level_buffer is valid for this calculation
        int level_buffer_read_idx = state->analysis_level_buffer_idx -1; // current sample's level
        if (level_buffer_read_idx < 0) level_buffer_read_idx = 0; 
        if (buffers->analysis_level_buffer[level_buffer_read_idx] > 0 && full_scale_rms_const > 0) { // Check for valid log input
             buffers->main_output_buffer[(state->main_output_buffer_idx - state->rms_ring_buffer_len / 2) * 2] = 
                floor((log10(buffers->analysis_level_buffer[level_buffer_read_idx] / full_scale_rms_const) + 9.6) * 3413 + 0.5); 
        } else {
             buffers->main_output_buffer[(state->main_output_buffer_idx - state->rms_ring_buffer_len / 2) * 2] = -32768; // Min value for silence in dB
        }
    } else if (config->left_debug_output_mode != OUTPUT_AUDIO) { 
        buffers->main_output_buffer[state->main_output_buffer_idx * 2] = 0; // Default to silence for other debug modes
    }

    // Right channel output
    if (config->right_debug_output_mode == OUTPUT_AUDIO) { 
        buffers->main_output_buffer[state->main_output_buffer_idx * 2 + 1] = (config->input_channels == 1) ? current_input_sample_frame[0] : current_input_sample_frame[config->input_channels - 1]; 
    } else if (config->right_debug_output_mode == OUTPUT_MONO) { 
        buffers->main_output_buffer[state->main_output_buffer_idx * 2 + 1] = (config->input_channels == 1) ? current_input_sample_frame[0] : (current_input_sample_frame[0] + current_input_sample_frame[config->input_channels - 1]) >> 1; 
    } else if (config->right_debug_output_mode == OUTPUT_FILTERED) { 
        buffers->main_output_buffer[state->main_output_buffer_idx * 2 + 1] = (int16_t)current_filtered_sample; 
    } else if (config->right_debug_output_mode == OUTPUT_LEVEL && state->main_output_buffer_idx >= state->rms_ring_buffer_len / 2) { 
        int level_buffer_read_idx = state->analysis_level_buffer_idx -1;
        if (level_buffer_read_idx < 0) level_buffer_read_idx = 0;
         if (buffers->analysis_level_buffer[level_buffer_read_idx] > 0 && full_scale_rms_const > 0) {
            buffers->main_output_buffer[(state->main_output_buffer_idx - state->rms_ring_buffer_len / 2) * 2 + 1] = 
                floor((log10(buffers->analysis_level_buffer[level_buffer_read_idx] / full_scale_rms_const) + 9.6) * 3413 + 0.5); 
        } else {
            buffers->main_output_buffer[(state->main_output_buffer_idx - state->rms_ring_buffer_len / 2) * 2 + 1] = -32768;
        }
    } else if (config->right_debug_output_mode != OUTPUT_AUDIO) {
        buffers->main_output_buffer[state->main_output_buffer_idx * 2 + 1] = 0; 
    }
}


// Performs tensor analysis, makes detection decisions, and handles audio mode transitions.
// Changed ProgramConfig to const ProgramConfig*
static void perform_detection_and_handle_transitions(const ProgramConfig *config, AudioBuffers *buffers, ProgramState *state) {
    // Call analyze_window to get tensor value for the current analysis window
    int tensor_raw_value = analyze_window(buffers->analysis_level_buffer, state->total_samples_processed, state->analysis_level_buffer_len, config->sample_rate); 
    int detected_audio_mode_this_step = AUDIO_MODE_NOTHING; // What this specific analysis step indicates

    // Update raw hit counters
    if (tensor_raw_value > config->detection_threshold) state->raw_music_hits++; 
    else if (tensor_raw_value < config->detection_threshold) state->raw_talk_hits++; 
    
    // Add current tensor result to buffer for averaging
    state->tensor_results_buffer[state->tensor_results_buffer_count++] = tensor_raw_value; 

    // If enough results are gathered, perform averaging and make a mode decision
    if (state->tensor_results_buffer_count == AVERAGING_BUFFER_COUNT) { 
        int averaged_tensor_value = 0;
        for (int i = 0; i < AVERAGING_BUFFER_COUNT; ++i) averaged_tensor_value += state->tensor_results_buffer[i]; 
        
        // Shift the results buffer for the next reading
        memmove(state->tensor_results_buffer, state->tensor_results_buffer + 1, (AVERAGING_BUFFER_COUNT - 1) * sizeof(signed char)); 
        state->tensor_results_buffer_count--; 

        // Debug output for averaged tensor value if selected
        if (config->left_debug_output_mode == OUTPUT_TENSOR || config->right_debug_output_mode == OUTPUT_TENSOR) { 
            int16_t *debug_out_ptr = buffers->main_output_buffer + state->main_output_buffer_idx * 2; 
            // Calculate pointer to where this tensor decision should align in output buffer (approximate middle of analysis period)
            debug_out_ptr -= (ANALYSIS_WINDOW_SECONDS * config->sample_rate / 2) * 2; 
            debug_out_ptr -= (AVERAGING_WINDOW_SECONDS * config->sample_rate / 2) * 2; 
            debug_out_ptr -= (state->analysis_step_samples / 2) * 2; 
            
            if (debug_out_ptr >= buffers->main_output_buffer) { 
                int16_t display_value = (averaged_tensor_value * 100 + AVERAGING_BUFFER_COUNT / 2) / AVERAGING_BUFFER_COUNT; 
                for (int k = 0; k < state->analysis_step_samples; ++k) {
                    if (debug_out_ptr + k * 2 < buffers->main_output_buffer + state->main_output_buffer_len * 2) { 
                       if (config->left_debug_output_mode == OUTPUT_TENSOR) debug_out_ptr[k * 2] = display_value - config->detection_threshold * 100; 
                       if (config->right_debug_output_mode == OUTPUT_TENSOR) debug_out_ptr[k * 2 + 1] = display_value - config->detection_threshold * 100; 
                    }
                }
            }
        }

        // --- Detection Logic (Hysteresis for mode switching) ---
        if (averaged_tensor_value > config->detection_threshold * AVERAGING_BUFFER_COUNT) { // Music indicated by average
            if (state->current_audio_mode == AUDIO_MODE_MUSIC) { // Still music
                if (state->talk_confirmation_counter > 0) { // If was counting towards talk
                    state->talk_confirmation_counter--; // Decrease count towards talk
                     if (state->talk_confirmation_counter > 0 && ++(state->pending_state_counter) >= MAX_PENDING_STATE_SECS * 1000 / ANALYSIS_STEP_MSECS) { 
                        if (config->verbose_level > 0 && !config->quiet_mode) fprintf(stderr, "TALK detection pending for %d secs, cancelled (remains MUSIC)...\n", MAX_PENDING_STATE_SECS); 
                        state->talk_confirmation_counter = 0; state->pending_state_counter = 0; 
                    }
                } else state->pending_state_counter = 0; 
            } else { // Potential transition to music (was talk or nothing)
                if (state->music_confirmation_counter == 0) { // Start of potential music segment
                    state->current_transition_sample_point = state->total_samples_processed - ((ANALYSIS_WINDOW_SECONDS + AVERAGING_WINDOW_SECONDS) * config->sample_rate) / 2; 
                    state->pending_state_counter = 0; 
                }
                state->talk_confirmation_counter = 0; // Reset counter for the other mode
                if (++(state->music_confirmation_counter) >= MIN_MUSIC_DURATION_SECS * 1000 / ANALYSIS_STEP_MSECS) { 
                    detected_audio_mode_this_step = AUDIO_MODE_MUSIC; 
                    state->music_confirmation_counter = 0; state->pending_state_counter = 0; 
                } else {
                    state->pending_state_counter++; 
                }
            }
        } else { // Talk indicated by average
            if (state->current_audio_mode == AUDIO_MODE_TALK) { // Still talk
                if (state->music_confirmation_counter > 0) { // If was counting towards music
                    state->music_confirmation_counter--;
                    if (state->music_confirmation_counter > 0 && ++(state->pending_state_counter) >= MAX_PENDING_STATE_SECS * 1000 / ANALYSIS_STEP_MSECS) { 
                        if (config->verbose_level > 0 && !config->quiet_mode) fprintf(stderr, "MUSIC detection pending for %d secs, cancelled (remains TALK)...\n", MAX_PENDING_STATE_SECS); 
                        state->music_confirmation_counter = 0; state->pending_state_counter = 0; 
                    }
                } else state->pending_state_counter = 0;
            } else { // Potential transition to talk (was music or nothing)
                if (state->talk_confirmation_counter == 0) { // Start of potential talk segment
                    state->current_transition_sample_point = state->total_samples_processed - ((ANALYSIS_WINDOW_SECONDS + AVERAGING_WINDOW_SECONDS) * config->sample_rate) / 2; 
                    state->pending_state_counter = 0; 
                }
                state->music_confirmation_counter = 0; // Reset counter for the other mode
                if (++(state->talk_confirmation_counter) >= MIN_TALK_DURATION_SECS * 1000 / ANALYSIS_STEP_MSECS) { 
                    detected_audio_mode_this_step = AUDIO_MODE_TALK; 
                    state->talk_confirmation_counter = 0; state->pending_state_counter = 0; 
                } else {
                    state->pending_state_counter++; 
                }
            }
        }
        // --- End Detection Logic ---

        // --- Handle Confirmed Transition ---
        if (detected_audio_mode_this_step != AUDIO_MODE_NOTHING && detected_audio_mode_this_step != state->current_audio_mode) {
            // Only perform full crossfade logic if we are actively silencing/passing
            if (config->processing_mode == PROCESSING_MODE_SILENCE_MUSIC || config->processing_mode == PROCESSING_MODE_SILENCE_TALK) {
                int64_t samples_in_output_buffer_before_current_input_chunk = state->total_samples_processed - state->main_output_buffer_idx;
                int transition_offset_in_buffer = (int)(state->current_transition_sample_point - samples_in_output_buffer_before_current_input_chunk);

                int current_mode_is_silenced_type =
                    should_silence_audio_mode_at_sample(config, state->current_audio_mode, state->current_transition_sample_point);
                int target_mode_is_silenced_type =
                    should_silence_audio_mode_at_sample(config, detected_audio_mode_this_step, state->current_transition_sample_point);

                if (current_mode_is_silenced_type == target_mode_is_silenced_type) {
                    // The classification changed, but the time gate leaves the output action unchanged.
                } else if (target_mode_is_silenced_type) { // Transition TO a silenced segment
                    int fade_out_start_idx_in_buffer = transition_offset_in_buffer;

                    if (fade_out_start_idx_in_buffer >= 0 &&
                        fade_out_start_idx_in_buffer + state->crossfade_buffer_len_samples <= state->main_output_buffer_idx) {
                        // Write audible audio up to the actual transition, then fade to silence.
                        fwrite(buffers->main_output_buffer, sizeof(int16_t) * 2, fade_out_start_idx_in_buffer, stdout);
                        state->samples_output_audible += fade_out_start_idx_in_buffer;
                        
                        // Prepare and write the full fade-out span before entering silence.
                        memcpy(buffers->crossfade_buffer, buffers->main_output_buffer + fade_out_start_idx_in_buffer * 2, state->crossfade_buffer_len_samples * sizeof(int16_t) * 2);
                        fade_out(buffers->crossfade_buffer, state->crossfade_buffer_len_samples, 2); 
                        fwrite(buffers->crossfade_buffer, sizeof(int16_t) * 2, state->crossfade_buffer_len_samples, stdout);
                        state->samples_output_audible += state->crossfade_buffer_len_samples; 
                        
                        int samples_written_this_transition = fade_out_start_idx_in_buffer + state->crossfade_buffer_len_samples;
                        memmove(buffers->main_output_buffer, buffers->main_output_buffer + samples_written_this_transition * 2, 
                                (state->main_output_buffer_idx - samples_written_this_transition) * sizeof(int16_t) * 2); 
                        state->main_output_buffer_idx -= samples_written_this_transition; 
                        if (config->verbose_level > 0 && !config->quiet_mode) fprintf(stderr, "Fade OUT to SILENCE: wrote %d, faded %d. Output buffer now %.1fs\n", fade_out_start_idx_in_buffer, state->crossfade_buffer_len_samples, (float)state->main_output_buffer_idx / config->sample_rate);
                    } else { // Crossfade region out of bounds, log warning and switch mode directly
                        if (config->verbose_level > 0 && !config->quiet_mode) fprintf(stderr, "Warning: Fade-out region out of bounds (idx %d, len %d, out_buf_idx %d). Mode switched directly.\n", fade_out_start_idx_in_buffer, state->crossfade_buffer_len_samples, state->main_output_buffer_idx);
                        // Fallback: write any pending confirmed audio without special fade handling for this transition
                        write_confirmed_audio_to_stdout(config, buffers, state); // Ensure buffer is managed
                    }
                } else { // Transition FROM a silenced segment TO an AUDIBLE segment
                    int fade_in_start_idx_in_buffer = transition_offset_in_buffer;

                    if (fade_in_start_idx_in_buffer >= 0 &&
                        fade_in_start_idx_in_buffer + state->crossfade_buffer_len_samples <= state->main_output_buffer_idx) {
                        // Write silence up to the start of the newly audible material.
                        for(int k = 0; k < fade_in_start_idx_in_buffer * 2; ++k) buffers->main_output_buffer[k] = 0;
                        fwrite(buffers->main_output_buffer, sizeof(int16_t) * 2, fade_in_start_idx_in_buffer, stdout);
                        state->samples_output_silenced += fade_in_start_idx_in_buffer;

                        // Fade the new audible segment up from silence.
                        int16_t* new_audible_segment_start = buffers->main_output_buffer + fade_in_start_idx_in_buffer * 2;
                        fade_in(new_audible_segment_start, state->crossfade_buffer_len_samples, 2);

                        fwrite(new_audible_segment_start, sizeof(int16_t) * 2, state->crossfade_buffer_len_samples, stdout);
                        state->samples_output_audible += state->crossfade_buffer_len_samples;

                        int samples_written_this_transition = fade_in_start_idx_in_buffer + state->crossfade_buffer_len_samples;
                        memmove(buffers->main_output_buffer, buffers->main_output_buffer + samples_written_this_transition * 2,
                                (state->main_output_buffer_idx - samples_written_this_transition) * sizeof(int16_t) * 2);
                        state->main_output_buffer_idx -= samples_written_this_transition;
                        if (config->verbose_level > 0 && !config->quiet_mode) fprintf(stderr, "Fade IN from SILENCE: silenced %d, faded %d. Output buffer now %.1fs\n", fade_in_start_idx_in_buffer, state->crossfade_buffer_len_samples, (float)state->main_output_buffer_idx / config->sample_rate);
                        if (!config->quiet_mode) fprintf (stderr, "Crossfade to %s at %i:%i\n", detected_audio_mode_this_step == AUDIO_MODE_MUSIC ? "MUSIC" : "TALK", MINS (state->samples_output_audible + state->samples_output_silenced, config->sample_rate), SECS (state->samples_output_audible + state->samples_output_silenced, config->sample_rate));
                    } else { // Crossfade region out of bounds, log warning and switch mode directly
                        if (config->verbose_level > 0 && !config->quiet_mode) fprintf(stderr, "Warning: Fade-in region out of bounds (idx %d, len %d, out_buf_idx %d). Mode switched directly.\n", fade_in_start_idx_in_buffer, state->crossfade_buffer_len_samples, state->main_output_buffer_idx);
                        // Fallback: write any pending confirmed audio without special fade handling for this transition
                        write_confirmed_audio_to_stdout(config, buffers, state); // Ensure buffer is managed
                    }
                }
            } else if (!config->quiet_mode) { // Not actively silencing (e.g. -p mode), just log detection
                 fprintf (stderr, "%i:%i: detected %s starting at %i:%i\n",
                     MINS (state->total_samples_processed, config->sample_rate), SECS (state->total_samples_processed, config->sample_rate), 
                     detected_audio_mode_this_step == AUDIO_MODE_MUSIC ? "MUSIC" : " TALK",
                     MINS (state->current_transition_sample_point, config->sample_rate), SECS (state->current_transition_sample_point, config->sample_rate)); 
            }
            state->current_audio_mode = detected_audio_mode_this_step; 
        }
        // --- End Handle Confirmed Transition ---

        // Update last_confirmed_sample_point: this is the latest sample for which the mode is considered stable
        if (state->music_confirmation_counter == 0 && state->talk_confirmation_counter == 0 && state->pending_state_counter == 0) { 
            state->last_confirmed_sample_point = state->total_samples_processed - 
                ((ANALYSIS_WINDOW_SECONDS + AVERAGING_WINDOW_SECONDS) * config->sample_rate + state->analysis_step_samples) / 2; 
            // If actively processing and a mode is set, account for half of crossfade buffer as part of transition uncertainty
            if (state->current_audio_mode != AUDIO_MODE_NOTHING && 
                (config->processing_mode == PROCESSING_MODE_SILENCE_MUSIC || config->processing_mode == PROCESSING_MODE_SILENCE_TALK)) {
                 state->last_confirmed_sample_point -= state->crossfade_buffer_len_samples / 2; 
            }
        } else { // If in a pending state, confirmation lags further behind; base it on the potential transition point
            state->last_confirmed_sample_point = state->current_transition_sample_point - state->crossfade_buffer_len_samples / 2; 
        }
         if (state->last_confirmed_sample_point < 0) state->last_confirmed_sample_point = 0; // Ensure it's not negative
    } // End of AVERAGING_BUFFER_COUNT block
}

// Checks if talk silencing should be bypassed due to time restrictions when -t and -x are active.
// Returns 1 if talk silencing SHOULD BE BYPASSED (i.e., time is outside active windows).
// Returns 0 if talk silencing SHOULD BE APPLIED (i.e., -t -x not active, or time is within active windows).
static int should_bypass_talk_silencing_due_to_time_restriction(const ProgramConfig *config, const ProgramState *state) {
    // This function is only relevant if -t (silence talk) and -x (time-restricted) are both set.
    if (!(config->processing_mode == PROCESSING_MODE_SILENCE_TALK && config->time_restricted_silence_enabled)) {
        return 0; // Not in -t -x mode, so no bypassing based on time. Normal -t logic applies.
    }

    return !is_time_restricted_window_active(config->stream_time_enabled,
                                             config->stream_start_epoch_ms,
                                             config->stream_time_utc_offset_minutes,
                                             state->total_samples_processed,
                                             config->sample_rate);
}

static int is_time_restricted_silence_active_at_sample(const ProgramConfig *config, int64_t sample_index) {
    return is_time_restricted_window_active(config->stream_time_enabled,
                                            config->stream_start_epoch_ms,
                                            config->stream_time_utc_offset_minutes,
                                            sample_index,
                                            config->sample_rate);
}

static int should_silence_audio_mode_at_sample(const ProgramConfig *config, int audio_mode, int64_t sample_index) {
    if (config->processing_mode == PROCESSING_MODE_SILENCE_ALL)
        return 1;

    if (config->processing_mode == PROCESSING_MODE_SILENCE_MUSIC)
        return audio_mode == AUDIO_MODE_MUSIC;

    if (config->processing_mode == PROCESSING_MODE_SILENCE_TALK && audio_mode == AUDIO_MODE_TALK) {
        if (config->time_restricted_silence_enabled && !is_time_restricted_silence_active_at_sample(config, sample_index))
            return 0;

        return 1;
    }

    return 0;
}


// Writes confirmed audio segments from the main_output_buffer to stdout.
// Handles silencing and keep-alive logic.
// Changed ProgramConfig to const ProgramConfig*
static void write_confirmed_audio_to_stdout(const ProgramConfig *config, AudioBuffers *buffers, ProgramState *state) {
    int64_t samples_in_output_buffer_before_current_input = state->total_samples_processed - state->main_output_buffer_idx;
    int samples_to_write_now = 0;
    
    if (state->last_confirmed_sample_point > samples_in_output_buffer_before_current_input) {
        samples_to_write_now = (int)(state->last_confirmed_sample_point - samples_in_output_buffer_before_current_input);
    }
    if (samples_to_write_now < 0) samples_to_write_now = 0;
    if (samples_to_write_now > state->main_output_buffer_idx) samples_to_write_now = state->main_output_buffer_idx;

    // Force flush if buffer is full, even if not all samples are "confirmed" by strict definition
    // This prevents deadlock if confirmed_sample isn't advancing but buffer is filling.
    if (state->main_output_buffer_idx == state->main_output_buffer_len && samples_to_write_now < state->main_output_buffer_idx) {
        samples_to_write_now = state->main_output_buffer_idx;
        if (config->verbose_level > 0 && !config->quiet_mode) {
            fprintf(stderr, "Note: Forcing full output buffer flush (%d samples).\n", samples_to_write_now);
        }
    }

    if (samples_to_write_now > 0) {
        int should_pass_audio_audible = 1; // Default: pass audio

        if (config->processing_mode == PROCESSING_MODE_SILENCE_ALL) { 
            should_pass_audio_audible = 0;
        } else if (config->processing_mode == PROCESSING_MODE_SILENCE_TALK) { 
            if (config->time_restricted_silence_enabled && should_bypass_talk_silencing_due_to_time_restriction(config, state)) {
                should_pass_audio_audible = 1; // -t -x active, but time condition bypasses silencing talk
            } else {
                // Normal -t logic (or -t -x active AND time condition met for silencing)
                should_pass_audio_audible = (state->current_audio_mode == AUDIO_MODE_MUSIC); // Pass if music, silence if talk
            }
        } else if (config->processing_mode == PROCESSING_MODE_SILENCE_MUSIC) { 
            // -x option does not affect -m mode as per current request.
            should_pass_audio_audible = (state->current_audio_mode == AUDIO_MODE_TALK); // Pass if talk, silence if music
        }
        // If PROCESSING_MODE_PASS_ALL, should_pass_audio_audible remains 1 (default).

        if (config->keep_alive_enabled && !should_pass_audio_audible && samples_to_write_now > state->crossfade_buffer_len_samples * 2) { 
            // Handle keep-alive during a long silence.
            int part1_len = samples_to_write_now / 2 - state->crossfade_buffer_len_samples; 
            int keepalive_len = state->crossfade_buffer_len_samples;                     
            int part2_len = samples_to_write_now - part1_len - keepalive_len;            

            if (part1_len < 0) { keepalive_len += part1_len; part1_len = 0; }
            if (keepalive_len < 0) { part2_len += keepalive_len; keepalive_len = 0; }
            if (part2_len < 0) { keepalive_len += part2_len; part2_len = 0; } // Ensure keepalive_len is not negative

            // Part 1: Silence before keep-alive
            if (part1_len > 0) {
                for (int k = 0; k < part1_len * 2; ++k) buffers->main_output_buffer[k] = 0;
                fwrite(buffers->main_output_buffer, sizeof(int16_t) * 2, part1_len, stdout);
                state->samples_output_silenced += part1_len;
            }

            // Part 2: Keep-alive blip
            if (keepalive_len > 0) {
                int16_t *keepalive_src_ptr = buffers->main_output_buffer + part1_len * 2;
                int16_t temp_keepalive_blip[keepalive_len * 2]; // Use a temporary buffer for modifications

                for (int k = 0; k < keepalive_len * 2; ++k) temp_keepalive_blip[k] = keepalive_src_ptr[k] >> 2; // Attenuate
                fade_in(temp_keepalive_blip, keepalive_len, 2); 
                fwrite(temp_keepalive_blip, sizeof(int16_t) * 2, keepalive_len, stdout); 
                state->samples_output_audible += keepalive_len; // Keep-alive blip counts as "audible"

                // Update crossfade_buffer with the tail of this keep-alive blip for the *next* transition
                if (keepalive_len >= state->crossfade_buffer_len_samples) {
                    memcpy(buffers->crossfade_buffer, temp_keepalive_blip + (keepalive_len - state->crossfade_buffer_len_samples) * 2, state->crossfade_buffer_len_samples * sizeof(int16_t) * 2); 
                } else { 
                    memcpy(buffers->crossfade_buffer, temp_keepalive_blip, keepalive_len * sizeof(int16_t) * 2);
                    memset(buffers->crossfade_buffer + keepalive_len * 2, 0, (state->crossfade_buffer_len_samples - keepalive_len) * sizeof(int16_t) * 2);
                }
                fade_out(buffers->crossfade_buffer, state->crossfade_buffer_len_samples, 2); 
                if (config->verbose_level > 0 && !config->quiet_mode) fprintf(stderr, "Keep-alive blip (%d samples) during silence at %i:%i\n", keepalive_len, MINS(state->samples_output_audible + state->samples_output_silenced - keepalive_len, config->sample_rate), SECS(state->samples_output_audible + state->samples_output_silenced - keepalive_len, config->sample_rate)); 
            }
            
            // Part 3: Silence after keep-alive
            if (part2_len > 0) {
                int16_t *part2_src_ptr = buffers->main_output_buffer + (part1_len + keepalive_len) * 2;
                for (int k = 0; k < part2_len * 2; ++k) part2_src_ptr[k] = 0;
                fwrite(part2_src_ptr, sizeof(int16_t) * 2, part2_len, stdout);
                state->samples_output_silenced += part2_len;
            }
            
        } else { // Normal write (either pass audio or fully silence it)
            if (!should_pass_audio_audible) { // Silence this segment
                for (int k = 0; k < samples_to_write_now * 2; ++k) buffers->main_output_buffer[k] = 0;
                state->samples_output_silenced += samples_to_write_now;
            } else { // Pass audio as is
                state->samples_output_audible += samples_to_write_now;
                // Prepare crossfade_buffer with the tail of this audible segment for the next transition
                if (samples_to_write_now >= state->crossfade_buffer_len_samples) {
                    memcpy(buffers->crossfade_buffer, buffers->main_output_buffer + (samples_to_write_now - state->crossfade_buffer_len_samples) * 2, state->crossfade_buffer_len_samples * sizeof(int16_t) * 2);
                } else {
                    memcpy(buffers->crossfade_buffer, buffers->main_output_buffer, samples_to_write_now * sizeof(int16_t) * 2);
                    memset(buffers->crossfade_buffer + samples_to_write_now * 2, 0, (state->crossfade_buffer_len_samples - samples_to_write_now) * sizeof(int16_t) * 2);
                }
                fade_out(buffers->crossfade_buffer, state->crossfade_buffer_len_samples, 2); 
            }
            fwrite (buffers->main_output_buffer, sizeof (int16_t) * 2, samples_to_write_now, stdout); 
        }

        // Shift main_output_buffer
        memmove(buffers->main_output_buffer, buffers->main_output_buffer + samples_to_write_now * 2, 
                (state->main_output_buffer_idx - samples_to_write_now) * sizeof(int16_t) * 2); 
        state->main_output_buffer_idx -= samples_to_write_now; 
        
        if (config->verbose_level > 0 && !config->quiet_mode && samples_to_write_now > 0) {
            fprintf(stderr, "%s %d samples (%.1f secs), output_buffer_idx now %d (%.1f secs)\n",
                    should_pass_audio_audible ? "Passed" : "Silenced", samples_to_write_now, (float)samples_to_write_now / config->sample_rate,
                    state->main_output_buffer_idx, (float)state->main_output_buffer_idx / config->sample_rate); 
        }
    }
}


// Flushes any remaining audio in the main_output_buffer at the end of the stream.
static void flush_remaining_audio(ProgramConfig *config, AudioBuffers *buffers, ProgramState *state) {
    if (state->main_output_buffer_idx > 0) { 
        int should_pass_audio_audible = 1; // Default: pass audio

        if (config->processing_mode == PROCESSING_MODE_SILENCE_ALL) { 
            should_pass_audio_audible = 0;
        } else if (config->processing_mode == PROCESSING_MODE_SILENCE_TALK) { 
            if (config->time_restricted_silence_enabled && should_bypass_talk_silencing_due_to_time_restriction(config, state)) {
                should_pass_audio_audible = 1; // Bypassed due to time restriction
            } else {
                should_pass_audio_audible = (state->current_audio_mode == AUDIO_MODE_MUSIC); 
            }
        } else if (config->processing_mode == PROCESSING_MODE_SILENCE_MUSIC) { 
            should_pass_audio_audible = (state->current_audio_mode == AUDIO_MODE_TALK); 
        }
        // If PROCESSING_MODE_PASS_ALL, should_pass_audio_audible remains 1.

        if (!should_pass_audio_audible) { // Silence remaining segment
            for (int k = 0; k < state->main_output_buffer_idx * 2; ++k) buffers->main_output_buffer[k] = 0;
            state->samples_output_silenced += state->main_output_buffer_idx; 
        } else {
            state->samples_output_audible += state->main_output_buffer_idx; 
        }
        fwrite(buffers->main_output_buffer, sizeof(int16_t) * 2, state->main_output_buffer_idx, stdout); 
        if (config->verbose_level > 0 && !config->quiet_mode) fprintf(stderr, "Final flush: %s %d samples (%.1f secs)\n",
            should_pass_audio_audible ? "Passed" : "Silenced", state->main_output_buffer_idx, (float)state->main_output_buffer_idx / config->sample_rate); 
        state->main_output_buffer_idx = 0;
    }
}

// Prints summary statistics at the end of processing.
static void print_summary_statistics(const ProgramConfig *config, const ProgramState *state) {
    if (config->quiet_mode) return; 

    fprintf(stderr, "Total input duration = %i:%i\n", MINS(state->total_samples_processed, config->sample_rate), SECS(state->total_samples_processed, config->sample_rate)); 
    if (config->verbose_level > 0 && !config->quiet_mode) fprintf(stderr, "Total windows analyzed = %d\n", state->num_analysis_windows_done); 
    
    fprintf(stderr, "Raw music hits = %d (%.1f%%), raw talk hits = %d (%.1f%%), unknowns = %d (%.1f%%)\n",
            state->raw_music_hits, state->num_analysis_windows_done ? state->raw_music_hits * 100.0 / state->num_analysis_windows_done : 0.0, 
            state->raw_talk_hits, state->num_analysis_windows_done ? state->raw_talk_hits * 100.0 / state->num_analysis_windows_done : 0.0, 
            state->num_analysis_windows_done - state->raw_music_hits - state->raw_talk_hits, 
            state->num_analysis_windows_done ? (state->num_analysis_windows_done - state->raw_music_hits - state->raw_talk_hits) * 100.0 / state->num_analysis_windows_done : 0.0); 
    
    int64_t total_output_duration_samples = state->samples_output_audible + state->samples_output_silenced;
    fprintf(stderr, "Audio passed audible = %i:%i (%.1f%%), audio silenced = %i:%i (%.1f%%)\n\n",
             MINS(state->samples_output_audible, config->sample_rate), SECS(state->samples_output_audible, config->sample_rate), 
             total_output_duration_samples ? state->samples_output_audible * 100.0 / total_output_duration_samples : 0.0,
             MINS(state->samples_output_silenced, config->sample_rate), SECS(state->samples_output_silenced, config->sample_rate), 
             total_output_duration_samples ? state->samples_output_silenced * 100.0 / total_output_duration_samples : 0.0); 

    if (analysis_binary_output_file) { 
        display_analysis_results(); 
    }
}

// Frees all dynamically allocated audio buffers.
static void cleanup_resources(AudioBuffers *buffers) {
    if (buffers->input_buffer) free(buffers->input_buffer); 
    if (buffers->mono_float_samples) free(buffers->mono_float_samples); 
    if (buffers->rms_level_ring_buffer) free(buffers->rms_level_ring_buffer); 
    if (buffers->analysis_level_buffer) free(buffers->analysis_level_buffer); 
    if (buffers->main_output_buffer) free(buffers->main_output_buffer); 
    if (buffers->crossfade_buffer) free(buffers->crossfade_buffer); 
}


// --- Original Static Functions (Implementations mostly unchanged, minor robustness/logging) ---

// Fade out: applies fade to samples in place
// num_samples_per_channel is the number of samples for ONE channel (e.g., length of the fade)
// stride is 2 for interleaved stereo, 1 for mono
static void fade_out (int16_t *samples, int num_samples_per_channel, int stride) { 
    if (num_samples_per_channel <= 1) return; 
    for (int i = 0; i < num_samples_per_channel; ++i) {
        float multiplier = (float)(num_samples_per_channel - 1 - i) / (num_samples_per_channel - 1); 
        samples[i * stride] = (int16_t)(samples[i * stride] * multiplier); 
        if (stride == 2) samples[i * stride + 1] = (int16_t)(samples[i * stride + 1] * multiplier); 
    }
}

// Fade in: applies fade to samples in place
static void fade_in (int16_t *samples, int num_samples_per_channel, int stride) { 
    if (num_samples_per_channel <= 1) return;
    for (int i = 0; i < num_samples_per_channel; ++i) {
        float multiplier = (float)i / (num_samples_per_channel - 1); 
        samples[i * stride] = (int16_t)(samples[i * stride] * multiplier); 
        if (stride == 2) samples[i * stride + 1] = (int16_t)(samples[i * stride + 1] * multiplier); 
    }
}

static int peak_to_trough_histogram [96] = { 0 }; 
static int cycles_histogram [256] = { 0 }; 
static int low_third_histogram [256] = { 0 }; 
static int mid_third_histogram [256] = { 0 }; 
static int high_third_histogram [256] = { 0 }; 
static int attack_ratio_histogram [256] = { 0 }; 
static int peak_jitter_histogram [256] = { 0 }; 

// Analyzes a window of audio levels to extract features for tensor discrimination.
static int analyze_window (float *levels, long current_total_samples, int num_samples_in_window, int sample_rate_hz) { 
    double full_scale_rms_const = 32768.0 * 32767.0 * 0.5; 
    if (full_scale_rms_const <= 0) full_scale_rms_const = 1.0; 

    float peak_level = levels[0], trough_level = levels[0]; 
    // int peak_pos = 0, trough_pos = 0; // Unused in current logic but kept for consistency
    int content_zones[4] = {0}; // low, mid, high, (spare) 
    int detected_cycles = 0; 
    int cycle_trigger_points[MAX_AUDIO_CYCLES]; 
    struct analysis_result features; 

    for (int i = 1; i < num_samples_in_window; ++i) { 
        if (levels[i] < trough_level) trough_level = levels[i]; 
        if (levels[i] > peak_level) peak_level = levels[i]; 
    }
    if (trough_level <= 1e-9) trough_level = 1e-9; // Avoid log(0) or division by zero later
    if (peak_level < trough_level) peak_level = trough_level;


    double dynamic_range_db = (peak_level > trough_level) ? log10(peak_level / trough_level) * 10.0 : 0.0; 
    double sqrt_peak_trough_ratio = (peak_level > trough_level && trough_level > 0) ? sqrt(peak_level / trough_level) : 1.0; 
    double cbrt_peak_trough_ratio = (peak_level > trough_level && trough_level > 0) ? cbrt(peak_level / trough_level) : 1.0; 

    features.range_dB = (int)floor(dynamic_range_db + 0.5); 
    if (features.range_dB >= 96) features.range_dB = 95; // Clamp to histogram size
    if (features.range_dB < 0) features.range_dB = 0;


    float prev_cycle_peak = levels[0], prev_cycle_trough = levels[0]; 
    int prev_cycle_peak_pos = 0, prev_cycle_trough_pos = 0; 

    for (int i = 1; i < num_samples_in_window; ++i) { 
        int zone_idx;
        if (cbrt_peak_trough_ratio > 1e-9 && peak_level / cbrt_peak_trough_ratio > 1e-9 && levels[i] > peak_level / cbrt_peak_trough_ratio) zone_idx = 2; // High
        else if (cbrt_peak_trough_ratio > 1e-9 && trough_level * cbrt_peak_trough_ratio > 1e-9 && levels[i] > trough_level * cbrt_peak_trough_ratio) zone_idx = 1; // Mid
        else zone_idx = 0; // Low
        content_zones[zone_idx]++; 

        if (detected_cycles & 1) { // Odd: finding peak, trigger on trough
            if (levels[i] > prev_cycle_peak) { prev_cycle_peak = levels[i]; prev_cycle_peak_pos = i; } 
            else if (sqrt_peak_trough_ratio > 1e-9 && prev_cycle_peak > 1e-9 && levels[i] < prev_cycle_peak / sqrt_peak_trough_ratio) { 
                if (detected_cycles < MAX_AUDIO_CYCLES) cycle_trigger_points[detected_cycles++] = prev_cycle_peak_pos; 
                prev_cycle_trough = levels[i]; // Start looking for next trough
            }
        } else { // Even (or initial): finding trough, trigger on peak
            if (levels[i] < prev_cycle_trough) { prev_cycle_trough = levels[i]; prev_cycle_trough_pos = i; } 
            else if (sqrt_peak_trough_ratio > 1e-9 && prev_cycle_trough > 1e-9 && levels[i] > prev_cycle_trough * sqrt_peak_trough_ratio) { 
                if (detected_cycles < MAX_AUDIO_CYCLES) cycle_trigger_points[detected_cycles++] = prev_cycle_trough_pos; 
                prev_cycle_peak = levels[i]; // Start looking for next peak
            }
        }
    }

    double calculated_attack_ratio = 0.5; 
    if (detected_cycles >= 4) { 
        int attack_phase_count = 0, total_attack_time = 0; 
        int decay_phase_count = 0, total_decay_time = 0; 
        for (int i = 2; i < detected_cycles; ++i) { // Start from 2nd transition
            if (cycle_trigger_points[i] < cycle_trigger_points[i-1]) continue; 
            if (i & 1) { /* Peak to Trough = Decay */ total_decay_time += cycle_trigger_points[i] - cycle_trigger_points[i-1]; decay_phase_count++; } 
            else { /* Trough to Peak = Attack */ total_attack_time += cycle_trigger_points[i] - cycle_trigger_points[i-1]; attack_phase_count++; } 
        }
        if (attack_phase_count > 0 && (total_attack_time + total_decay_time > 0)) { 
             calculated_attack_ratio = (double)total_attack_time / (total_attack_time + total_decay_time); 
             if (attack_phase_count != decay_phase_count && attack_phase_count > 0) // Simplified adjustment
                calculated_attack_ratio *= (double) (attack_phase_count + decay_phase_count) / (attack_phase_count * 2.0); 
        } else if (attack_phase_count > 0 && total_attack_time > 0) calculated_attack_ratio = 1.0; 
        else if (decay_phase_count > 0 && total_decay_time > 0) calculated_attack_ratio = 0.0; 
    }


    double calculated_peak_jitter = 1.0; 
    if (detected_cycles >= 6) { 
        int num_detected_peaks = detected_cycles >> 1; 
        if (num_detected_peaks > 1) { 
            int first_peak_idx = (cycle_trigger_points[0] < cycle_trigger_points[1]) ? 1 : 0; 
            if (num_detected_peaks > 1 && (first_peak_idx + (num_detected_peaks-1)*2) < detected_cycles) {
                 double avg_period = (double)(cycle_trigger_points[first_peak_idx + (num_detected_peaks-1)*2] - cycle_trigger_points[first_peak_idx]) / (num_detected_peaks - 1); 
                if (avg_period > 1e-6 && num_detected_peaks > 2) { 
                    double total_error_sum = 0.0; 
                    for (int i = 1; i < num_detected_peaks -1; ++i) { 
                        int current_peak_actual_pos_idx = first_peak_idx + i * 2;
                        if (current_peak_actual_pos_idx >= detected_cycles) break;
                        double predicted_peak_pos = cycle_trigger_points[first_peak_idx] + (avg_period * i); 
                        total_error_sum += fabs(cycle_trigger_points[current_peak_actual_pos_idx] - predicted_peak_pos); 
                    }
                    calculated_peak_jitter = (total_error_sum / (num_detected_peaks - 2)) / avg_period; 
                    if (calculated_peak_jitter > 1.0) calculated_peak_jitter = 1.0; 
                    if (calculated_peak_jitter < 0.0) calculated_peak_jitter = 0.0; 
                }
            }
        }
    }
    
    double low_frac = num_samples_in_window > 0 ? (double)content_zones[0] / num_samples_in_window : 0.0; 
    double mid_frac = num_samples_in_window > 0 ? (double)content_zones[1] / num_samples_in_window : 0.0; 
    double high_frac = num_samples_in_window > 0 ? (double)content_zones[2] / num_samples_in_window : 0.0; 
    low_frac *= (1.0 - low_frac) * 0.75 + 1.0; 
    mid_frac *= (1.0 - mid_frac) * 0.75 + 1.0; 
    high_frac *= (1.0 - high_frac) * 0.75 + 1.0; 

    features.low_third = (int)floor(low_frac * 255.0 + 0.5); 
    features.mid_third = (int)floor(mid_frac * 255.0 + 0.5); 
    features.high_third = (int)floor(high_frac * 255.0 + 0.5); 
    features.attack_ratio = (int)floor(calculated_attack_ratio * 255.0 + 0.5); 
    features.peak_jitter = (int)floor(calculated_peak_jitter * 255.0 + 0.5); 
    features.cycles = (detected_cycles < 256) ? detected_cycles : 255; 
    features.spare = 0; 

    if (verbose_g && ((current_total_samples - num_samples_in_window) % (sample_rate_hz * verbose_g)) == 0) { 
        fprintf(stderr, "%i:%i-%i:%i: level: %5.1f dB - %5.1f dB, peak/trough = %4.1f dB, cycles = %2d, zones = %.3f, %.3f, %.3f, attack = %.3f, jitter = %.3f\n",
                 MINS(current_total_samples - num_samples_in_window, sample_rate_hz), SECS(current_total_samples - num_samples_in_window, sample_rate_hz),
                 MINS(current_total_samples, sample_rate_hz), SECS(current_total_samples, sample_rate_hz),
                 (trough_level > 1e-9) ? log10(trough_level / full_scale_rms_const) * 10.0 : -96.0, 
                 (peak_level > 1e-9) ? log10(peak_level / full_scale_rms_const) * 10.0 : -96.0, 
                 dynamic_range_db, features.cycles,
                 features.low_third / 255.0, features.mid_third / 255.0, features.high_third / 255.0,
                 calculated_attack_ratio, calculated_peak_jitter);
    }

    // Update histograms (using global static arrays)
    peak_to_trough_histogram[features.range_dB]++; 
    cycles_histogram[features.cycles]++; 
    low_third_histogram[features.low_third]++; 
    mid_third_histogram[features.mid_third]++; 
    high_third_histogram[features.high_third]++; 
    if (detected_cycles >= 4) attack_ratio_histogram[features.attack_ratio]++; 
    if (detected_cycles >= 6) peak_jitter_histogram[features.peak_jitter]++; 

    if (analysis_binary_output_file) { 
        fwrite(&features, sizeof(features), 1, analysis_binary_output_file); 
    }
    return *analysis_result_to_tensor_pointer (&features, loaded_tensor_data); // Use global tensor data
}

// Displays analysis histograms (uses global histogram arrays).
static void display_analysis_results (void) { 
    display_histogram ("peak_to_trough", peak_to_trough_histogram, 96); 
    display_histogram ("cycles", cycles_histogram, 256); 
    display_histogram ("lower third", low_third_histogram, 256); 
    display_histogram ("middle third", mid_third_histogram, 256); 
    display_histogram ("upper third", high_third_histogram, 256); 
    display_histogram ("attack ratio", attack_ratio_histogram, 256); 
    display_histogram ("peak jitter", peak_jitter_histogram, 256); 
}

static void display_population (int *histogram, int count, int percent_to_cover); // Forward declaration

// Displays a single histogram's statistics.
static void display_histogram (const char *name, int *histogram, int count) { 
    int min_val = count, max_val = -1, total_hits = 0, value_sum = 0; 
    int median_hits_tracker = 0, max_single_bin_hits = 0, mode_val1 = 0, mode_val2 = 0; 
    double median_val = 0.0; 

    for (int val_idx = 0; val_idx < count; ++val_idx) { 
        if (histogram[val_idx]) {
            if (histogram[val_idx] > max_single_bin_hits) { 
                max_single_bin_hits = histogram[val_idx]; 
                mode_val1 = mode_val2 = val_idx; 
            } else if (histogram[val_idx] == max_single_bin_hits) { 
                mode_val2 = val_idx; 
            }
            if (val_idx < min_val) min_val = val_idx; 
            if (val_idx > max_val) max_val = val_idx; 
            value_sum += histogram[val_idx] * val_idx; 
            total_hits += histogram[val_idx]; 
        }
    }

    if (total_hits > 0) { // Check hits > 0 before calculating median
        for (int val_idx = 0; val_idx < count; ++val_idx) { 
            if (histogram[val_idx]) {
                if (median_hits_tracker + histogram[val_idx] >= total_hits / 2.0) { // Use >= for median calculation robustness
                    if (histogram[val_idx] > 0) median_val = val_idx - 0.5 + (total_hits / 2.0 - median_hits_tracker) / histogram[val_idx]; 
                    else median_val = val_idx; 
                    break;
                } else {
                    median_hits_tracker += histogram[val_idx]; 
                }
            }
        }
        fprintf(stderr, "%s: range = %d to %d, mean = %.2f, median = %.2f, mode = %.1f\n",
                name, min_val == count ? 0 : min_val, max_val, (double)value_sum / total_hits, median_val, (mode_val1 + mode_val2) / 2.0); 
        display_population(histogram, count, 50); display_population(histogram, count, 75); 
        display_population(histogram, count, 90); display_population(histogram, count, 95); 
        display_population(histogram, count, 98); 
    } else {
         fprintf(stderr, "%s: no data points.\n", name); 
    }
}

// Displays population distribution for a histogram.
static void display_population (int *histogram, int count, int percent_to_cover) { 
    int current_low_val = 0, current_high_val = 0, total_sum_of_hits = 0; // Initialize
    for (int val_idx = 0; val_idx < count; ++val_idx) { 
        if (histogram[val_idx]) {
            if (total_sum_of_hits == 0) current_low_val = val_idx; // First bin with hits
            total_sum_of_hits += histogram[val_idx]; 
            current_high_val = val_idx; // Last bin with hits
        }
    }

    if (total_sum_of_hits > 0) { 
        int toggle_trim = 0; 
        int target_hits_to_cover = floor((double)total_sum_of_hits * percent_to_cover / 100.0 + 0.5); 
        int current_covered_hits = total_sum_of_hits; 

        // Trim from ends until target population is reached
        while (current_low_val <= current_high_val && current_covered_hits > target_hits_to_cover) { 
            if (histogram[current_low_val] < histogram[current_high_val] || 
                (histogram[current_low_val] == histogram[current_high_val] && (toggle_trim ^= 1))) { 
                // Try trimming from low end
                if (current_covered_hits - histogram[current_low_val] >= target_hits_to_cover || 
                    (current_covered_hits - histogram[current_low_val] / 2.0 > target_hits_to_cover - 0.01)) { 
                     if (histogram[current_low_val] > 0) current_covered_hits -= histogram[current_low_val++]; else current_low_val++; 
                } else break; 
            } else {
                // Try trimming from high end
                 if (current_covered_hits - histogram[current_high_val] >= target_hits_to_cover ||
                     (current_covered_hits - histogram[current_high_val] / 2.0 > target_hits_to_cover - 0.01)) { 
                    if (histogram[current_high_val] > 0) current_covered_hits -= histogram[current_high_val--]; else current_high_val--; 
                 } else break;
            }
        }
        int final_covered_hits = 0;
        for (int val_idx = current_low_val; val_idx <= current_high_val; ++val_idx) final_covered_hits += histogram[val_idx]; 
        
        fprintf(stderr, "    %d (%.1f%% of total): %d to %d covers approx %d%% of population\n", 
                final_covered_hits, total_sum_of_hits > 0 ? final_covered_hits * 100.0 / total_sum_of_hits : 0.0, 
                current_low_val, current_high_val, percent_to_cover); 
    }
}
