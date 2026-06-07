# PCIe Sub-NUMA Latency Harness

Measures TX (CPU→NIC) and RX (NIC→CPU) DMA latency using DPDK hardware
timestamps. Sweeps each PCIe slot across all SNC NUMA nodes to identify
which slot + node combination gives lowest latency for a network device.

---

## Prerequisites

| Requirement | Notes |
|---|---|
| DPDK ≥ 21.11 | pkg-config based build assumed |
| Intel NIC (X710/E810) or Mellanox | With HW timestamp support |
| IOMMU enabled in BIOS and kernel | `intel_iommu=on` or `amd_iommu=on` |
| SNC/NPS mode enabled in BIOS | Intel: Sub-NUMA Clustering (SNC2/SNC4). AMD Zen 4+: Nodes Per Socket (NPS2/NPS4). Verify: `numactl --hardware` shows ≥2 nodes per socket |
| External loopback cable | Required for round-trip (loopback) mode |
| Root access | DPDK + VFIO require it |

---

## Directory Layout

```
dpdk-latency-harness/
  src/pcie_latency.c        Main measurement binary
  Makefile
  scripts/
    prepare_system.sh       One-time system setup (hugepages, VFIO, governor)
    run_test.sh             Sweep all slots × SNC nodes
    analyse_results.py      Plot CDFs and heatmap, rank slots
  results/                  Created by run_test.sh
  plots/                    Created by analyse_results.py
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

Reboot and verify either way:

```bash
numactl --hardware
# Intel SNC2/SNC4 or AMD NPS2/NPS4: should show 2 or 4 nodes per socket
```

### Step 2: Kernel boot parameters

Add to `/etc/default/grub` `GRUB_CMDLINE_LINUX`:

```
intel_iommu=on iommu=pt isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7 default_hugepagesz=2M hugepagesz=2M hugepages=1024
```

Update grub and reboot.

### Step 3: Prepare system

```bash
sudo ./scripts/prepare_system.sh
```

This:
- Sets CPU governor to `performance`
- Disables TurboBoost
- Allocates 2MB hugepages
- Loads `vfio-pci`
- Lists NIC candidates with their NUMA nodes

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

# Or use lstopo to see slot↔root-complex mapping
lstopo --output-format console
```

### Step 5: Build

```bash
make
```

### Step 6: Run the sweep

```bash
# Edit SLOTS and NODES to match your system
sudo SLOTS="0000:81:00.0 0000:82:00.0" \
     NODES="0 1 2 3" \
     ./scripts/run_test.sh --n 100000
```

This runs `pcie_latency` once per (slot, SNC node) combination, writing:
- `results/slot_<bdf>_node<N>_tx.csv`  — raw TX latency samples (ns)
- `results/slot_<bdf>_node<N>_rx.csv`  — raw RX latency samples (ns)
- `results/slot_<bdf>_node<N>.log`     — full DPDK output
- `results/summary.csv`               — p50/p99/p999 summary table

### Step 7: Analyse

```bash
pip3 install matplotlib numpy
python3 scripts/analyse_results.py --results-dir ./results --output-dir ./plots
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
sudo numactl --cpunodebind=1 --membind=1 \
    ./pcie_latency -l 0 -a 0000:81:00.0 -- -p 0 -n 100000 -s 1
```

Flags:
```
-p PORT      DPDK port ID (default 0)
-n SAMPLES   number of samples (default 100000)
-s SNC_NODE  expected SNC node — triggers a warning if core is on wrong node
-l           loopback mode (default on, requires external cable)
-T           TX-only measurement (no Rx)
-R           RX-only measurement (no Tx)
```

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
| Low TX+RX on node N, high elsewhere | Slot's root complex is local to node N → use that slot |
| Uniformly high across all nodes | Slot is on a different socket entirely |
| TX low, RX high | DMA write path hitting remote memory — check `numactl --membind` |
| p99 >> p50 | Occasional cross-SNC traffic; verify IRQ affinity and memory pinning |

---

## Caveats

- **HW timestamp support**: Intel E810 (ice), X710 (i40e), and Mellanox CX-5/6
  (mlx5) all support `RTE_MBUF_F_RX_IEEE1588_TMST`. Older NICs may not.
- **IOMMU overhead**: Test with `iommu=pt` (passthrough) to minimise IOMMU
  translation latency in the DMA path.
- **Loopback asymmetry**: External cable loopback adds NIC retransmit latency
  to the round trip. TX and RX figures are measured independently to avoid this.
- **Memory binding is critical**: If DMA buffers land on the wrong NUMA node,
  RX latency will be dominated by cross-SNC memory access (~40–60ns on SPR),
  masking the slot difference. Verify with `/proc/<pid>/numa_maps`.
