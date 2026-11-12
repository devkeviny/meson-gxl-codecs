#!/bin/bash
# live-watchdog.sh — keep the live running across encoder wedges.
#
# The HCodec firmware wedges on startup in ~40% of encoder starts (first
# IDR, MB_HEADER phase, RISC stuck — see TESTING.md). The driver drops
# the frame and retries, but a wedged instance never recovers in-process:
# only a fresh ffmpeg process gets a new coin flip. So this watchdog
# watches the encoder's -progress file and restarts the whole pipeline
# when no frame is produced for 6 seconds.
#
# Each restart is an independent ~60-80% success, so 2-3 restarts give
# >99% success. A wedge costs ~10 s of stream (6 s detection + restart),
# then the live resumes. For RTMP output, YouTube treats a quick
# reconnect with the same stream key as the same live event.
#
# Usage: live-watchdog.sh <duration_seconds> [output]
#   output defaults to /tmp/live-out-<attempt>.flv per attempt, or set
#   LIVE_OUTPUT to an RTMP URL (same URL for every attempt).
set -u

DUR="${1:-0}"		# 0 = run forever
LIVE_OUTPUT="${LIVE_OUTPUT:-}"
LOG=/root/live-watchdog.log
rm -f "$LOG"

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; sync; }

START=$(date +%s)
ATTEMPT=0
while :; do
	[ "$DUR" -gt 0 ] && [ $(( $(date +%s) - START )) -ge "$DUR" ] && break
	ATTEMPT=$((ATTEMPT + 1))
	if [ -n "$LIVE_OUTPUT" ]; then
		OUT="$LIVE_OUTPUT"
	else
		OUT="/tmp/live-out-$ATTEMPT.flv"
	fi
	PROG="/tmp/live-progress-$ATTEMPT.txt"
	rm -f "$PROG"
	log "=== attempt $ATTEMPT -> $OUT ==="
	setsid bash "$(dirname "$0")/live-pipe.sh" "$OUT" "$PROG" \
		> /dev/null 2>&1 &
	PGID=$!
	LAST=-1
	STALL=0
	while kill -0 "$PGID" 2>/dev/null; do
		sleep 2
		if [ -f "$PROG" ]; then
			FRAME=$(grep -a "^frame=" "$PROG" | tail -1 | cut -d= -f2)
			FRAME=${FRAME:-0}
		else
			FRAME=0
		fi
		if [ "$FRAME" = "$LAST" ]; then
			STALL=$((STALL + 2))
		else
			STALL=0
			LAST=$FRAME
		fi
		if [ "$STALL" -ge 6 ]; then
			log "WEDGE detected (frame stuck at $FRAME for ${STALL}s) — restarting"
			break
		fi
		[ "$DUR" -gt 0 ] && [ $(( $(date +%s) - START )) -ge "$DUR" ] && break
	done
	kill -9 -"$PGID" 2>/dev/null
	wait "$PGID" 2>/dev/null
	sleep 1
	if [ -z "$LIVE_OUTPUT" ] && [ -f "$OUT" ]; then
		log "output $OUT: $(stat -c%s "$OUT") bytes"
	fi
done
log "watchdog done: $ATTEMPT attempts"
echo WATCHDOG_DONE >> "$LOG"
sync
