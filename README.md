# PCIe Sub-NUMA Latency Harness

Measures TX (CPU→NIC) and RX (NIC→CPU) DMA latency using DPDK hardware
timestamps. Tests each PCIe slot across all SNC NUMA nodes to identify
which slot + node combination gives lowest latency for a network device.

---

## Prerequisites

| Requirement | Notes |
|---|---|
| DPDK ≥ 21.11 | pkg-config based build assumed |
| Intel NIC (X710/E810) or Mellanox | With HW timestamp support |
| SNC mode enabled in BIOS | Verify: `numactl --hardware` shows ≥2 nodes per socket |
| External loopback cable | Required for round-trip (loopback) mode |
| Root access | DPDK + VFIO require it |

IOMMU is **not required**. `prepare_system.sh` detects whether it is active
and configures VFIO accordingly. See the IOMMU section below.

---

## Directory Layout

```
generic/
  pcie_latency.c        Main measurement binary
  Makefile
  prepare_system.sh     One-time system setup (hugepages, VFIO, governor)
  run_test.sh           Sweep all slots × SNC nodes
  analyse_results.py    Plot CDFs and heatmap, rank slots
  results/              Created by run_test.sh
  plots/                Created by analyse_results.py
```

---

## End-to-End Workflow

### Step 1: Enable SNC in BIOS

On Intel Sapphire Rapids/Ice Lake:
- BIOS → Memory → Sub-NUMA Clustering → **SNC2** or **SNC4**
- Reboot and verify:

```bash
numactl --hardware
# Should show 2 or 4 nodes per socket
```

Without SNC enabled all slots appear on node 0 and the sweep will not
produce meaningful cross-node comparisons.

### Step 2: Kernel boot parameters

Add to `/etc/default/grub` `GRUB_CMDLINE_LINUX` and update grub:

```
isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7 default_hugepagesz=2M hugepagesz=2M hugepages=1024
```

Replace `4-7` with the cores you intend to pin the measurement thread to.
`isolcpus` prevents OS tasks from landing on those cores mid-measurement.

**IOMMU (optional):** if your platform supports it and you want full DMA
isolation, also add:

```
intel_iommu=on iommu=pt
```

`iommu=pt` (passthrough) avoids IOMMU address translation overhead in the
DMA path. If you do not add these parameters the harness will run in
no-IOMMU mode automatically — see the IOMMU section below.

Reboot after changing these parameters.

### Step 3: Prepare system

```bash
sudo ./prepare_system.sh
```

This:
- Sets CPU governor to `performance`
- Disables TurboBoost (Intel: `intel_pstate/no_turbo`)
- Suppresses deep C-states via `/dev/cpu_dma_latency`
- Allocates 2MB hugepages and mounts `hugetlbfs`
- **Detects IOMMU** and configures VFIO in the appropriate mode
- Writes the detected mode to `/tmp/dpdk_iommu_mode` for use by `run_test.sh`
- Displays NUMA topology and lists NIC candidates with their NUMA nodes

#### IOMMU detection

The script uses three independent checks to determine whether the IOMMU is active:

1. `/sys/kernel/iommu_groups` is populated
2. `dmesg` contains IOMMU initialisation messages (VT-d / AMD-Vi)
3. `intel_iommu=on` or `amd_iommu=on` is present in `/proc/cmdline`

**With IOMMU active** — standard `vfio-pci` is used. Full DMA isolation is
enforced by the hardware. No extra DPDK flags are needed.

```
[prepare] IOMMU detected: /sys/kernel/iommu_groups populated
[prepare] Mode: IOMMU active — using standard vfio-pci + vfio_iommu_type1
```

**Without IOMMU** — `vfio-pci` is configured in no-IOMMU mode by writing to
`/sys/module/vfio/parameters/enable_unsafe_noiommu_mode`. The harness runs
normally; there is no hardware enforcement preventing the NIC from accessing
arbitrary host memory, so this mode should only be used in a trusted lab
environment. `run_test.sh` automatically adds `--iova-mode=pa` to the DPDK
EAL arguments when this mode is detected.

```
[prepare] Mode: IOMMU NOT detected — enabling vfio-pci no-IOMMU mode
[prepare] WARNING: no DMA isolation. Acceptable in a trusted lab environment.
[prepare] Runtime EAL flag required: --iova-mode=pa
```

### Step 4: Identify PCIe slots

```bash
# Show all NICs with NUMA node affinity
for dev in /sys/bus/pci/devices/*/; do
    bdf=$(basename "$dev")
    class=$(cat "$dev/class" 2>/dev/null)
    if [[ "$class" == "0x020000" ]]; then
        node=$(cat "$dev/numa_node")
        echo "$bdf  node=$node"
    fi
done

# Visual slot↔root-complex mapping
lstopo --output-format console
```

### Step 5: Build

```bash
make
```

If `pkg-config` cannot find DPDK, set `RTE_SDK`:

```bash
RTE_SDK=/opt/dpdk RTE_TARGET=x86_64-native-linuxapp-gcc make
```

### Step 6: Run the sweep

```bash
sudo SLOTS="0000:81:00.0 0000:82:00.0" \
     NODES="0 1 2 3" \
     ./run_test.sh --n 100000
```

`run_test.sh` reads `/tmp/dpdk_iommu_mode` written by `prepare_system.sh`
and automatically passes the correct EAL flags for whichever mode is active.
No manual intervention is needed.

For each (slot, SNC node) combination the script writes:
- `results/slot_<bdf>_node<N>_tx.csv`  — raw TX latency samples (ns)
- `results/slot_<bdf>_node<N>_rx.csv`  — raw RX latency samples (ns)
- `results/slot_<bdf>_node<N>.log`     — full DPDK output
- `results/summary.csv`               — p50/p99/p999 summary table

### Step 7: Analyse

```bash
pip3 install matplotlib numpy
python3 analyse_results.py --results-dir ./results --output-dir ./plots
```

Produces:
- `plots/cdf_tx.png`      — TX latency CDF per slot (best node)
- `plots/cdf_rx.png`      — RX latency CDF per slot (best node)
- `plots/heatmap_p99.png` — Combined p99 heatmap (slot × SNC node)
- Console: slot ranking by combined Tx+Rx p99

---

## Manual Single Run

To test one slot on one SNC node directly:

```bash
# IOMMU active
sudo numactl --cpunodebind=1 --membind=1 \
    ./pcie_latency -l 0 -a 0000:81:00.0 -- -p 0 -n 100000 -s 1

# No IOMMU
sudo numactl --cpunodebind=1 --membind=1 \
    ./pcie_latency -l 0 --iova-mode=pa -a 0000:81:00.0 -- -p 0 -n 100000 -s 1
```

Application flags (after `--`):

| Flag | Default | Description |
|---|---|---|
| `-p PORT` | `0` | DPDK port ID |
| `-n SAMPLES` | `100000` | Number of packets to measure |
| `-s SNC_NODE` | (none) | Expected SNC node — warns if core is on a different node |
| `-l` | on | Loopback mode, requires external cable |
| `-T` | off | TX-only (no RX measurement) |
| `-R` | off | RX-only (no TX measurement) |

---

## What the Latencies Mean

### TX latency (CPU → NIC)
```
SW descriptor post (TSC)
  → PCIe write: descriptor → NIC Tx ring
    → NIC fetches descriptor
      → NIC fetches packet data (DMA read)
        → NIC transmits frame
          ← HW TX timestamp
```
Measured as: `hw_tx_timestamp_ns − sw_post_tsc_ns`

### RX latency (NIC → CPU)
```
Packet arrives at NIC
  → HW RX timestamp recorded
    → NIC DMA writes packet to memory (host buffer)
      → PCIe write: completion → host memory
        → Core polls Rx ring
          ← SW poll TSC
```
Measured as: `sw_poll_tsc_ns − hw_rx_timestamp_ns`

---

## Interpreting Results

| Pattern | Meaning |
|---|---|
| Low TX+RX on node N, high elsewhere | Slot's root complex is local to node N → use that slot with cores pinned to node N |
| Uniformly high across all nodes | Slot is on a different socket entirely |
| TX low, RX high | DMA write path hitting remote memory — check `numactl --membind` |
| p99 >> p50 | Occasional cross-SNC traffic; verify IRQ affinity and C-state suppression |
| All nodes identical | SNC not active in BIOS — enable SNC2/SNC4 and reboot |

---

## Caveats

- **HW timestamp support**: Intel E810 (ice), X710 (i40e), and Mellanox CX-5/6
  (mlx5) support `RTE_MBUF_F_RX_IEEE1588_TMST`. Older NICs may not — the
  harness will warn at startup if the offload is unavailable.
- **Memory binding is critical**: If DMA buffers land on the wrong NUMA node,
  RX latency will be dominated by cross-SNC memory access (~40–60ns on SPR),
  masking the slot difference. Verify with `/proc/<pid>/numa_maps`.
- **Loopback asymmetry**: External cable loopback adds NIC retransmit latency
  to the round trip. TX and RX figures are measured independently to avoid this.
- **No-IOMMU mode**: results are measurement-valid but there is no DMA
  isolation. Only use in a controlled lab environment.
