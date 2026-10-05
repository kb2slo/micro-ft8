# Host build of the decode benchmark for the host/device parity check. `make` then
#   ./decode_bench_host ../data/slot_*.raw
ROOT := ../../..
LIB  := $(ROOT)/components/ft8_lib
SRCS := host_main.c ../main/bench_core.c \
        $(LIB)/vendor/ft8/decode.c $(LIB)/vendor/ft8/message.c $(LIB)/vendor/ft8/text.c \
        $(LIB)/vendor/ft8/encode.c $(LIB)/vendor/ft8/constants.c $(LIB)/vendor/ft8/crc.c \
        $(LIB)/vendor/ft8/ldpc.c $(LIB)/vendor/fft/kiss_fft.c $(LIB)/vendor/fft/kiss_fftr.c \
        $(LIB)/common/monitor.c $(LIB)/common/fft_wrapper.c \
        $(ROOT)/components/decode_pipeline/decode_subtract.c
OPT  ?= -O2
CFLAGS := $(OPT) -std=gnu11 -DMONITOR_NFFT_MAX=1920 -I../main -I$(LIB) -I$(LIB)/vendor \
          -I$(ROOT)/components/decode_pipeline/include -Wno-format

decode_bench_host: $(SRCS) ../main/bench_core.h ../main/bench_configs.h \
        $(ROOT)/components/decode_pipeline/include/decode_limits.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

clean:
	rm -f decode_bench_host
