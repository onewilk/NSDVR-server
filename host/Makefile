# Host (macOS, clang) build of the NSDVR extension code.
#   make            build everything (mock server, test client, tests, bench, vector generator)
#   make test       unit + fuzz tests (ASan/UBSan)
#   make bench      encoder timing, Opus pseudostack / state measurements (release build)
#   make selftest   end-to-end: hostmock + hostclient scenarios over loopback
#   make vectors    regenerate the shared test vectors (VECTORS_DIR)
#   make demo-h264  build/demo.h264 with $(CLIENT_REPO)/tools/make_demo_h264.swift
#
# The protocol/codec sources are the sysmodule's own files (sysmodule/source/next) and the same
# libopus tree (third_party/opus) with the same configuration (third_party/opus_nx).

ROOT     := ..
NEXT_DIR := $(ROOT)/sysmodule/source/next
OPUS_DIR := $(ROOT)/third_party/opus
OPUS_NX  := $(ROOT)/third_party/opus_nx
# The NSDVR client repository (https://github.com/onewilk/NSDVR), cloned next to this one.
# TEST_WAV can be any 48 kHz 16-bit WAV; see README.md for the one used for the vectors and measurements.
CLIENT_REPO ?= $(abspath $(ROOT)/../NSDVR)
VECTORS_DIR ?= $(CLIENT_REPO)/tools/ext_vectors
TEST_WAV    ?= $(CLIENT_REPO)/audio_eval/src/aliens_48k.wav

include $(OPUS_NX)/opus_files.mk

UNAME_M := $(shell uname -m)
OPUS_SRC := $(OPUS_NX_SRC)
ifneq ($(filter arm64 aarch64,$(UNAME_M)),)
OPUS_SRC += $(OPUS_NX_SRC_NEON)
endif

BUILD ?= build
CC    := clang
OPT   := -O2 -g
# Same optimisation level as the sysmodule's libopus (sysmodule/Makefile OPUS_OPT)
OPUS_OPT ?= -O2
SAN   := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
WARN  := -Wall -Wextra -Wno-unused-parameter -Werror

OPUS_CFLAGS := -std=gnu11 -include $(OPUS_NX)/next_opus_config.h $(OPUS_NX_INC) -w -MMD -MP
NEXT_CFLAGS := -std=gnu11 $(WARN) -I$(NEXT_DIR) -I$(OPUS_DIR)/include -Isrc -Itests -MMD -MP

NEXT_SRC := next_proto.c next_audio.c next_diag.c next_session.c

opus_objs = $(patsubst %.c,$(BUILD)/$(1)/opus/%.o,$(OPUS_SRC)) $(BUILD)/$(1)/opus/silk_enc_stub.o $(BUILD)/$(1)/opus/opus_nx_helpers.o
next_objs = $(patsubst %.c,$(BUILD)/$(1)/next/%.o,$(NEXT_SRC))

OPUS_REL := $(call opus_objs,rel)
OPUS_SAN := $(call opus_objs,san)
NEXT_REL := $(call next_objs,rel)
NEXT_SAN := $(call next_objs,san)

HOSTMOCK := $(BUILD)/sysdvr_hostmock
CLIENT   := $(BUILD)/sysdvr_hostclient
TESTS    := $(BUILD)/next_tests
BENCH    := $(BUILD)/next_bench
VECTORS  := $(BUILD)/next_vectors

.PHONY: all test bench selftest vectors demo-h264 clean
all: $(HOSTMOCK) $(CLIENT) $(TESTS) $(BENCH) $(VECTORS)

# ---------------------------------------------------------------- objects
$(BUILD)/rel/opus/%.o: $(OPUS_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) -g $(OPUS_OPT) $(OPUS_CFLAGS) -c $< -o $@
$(BUILD)/san/opus/%.o: $(OPUS_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(SAN) $(OPUS_CFLAGS) -c $< -o $@
$(BUILD)/rel/opus/%.o: $(OPUS_NX)/%.c
	@mkdir -p $(dir $@)
	@$(CC) -g $(OPUS_OPT) $(OPUS_CFLAGS) -c $< -o $@
$(BUILD)/san/opus/%.o: $(OPUS_NX)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(SAN) $(OPUS_CFLAGS) -c $< -o $@

$(BUILD)/rel/next/%.o: $(NEXT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) $(NEXT_CFLAGS) -c $< -o $@
$(BUILD)/san/next/%.o: $(NEXT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SAN) $(NEXT_CFLAGS) -c $< -o $@

$(BUILD)/rel/host/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) $(NEXT_CFLAGS) -c $< -o $@
$(BUILD)/rel/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) $(NEXT_CFLAGS) -c $< -o $@
$(BUILD)/san/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SAN) $(NEXT_CFLAGS) -c $< -o $@
$(BUILD)/san/host/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SAN) $(NEXT_CFLAGS) -c $< -o $@

$(BUILD)/libopus_nx_rel.a: $(OPUS_REL)
	@rm -f $@
	ar rcs $@ $^
$(BUILD)/libopus_nx_san.a: $(OPUS_SAN)
	@rm -f $@
	ar rcs $@ $^

# ---------------------------------------------------------------- programs
$(HOSTMOCK): $(BUILD)/rel/host/hostmock.o $(BUILD)/rel/host/plat_host.o $(BUILD)/rel/host/media.o $(NEXT_REL) $(BUILD)/libopus_nx_rel.a
	$(CC) $(OPT) -o $@ $^ -lpthread -lm

$(CLIENT): $(BUILD)/rel/host/hostclient.o $(BUILD)/rel/host/media.o $(BUILD)/rel/next/next_proto.o $(BUILD)/rel/next/next_audio.o $(BUILD)/libopus_nx_rel.a
	$(CC) $(OPT) -o $@ $^ -lpthread -lm

$(TESTS): $(BUILD)/san/tests/test_main.o $(BUILD)/san/tests/ref.o $(BUILD)/san/tests/sim.o $(BUILD)/san/host/media.o $(NEXT_SAN) $(BUILD)/libopus_nx_san.a
	$(CC) $(SAN) -o $@ $^ -lm

$(BENCH): $(BUILD)/rel/tests/bench.o $(BUILD)/rel/tests/ref.o $(BUILD)/rel/tests/sim.o $(BUILD)/rel/host/media.o $(filter-out %next_session.o,$(NEXT_REL)) $(BUILD)/libopus_nx_rel.a
	$(CC) $(OPT) -o $@ $^ -lm

$(VECTORS): $(BUILD)/rel/tests/gen_vectors.o $(BUILD)/rel/tests/ref.o $(BUILD)/rel/tests/sim.o $(BUILD)/rel/host/media.o $(filter-out %next_session.o,$(NEXT_REL)) $(BUILD)/libopus_nx_rel.a
	$(CC) $(OPT) -o $@ $^ -lm

# ---------------------------------------------------------------- actions
test: $(TESTS)
	$(TESTS) $(TEST_WAV)

bench: $(BENCH)
	$(BENCH) $(TEST_WAV)

vectors: $(VECTORS)
	$(VECTORS) $(TEST_WAV) $(VECTORS_DIR)

demo-h264: $(BUILD)/demo.h264
$(BUILD)/demo.h264:
	@mkdir -p $(BUILD)
	swift $(CLIENT_REPO)/tools/make_demo_h264.swift $@ 10 "NSDVR hostmock"

selftest: $(HOSTMOCK) $(CLIENT)
	./tests/selftest.sh

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
