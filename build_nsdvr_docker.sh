#!/bin/bash
# Builds the NSDVR experimental sysmodule (branch nsdvr) and the official baseline (804fd36)
# with the devkitpro/devkita64 Docker image (no local devkitPro needed) and packages them like
# ReleaseSysmodule.sh does (atmosphere/contents/00FF0000A53BB665/...). Output: out/nsdvr, out/baseline.
# Nothing is deployed anywhere.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
IMAGE=${IMAGE:-devkitpro/devkita64:latest}
BASE_COMMIT=${BASE_COMMIT:-804fd36}

rm -rf out/baseline_src && mkdir -p out/baseline_src
git archive "$BASE_COMMIT" sysmodule | tar -x -C out/baseline_src

build() { # <dir relative to repo> <out name>
	docker run --rm -v "$ROOT":/work -w "/work/$1" "$IMAGE" bash -c 'make clean >/dev/null && make -j8 2>&1 | grep -E "error|warning:" || true'
	local o="out/$2" c="out/$2/atmosphere/contents/00FF0000A53BB665"
	rm -rf "$o" && mkdir -p "$c/flags" "$o/debug"
	cp "$1/sysmodule.nsp" "$c/exefs.nsp"
	cp "$1/toolbox.json" "$c/toolbox.json"
	echo . > "$c/flags/boot2.flag"
	cp "$1/sysmodule.elf" "$o/debug/main.elf"
	cp "$1/sysmodule.nso" "$o/debug/sysmodule.nso"
	cp "$1/build/sysmodule.map" "$o/debug/sysmodule.map" 2>/dev/null || true
	strings "$1/sysmodule.nsp" | grep -q SYSDVR_BUILD_FULL || { echo "version tag missing in $2"; exit 1; }
}

build out/baseline_src/sysmodule baseline
build sysmodule nsdvr

docker run --rm -v "$ROOT":/work -w /work "$IMAGE" bash -c '
export PATH=$DEVKITPRO/devkitA64/bin:$PATH
for n in baseline nsdvr; do
  echo "== $n"; aarch64-none-elf-size out/$n/debug/main.elf
  aarch64-none-elf-size -A out/$n/debug/main.elf | grep -E "^\.(text|rodata|eh_frame|data|bss) "
  aarch64-none-elf-readelf -lW out/$n/debug/main.elf | grep LOAD
done' | tee out/SIZES.txt
