CXX      = g++
CC       = gcc
CXXFLAGS = -DHOST_MOCK -std=c++17 -g -Wall -Wno-unused-variable
CFLAGS   = -DHOST_MOCK -g -Wall

# host_mock first so mock esp_log.h shadows the real one
INCLUDES = -I. \
           -I../components/station/include \
           -I../components/qso_log/include \
           -I../components/decode_types/include \
           -I../components/radio_control/include \
           -I../components/autoseq/include \
           -I../components/decode_tx_state/include \
           -I../components/callsign_hashtable/include \
           -I../components/protocol/include \
           -I../components/debug_log/include \
           -I../components/adif/include \
           -I../components/storage_service/include \
           -I../components/rtc_core/include \
           -I../components/decode_pipeline/include \
           -I../components/dial_history/include \
           -I../components/swr_sweep/include \
           -I../components/ft8_lib -I../components/ft8_lib/vendor \
           -I../components/ft8_lib/vendor/ft8 \
           -I../components/porta_proto/include

CORE_SRC   = ../components/autoseq/autoseq.cpp
MOCK_SRC   = host_main.cpp host_mocks.cpp json_parser.cpp scenario.cpp
# FT8 encode sources (uncomment to link real encoder):
# FT8_SRC  = ../components/ft8_lib/vendor/ft8/encode.c ../components/ft8_lib/vendor/ft8/message.c \
#            ../components/ft8_lib/vendor/ft8/crc.c ../components/ft8_lib/vendor/ft8/text.c

ALL_CXX_SRC = $(CORE_SRC) $(MOCK_SRC)
ALL_C_SRC   = $(FT8_SRC)

CXX_OBJ = $(ALL_CXX_SRC:.cpp=.o)
C_OBJ   = $(ALL_C_SRC:.c=.o)
OBJ     = $(CXX_OBJ) $(C_OBJ)

TARGET = host_test
SIM_TARGET = sim_station
UNIQUE_TARGET = host_test_unique_callsign
CQ_TRIES_TARGET = host_test_cq_tries
ADIF_TARGET = host_test_adif_merge
STATION_TARGET = host_test_station
DECODE_SORT_TARGET = host_test_decode_sort
QSO_BROWSE_TARGET = host_test_qso_browse
ADIF_DELETE_TARGET = host_test_adif_delete
FILE_LIST_TARGET = host_test_file_list
PORTA_PROTO_TARGET = host_test_porta_proto
PAIRING_TOKEN_TARGET = host_test_pairing_token
WEB_PAGE_TARGET = host_test_web_page
WEB_MANIFEST_TARGET = host_test_web_manifest
BUNDLE_HOST_TARGET = host_test_bundle_host
RECENT_QSO_TARGET = host_test_recent_qso
QUEUE_HEAD_TARGET = host_test_queue_head
ROGERS_ONCE_TARGET = host_test_rogers_once
DIAL_HISTORY_TARGET = host_test_dial_history
ADIF_LOGGER_TARGET = host_test_adif_logger
CAT_REPLY_PARSER_TARGET = host_test_cat_reply_parser
SWR_SWEEP_TARGET = host_test_swr_sweep
SLOT_DEFICIT_TARGET = host_test_slot_deficit
SAMPLE_RING_TARGET = host_test_sample_ring
CORE_BUSY_TARGET = host_test_core_busy
DECODE_RETRY_TARGET = host_test_decode_retry
DECODE_SYNC_TARGET = host_test_decode_sync
DECODE_SNR_TARGET = host_test_decode_snr
PORTA_OUT_QUEUE_TARGET = host_test_porta_out_queue
HEARD_GRID_TARGET = host_test_heard_grid
DECODE_FIELDS_TARGET = host_test_decode_fields
RTC_CORE_TARGET = host_test_rtc_core

all: $(TARGET) $(UNIQUE_TARGET) $(CQ_TRIES_TARGET) $(ADIF_TARGET) $(STATION_TARGET) \
     $(DECODE_SORT_TARGET) $(QSO_BROWSE_TARGET) $(ADIF_DELETE_TARGET) \
     $(FILE_LIST_TARGET) \
     $(PORTA_PROTO_TARGET) $(PAIRING_TOKEN_TARGET) \
     $(WEB_PAGE_TARGET) $(WEB_MANIFEST_TARGET) $(BUNDLE_HOST_TARGET) \
     $(RECENT_QSO_TARGET) $(QUEUE_HEAD_TARGET) $(ROGERS_ONCE_TARGET) $(DIAL_HISTORY_TARGET) $(ADIF_LOGGER_TARGET) \
     $(CAT_REPLY_PARSER_TARGET) $(SWR_SWEEP_TARGET) \
     $(SLOT_DEFICIT_TARGET) $(SAMPLE_RING_TARGET) $(CORE_BUSY_TARGET) \
     $(DECODE_RETRY_TARGET) $(DECODE_SYNC_TARGET) $(DECODE_SNR_TARGET) \
     $(PORTA_OUT_QUEUE_TARGET) $(HEARD_GRID_TARGET) $(DECODE_FIELDS_TARGET) $(RTC_CORE_TARGET) \
     $(SIM_TARGET)

$(TARGET): $(OBJ) porta_event_json.o heard_grid.o
	$(CXX) $(CXXFLAGS) -o $@ $^

porta_event_json.o: ../components/porta_proto/porta_event_json.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

SIM_OBJ = sim_station.o scenario.o host_mocks.o json_parser.o \
          ../components/autoseq/autoseq.o porta_event_json.o swr_sweep.o heard_grid.o
$(SIM_TARGET): $(SIM_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

UNIQUE_OBJ = ../components/autoseq/autoseq.o host_mocks.o test_unique_callsign.o
$(UNIQUE_TARGET): $(UNIQUE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

CQ_TRIES_OBJ = ../components/autoseq/autoseq.o host_mocks.o test_cq_tries.o
$(CQ_TRIES_TARGET): $(CQ_TRIES_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

RECENT_QSO_OBJ = ../components/autoseq/autoseq.o host_mocks.o test_recent_qso.o
$(RECENT_QSO_TARGET): $(RECENT_QSO_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

QUEUE_HEAD_OBJ = ../components/autoseq/autoseq.o host_mocks.o test_queue_head.o
HEARD_GRID_OBJ = ../components/autoseq/autoseq.o heard_grid.o decode_fields.o host_mocks.o test_heard_grid.o
heard_grid.o: ../components/decode_pipeline/heard_grid.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(HEARD_GRID_TARGET): $(HEARD_GRID_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

DECODE_FIELDS_OBJ = decode_fields.o test_decode_fields.o
decode_fields.o: ../components/decode_pipeline/decode_fields.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(DECODE_FIELDS_TARGET): $(DECODE_FIELDS_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

ROGERS_ONCE_OBJ = ../components/autoseq/autoseq.o host_mocks.o test_rogers_once.o
$(ROGERS_ONCE_TARGET): $(ROGERS_ONCE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

# settimeofday is renamed so the test never sets this machine's clock; the
# test defines rtc_test_settimeofday.
RTC_CORE_OBJ = rtc_core.o test_rtc_core.o
rtc_core.o: ../components/rtc_core/rtc_core.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Dsettimeofday=rtc_test_settimeofday -c $< -o $@
$(RTC_CORE_TARGET): $(RTC_CORE_OBJ)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $^

DIAL_HISTORY_OBJ = dial_history.o test_dial_history.o
dial_history.o: ../components/dial_history/dial_history.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(DIAL_HISTORY_TARGET): $(DIAL_HISTORY_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

CAT_REPLY_PARSER_OBJ = cat_reply_parser.o test_cat_reply_parser.o
cat_reply_parser.o: ../components/radio_control/cat_reply_parser.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(CAT_REPLY_PARSER_TARGET): $(CAT_REPLY_PARSER_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

SWR_SWEEP_OBJ = swr_sweep.o test_swr_sweep.o
swr_sweep.o: ../components/swr_sweep/swr_sweep.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(SWR_SWEEP_TARGET): $(SWR_SWEEP_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

SLOT_DEFICIT_OBJ = ft8_slot_deficit.o test_slot_deficit.o
ft8_slot_deficit.o: ../components/ft8_audio_pipeline/ft8_slot_deficit.c
	$(CC) $(CFLAGS) -I../components/ft8_audio_pipeline/include -c $< -o $@
test_slot_deficit.o: test_slot_deficit.cpp
	$(CXX) $(CXXFLAGS) -I../components/ft8_audio_pipeline/include -c $< -o $@
$(SLOT_DEFICIT_TARGET): $(SLOT_DEFICIT_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

SAMPLE_RING_OBJ = ft8_sample_ring.o test_sample_ring.o
ft8_sample_ring.o: ../components/ft8_audio_pipeline/ft8_sample_ring.c
	$(CC) $(CFLAGS) -I../components/ft8_audio_pipeline/include -c $< -o $@
test_sample_ring.o: test_sample_ring.cpp
	$(CXX) $(CXXFLAGS) -I../components/ft8_audio_pipeline/include -c $< -o $@
$(SAMPLE_RING_TARGET): $(SAMPLE_RING_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

CORE_BUSY_OBJ = decode_core_busy.o test_core_busy.o
decode_core_busy.o: ../components/decode_pipeline/decode_core_busy.c
	$(CC) $(CFLAGS) -I../components/decode_pipeline/include -c $< -o $@
test_core_busy.o: test_core_busy.cpp
	$(CXX) $(CXXFLAGS) -I../components/decode_pipeline/include -c $< -o $@
$(CORE_BUSY_TARGET): $(CORE_BUSY_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

DECODE_RETRY_OBJ = decode_retry.o test_decode_retry.o
DECODE_RETRY_INC = -I../components/decode_pipeline/include -I../components/ft8_lib/vendor
decode_retry.o: ../components/decode_pipeline/decode_retry.c
	$(CC) $(CFLAGS) $(DECODE_RETRY_INC) -c $< -o $@
test_decode_retry.o: test_decode_retry.cpp
	$(CXX) $(CXXFLAGS) $(DECODE_RETRY_INC) -c $< -o $@
$(DECODE_RETRY_TARGET): $(DECODE_RETRY_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

DECODE_SYNC_OBJ = decode_sync.o test_decode_sync.o
decode_sync.o: ../components/decode_pipeline/decode_sync.c
	$(CC) $(CFLAGS) -I../components/decode_pipeline/include -c $< -o $@
test_decode_sync.o: test_decode_sync.cpp
	$(CXX) $(CXXFLAGS) -I../components/decode_pipeline/include -c $< -o $@
$(DECODE_SYNC_TARGET): $(DECODE_SYNC_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lm

# decode_snr re-encodes the message for its tones: encode.c, constants.c, crc.c.
DECODE_SNR_OBJ = decode_snr.o snr_encode.o snr_constants.o snr_crc.o test_decode_snr.o
DECODE_SNR_INC = -I../components/decode_pipeline/include -I../components/ft8_lib -I../components/ft8_lib/vendor
decode_snr.o: ../components/decode_pipeline/decode_snr.c
	$(CC) $(CFLAGS) $(DECODE_SNR_INC) -c $< -o $@
snr_encode.o: ../components/ft8_lib/vendor/ft8/encode.c
	$(CC) $(CFLAGS) $(DECODE_SNR_INC) -c $< -o $@
snr_constants.o: ../components/ft8_lib/vendor/ft8/constants.c
	$(CC) $(CFLAGS) $(DECODE_SNR_INC) -c $< -o $@
snr_crc.o: ../components/ft8_lib/vendor/ft8/crc.c
	$(CC) $(CFLAGS) $(DECODE_SNR_INC) -c $< -o $@
test_decode_snr.o: test_decode_snr.cpp
	$(CXX) $(CXXFLAGS) $(DECODE_SNR_INC) -c $< -o $@
$(DECODE_SNR_TARGET): $(DECODE_SNR_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lm

$(QUEUE_HEAD_TARGET): $(QUEUE_HEAD_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

ADIF_OBJ = adif.o test_adif_merge.o
adif.o: ../components/adif/adif.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(ADIF_TARGET): $(ADIF_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

STATION_OBJ = station.o test_station.o
station.o: ../components/station/station.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(STATION_TARGET): $(STATION_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

DECODE_SORT_OBJ = test_decode_sort.o
$(DECODE_SORT_TARGET): $(DECODE_SORT_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

PORTA_PROTO_OBJ = porta_proto.o porta_event_json.o porta_link_test.o test_porta_proto.o
porta_proto.o: ../components/porta_proto/porta_proto.c
	$(CC) $(CFLAGS) -I../components/porta_proto/include -c $< -o $@
porta_link_test.o: ../components/porta_proto/porta_link_test.c
	$(CC) $(CFLAGS) -I../components/porta_proto/include -c $< -o $@
test_porta_proto.o: test_porta_proto.cpp
	$(CXX) $(CXXFLAGS) -I../components/porta_proto/include -c $< -o $@
$(PORTA_PROTO_TARGET): $(PORTA_PROTO_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

PORTA_OUT_QUEUE_OBJ = porta_out_queue.o porta_proto.o test_porta_out_queue.o
porta_out_queue.o: ../components/porta/porta_out_queue.c
	$(CC) $(CFLAGS) -I../components/porta/include -I../components/porta_proto/include -c $< -o $@
test_porta_out_queue.o: test_porta_out_queue.cpp
	$(CXX) $(CXXFLAGS) -I../components/porta/include -I../components/porta_proto/include \
	      -I../components/decode_types/include -c $< -o $@
$(PORTA_OUT_QUEUE_TARGET): $(PORTA_OUT_QUEUE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

PAIRING_TOKEN_OBJ = pairing_token.o test_pairing_token.o
pairing_token.o: ../sidekick/main/pairing_token.c
	$(CC) $(CFLAGS) -I../sidekick/main -c $< -o $@
test_pairing_token.o: test_pairing_token.cpp
	$(CXX) $(CXXFLAGS) -I../sidekick/main -c $< -o $@
$(PAIRING_TOKEN_TARGET): $(PAIRING_TOKEN_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

WEB_PAGE_OBJ = web_page.o test_web_page.o
web_page.o: ../sidekick/main/web_page.c
	$(CC) $(CFLAGS) -I../sidekick/main -c $< -o $@
test_web_page.o: test_web_page.cpp
	$(CXX) $(CXXFLAGS) -I../sidekick/main -c $< -o $@
$(WEB_PAGE_TARGET): $(WEB_PAGE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

WEB_MANIFEST_OBJ = web_manifest.o test_web_manifest.o
web_manifest.o: ../sidekick/main/web_manifest.c
	$(CC) $(CFLAGS) -I../sidekick/main -c $< -o $@
test_web_manifest.o: test_web_manifest.cpp
	$(CXX) $(CXXFLAGS) -I../sidekick/main -c $< -o $@
$(WEB_MANIFEST_TARGET): $(WEB_MANIFEST_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

BUNDLE_HOST_OBJ = bundle_host.o test_bundle_host.o
bundle_host.o: ../sidekick/main/bundle_host.c
	$(CC) $(CFLAGS) -I../sidekick/main -c $< -o $@
test_bundle_host.o: test_bundle_host.cpp
	$(CXX) $(CXXFLAGS) -I../sidekick/main -c $< -o $@
$(BUNDLE_HOST_TARGET): $(BUNDLE_HOST_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

QSO_BROWSE_OBJ = qso_browse.o test_qso_browse.o
qso_browse.o: ../components/qso_log/qso_browse.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(QSO_BROWSE_TARGET): $(QSO_BROWSE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

ADIF_DELETE_OBJ = qso_browse.o adif_delete.o test_adif_delete.o
adif_delete.o: ../components/qso_log/adif_delete.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I../components/storage_service/include -c $< -o $@
$(ADIF_DELETE_TARGET): $(ADIF_DELETE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

ADIF_LOGGER_OBJ = adif.o adif_logger.o host_mocks.o test_adif_logger.o
adif_logger.o: ../components/qso_log/adif_logger.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(ADIF_LOGGER_TARGET): $(ADIF_LOGGER_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

FILE_LIST_OBJ = qso_browse.o file_list.o test_file_list.o
file_list.o: ../components/qso_log/file_list.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(FILE_LIST_TARGET): $(FILE_LIST_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJ) porta_event_json.o sim_station.o $(SIM_TARGET) \
	      $(UNIQUE_OBJ) $(CQ_TRIES_OBJ) $(ADIF_OBJ) $(STATION_OBJ) \
	      $(DECODE_SORT_OBJ) $(QSO_BROWSE_OBJ) $(ADIF_DELETE_OBJ) \
	      $(FILE_LIST_OBJ) \
	      $(TARGET) $(UNIQUE_TARGET) $(CQ_TRIES_TARGET) $(ADIF_TARGET) $(STATION_TARGET) \
	      $(DECODE_SORT_TARGET) $(QSO_BROWSE_TARGET) $(ADIF_DELETE_TARGET) \
	      $(FILE_LIST_TARGET) \
	      $(PAIRING_TOKEN_OBJ) $(PAIRING_TOKEN_TARGET) \
	      $(WEB_PAGE_OBJ) $(WEB_PAGE_TARGET) \
	      $(WEB_MANIFEST_OBJ) $(WEB_MANIFEST_TARGET) \
	      $(BUNDLE_HOST_OBJ) $(BUNDLE_HOST_TARGET) \
	      $(DIAL_HISTORY_OBJ) $(DIAL_HISTORY_TARGET) \
	      $(ADIF_LOGGER_OBJ) $(ADIF_LOGGER_TARGET) \
	      $(CAT_REPLY_PARSER_OBJ) $(CAT_REPLY_PARSER_TARGET) \
	      $(SWR_SWEEP_OBJ) $(SWR_SWEEP_TARGET) \
	      $(PORTA_PROTO_OBJ) $(PORTA_PROTO_TARGET) \
	      $(PORTA_OUT_QUEUE_OBJ) $(PORTA_OUT_QUEUE_TARGET) \
	      $(DECODE_RETRY_OBJ) $(DECODE_RETRY_TARGET) \
	      $(DECODE_SYNC_OBJ) $(DECODE_SYNC_TARGET) \
	      $(DECODE_SNR_OBJ) $(DECODE_SNR_TARGET) \
	      $(ROGERS_ONCE_OBJ) $(ROGERS_ONCE_TARGET) \
	      $(HEARD_GRID_OBJ) $(HEARD_GRID_TARGET) \
	      $(DECODE_FIELDS_OBJ) $(DECODE_FIELDS_TARGET) \
	      $(RTC_CORE_OBJ) $(RTC_CORE_TARGET)

.PHONY: all clean
