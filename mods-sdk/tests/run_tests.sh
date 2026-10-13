#!/bin/bash
# Builds every test mod and checks that the sandbox handles it as expected.
# Usage: run_tests.sh <path to modcheck>
set -u
MODCHECK="${1:?usage: run_tests.sh <path to modcheck>}"
cd "$(dirname "$0")"
declare -A EXPECT=(
	[bad-pointer]="FAULTED"     # host refuses out-of-bounds strings
	[crash]="FAULTED"           # a trap stops only this mod
	[infinite-loop]="FAULTED"   # fuel budget stops runaway code
	[memory-hog]="OK"           # memory.grow is refused past the cap
	[no-permission]="OK"        # condition_set returns -1 without permission
	[with-permission]="OK"      # condition_set works with permission
	[wants-wasi]="FAILED"       # no file/system access: WASI imports don't exist
	[wrong-api]="FAILED"        # API version mismatch is rejected
	[event-logger]="OK"         # receives events and frames
)
fail=0
for mod in "${!EXPECT[@]}"; do
	clang --target=wasm32 -O1 -nostdlib -fno-builtin -Wl,--no-entry -I ../c -o "$mod/mod.wasm" "$mod/mod.c" || { echo "build failed: $mod"; fail=1; continue; }
	got=$(timeout 60 "$MODCHECK" "$mod" 600 | grep -oE "^(OK|FAULTED|FAILED)")
	if [ "$got" = "${EXPECT[$mod]}" ]; then echo "pass  $mod ($got)"; else echo "FAIL  $mod: expected ${EXPECT[$mod]}, got ${got:-nothing}"; fail=1; fi
done
exit $fail
