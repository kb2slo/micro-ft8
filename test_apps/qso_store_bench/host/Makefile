# Host build of the QSO store bench: `make`, then ./qso_store_bench_host.
ROOT := ../../..
SRCS := host_main.cpp ../main/bench_core.cpp \
        $(ROOT)/components/qso_store/qso_store.cpp $(ROOT)/components/qso_store/qso_store_fields.cpp \
        $(ROOT)/components/qso_store/qso_store_journal.cpp $(ROOT)/components/qso_store/qso_store_query.cpp \
        $(ROOT)/components/adif/adif.cpp
OPT  ?= -Og
CXXFLAGS := $(OPT) -std=gnu++17 -Wall -I../main -I$(ROOT)/components/qso_store/include -I$(ROOT)/components/adif/include

qso_store_bench_host: $(SRCS) ../main/bench_core.h $(ROOT)/components/qso_store/include/qso_store.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

clean:
	rm -f qso_store_bench_host
