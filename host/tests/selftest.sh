#!/bin/bash
# End-to-end self test: sysdvr_hostmock + sysdvr_hostclient over loopback (ports 29911/29922,
# so a hostmock running on the default ports is not disturbed).
set -u
cd "$(dirname "$0")/.."
MOCK=./build/sysdvr_hostmock
CLIENT=./build/sysdvr_hostclient
WAV=${TEST_WAV:-../../NSDVR/audio_eval/src/aliens_48k.wav}
H264=${TEST_H264:-build/demo.h264}
VP=29911; AP=29922
PASS=0; FAIL=0
mkdir -p build/selftest

run() {
	local name="$1"; shift
	local mockargs="$1"; shift
	$MOCK --video-port $VP --audio-port $AP --no-beacon --once --quiet-diag --wav "$WAV" $mockargs > "build/selftest/$name.mock.log" 2>&1 &
	local pid=$!
	sleep 0.4
	if $CLIENT --video-port $VP --audio-port $AP --quiet "$@" > "build/selftest/$name.client.log" 2>&1; then
		PASS=$((PASS+1)); echo "PASS  $name"
	else
		FAIL=$((FAIL+1)); echo "FAIL  $name (see build/selftest/$name.client.log)"
	fi
	grep -E "^(video|diag totals|audio):" "build/selftest/$name.client.log" | sed 's/^/      /'
	wait $pid 2>/dev/null
}

VIDEO=""
[ -f "$H264" ] && VIDEO="--h264 $H264"

run ext_codec_switching "$VIDEO" --seconds 9 --codec opus \
	--ctrl "1.5:adpcm,3:pcm24,4.5:opus:64:10:10,6:opus:160:3:20,7.5:pcm48" --expect-codecs pcm48,pcm24,adpcm,opus
run official_client "$VIDEO" --legacy --seconds 4 --expect-no-diag
run stall_300ms_every_2s "$VIDEO --stall-every 2 --stall-ms 300" --seconds 8 --codec adpcm --expect-gaps-after-slow 2 --expect-min-gaps 2
run short_stall_no_gap "$VIDEO --stall-every 2 --stall-ms 60" --seconds 6 --codec pcm24 --expect-max-gaps 0
run source_drops_only "$VIDEO --drop-every 25" --seconds 6 --codec opus --expect-min-gaps 4 --expect-max-gaps-after-slow 0
run control_fuzzing "" --seconds 8 --codec opus --fuzz-ctrl
run batching0_opus10ms "" --seconds 4 --codec opus --frame 10 --batching 0
run batching5_opus20ms_c10 "" --seconds 4 --codec opus --complexity 10 --kbps 256 --batching 5
run no_opus_memory "--no-opus" --seconds 3 --codec opus --expect-codecs pcm48

echo
echo "selftest: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
