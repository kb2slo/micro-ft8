# Offline I/Q front end: components/iq_frontend over a recording, and synthetic I/Q.
#   make -C tools/iq_convert
ROOT := ../..
LIB  := $(ROOT)/components/ft8_lib
SRCS := iq_convert.c $(ROOT)/components/iq_frontend/iq_frontend.c $(LIB)/vendor/fft/kiss_fft.c
OPT  ?= -O2
.PHONY: all clean
all: iq_convert

CFLAGS := $(OPT) -std=gnu11 -Wall -I$(ROOT)/components/iq_frontend/include -I$(LIB)/vendor

iq_convert: $(SRCS) $(ROOT)/components/iq_frontend/include/iq_frontend.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

clean:
	rm -f iq_convert
