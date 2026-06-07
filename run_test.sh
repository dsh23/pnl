#!/usr/bin/env bash
# run_test.sh
#
# Sweeps PCIe slots across SNC NUMA nodes and collects latency samples.
# Produces per-slot, per-direction CSV files and a summary table.
#
# Prerequisites:
#   - pcie_latency binary built (make)
#   - prepare_system.sh run (hugepages, VFIO, IOMMU detection)
#   - VFIO driver bound to NIC (dpdk-devbind.py --bind=vfio-pci <BDF>)
#   - External loopback cable fitted
#   - Run as root
#
# Usage:
#   ./run_test.sh [--slots "0000:81:00.0 0000:82:00.0"] [--nodes "0 1 2 3"]
#                [--tx-only] [--rx-only]
#
# Use --tx-only when no loopback cable is fitted.
#
set -euo pipefail

###############################################################################
# Defaults — edit these for your system
###############################################################################

BINARY="./pcie_latency"
SAMPLES=100000
RESULT_DIR="./results"

# PCI slots to test — space-separated BDF addresses
SLOTS="${SLOTS:-"0000:81:00.0 0000:82:00.0"}"

# SNC NUMA nodes to pin the test core to
NODES="${NODES:-"0 1 2 3"}"

# Measurement mode: "" = loopback (default), "-T" = TX only, "-R" = RX only
MEASURE_MODE=""

# Output
SUMMARY="${RESULT_DIR}/summary.csv"

###############################################################################
# Parse arguments
###############################################################################

while [[ $# -gt 0 ]]; do
    case "$1" in
        --slots)   SLOTS="$2";    shift 2 ;;
        --nodes)   NODES="$2";    shift 2 ;;
        --n)       SAMPLES="$2";  shift 2 ;;
        --tx-only) MEASURE_MODE="-T"; shift ;;
        --rx-only) MEASURE_MODE="-R"; shift ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

###############################################################################
# Helpers
###############################################################################

log()  { echo "[$(date '+%H:%M:%S')] $*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }

check_root() {
    [[ $EUID -eq 0 ]] || die "Must run as root (DPDK requires it)"
}

check_hugepages() {
    local nr
    nr=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)
    if [[ $nr -lt 512 ]]; then
        log "WARNING: only $nr 2MB hugepages allocated. DPDK needs ≥512."
        log "Run: echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
    else
        log "Hugepages: $nr x 2MB available"
    fi
}

# Read IOMMU mode written by prepare_system.sh.
# Determines whether to pass --iova-mode=pa to the DPDK EAL.
detect_iommu_mode() {
    if [[ -f /tmp/dpdk_iommu_mode ]]; then
        IOMMU_MODE=$(cat /tmp/dpdk_iommu_mode)
    else
        log "WARNING: /tmp/dpdk_iommu_mode not found. Run prepare_system.sh first."
        log "         Performing inline IOMMU detection..."
        if [[ -d /sys/kernel/iommu_groups ]] && \
           [[ $(ls /sys/kernel/iommu_groups 2>/dev/null | wc -l) -gt 0 ]]; then
            IOMMU_MODE="iommu"
        elif dmesg | grep -qi "iommu.*enabled\|iommu.*remapping\|AMD-Vi\|VT-d" 2>/dev/null; then
            IOMMU_MODE="iommu"
        elif grep -qE "intel_iommu=on|amd_iommu=on" /proc/cmdline 2>/dev/null; then
            IOMMU_MODE="iommu"
        else
            IOMMU_MODE="noiommu"
        fi
    fi

    if [[ "$IOMMU_MODE" == "iommu" ]]; then
        log "IOMMU mode: active — standard vfio-pci, no extra EAL flags"
        EAL_IOVA_FLAG=""
    else
        log "IOMMU mode: absent — no-IOMMU mode, EAL flag: --iova-mode=pa"
        log "  NOTE: no DMA isolation. Results are valid but environment is not hardened."
        EAL_IOVA_FLAG="--iova-mode=pa"
        if [[ -f /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]]; then
            echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
        fi
    fi
}

bind_slot_vfio() {
    local slot="$1"
    local driver
    driver=$(lspci -s "$slot" -k 2>/dev/null | grep "Kernel driver" | awk '{print $NF}')
    if [[ "$driver" != "vfio-pci" ]]; then
        log "Binding $slot to vfio-pci [mode: $IOMMU_MODE] (current: ${driver:-none})"
        dpdk-devbind.py --bind=vfio-pci "$slot" 2>/dev/null \
            || python3 "$(find /usr -name devbind.py 2>/dev/null | head -1)" \
                       --bind=vfio-pci "$slot" \
            || die "Could not bind $slot to vfio-pci. Load vfio-pci module first."
    else
        log "$slot already bound to vfio-pci [mode: $IOMMU_MODE]"
    fi
}

numa_node_of_slot() {
    local slot="$1"
    cat "/sys/bus/pci/devices/${slot}/numa_node" 2>/dev/null || echo "-1"
}

extract_pctile() {
    local f="$1" pct="$2"
    awk -F, 'NR>1 {print $1}' "$f" \
        | sort -n \
        | awk -v p="$pct" '
            BEGIN{n=0}
            {lines[n++]=$1}
            END{idx=int(n*p/100); print lines[idx]}
        ' 2>/dev/null || echo "N/A"
}

###############################################################################
# Main sweep
###############################################################################

main() {
    check_root
    check_hugepages
    detect_iommu_mode

    mkdir -p "$RESULT_DIR"

    # Write summary header
    echo "slot,snc_node,nic_node,local,tx_p50_ns,tx_p99_ns,tx_p999_ns,rx_p50_ns,rx_p99_ns,rx_p999_ns" \
        > "$SUMMARY"

    for slot in $SLOTS; do
        log "=== Testing slot: $slot ==="
        bind_slot_vfio "$slot"

        local_node=$(numa_node_of_slot "$slot")
        log "  NIC NUMA node: $local_node"

        for node in $NODES; do
            log "  --- SNC node: $node ---"

            out_prefix="${RESULT_DIR}/slot_${slot//:/}_node${node}"
            tx_csv="${out_prefix}_tx.csv"
            rx_csv="${out_prefix}_rx.csv"
            log_file="${out_prefix}.log"

            numactl \
                --cpunodebind="$node" \
                --membind="$node" \
                "$BINARY" \
                    -l 0    \
                    ${EAL_IOVA_FLAG:+"$EAL_IOVA_FLAG"} \
                    -a "$slot" \
                    -- \
                    -p 0 \
                    -n "$SAMPLES" \
                    -s "$node" \
                    ${MEASURE_MODE:+"$MEASURE_MODE"} \
                2>&1 | tee "$log_file"

            # Move results to named location
            [[ -f /tmp/tx_samples.csv ]] && mv /tmp/tx_samples.csv "$tx_csv"
            [[ -f /tmp/rx_samples.csv ]] && mv /tmp/rx_samples.csv "$rx_csv"

            tx_p50="N/A"; tx_p99="N/A"; tx_p999="N/A"
            rx_p50="N/A"; rx_p99="N/A"; rx_p999="N/A"

            if [[ -f "$tx_csv" ]]; then
                tx_p50=$(extract_pctile  "$tx_csv" 50)
                tx_p99=$(extract_pctile  "$tx_csv" 99)
                tx_p999=$(extract_pctile "$tx_csv" 99.9)
            fi
            if [[ -f "$rx_csv" ]]; then
                rx_p50=$(extract_pctile  "$rx_csv" 50)
                rx_p99=$(extract_pctile  "$rx_csv" 99)
                rx_p999=$(extract_pctile "$rx_csv" 99.9)
            fi

            is_local=$( [[ "$node" == "$local_node" ]] && echo "yes" || echo "no" )

            echo "${slot},${node},${local_node},${is_local},${tx_p50},${tx_p99},${tx_p999},${rx_p50},${rx_p99},${rx_p999}" \
                >> "$SUMMARY"

            log "  tx p50=${tx_p50}ns p99=${tx_p99}ns | rx p50=${rx_p50}ns p99=${rx_p99}ns (local=${is_local})"
        done
    done

    log ""
    log "=== Summary ==="
    column -t -s, "$SUMMARY"
    log ""
    log "Full results in: $RESULT_DIR"
}

main "$@"
