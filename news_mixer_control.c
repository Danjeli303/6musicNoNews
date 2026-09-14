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
#define COMMAND_TIMEOUT_MILLISECONDS 2000

typedef enum {
    EVENT_NONE = 0,
    EVENT_NEWS_ON,
    EVENT_NEWS_OFF,
    EVENT_SCHEDULE_ON,
    EVENT_SCHEDULE_OFF
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
        { "NEWS_EVENT schedule_on sample=", EVENT_SCHEDULE_ON },
        { "NEWS_EVENT schedule_off sample=", EVENT_SCHEDULE_OFF },
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

static void sleep_milliseconds(int milliseconds)
{
    struct timespec delay = {
        milliseconds / 1000,
        (milliseconds % 1000) * 1000000L,
    };

    while (nanosleep(&delay, &delay) && errno == EINTR) {
    }
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

/* Return 1 for a fade to FIP, 0 for a fade to BBC, and -1 for no fade. */
static int transition_for_event(NewsEvent event, int *schedule_active,
                                int *news_active)
{
    if (event == EVENT_SCHEDULE_ON) {
        *schedule_active = 1;
    } else if (event == EVENT_NEWS_ON && !*news_active) {
        *news_active = 1;
        return 1;
    } else if (event == EVENT_SCHEDULE_OFF) {
        *schedule_active = 0;
        if (*news_active) {
            *news_active = 0;
            return 0;
        }
    } else if (event == EVENT_NEWS_OFF && *news_active && !*schedule_active) {
        /* Unscheduled operation has no schedule_off event to close the gate. */
        *news_active = 0;
        return 0;
    }
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

static int fade_mix(void *socket, int news_on, double fip_volume, int fade_milliseconds)
{
    int steps = fade_milliseconds / 50;

    if (steps < 1)
        steps = 1;
    if (steps > 100)
        steps = 100;

    for (int step = 1; step <= steps; ++step) {
        double bbc_level, fip_level;
        struct timespec delay = {
            fade_milliseconds / steps / 1000,
            (fade_milliseconds / steps % 1000) * 1000000L,
        };

        mix_levels((double)step / steps, news_on, fip_volume,
                   &bbc_level, &fip_level);
        if (!send_volume(socket, "bbc", bbc_level) ||
            !send_volume(socket, "fip", fip_level)) {
            fprintf(stderr, "Mixer control command failed: %s\n", zmq_strerror(errno));
            return 0;
        }
        while (nanosleep(&delay, &delay) && errno == EINTR) {
        }
    }
    return 1;
}

static int write_status(const char *path, int news_active, long long sample)
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
                "{\"news_active\":%s,\"sample\":%lld,\"updated_at_unix\":%lld}\n",
                news_active ? "true" : "false", sample, (long long)time(NULL)) < 0) {
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
    int schedule_active = 0;
    long long last_sample = 0;

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
    socket = context ? zmq_socket(context, ZMQ_REQ) : NULL;
    if (!context || !socket) {
        fprintf(stderr, "Could not create mixer control socket.\n");
        return 1;
    }
    {
        int timeout = COMMAND_TIMEOUT_MILLISECONDS;
        int linger = 0;
        int enabled = 1;
        zmq_setsockopt(socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
        zmq_setsockopt(socket, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));
        zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
        zmq_setsockopt(socket, ZMQ_REQ_RELAXED, &enabled, sizeof(enabled));
        zmq_setsockopt(socket, ZMQ_REQ_CORRELATE, &enabled, sizeof(enabled));
    }
    if (zmq_connect(socket, endpoint) != 0) {
        fprintf(stderr, "Could not connect mixer control to %s: %s\n",
                endpoint, zmq_strerror(errno));
        zmq_close(socket);
        zmq_ctx_term(context);
        return 1;
    }

    if (!write_status(status_path, 0, 0))
        fprintf(stderr, "Could not initialize news status file: %s\n", status_path);

    while (fgets(line, sizeof(line), stdin)) {
        ParsedEvent parsed = parse_event(line);
        int transition;
        fputs(line, stderr);
        fflush(stderr);
        if (parsed.event == EVENT_NONE)
            continue;
        last_sample = parsed.sample;
        transition = transition_for_event(parsed.event, &schedule_active,
                                          &news_active);
        if (transition == 1) {
            sleep_milliseconds(parsed.delay_milliseconds);
            write_status(status_path, 1, last_sample);
            fade_mix(socket, 1, fip_volume, fade_out_milliseconds);
        } else if (transition == 0) {
            sleep_milliseconds(parsed.delay_milliseconds);
            fade_mix(socket, 0, fip_volume, fade_in_milliseconds);
            write_status(status_path, 0, last_sample);
        }
    }

    write_status(status_path, 0, last_sample);
    zmq_close(socket);
    zmq_ctx_term(context);
    return 0;
}
