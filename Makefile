# Skipper Makefile

CC := gcc
# Fastest in the news-window native profile benchmark; override if needed.
OPTFLAGS ?= -Ofast -flto

programs := skipper news_identifier news_mixer_control
tools := tensor-gen bin2c
tests := skipper_tests news_identifier_tests news_mixer_control_tests

.PHONY: all tools test audio-test web-test sample-recording-test clean

all: $(programs)

tools: $(tools)

skipper_common := skipper_time.c skipper_tensor.c
skipper_common_headers := skipper_time.h skipper_tensor.h

skipper: skipper.c biquad.c lzwlib.c $(skipper_common) skipper.h biquad.h lzwlib.h 4d-tensor.h $(skipper_common_headers)
	$(CC) $(CFLAGS) skipper.c biquad.c lzwlib.c $(skipper_common) $(OPTFLAGS) -lm -o skipper

tensor-gen: tensor-gen.c lzwlib.c skipper.h lzwlib.h
	$(CC) tensor-gen.c lzwlib.c -lm -o tensor-gen

bin2c: bin2c.c
	$(CC) bin2c.c lzwlib.c -lm -o bin2c

news_identifier: biquad.c lzwlib.c skipper.h biquad.h lzwlib.h 4d-tensor.h $(skipper_common_headers) $(skipper_common) NewsIdentifier.c
	$(CC) $(CFLAGS) NewsIdentifier.c biquad.c lzwlib.c $(skipper_common) $(OPTFLAGS) -lm -o news_identifier

ZMQ_CFLAGS ?= $(shell if command -v pkg-config >/dev/null 2>&1; then pkg-config --cflags libzmq; elif command -v brew >/dev/null 2>&1; then printf -- '-I%s/include' "$$(brew --prefix zeromq)"; fi)
ZMQ_LIBS ?= $(shell if command -v pkg-config >/dev/null 2>&1; then pkg-config --libs libzmq; elif command -v brew >/dev/null 2>&1; then printf -- '-L%s/lib -lzmq' "$$(brew --prefix zeromq)"; else printf -- '-lzmq'; fi)

news_mixer_control: news_mixer_control.c
	$(CC) $(CFLAGS) $(ZMQ_CFLAGS) news_mixer_control.c $(OPTFLAGS) $(ZMQ_LIBS) -o news_mixer_control

skipper_tests: skipper_tests.c skipper.c biquad.c lzwlib.c $(skipper_common) skipper.h biquad.h lzwlib.h 4d-tensor.h $(skipper_common_headers)
	$(CC) $(CFLAGS) skipper_tests.c biquad.c lzwlib.c $(skipper_common) -Wall -Wextra -lm -o skipper_tests

news_identifier_tests: news_identifier_tests.c NewsIdentifier.c biquad.c lzwlib.c $(skipper_common) skipper.h biquad.h lzwlib.h 4d-tensor.h $(skipper_common_headers)
	$(CC) $(CFLAGS) news_identifier_tests.c biquad.c lzwlib.c $(skipper_common) -Wall -Wextra -lm -o news_identifier_tests

news_mixer_control_tests: news_mixer_control_tests.c news_mixer_control.c
	$(CC) $(CFLAGS) $(ZMQ_CFLAGS) news_mixer_control_tests.c -Wall -Wextra $(ZMQ_LIBS) -o news_mixer_control_tests

test: $(tests)
	./skipper_tests
	./news_identifier_tests
	./news_mixer_control_tests
	python3 -m unittest tests/test_skip_news_web.py

web-test:
	python3 -m unittest tests/test_skip_news_web.py

audio-test: skipper news_identifier news_mixer_control
	./audio_validation_tests.sh

sample-recording-test: skipper
	./sample_recording_tests.sh

clean:
	rm -f $(programs) $(tools) $(tests)
