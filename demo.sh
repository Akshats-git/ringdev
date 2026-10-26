#!/bin/bash
# Build, load the module, run the demo, show the stats, unload.
# Uses sudo for insmod/rmmod/dmesg only.
set -euo pipefail
cd "$(dirname "$0")"

make >/dev/null 2>&1 || { make; exit 1; }
sudo insmod ./ringdev.ko "$@"
trap 'sudo rmmod ringdev' EXIT

for _ in $(seq 50); do [ -c /dev/ringdev ] && break; sleep 0.1; done
[ -c /dev/ringdev ] || { echo "/dev/ringdev did not appear" >&2; exit 1; }

echo "== demo"
./bin/demo

echo "== tests"
for t in basic mmap ops blocking poll invalid; do ./bin/test_$t | tail -1; done
./bin/test_stress | tail -1
./bin/test_kill | tail -2

echo "== debugfs stats (needs root to read)"
if [ -r /sys/kernel/debug/ringdev/stats ]; then
	cat /sys/kernel/debug/ringdev/stats
else
	echo "run: sudo cat /sys/kernel/debug/ringdev/stats"
fi

echo "== dmesg"
sudo dmesg | grep ringdev | tail -5
