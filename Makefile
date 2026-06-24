# Skipper Makefile

CC := gcc

utils := skipper tensor-gen bin2c silencer
tests := skipper_tests silencer_tests

.PHONY: all test audio-test clean

all: $(utils)

skipper_common := skipper_time.c skipper_tensor.c
skipper_common_headers := skipper_time.h skipper_tensor.h

skipper: skipper.c biquad.c lzwlib.c $(skipper_common) skipper.h biquad.h lzwlib.h 4d-tensor.h $(skipper_common_headers)
	$(CC) skipper.c biquad.c lzwlib.c $(skipper_common) -O3 -lm -o skipper

tensor-gen: tensor-gen.c lzwlib.c skipper.h lzwlib.h
	$(CC) tensor-gen.c lzwlib.c -lm -o tensor-gen

bin2c: bin2c.c
	$(CC) bin2c.c lzwlib.c -lm -o bin2c

silencer:  biquad.c lzwlib.c skipper.h biquad.h lzwlib.h 4d-tensor.h $(skipper_common_headers) $(skipper_common) silencer.c
	$(CC) $(CFLAGS) silencer.c biquad.c lzwlib.c $(skipper_common) -O3 -lm -o silencer

skipper_tests: skipper_tests.c skipper.c biquad.c lzwlib.c $(skipper_common) skipper.h biquad.h lzwlib.h 4d-tensor.h $(skipper_common_headers)
	$(CC) $(CFLAGS) skipper_tests.c biquad.c lzwlib.c $(skipper_common) -Wall -Wextra -lm -o skipper_tests

silencer_tests: silencer_tests.c silencer.c biquad.c lzwlib.c $(skipper_common) skipper.h biquad.h lzwlib.h 4d-tensor.h $(skipper_common_headers)
	$(CC) $(CFLAGS) silencer_tests.c biquad.c lzwlib.c $(skipper_common) -Wall -Wextra -lm -o silencer_tests

test: $(tests)
	./skipper_tests
	./silencer_tests

audio-test: skipper silencer
	./audio_validation_tests.sh

clean:
	rm -f skipper tensor-gen bin2c silencer $(tests)
