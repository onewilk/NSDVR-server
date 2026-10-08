#!/bin/bash
# Run inside devkitpro/devkita64 with the repo at /work:
#   docker run --rm -v "$PWD":/work devkitpro/devkita64 bash /work/host/tools/stackcheck.sh
#   python3 host/tools/maxstack.py out/stackcheck/ci NextSession_Audio NextSession_Video
# Compile libopus + next + the TCP glue with -fcallgraph-info=su to get per-function stack usage and call edges
set -e
export PATH=$DEVKITPRO/devkitA64/bin:$PATH
cd /work
OPUS_DIR=third_party/opus
OUT=out/stackcheck/ci
rm -rf $OUT; mkdir -p $OUT
SRCS=$(make -s -f - print OPUS_DIR=$OPUS_DIR <<'MK'
include third_party/opus_nx/opus_files.mk
print:
	@echo $(OPUS_NX_SRC) $(OPUS_NX_SRC_NEON)
MK
)
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
i=0
for s in $SRCS silk_enc_stub_placeholder; do [ "$s" = silk_enc_stub_placeholder ] && continue
  aarch64-none-elf-gcc -g -O2 -std=gnu11 -ffunction-sections -fdata-sections $ARCH -D__SWITCH__ -include third_party/opus_nx/next_opus_config.h \
    -I$OPUS_DIR -I$OPUS_DIR/include -I$OPUS_DIR/celt -I$OPUS_DIR/silk -I$OPUS_DIR/silk/fixed -w \
    -fcallgraph-info=su -c $OPUS_DIR/$s -o $OUT/opus_$(echo $s | tr / _).o -dumpbase $OUT/opus_$(echo $s | tr / _) &
  i=$((i+1)); if [ $((i % 8)) -eq 0 ]; then wait; fi
done
wait
for s in sysmodule/source/next/*.c; do
  aarch64-none-elf-gcc -g -O3 -Wall -ffunction-sections $ARCH -D__SWITCH__ -I$OPUS_DIR/include -I$DEVKITPRO/libnx/include \
    -fcallgraph-info=su -c $s -o $OUT/next_$(basename $s).o -dumpbase $OUT/next_$(basename $s)
done
ls $OUT/*.ci | wc -l
