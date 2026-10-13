CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -shared -fPIC -Wall -Wextra

recorder.so: recorder/recorder.c
	$(CC) $(CFLAGS) -o $@ $< -ldl -lpthread

test: recorder.so
	tests/test_recorder.sh

clean:
	rm -f recorder.so pulltheplug.trace pulltheplug.trace.data

.PHONY: test clean
