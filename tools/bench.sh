#!/bin/bash
# Time the display of a device running a debug build (-D OVERHEAD_DEBUG=1, see
# CMakeLists.txt), through /api/debug/bench. See docs/PERFORMANCE.md.
#
#   tools/bench.sh [host] [runs]          every mode, 3 runs each
#   MODES="settings scope" tools/bench.sh  just those
#
# Traffic changes from minute to minute, and the scope numbers with it: compare
# builds with runs taken back to back, not against figures from another day.
H=${1:-overhead.local}
RUNS=${2:-3}
for i in $(seq 1 40); do curl -s -m 3 "http://$H/api/state" >/dev/null && break; sleep 3; done
curl -s -m 5 "http://$H/api/state" | python3 -c "
import sys, json
d = json.load(sys.stdin); s = d['settings']
print('firmware', d['firmware'], '| up', d['uptime_s'], 's | rotation', s['rotation'], '| sweep', s['sweep'], '| trails', s['trail_s'])"
for what in ${MODES:-settings scope radar big covered}; do
    for r in $(seq 1 "$RUNS"); do
        curl -s -m 30 -X POST -H "Content-Type: application/json" "http://$H/api/debug/bench?what=$what&ms=5000" | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
except ValueError:
    sys.exit('no /api/debug/bench: is it a debug build (CONFIG_LV_USE_SNAPSHOT=y)?')
print(f\"{d['what']:9} fps {d['fps']:5.1f}  avg {d['avg_ms']:6.1f} ms  max {d['max_ms']:6.1f} ms  \"
      f\"busy {d['busy_pct']:3}% (flush {d['flush_pct']}%, wait {d['wait_pct']}%)  drawn {d['drawn_kpx']} kpx  \"
      f\"requests {d['requests']}, biggest {d['biggest_request']}\\n          \"
      f\"a frame: prep {d['prep_us']/1000:.1f} + render {d['render_us']/1000:.1f} (scope {d['scope_cb_us']/1000:.1f}) \"
      f\"+ flush {d['flush_us']/1000:.1f} ms, {d['flushes_per_frame']} flushes; rendered {d['rendered']} of {d['frames']}, \"
      f\"sweep steps {d['sweep_ticks']}  \"
      f\"internal {d['internal_free']} (min {d['internal_min']})  openaip {d['openaip']}\")"
    done
done
