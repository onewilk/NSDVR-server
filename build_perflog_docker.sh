#!/bin/bash
# Builds the two test-only perf logger variants (make PERFLOG=1) with the devkitpro/devkita64 Docker
# image and packages them like ReleaseSysmodule.sh (atmosphere/contents/00FF0000A53BB665/...):
#   out/perflog-official  branch perflog       = official SysDVR (master) + perf logger
#   out/perflog-nsdvr     branch nsdvr-perflog = NSDVR extension + perf logger
# The logger writes /config/sysdvr/perf/perf_<unix time>.csv on the SD card (see
# sysmodule/source/perflog/perflog.h). Nothing is deployed anywhere.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
IMAGE=${IMAGE:-devkitpro/devkita64:latest}

build() { # <branch> <out name>
	local src="out/$2_src"
	rm -rf "$src" && mkdir -p "$src"
	# third_party (libopus) only exists on the extension branch
	git archive "$1" $(git ls-tree --name-only "$1" | grep -E '^(sysmodule|third_party)$') | tar -x -C "$src"
	docker run --rm -v "$ROOT":/work -w "/work/$src/sysmodule" "$IMAGE" bash -c 'make -j8 PERFLOG=1 2>&1 | grep -E "error|warning:" || true'
	local o="out/$2" c="out/$2/atmosphere/contents/00FF0000A53BB665"
	rm -rf "$o" && mkdir -p "$c/flags" "$o/debug"
	cp "$src/sysmodule/sysmodule.nsp" "$c/exefs.nsp"
	cp "$src/sysmodule/toolbox.json" "$c/toolbox.json"
	echo . > "$c/flags/boot2.flag"
	cp "$src/sysmodule/sysmodule.elf" "$o/debug/main.elf"
	strings "$src/sysmodule/sysmodule.nsp" | grep -q SYSDVR_BUILD_FULL || { echo "version tag missing in $2"; exit 1; }
	rm -rf "$src"
}

build perflog perflog-official
build nsdvr-perflog perflog-nsdvr

docker run --rm -v "$ROOT":/work -w /work "$IMAGE" bash -c '
export PATH=$DEVKITPRO/devkitA64/bin:$PATH
for n in perflog-official perflog-nsdvr; do
  echo "== $n"; aarch64-none-elf-size out/$n/debug/main.elf
  aarch64-none-elf-nm out/$n/debug/main.elf | grep -c " PerfLog_" | sed "s/^/PerfLog symbols: /"
done' | tee out/PERFLOG_SIZES.txt
