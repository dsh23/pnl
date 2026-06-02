#!/usr/bin/env python3
"""
analyse_results.py

Reads raw sample CSVs produced by pcie_latency and generates:
  - Per-slot latency CDFs (Tx and Rx)
  - Summary bar chart: p99 latency by slot × SNC node
  - Annotated summary table
  - Slot ranking by combined Tx+Rx p99

Usage:
    python3 analyse_results.py --results-dir ./results [--output-dir ./plots]
"""

import argparse
import os
import sys
import glob
import csv
import math
import re
from collections import defaultdict

try:
    import numpy as np
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False
    print("WARNING: matplotlib/numpy not found. Text-only output.", file=sys.stderr)


###############################################################################
# Data loading
###############################################################################

def load_samples(path):
    """Load a single-column CSV of nanosecond samples."""
    samples = []
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                samples.append(int(row["sample_ns"]))
            except (KeyError, ValueError):
                pass
    return sorted(samples)


def parse_filename(fname):
    """
    Extract slot and node from filenames like:
      slot_000081000_node1_tx.csv
    Returns (slot_str, node_int, direction_str) or None.
    """
    m = re.search(r"slot_([0-9a-f]+)_node(\d+)_(tx|rx)\.csv", fname)
    if not m:
        return None
    slot_raw, node, direction = m.group(1), int(m.group(2)), m.group(3)
    # Reconstruct BDF: xxxxxxxx → xxxx:xx:xx.x (heuristic)
    s = slot_raw
    bdf = f"{s[0:4]}:{s[4:6]}:{s[6:8]}.{s[8]}" if len(s) >= 9 else slot_raw
    return bdf, node, direction


def load_all(results_dir):
    """Return dict: {(slot, node, direction): [samples]}"""
    data = {}
    for path in sorted(glob.glob(os.path.join(results_dir, "slot_*_node*_*.csv"))):
        parsed = parse_filename(os.path.basename(path))
        if parsed is None:
            continue
        slot, node, direction = parsed
        samples = load_samples(path)
        if samples:
            data[(slot, node, direction)] = samples
    return data


###############################################################################
# Statistics
###############################################################################

def percentile(samples, p):
    """samples must be sorted."""
    if not samples:
        return float("nan")
    idx = min(int(math.ceil(len(samples) * p / 100)) - 1, len(samples) - 1)
    return samples[max(0, idx)]


def summarise(samples):
    return {
        "n":    len(samples),
        "min":  samples[0],
        "p50":  percentile(samples, 50),
        "p90":  percentile(samples, 90),
        "p99":  percentile(samples, 99),
        "p999": percentile(samples, 99.9),
        "max":  samples[-1],
        "mean": sum(samples) // len(samples),
    }


###############################################################################
# Text report
###############################################################################

def print_table(data):
    slots    = sorted(set(k[0] for k in data))
    nodes    = sorted(set(k[1] for k in data))
    print("\n" + "="*80)
    print(f" {'SLOT':<20} {'NODE':>4}  {'DIR':>3}  {'N':>7}  "
          f"{'p50':>7}  {'p99':>7}  {'p999':>8}  {'max':>7}  ns")
    print("="*80)
    for slot in slots:
        for node in nodes:
            for direction in ("tx", "rx"):
                key = (slot, node, direction)
                if key not in data:
                    continue
                s = summarise(data[key])
                print(f" {slot:<20} {node:>4}  {direction:>3}  {s['n']:>7}  "
                      f"{s['p50']:>7}  {s['p99']:>7}  {s['p999']:>8}  {s['max']:>7}")
    print("="*80)


def rank_slots(data):
    """
    Rank slots by p99_tx + p99_rx on their local (lowest-latency) node.
    """
    slot_scores = {}
    for slot in sorted(set(k[0] for k in data)):
        best = float("inf")
        for node in sorted(set(k[1] for k in data)):
            tx = data.get((slot, node, "tx"))
            rx = data.get((slot, node, "rx"))
            if tx and rx:
                score = percentile(tx, 99) + percentile(rx, 99)
                if score < best:
                    best = score
                    slot_scores[slot] = (score, node)
    ranked = sorted(slot_scores.items(), key=lambda x: x[1][0])
    print("\n=== Slot Ranking (by best p99_tx + p99_rx) ===")
    for rank, (slot, (score, node)) in enumerate(ranked, 1):
        print(f"  #{rank}  {slot}  (best on node {node}, combined p99 = {score} ns)")
    return ranked


###############################################################################
# Plots
###############################################################################

def plot_cdf(data, output_dir, direction):
    """CDF per slot, best node only."""
    if not HAS_MATPLOTLIB:
        return

    fig, ax = plt.subplots(figsize=(10, 6))
    slots = sorted(set(k[0] for k in data if k[2] == direction))

    for slot in slots:
        # Find the node with lowest p99
        best_node, best_p99 = None, float("inf")
        for node in sorted(set(k[1] for k in data)):
            s = data.get((slot, node, direction))
            if s:
                p99 = percentile(s, 99)
                if p99 < best_p99:
                    best_p99, best_node = p99, node

        if best_node is None:
            continue

        samples = data[(slot, best_node, direction)]
        cdf_x = samples
        cdf_y = [(i + 1) / len(samples) for i in range(len(samples))]
        ax.plot(cdf_x, cdf_y, label=f"{slot} (node {best_node})", linewidth=1.5)

    ax.set_xlabel("Latency (ns)")
    ax.set_ylabel("CDF")
    ax.set_title(f"{'TX' if direction == 'tx' else 'RX'} Latency CDF — best node per slot")
    ax.set_xlim(left=0)
    ax.set_ylim(0, 1.01)
    ax.axvline(x=0, color="k", linewidth=0.5)
    ax.yaxis.set_major_formatter(ticker.PercentFormatter(xmax=1))
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)

    outpath = os.path.join(output_dir, f"cdf_{direction}.png")
    fig.savefig(outpath, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  CDF plot: {outpath}")


def plot_p99_heatmap(data, output_dir):
    """Heatmap: slot × node, coloured by combined p99 Tx+Rx."""
    if not HAS_MATPLOTLIB:
        return

    slots = sorted(set(k[0] for k in data))
    nodes = sorted(set(k[1] for k in data))

    matrix = np.full((len(slots), len(nodes)), np.nan)

    for i, slot in enumerate(slots):
        for j, node in enumerate(nodes):
            tx = data.get((slot, node, "tx"))
            rx = data.get((slot, node, "rx"))
            if tx and rx:
                matrix[i, j] = percentile(tx, 99) + percentile(rx, 99)

    fig, ax = plt.subplots(figsize=(max(6, len(nodes) * 1.5), max(4, len(slots) * 0.8)))
    im = ax.imshow(matrix, cmap="RdYlGn_r", aspect="auto")
    plt.colorbar(im, ax=ax, label="p99 TX + RX latency (ns)")

    ax.set_xticks(range(len(nodes)))
    ax.set_xticklabels([f"Node {n}" for n in nodes])
    ax.set_yticks(range(len(slots)))
    ax.set_yticklabels(slots, fontsize=8)
    ax.set_xlabel("SNC Node (core pinning)")
    ax.set_ylabel("PCIe Slot")
    ax.set_title("Combined p99 Tx+Rx Latency Heatmap\n(lower = better, green = local root complex)")

    # Annotate cells
    for i in range(len(slots)):
        for j in range(len(nodes)):
            val = matrix[i, j]
            if not np.isnan(val):
                ax.text(j, i, f"{int(val)}", ha="center", va="center",
                        fontsize=7, color="black")

    outpath = os.path.join(output_dir, "heatmap_p99.png")
    fig.tight_layout()
    fig.savefig(outpath, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Heatmap:  {outpath}")


###############################################################################
# Entry point
###############################################################################

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-dir", default="./results",
                        help="Directory containing slot_*_node*_*.csv files")
    parser.add_argument("--output-dir",  default="./plots",
                        help="Directory for output plots")
    args = parser.parse_args()

    if not os.path.isdir(args.results_dir):
        sys.exit(f"ERROR: results directory not found: {args.results_dir}")

    os.makedirs(args.output_dir, exist_ok=True)

    data = load_all(args.results_dir)
    if not data:
        sys.exit("No matching CSV files found. Run pcie_latency first.")

    print(f"Loaded {len(data)} measurement series from {args.results_dir}")

    print_table(data)
    rank_slots(data)

    if HAS_MATPLOTLIB:
        print("\nGenerating plots...")
        plot_cdf(data, args.output_dir, "tx")
        plot_cdf(data, args.output_dir, "rx")
        plot_p99_heatmap(data, args.output_dir)
    else:
        print("\nInstall matplotlib + numpy for plots: pip3 install matplotlib numpy")


if __name__ == "__main__":
    main()
