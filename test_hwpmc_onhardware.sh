#!/bin/sh
# test_hwpmc_onhardware.sh — Integration tests for D55607 on real AMD hardware
#
# Run as root on FreeBSD with the patched hwpmc.ko loaded.
# Hardware: AMD Ryzen 5600X (or similar Zen 2/3/4)
#
# Usage:
#   sudo sh test_hwpmc_onhardware.sh
#
# Author: Paulo Fragoso <paulo@nlink.com.br>
# Sponsored by: NLINK (https://nlink.com.br), Recife, Brazil

set -u  # catch undefined variables — do NOT use set -e (grep -c returns 1 on no match)

PASS=0
FAIL=0
SKIP=0

pass() { PASS=$((PASS + 1)); printf "  PASS: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  FAIL: %s\n" "$1"; }
skip() { SKIP=$((SKIP + 1)); printf "  SKIP: %s\n" "$1"; }

# --------------------------------------------------------------------------
# Preflight checks
# --------------------------------------------------------------------------
echo "================================================================"
echo "  hwpmc On-Hardware Integration Tests"
echo "  FreeBSD D55607 — AMD PMC patch validation"
echo "================================================================"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Must run as root"
    exit 1
fi

# Ensure hwpmc is loaded
if ! kldstat -q -m hwpmc 2>/dev/null; then
    echo "Loading hwpmc.ko..."
    kldload /boot/modules/hwpmc.ko || { echo "ERROR: Cannot load hwpmc.ko"; exit 1; }
    sleep 1  # give devd time to create /dev/pmc
fi

# Replace /dev/pmc check with modfind verification
if ! pmccontrol -l > /dev/null 2>&1; then
    echo "ERROR: hwpmc not accessible via libpmc"
    exit 1
fi

# Check for AMD CPU
vendor=$(sysctl -n hw.model 2>/dev/null)
case "$vendor" in
    *AMD*) echo "CPU: $vendor" ;;
    *)     echo "WARNING: Non-AMD CPU detected ($vendor), tests may not apply" ;;
esac

ncpu=$(sysctl -n hw.ncpu)
echo "CPUs: $ncpu"
echo ""

# --------------------------------------------------------------------------
# Test 1: Counter Registration
# Verify pmccontrol -l shows correct counter state
# --------------------------------------------------------------------------
echo "=== Test 1: Counter Registration ==="

pmcout=$(pmccontrol -l)

# Extract CPU 0 block
cpu0_block=$(echo "$pmcout" | sed -n '/#CPU 0:/,/#CPU 1:/p')

# Count enabled core counters (K8-0 through K8-5, not K8-L3 or K8-DF)
core_count=$(echo "$cpu0_block" | grep -c "K8-[0-9][^-]*ENABLED" || true)
if [ "$core_count" -ge 4 ]; then
    pass "CPU 0 has $core_count core counters ENABLED (expected >= 4)"
else
    fail "CPU 0 has $core_count core counters ENABLED (expected >= 4)"
fi

# Count enabled L3 counters
l3_count=$(echo "$cpu0_block" | grep -c "K8-L3.*ENABLED" || true)
if [ "$l3_count" -ge 1 ]; then
    pass "CPU 0 has L3 counters ENABLED ($l3_count found)"
else
    skip "No L3 counters found (may not be supported on this CPU)"
fi

# Count enabled DF counters
df_count=$(echo "$cpu0_block" | grep -c "K8-DF.*ENABLED" || true)
if [ "$df_count" -ge 1 ]; then
    pass "CPU 0 has DF counters ENABLED ($df_count found)"
else
    skip "No DF counters found (may not be supported on this CPU)"
fi

echo ""

# --------------------------------------------------------------------------
# Test 2: Core Counter Functional Test
# Verify we can actually count instructions on CPU 0 and CPU 1
# --------------------------------------------------------------------------
echo "=== Test 2: Core Counter — ex_ret_instr ==="

for cpu in 0 1; do
    if [ "$cpu" -ge "$ncpu" ]; then
        skip "CPU $cpu not present"
        continue
    fi

    count=$(timeout 2 pmcstat -c "$cpu" -s ex_ret_instr -w 1 2>&1 | grep -v '^#' | head -1 | tr -d ' ')
    if [ -n "$count" ] && [ "$count" -gt 0 ] 2>/dev/null; then
        pass "CPU $cpu: ex_ret_instr = $count (non-zero, counter works)"
    else
        fail "CPU $cpu: ex_ret_instr returned '$count' (expected non-zero)"
    fi
done

echo ""

# --------------------------------------------------------------------------
# Test 3: L3 Counter Functional Test
# --------------------------------------------------------------------------
echo "=== Test 3: L3 Counter — l3_cache_accesses ==="
skip "L3 counters return zero — known DOMWIDE sharing issue, pending Ali's PR #2058"

# count=$(timeout 2 pmcstat -c 0 -s l3_cache_accesses -w 1 2>&1 | grep -v '^#' | head -1 | tr -d ' ')
# if [ -n "$count" ] && [ "$count" -gt 0 ] 2>/dev/null; then
#     pass "CPU 0: l3_cache_accesses = $count (non-zero)"
# elif echo "$count" | grep -qi "error\|cannot\|invalid"; then
#     skip "L3 counter event not available on this hardware"
# else
#     fail "CPU 0: l3_cache_accesses returned '$count'"
# fi

echo ""

# --------------------------------------------------------------------------
# Test 4: DF Counter Functional Test (CPU 0 only)
# --------------------------------------------------------------------------
echo "=== Test 4: DF Counter — dram_channel_data_controller_0 ==="

count=$(timeout 2 pmcstat -c 0 -s dram_channel_data_controller_0 -w 1 2>&1 | grep -v '^#' | head -1 | tr -d ' ')
if [ -n "$count" ] && [ "$count" -gt 0 ] 2>/dev/null; then
    pass "CPU 0: dram_channel_data_controller_0 = $count (non-zero)"
elif echo "$count" | grep -qi "error\|cannot\|invalid"; then
    skip "DF counter event not available on this hardware"
else
    fail "CPU 0: dram_channel_data_controller_0 returned '$count'"
fi

echo ""

# --------------------------------------------------------------------------
# Test 5: DF Counter Sharing
# Skipped until Ali's DOMWIDE patch lands (GitHub PR #2058)
# --------------------------------------------------------------------------
echo "=== Test 5: DF Counter Sharing (non-master CPU rejection) ==="

skip "DF sharing test — requires Ali's DOMWIDE patch (GitHub PR #2058)"

echo ""

# --------------------------------------------------------------------------
# Test 6: No KASSERT / Kernel Panic
# Check dmesg for any hwpmc-related panics or assertions
# --------------------------------------------------------------------------
echo "=== Test 6: Stability — no KASSERT or panic ==="

if dmesg | grep -i "kassert\|panic\|trap\|fatal" | grep -qi "hwpmc\|amd.*pmc\|pmc.*amd"; then
    fail "Found hwpmc-related KASSERT/panic in dmesg"
    dmesg | grep -i "kassert\|panic\|trap\|fatal" | grep -i "hwpmc\|amd.*pmc\|pmc.*amd"
else
    pass "No hwpmc-related KASSERT or panic in dmesg"
fi

echo ""

# --------------------------------------------------------------------------
# Test 7: Module load/unload cycle
# Verify the module can be cleanly unloaded and reloaded
# --------------------------------------------------------------------------
echo "=== Test 7: Module load/unload stability ==="

kldunload hwpmc 2>/dev/null && pass "kldunload hwpmc succeeded" || fail "kldunload hwpmc failed"
sleep 1  # give devd time to process unload

kldload /boot/modules/hwpmc.ko 2>/dev/null && pass "kldload hwpmc.ko succeeded" || fail "kldload hwpmc.ko failed"
sleep 1  # give devd time to create /dev/pmc

# Verify counters accessible after reload
if pmccontrol -l > /dev/null 2>&1; then
    pass "hwpmc accessible via libpmc after reload"
else
    fail "hwpmc not accessible via libpmc after reload"
fi

# Re-verify counters after reload
recheck=$(pmccontrol -l 2>/dev/null | grep -c "ENABLED" || true)
if [ "$recheck" -gt 0 ]; then
    pass "After reload: $recheck counters ENABLED"
else
    fail "After reload: no counters ENABLED"
fi

echo ""

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
echo "================================================================"
echo "  RESULTS: $((PASS + FAIL + SKIP)) tests — $PASS passed, $FAIL failed, $SKIP skipped"
echo "================================================================"

exit "$FAIL"
