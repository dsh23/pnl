#!/usr/bin/env bash
# prepare_system.sh
#
# One-time system preparation before running pcie_latency.
# Run as root before first test.
#
set -euo pipefail

HUGEPAGES_2M=${HUGEPAGES_2M:-1024}
HUGEPAGES_1G=${HUGEPAGES_1G:-0}

log() { echo "[prepare] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "Run as root"

###############################################################################
# 1. Disable CPU frequency scaling and C-states
###############################################################################

log "Setting CPU governor to performance..."
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [[ -f "$cpu" ]] && echo performance > "$cpu"
done

log "Disabling deep C-states via /dev/cpu_dma_latency..."
# Keep a persistent fd open — harness also does this internally
exec 9>/dev/cpu_dma_latency
printf '\x00\x00\x00\x00' >&9
log "  /dev/cpu_dma_latency held at 0 (fd 9) for this shell session"

###############################################################################
# 2. Disable CPU boost (vendor-aware)
#
# Three cases in order of preference:
#   Intel:        /sys/devices/system/cpu/intel_pstate/no_turbo
#   AMD Zen4+:    /sys/devices/system/cpu/amd_pstate/cpb_boost  (kernel >= 6.3)
#                 driver modes: active (amd-pstate-epp) / guided (amd-pstate)
#   AMD legacy:   /sys/devices/system/cpu/cpufreq/boost        (acpi-cpufreq)
#                 write 0 to disable (inverted polarity vs Intel)
###############################################################################

disable_boost() {
    # Intel pstate
    if [[ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
        log "Intel TurboBoost disabled (intel_pstate)"
        return
    fi

    # AMD amd-pstate cpb_boost (Zen 4+ with kernel >= 6.3, all driver modes)
    if [[ -f /sys/devices/system/cpu/amd_pstate/cpb_boost ]]; then
        echo "disabled" > /sys/devices/system/cpu/amd_pstate/cpb_boost
        log "AMD Precision Boost disabled (amd_pstate/cpb_boost)"
        return
    fi

    # AMD acpi-cpufreq fallback (older kernels or amd_pstate=disable boot param)
    if [[ -f /sys/devices/system/cpu/cpufreq/boost ]]; then
        echo 0 > /sys/devices/system/cpu/cpufreq/boost
        log "AMD Precision Boost disabled (cpufreq/boost)"
        return
    fi

    log "WARNING: no boost control interface found — boost not disabled."
    log "  Intel: add intel_iommu=on to kernel cmdline and reboot"
    log "  AMD:   kernel >= 6.3 exposes amd_pstate/cpb_boost; older kernels"
    log "         need amd_pstate=passive or amd_pstate=disable in cmdline"
}

disable_boost

###############################################################################
# 3. Hugepages
###############################################################################

log "Configuring hugepages: ${HUGEPAGES_2M} x 2MB, ${HUGEPAGES_1G} x 1GB..."

echo "$HUGEPAGES_2M" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

if [[ $HUGEPAGES_1G -gt 0 ]]; then
    echo "$HUGEPAGES_1G" > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages || \
        log "WARNING: 1G hugepages not supported on this kernel"
fi

# Mount hugetlbfs if not already mounted
if ! mountpoint -q /dev/hugepages; then
    mount -t hugetlbfs nodev /dev/hugepages
    log "Mounted hugetlbfs at /dev/hugepages"
fi

actual=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
log "  Allocated: ${actual} x 2MB hugepages"

###############################################################################
# 4. VFIO kernel modules
###############################################################################

log "Loading VFIO modules..."
modprobe vfio
modprobe vfio-pci
modprobe vfio_iommu_type1 allow_unsafe_interrupts=1 2>/dev/null || true
log "  vfio-pci loaded"

###############################################################################
# 5. IOMMU detection — run with or without IOMMU
#
# Two operating modes:
#   IOMMU present:  standard vfio-pci + vfio_iommu_type1
#                   Bind: dpdk-devbind.py --bind=vfio-pci <BDF>
#
#   IOMMU absent:   vfio-pci in no-IOMMU mode (VFIO_NOIOMMU)
#                   Requires: echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
#                   DPDK EAL flag at runtime: --iova-mode=pa
#
# Detected mode written to /tmp/dpdk_iommu_mode for use by run_test.sh
###############################################################################

log "Detecting IOMMU..."

IOMMU_ACTIVE=false

# Check 1: IOMMU groups populated (most reliable)
if [[ -d /sys/kernel/iommu_groups ]] && \
   [[ $(ls /sys/kernel/iommu_groups 2>/dev/null | wc -l) -gt 0 ]]; then
    IOMMU_ACTIVE=true
    log "  IOMMU detected: /sys/kernel/iommu_groups populated"
fi

# Check 2: dmesg — Intel VT-d or AMD-Vi
if [[ "$IOMMU_ACTIVE" == "false" ]]; then
    if dmesg | grep -qi "iommu.*enabled\|iommu.*remapping\|AMD-Vi\|DMAR.*IOMMU\|VT-d" 2>/dev/null; then
        IOMMU_ACTIVE=true
        log "  IOMMU detected: dmesg shows IOMMU initialisation"
    fi
fi

# Check 3: kernel command line
if [[ "$IOMMU_ACTIVE" == "false" ]]; then
    if grep -qE "intel_iommu=on|amd_iommu=on" /proc/cmdline 2>/dev/null; then
        IOMMU_ACTIVE=true
        log "  IOMMU detected: enabled via kernel command line"
    fi
fi

if [[ "$IOMMU_ACTIVE" == "true" ]]; then
    log "  Mode: IOMMU active — using standard vfio-pci + vfio_iommu_type1"
    modprobe vfio_iommu_type1 allow_unsafe_interrupts=1 2>/dev/null || true
    echo "iommu" > /tmp/dpdk_iommu_mode
    log "  DMA isolation: enabled"
    log "  Runtime EAL flag: none required"
else
    log "  Mode: IOMMU NOT detected — enabling vfio-pci no-IOMMU mode"
    log "  WARNING: no DMA isolation. Acceptable in a trusted lab environment."
    log "           To enable IOMMU: add intel_iommu=on (or amd_iommu=on) iommu=pt"
    log "           to kernel command line and reboot. Then re-run this script."

    if [[ -f /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]]; then
        echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
        log "  /sys/module/vfio/parameters/enable_unsafe_noiommu_mode = 1"
    else
        log "  WARNING: enable_unsafe_noiommu_mode not available in this kernel."
        log "           Rebuild kernel with CONFIG_VFIO_NOIOMMU=y, or enable IOMMU."
    fi

    echo "noiommu" > /tmp/dpdk_iommu_mode
    log "  Runtime EAL flag required: --iova-mode=pa"
fi

log "  IOMMU mode written to: /tmp/dpdk_iommu_mode (read by run_test.sh)"

###############################################################################
# 6. IRQ affinity advisory
###############################################################################

log "IRQ affinity: recommend isolating test cores via isolcpus= kernel param"
log "  Example: isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7"
log "  Current: $(cat /proc/cmdline | grep -o 'isolcpus=[^ ]*' || echo 'not set')"

###############################################################################
# 7. Show NUMA topology (Intel SNC / AMD NPS)
#
# Intel: Sub-NUMA Clustering (SNC2 / SNC4) — set in BIOS
# AMD:   Nodes Per Socket  (NPS1 / NPS2 / NPS4) — set in BIOS
#        Zen 4 (Genoa/EPYC 9004): NPS4 creates 4 NUMA domains per socket,
#        each mapped to one IOD quadrant. PCIe devices are local to the NUMA
#        domain whose IOD quadrant owns the root complex for that slot.
#        Verify with: numactl --hardware (expect >= 2 nodes per socket for
#        NPS2/NPS4, or >= 4 for NPS4)
###############################################################################

detect_cpu_vendor() {
    if grep -q "AuthenticAMD" /proc/cpuinfo 2>/dev/null; then
        echo "amd"
    elif grep -q "GenuineIntel" /proc/cpuinfo 2>/dev/null; then
        echo "intel"
    else
        echo "unknown"
    fi
}

VENDOR=$(detect_cpu_vendor)

log ""
log "=== Topology (vendor: ${VENDOR}) ==="
if [[ "$VENDOR" == "amd" ]]; then
    log "AMD NPS mode — check BIOS: DF Common Options → Memory Addressing → NPS"
    log "  NPS1 = 1 NUMA node/socket (default)"
    log "  NPS2 = 2 NUMA nodes/socket"
    log "  NPS4 = 4 NUMA nodes/socket (each IOD quadrant = 1 domain)"
    log "  PCIe device locality follows its IOD quadrant's NUMA domain"
elif [[ "$VENDOR" == "intel" ]]; then
    log "Intel SNC mode — check BIOS: Memory → Sub-NUMA Clustering → SNC2/SNC4"
fi
numactl --hardware
echo ""

if command -v lstopo &>/dev/null; then
    log "=== Hardware locality (lstopo) ==="
    lstopo --output-format console 2>/dev/null | head -40 || true
fi

###############################################################################
# 8. List PCI NIC candidates
###############################################################################

log ""
log "=== Network devices and their NUMA nodes ==="
for dev in /sys/bus/pci/devices/*/; do
    bdf=$(basename "$dev")
    class=$(cat "$dev/class" 2>/dev/null || echo "")
    if [[ "$class" == "0x020000" || "$class" == "0x020001" ]]; then
        node=$(cat "$dev/numa_node" 2>/dev/null || echo "?")
        driver=$(cat "$dev/uevent" 2>/dev/null | grep DRIVER | cut -d= -f2 || echo "?")
        printf "  %-16s  NUMA node: %2s  driver: %s\n" "$bdf" "$node" "$driver"
    fi
done

log ""
log "System prepared. You can now run: ./run_test.sh"
