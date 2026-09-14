#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <zmq.h>

#define DEFAULT_ENDPOINT "tcp://127.0.0.1:5555"
#define DEFAULT_FIP_VOLUME 0.85
#define DEFAULT_FADE_MILLISECONDS 1200
#define COMMAND_TIMEOUT_MILLISECONDS 10000

typedef enum {
    EVENT_NONE = 0,
    EVENT_NEWS_ON,
    EVENT_NEWS_OFF
} NewsEvent;

typedef struct {
    NewsEvent event;
    long long sample;
    int delay_milliseconds;
} ParsedEvent;

static ParsedEvent parse_event(const char *line)
{
    static const struct {
        const char *prefix;
        NewsEvent event;
    } events[] = {
        { "NEWS_EVENT news_on sample=", EVENT_NEWS_ON },
        { "NEWS_EVENT news_off sample=", EVENT_NEWS_OFF },
    };
    ParsedEvent parsed = { EVENT_NONE, 0, 0 };

    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        size_t prefix_length = strlen(events[i].prefix);
        char *end = NULL;
        long long sample;

        if (strncmp(line, events[i].prefix, prefix_length) != 0)
            continue;
        errno = 0;
        sample = strtoll(line + prefix_length, &end, 10);
        if (errno || end == line + prefix_length || sample < 0)
            return parsed;
        if (strncmp(end, " delay_ms=", 10) == 0) {
            char *delay_end = NULL;
            long delay;
            errno = 0;
            delay = strtol(end + 10, &delay_end, 10);
            if (errno || delay_end == end + 10 ||
                (*delay_end != '\0' && *delay_end != '\n') ||
                delay < 0 || delay > 120000)
                return parsed;
            parsed.delay_milliseconds = (int)delay;
        } else if (*end != '\0' && *end != '\n') {
            return parsed;
        }
        parsed.event = events[i].event;
        parsed.sample = sample;
        return parsed;
    }
    return parsed;
}

static void mix_levels(double portion, int news_on, double fip_volume,
                       double *bbc_level, double *fip_level)
{
    if (portion < 0.0)
        portion = 0.0;
    if (portion > 1.0)
        portion = 1.0;
    *bbc_level = news_on ? 1.0 - portion : portion;
    *fip_level = news_on ? fip_volume * portion : fip_volume * (1.0 - portion);
}

static void interpolate_levels(double portion,
                               double start_bbc_level, double start_fip_level,
                               double target_bbc_level, double target_fip_level,
                               double *bbc_level, double *fip_level)
{
    if (portion < 0.0)
        portion = 0.0;
    if (portion > 1.0)
        portion = 1.0;
    *bbc_level = start_bbc_level +
                 (target_bbc_level - start_bbc_level) * portion;
    *fip_level = start_fip_level +
                 (target_fip_level - start_fip_level) * portion;
}

/* The latest news event is the complete and authoritative station state. */
static int state_for_event(NewsEvent event)
{
    if (event == EVENT_NEWS_ON)
        return 1;
    if (event == EVENT_NEWS_OFF)
        return 0;
    return -1;
}

static int send_volume(void *socket, const char *target, double volume)
{
    char command[96];
    char reply[128];
    int command_length = snprintf(command, sizeof(command),
                                  "volume@%s volume %.6f", target, volume);
    int received;

    if (command_length < 0 || (size_t)command_length >= sizeof(command))
        return 0;
    if (zmq_send(socket, command, (size_t)command_length, 0) < 0)
        return 0;
    received = (int)zmq_recv(socket, reply, sizeof(reply) - 1, 0);
    if (received < 0)
        return 0;
    reply[received] = '\0';
    return reply[0] == '0';
}

static int fade_mix(void *socket, int news_on, double fip_volume,
                    int fade_milliseconds,
                    double *current_bbc_level, double *current_fip_level)
{
    const double start_bbc_level = *current_bbc_level;
    const double start_fip_level = *current_fip_level;
    const double target_bbc_level = news_on ? 0.0 : 1.0;
    const double target_fip_level = news_on ? fip_volume : 0.0;
    int steps = fade_milliseconds / 100;

    if (steps < 1)
        steps = 1;
    if (steps > 20)
        steps = 20;

    for (int step = 1; step <= steps; ++step) {
        double bbc_level, fip_level;

        interpolate_levels((double)step / steps,
                           start_bbc_level, start_fip_level,
                           target_bbc_level, target_fip_level,
                           &bbc_level, &fip_level);
        if (!send_volume(socket, "bbc", bbc_level)) {
            fprintf(stderr, "Mixer control command failed: %s\n", zmq_strerror(errno));
            return 0;
        }
        *current_bbc_level = bbc_level;
        if (!send_volume(socket, "fip", fip_level)) {
            fprintf(stderr, "Mixer control command failed: %s\n", zmq_strerror(errno));
            return 0;
        }
        *current_fip_level = fip_level;
    }
    return 1;
}

static int write_status(const char *path, int news_active, long long sample,
                        int transitioning)
{
    char temporary_path[4096];
    FILE *file;

    if (snprintf(temporary_path, sizeof(temporary_path), "%s.tmp.%ld",
                 path, (long)getpid()) >= (int)sizeof(temporary_path))
        return 0;
    file = fopen(temporary_path, "w");
    if (!file)
        return 0;
    if (fprintf(file,
                "{\"news_active\":%s,\"transitioning\":%s,\"sample\":%lld,"
                "\"updated_at_unix\":%lld}\n",
                news_active ? "true" : "false",
                transitioning ? "true" : "false",
                sample, (long long)time(NULL)) < 0) {
        fclose(file);
        remove(temporary_path);
        return 0;
    }
    if (fclose(file) != 0) {
        remove(temporary_path);
        return 0;
    }
    if (rename(temporary_path, path) != 0) {
        remove(temporary_path);
        return 0;
    }
    return 1;
}

static void *connect_control_socket(void *context, const char *endpoint)
{
    void *socket = zmq_socket(context, ZMQ_REQ);
    int timeout = COMMAND_TIMEOUT_MILLISECONDS;
    int linger = 0;

    if (!socket)
        return NULL;
    zmq_setsockopt(socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(socket, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
    if (zmq_connect(socket, endpoint) == 0)
        return socket;
    zmq_close(socket);
    return NULL;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s <status-file> [endpoint] [fip-volume] [fade-out-ms] [fade-in-ms]\n",
            program);
}

int main(int argc, char **argv)
{
    const char *status_path;
    const char *endpoint = DEFAULT_ENDPOINT;
    double fip_volume = DEFAULT_FIP_VOLUME;
    int fade_out_milliseconds = DEFAULT_FADE_MILLISECONDS;
    int fade_in_milliseconds = DEFAULT_FADE_MILLISECONDS;
    void *context;
    void *socket;
    char line[1024];
    int news_active = 0;
    long long last_sample = 0;
    double current_bbc_level = 1.0;
    double current_fip_level = 0.0;

    if (argc < 2 || argc > 6) {
        usage(argv[0]);
        return 1;
    }
    status_path = argv[1];
    if (argc >= 3)
        endpoint = argv[2];
    if (argc >= 4)
        fip_volume = strtod(argv[3], NULL);
    if (argc >= 5)
        fade_out_milliseconds = (int)strtol(argv[4], NULL, 10);
    if (argc >= 6)
        fade_in_milliseconds = (int)strtol(argv[5], NULL, 10);
    if (fip_volume < 0.0 || fip_volume > 4.0 ||
        fade_out_milliseconds < 0 || fade_out_milliseconds > 10000 ||
        fade_in_milliseconds < 0 || fade_in_milliseconds > 10000) {
        usage(argv[0]);
        return 1;
    }

    context = zmq_ctx_new();
    socket = context ? connect_control_socket(context, endpoint) : NULL;
    if (!context || !socket) {
        fprintf(stderr, "Could not connect mixer control to %s: %s\n",
                endpoint, zmq_strerror(errno));
        if (socket)
            zmq_close(socket);
        if (context)
            zmq_ctx_term(context);
        return 1;
    }

    if (!write_status(status_path, 0, 0, 0))
        fprintf(stderr, "Could not initialize news status file: %s\n", status_path);

    while (fgets(line, sizeof(line), stdin)) {
        ParsedEvent parsed = parse_event(line);
        int target_state;
        int fade_succeeded;
        fputs(line, stderr);
        fflush(stderr);
        if (parsed.event == EVENT_NONE)
            continue;
        last_sample = parsed.sample;
        target_state = state_for_event(parsed.event);
        news_active = target_state;
        write_status(status_path, news_active, last_sample, 1);
        if (!socket)
            socket = connect_control_socket(context, endpoint);
        fade_succeeded = socket && fade_mix(
            socket, news_active, fip_volume,
            news_active ? fade_out_milliseconds : fade_in_milliseconds,
            &current_bbc_level, &current_fip_level);
        if (!fade_succeeded) {
            if (socket)
                zmq_close(socket);
            socket = connect_control_socket(context, endpoint);
            if (socket) {
                fade_succeeded = fade_mix(
                    socket, news_active, fip_volume,
                    news_active ? fade_out_milliseconds : fade_in_milliseconds,
                    &current_bbc_level, &current_fip_level);
            }
        }
        write_status(status_path, news_active, last_sample, !fade_succeeded);
    }

    write_status(status_path, 0, last_sample, 0);
    if (socket)
        zmq_close(socket);
    zmq_ctx_term(context);
    return 0;
}
