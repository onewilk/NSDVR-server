# libopus build glue for NSDVR

`../opus` is the unmodified libopus **1.6.1** release tarball from xiph
(`https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz`,
SHA-256 `6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1`, matches xiph's SHA256SUMS.txt),
with only the `dnn/`, `doc/`, `tests/`, `celt/tests/` and `silk/tests/` directories removed (not needed, 22 MB of model data).

Files here:

- `next_opus_config.h` – force-included configuration (fixed point, static pseudostack, no heap, hardening).
- `silk_enc_stub.c` – replaces `silk/enc_API.c`; only `OPUS_APPLICATION_RESTRICTED_CELT` is used, which never touches SILK.
- `opus_files.mk` – the source list used by both `sysmodule/Makefile` and `host/Makefile`.
