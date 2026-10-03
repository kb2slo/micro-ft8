# Host decoder for the recall harness (RFC 0006 P1).
#   make -C tools/decode_recall
ROOT := ../..
LIB  := $(ROOT)/components/ft8_lib
BENCH := $(ROOT)/test_apps/decode_bench/main
SRCS := decode_recall.c $(BENCH)/bench_core.c \
        $(ROOT)/components/decode_pipeline/decode_subtract.c \
        $(LIB)/vendor/ft8/decode.c $(LIB)/vendor/ft8/message.c $(LIB)/vendor/ft8/text.c \
        $(LIB)/vendor/ft8/encode.c $(LIB)/vendor/ft8/constants.c $(LIB)/vendor/ft8/crc.c \
        $(LIB)/vendor/ft8/ldpc.c $(LIB)/vendor/fft/kiss_fft.c $(LIB)/vendor/fft/kiss_fftr.c \
        $(LIB)/common/monitor.c $(LIB)/common/fft_wrapper.c \
        $(LIB)/vendor/common/wave.c
OPT  ?= -O2
.PHONY: all
all: decode_recall

CFLAGS := $(OPT) -std=gnu11 -DMONITOR_NFFT_MAX=3840 \
          -I$(BENCH) -I$(LIB) -I$(LIB)/vendor \
          -I$(ROOT)/components/decode_pipeline/include -Wno-format

# gen_ft8.c has its own main. Rename that symbol in this one object.
gen_ft8.o: $(LIB)/vendor/demo/gen_ft8.c
	$(CC) $(CFLAGS) -Dmain=gen_ft8_unused_main -c -o $@ $<

decode_recall: $(SRCS) gen_ft8.o $(BENCH)/bench_core.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) gen_ft8.o -lm

clean:
	rm -f decode_recall gen_ft8.o
