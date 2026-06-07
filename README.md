# PCIe Sub-NUMA Latency Harness

Measures TX (CPU→NIC) and RX (NIC→CPU) DMA latency using DPDK timestamps.
Sweeps each PCIe slot across all SNC/NPS NUMA nodes to identify which slot +
node combination gives lowest latency for a network device.

---

## Prerequisites

| Requirement | Notes |
|---|---|
| DPDK 24.11 | pkg-config based build assumed. Minimum 21.11. |
| Solarflare X2522, Intel X710/E810, or Mellanox CX-5/6 | See NIC notes below |
| IOMMU enabled in BIOS and kernel | `intel_iommu=on` or `amd_iommu=on` |
| SNC/NPS mode enabled in BIOS | Intel: SNC2/SNC4. AMD Zen 4+: NPS2/NPS4. Verify: `numactl --hardware` shows ≥2 nodes per socket |
| Cable for RX measurement | Either a loopback module per port, or one patch cable connecting port .0 to port .1 on the same card. Not required for TX-only mode. |
| Root access | DPDK + VFIO require it |

### NIC notes

**Solarflare X2522 (sfc PMD)**
- HW Rx timestamps supported via `RTE_ETH_RX_OFFLOAD_TIMESTAMP` dynfield path
- HW Tx timestamps not supported (`timesync_read_tx_timestamp` not implemented)
  — TX latency is measured as TSC(post-burst) − TSC(pre-burst), which captures
  PCIe descriptor write round-trip time. Relative slot×node comparisons are valid.
- `rte_eth_read_clock()` not supported — NIC clock unavailable, all measurements
  use TSC only in practice
- Requires `modprobe -r sfc` before binding to vfio-pci

**Intel E810 (ice) / X710 (i40e)**
- Full HW TX and RX timestamp support via IEEE 1588 path

**Mellanox CX-5/6 (mlx5)**
- HW Rx timestamps via dynfield path; requires `rdma-core` and `libibverbs`

---

## Directory Layout

```
  pcie_latency.c        Main measurement binary
  Makefile
  prepare_system.sh     One-time system setup (hugepages, VFIO, governor)
  run_test.sh           Sweep all slots × SNC/NPS nodes
  analyse_results.py    Plot CDFs and heatmap, rank slots
  results/              Created by run_test.sh
  plots/                Created by analyse_results.py
```

---

## End-to-End Workflow

### Step 1: Enable SNC/NPS in BIOS

**Intel (Sapphire Rapids / Ice Lake):**
- BIOS → Memory → Sub-NUMA Clustering → **SNC2** or **SNC4**

**AMD Zen 4+ (EPYC 9004 / Threadripper 7000):**
- BIOS → AMD CBS → DF Common Options → Memory Addressing → **NPS2** or **NPS4**
- Under NPS4, the IOD is divided into four quadrants, each a separate NUMA domain.
  PCIe devices are local to the domain whose IOD quadrant owns the root complex for
  that slot — use `cat /sys/bus/pci/devices/<bdf>/numa_node` to confirm.

Reboot and verify:

```bash
numactl --hardware
# Should show 2 or 4 nodes per socket with SNC2/SNC4 or NPS2/NPS4
```

### Step 2: Kernel boot parameters

Add to `/etc/default/grub` `GRUB_CMDLINE_LINUX`:

**Intel:**
```
intel_iommu=on iommu=pt isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7 default_hugepagesz=2M hugepagesz=2M hugepages=1024
```

**AMD:**
```
amd_iommu=on iommu=pt isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7 default_hugepagesz=2M hugepagesz=2M hugepages=1024
```

Update grub and reboot.

### Step 3: Install DPDK (RHEL 9)

```bash
sudo subscription-manager repos \
    --enable rhel-9-for-x86_64-appstream-rpms \
    --enable codeready-builder-for-rhel-9-x86_64-rpms

sudo dnf install -y dpdk dpdk-devel dpdk-tools \
    gcc make pkg-config numactl numactl-devel \
    hwloc hwloc-gui pciutils

# Verify pkg-config finds DPDK
pkg-config --modversion libdpdk
```

### Step 4: Prepare system

```bash
sudo ./prepare_system.sh
```

This sets CPU governor, disables CPU boost (Intel and AMD), allocates hugepages,
loads vfio modules, detects IOMMU mode, and lists all NICs with their NUMA nodes,
drivers, and vendor names.

**Solarflare note:** the sfc driver does not report itself via `uevent`; the script
uses the sysfs `driver` symlink instead, so Solarflare devices will show correctly.

### Step 5: Identify PCIe slots

The `prepare_system.sh` output lists all NICs. Alternatively:

```bash
# Show all NICs with NUMA node and driver
for dev in /sys/bus/pci/devices/*/; do
    bdf=$(basename "$dev")
    class=$(cat "$dev/class" 2>/dev/null)
    if [[ "$class" == "0x020000" ]]; then
        node=$(cat "$dev/numa_node")
        driver=$( [[ -L "$dev/driver" ]] && basename "$(readlink "$dev/driver")" || echo "unbound" )
        echo "$bdf  node=$node  driver=$driver"
    fi
done

# Or use lstopo to see slot↔root-complex mapping
lstopo --output-format console
```

### Step 6: Bind NICs to vfio-pci

**Solarflare (unload kernel driver first):**
```bash
sudo modprobe -r sfc
sudo dpdk-devbind.py --bind=vfio-pci 0000:f2:00.0 0000:f2:00.1
```

**Intel / Mellanox:**
```bash
sudo dpdk-devbind.py --bind=vfio-pci 0000:81:00.0
```

Confirm binding:
```bash
dpdk-devbind.py --status
```

### Step 7: Build

```bash
make
```

### Step 8: Run the sweep

**Two-port mode** (patch cable between port .0 and port .1 on each card —
recommended, gives full TX + RX measurements):

```bash
sudo SLOTS="0000:f2:00.0" NODES="0 1 2 3 4 5 6 7 8" \
    ./run_test.sh --n 100000 --two-port
```

`run_test.sh` automatically derives the RX port BDF by replacing `.0` with `.1`
and binds it to vfio-pci.

**TX-only mode** (no cable required):

```bash
sudo SLOTS="0000:f2:00.0" NODES="0 1 2 3 4 5 6 7 8" \
    ./run_test.sh --n 100000 --tx-only
```

**Multiple slots** (sweeps all slots × all nodes):

```bash
sudo SLOTS="0000:62:00.0 0000:91:00.0 0000:e1:00.0 0000:f2:00.0" \
     NODES="0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15" \
    ./run_test.sh --n 100000 --two-port
```

Each run writes:
- `results/slot_<bdf>_node<N>_tx.csv`  — raw TX latency samples (ns)
- `results/slot_<bdf>_node<N>_rx.csv`  — raw RX latency samples (ns)
- `results/slot_<bdf>_node<N>.log`     — full DPDK output
- `results/summary.csv`               — p50/p99/p999 summary table

### Step 9: Analyse

```bash
pip3 install matplotlib numpy
python3 analyse_results.py --results-dir ./results --output-dir ./plots
```

Produces:
- `plots/cdf_tx.png`      — TX latency CDF per slot (best node)
- `plots/cdf_rx.png`      — RX latency CDF per slot (best node)
- `plots/heatmap_p99.png` — Combined p99 heatmap (slot × SNC/NPS node)
- Console: slot ranking by combined TX+RX p99

---

## Manual Single Run

**Two-port (patch cable between .0 and .1):**
```bash
sudo numactl --cpunodebind=8 --membind=8 \
    ./pcie_latency -l 0 -a 0000:f2:00.0 -a 0000:f2:00.1 \
    -- -p 0 -q 1 -n 100000 -s 8
```

**TX-only (no cable):**
```bash
sudo numactl --cpunodebind=8 --membind=8 \
    ./pcie_latency -l 0 -a 0000:f2:00.0 \
    -- -p 0 -n 100000 -s 8 -T
```

### Flags

```
-p TX_PORT   DPDK TX port ID (default 0)
-q RX_PORT   DPDK RX port ID for two-port mode (e.g. -p 0 -q 1)
             When -q is given TX and RX use separate ports.
             Without -q single-port loopback mode is used.
-n SAMPLES   number of samples (default 100000)
-s SNC_NODE  expected SNC/NPS node — warns if core is on wrong node
-T           TX-only (no RX measurement, no cable needed)
-R           RX-only (no TX measurement)
```

---

## What the Latencies Mean

### TX latency (CPU → NIC)

**With HW TX timestamps (Intel E810/X710):**
```
SW descriptor post (TSC)
  → PCIe write: descriptor → NIC Tx ring
    → NIC fetches descriptor + packet data (DMA read)
      → NIC transmits frame
        ← HW TX timestamp
```
Measured as: `hw_tx_timestamp_ns − sw_post_tsc_ns`

**Without HW TX timestamps (Solarflare X2522):**
```
SW pre-burst TSC
  → rte_eth_tx_burst() — descriptor posted, PCIe write completes
    ← SW post-burst TSC
```
Measured as: `tsc_post_burst_ns − tsc_pre_burst_ns`

This captures the PCIe descriptor write round-trip as seen by software.
Absolute values differ from the HW path but relative slot×node comparisons
are equally valid for identifying the local root complex.

### RX latency (NIC → CPU)

```
Packet arrives at NIC
  → HW RX timestamp recorded (via RTE_MBUF_DYNFIELD_TIMESTAMP_NAME)
    → NIC DMA writes packet to host memory
      → PCIe write completion → host memory
        → Core polls Rx ring
          ← SW poll TSC
```
Measured as: `sw_poll_tsc_ns − hw_rx_timestamp_ns`

---

## Interpreting Results

| Pattern | Meaning |
|---|---|
| Low TX+RX on node N, high elsewhere | Slot's root complex is local to node N → use that slot |
| Uniformly high across all nodes | Slot is on a different socket entirely |
| TX low, RX high | DMA write path hitting remote memory — check `numactl --membind` |
| p99 >> p50 | Occasional cross-SNC/NPS traffic; verify IRQ affinity and memory pinning |

---

## Caveats

- **Solarflare X2522 clock**: `rte_eth_read_clock()` is not supported by the sfc
  PMD, so NIC-clock-based latency conversion is unavailable. All measurements fall
  back to TSC. HW RX timestamps from the dynfield are still recorded if the PMD
  supports `RTE_ETH_RX_OFFLOAD_TIMESTAMP`, but cannot be converted to wall-clock ns
  without a NIC clock reference, so RX samples may be zero in pure sfc mode.
- **IOMMU overhead**: Test with `iommu=pt` (passthrough) to minimise IOMMU
  translation latency in the DMA path.
- **Two-port vs loopback asymmetry**: With a patch cable between ports, TX and RX
  paths are independent and measured separately, which avoids loopback retransmit
  latency being added to the round trip.
- **Memory binding is critical**: If DMA buffers land on the wrong NUMA node,
  RX latency will be dominated by cross-SNC/NPS memory access (~40–60 ns on SPR),
  masking the slot difference. Verify with `/proc/<pid>/numa_maps`.
- **AMD NPS**: Under NPS4 each IOD quadrant is a separate NUMA domain. PCIe device
  locality follows whichever quadrant owns the root complex for that slot. Run
  `numactl --hardware` to confirm node count and check
  `/sys/bus/pci/devices/<bdf>/numa_node` to identify the local domain for each slot.
