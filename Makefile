CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -shared -fPIC -Wall -Wextra

recorder.so: recorder/recorder.c
	$(CC) $(CFLAGS) -o $@ $< -ldl -lpthread

test: recorder.so
	tests/test_recorder.sh
	tests/test_replay.sh

clean:
	rm -f recorder.so pulltheplug.trace pulltheplug.trace.data

demo: recorder.so
	rm -f pulltheplug.trace pulltheplug.trace.data
	LD_PRELOAD=$(CURDIR)/recorder.so python3 examples/checkout/app.py
	-python3 replay.py pulltheplug.trace --invariants examples/checkout/invariants.sql

.PHONY: test demo clean
