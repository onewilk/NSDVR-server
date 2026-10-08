# libopus source selection shared by host/Makefile and sysmodule/Makefile. Set OPUS_DIR first.
# Fixed point, no SILK encoder (see silk_enc_stub.c), no DNN/QEXT/multistream/projection.
include $(OPUS_DIR)/celt_sources.mk
include $(OPUS_DIR)/silk_sources.mk
include $(OPUS_DIR)/opus_sources.mk

OPUS_NX_SRC := $(CELT_SOURCES) \
	$(filter-out silk/enc_API.c,$(SILK_SOURCES)) \
	$(SILK_SOURCES_FIXED) \
	src/opus.c src/opus_decoder.c src/opus_encoder.c src/extensions.c src/repacketizer.c

# AArch64 only (Cortex-A57 on the Switch, Apple Silicon on the host)
OPUS_NX_SRC_NEON := $(CELT_SOURCES_ARM_NEON_INTR) $(SILK_SOURCES_ARM_NEON_INTR) $(SILK_SOURCES_FIXED_ARM_NEON_INTR)

OPUS_NX_INC := -I$(OPUS_DIR) -I$(OPUS_DIR)/include -I$(OPUS_DIR)/celt -I$(OPUS_DIR)/silk -I$(OPUS_DIR)/silk/fixed
