#!/usr/bin/env bash
# Public seam acceptance: real Windows processes, real GPU, no mocks.
set -euo pipefail
cd "$(dirname "$0")"
: "${PROBE_RESULTS:?Set PROBE_RESULTS to a fresh output directory}"
mkdir "$PROBE_RESULTS"
PROBE_RESULTS=$(cd "$PROBE_RESULTS" && pwd)
for colour in rgba bgra; do
  for mode in normal invalid-handle invalid-wire missing-wire partial-import; do
    dest="$PROBE_RESULTS/$colour-$mode"
    mkdir -p "$dest"
    PROBE_LOG_DIR="$dest" bash run.sh "$colour" 3 "$mode" > "$dest/launcher.log" 2>&1
    # Only explicit logs from this invocation establish acceptance. Preserve
    # launcher.log and steam-325610.log independently for Proton diagnostics.
    shopt -s nullglob
    producers=("$dest"/process-logs/producer-*.log)
    consumers=("$dest"/process-logs/consumer-*.log)
    [[ ${#producers[@]} == 1 && ${#consumers[@]} == 3 ]]
    cat "${producers[@]}" "${consumers[@]}" | tr -d '\r' > "$dest/combined.log"
    rg -q "PASS probe mode=$mode cycles=3" "$dest/combined.log"
    if [[ $mode == normal ]]; then
      for pair in final:11 hudless:22 depth:33 motion:44 srout0:55 srout1:66 reactive:77; do
        resource=${pair%:*}; expected=${pair#*:}
        [[ $(rg -c "PASS contents resource=$resource expected=0x$expected" "$dest/combined.log") == 3 ]]
      done
      for pair in srout0:91 srout1:a2; do
        resource=${pair%:*}; expected=${pair#*:}
        [[ $(rg -c "PASS contents resource=$resource expected=0x$expected" "$dest/combined.log") == 3 ]]
      done
      [[ $(rg -c '^BASELINE resource=game-fence API=OpenSharedHandleByName HRESULT=0x80004001$' "$dest/combined.log") == 3 ]]
    else
      [[ $(rg -c "PASS expected failure mode=$mode exit=20" "$dest/combined.log") == 3 ]]
    fi
  done
done
printf 'PASS complete GPU probe suite\n'
