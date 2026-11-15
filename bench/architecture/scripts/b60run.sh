#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Run one command on the B60 without overlapping other users of that GPU:
#   scripts/b60run.sh <command ...>
# Waits while a process matching $B60_BUSY_RE runs, then takes the lock file $B60_LOCK for the duration of the command.
# If such a process appeared while the command ran, "CONTENDED" is written to stderr and to $B60_CONTENDED_LOG (default results/b60_contended.txt):
# repeat that measurement.
LOCK=${B60_LOCK:-/tmp/b60.lock}
RE=${B60_BUSY_RE:-'longctx_b60\.py|pass_prof|gemm_big_test|pf_check|bin/yasakani (bench|selfcheck|chat|run)'}
export SYCL_CACHE_PERSISTENT=1
# a "flock <lock> <command>" line is a job that waits for (or holds) the lock; the job itself is listed separately once it runs
busy() { pgrep -af "$RE" | grep -v -e 'yasakani serve' -e 'b60run.sh' -e 'pgrep' -e '/bin/bash -c' | grep -v -E '^[0-9]+ flock ' | grep -q .; }
CLOG=${B60_CONTENDED_LOG:-$(dirname "$0")/../results/b60_contended.txt}
n=0
while busy; do n=$((n + 1)); [ $n -gt 240 ] && { echo "B60 busy for two hours, giving up" >&2; exit 75; }; sleep 30; done
flock "$LOCK" "$@"; rc=$?
if busy; then echo "CONTENDED: another B60 job was running when '$*' finished" >&2; echo "CONTENDED $(date +%H:%M:%S) ${ARCH_ONLY:+ARCH_ONLY=$ARCH_ONLY }${ARCH_BITS:+ARCH_BITS=$ARCH_BITS }$*" >> "$CLOG" 2>/dev/null; fi
exit $rc
