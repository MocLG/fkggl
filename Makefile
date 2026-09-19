CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lm

all: photo

photo: photo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test: photo
	python3 stress_test.py main

test-fd: photo
	$(CC) $(CFLAGS) -DPHOTO_STATIC -c photo.c -o /tmp/photo_lib.o
	$(CC) $(CFLAGS) -I. /tmp/photo_lib.o tests/test_fd_api.c -lm -o /tmp/test_fd_api
	/tmp/test_fd_api

test-scale: photo
	python3 stress_test.py scale

test-all: photo
	python3 stress_test.py all

clean:
	rm -f photo

.PHONY: all test test-fd test-scale test-all clean
