/*
 * pcie_latency.c
 *
 * PCIe latency measurement harness for sub-NUMA cluster evaluation.
 *
 * Measures Tx (CPU→NIC) and Rx (NIC→CPU) DMA latency using DPDK
 * hardware timestamps where available. Pin this process to a specific
 * SNC/NPS domain using numactl before running.
 *
 * Build:  see Makefile
 * Run:    see run_test.sh
 *
 * Methodology:
 *   Tx latency = sw_post_burst_tsc - sw_descriptor_post_tsc
 *     (TSC-only: measures time from descriptor write to burst return,
 *      i.e. the PCIe descriptor write round-trip visible to SW.
 *      NICs that do not implement timesync TX — e.g. sfc/X2522 — use
 *      this path. Relative slot×node comparisons remain valid.)
 *
 *   Rx latency = sw_poll_completion_tsc - hw_rx_timestamp
 *     (HW Rx timestamp via RTE_MBUF_DYNFIELD_TIMESTAMP_NAME, enabled
 *      when NIC advertises RTE_ETH_RX_OFFLOAD_TIMESTAMP. Falls back to
 *      SW-only TSC if the NIC does not support it.)
 *
 * Clock conversion uses rte_eth_read_clock() to calibrate NIC→ns.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <getopt.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mbuf_dyn.h>
#include <rte_mempool.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_malloc.h>


/* --------------------------------------------------------------------------
 * Configuration constants
 * -------------------------------------------------------------------------- */

#define APP_NAME            "pcie_latency"
#define MAX_SAMPLES         (1 << 20)   /* 1M samples */
#define DEFAULT_SAMPLES     100000
#define DEFAULT_BURST       1           /* single-packet for latency */
#define MEMPOOL_CACHE_SZ    256
#define NUM_RX_DESC         512
#define NUM_TX_DESC         512
#define PKT_DATA_LEN        64          /* minimum Ethernet frame payload */
#define WARMUP_PKTS         10000
#define HISTO_BUCKETS       128
#define HISTO_MAX_NS        10000       /* 10 µs ceiling */

/* --------------------------------------------------------------------------
 * Percentile / histogram structures
 * -------------------------------------------------------------------------- */

typedef struct {
    uint64_t samples[MAX_SAMPLES];
    uint32_t count;
} sample_buf_t;

typedef struct {
    uint64_t p50, p90, p99, p999;
    uint64_t min, max, mean;
    uint64_t histo[HISTO_BUCKETS];   /* ns buckets, width = HISTO_MAX_NS/HISTO_BUCKETS */
} latency_stats_t;

/* --------------------------------------------------------------------------
 * Global state
 * -------------------------------------------------------------------------- */

static volatile bool g_running = true;

static struct {
    uint16_t port_id;           /* TX port (always used) */
    uint16_t rx_port_id;        /* RX port (two-port mode: .0 TX, .1 RX) */
    bool     two_port;          /* true when -q is supplied */
    uint32_t n_samples;
    uint32_t burst;
    bool     loopback;          /* single-port external loopback cable */
    bool     oneway_tx;         /* measure Tx only */
    bool     oneway_rx;         /* measure Rx only */
    int      snc_node;          /* expected NUMA/SNC node for validation */
} g_cfg = {
    .port_id    = 0,
    .rx_port_id = 1,
    .two_port   = false,
    .n_samples  = DEFAULT_SAMPLES,
    .burst      = DEFAULT_BURST,
    .loopback   = true,
    .snc_node   = -1,
};

static struct rte_mempool *g_pktmbuf_pool;
static sample_buf_t        g_tx_samples;
static sample_buf_t        g_rx_samples;

/* Clock conversion state */
static uint64_t g_hz_tsc;          /* TSC frequency */
static uint64_t g_hz_nic;          /* NIC clock frequency */

/*
 * Dynfield offset for RTE_MBUF_DYNFIELD_TIMESTAMP_NAME.
 * Registered at port init time when RTE_ETH_RX_OFFLOAD_TIMESTAMP
 * is available. -1 means HW Rx timestamps are not available.
 */
static int      g_ts_dynfield_offset = -1;
static uint64_t g_ts_dynflag_mask   = 0;  /* ol_flags bit for RX timestamp */

/* Runtime capability flags set during port_init */
static bool     g_hw_rx_ts = false;   /* NIC supports HW Rx timestamps */
static bool     g_hw_tx_ts = false;   /* NIC supports timesync TX path */

/* --------------------------------------------------------------------------
 * Signal handler
 * -------------------------------------------------------------------------- */

static void sig_handler(int sig)
{
    (void)sig;
    g_running = false;
}

/* --------------------------------------------------------------------------
 * Topology validation
 * -------------------------------------------------------------------------- */

/*
 * Detect CPU vendor by reading /proc/cpuinfo.
 * Returns 'A' for AMD, 'I' for Intel, '?' for unknown.
 */
static char detect_cpu_vendor(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return '?';

    char line[128];
    char vendor = '?';
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "vendor_id", 9) == 0) {
            if (strstr(line, "AuthenticAMD"))      vendor = 'A';
            else if (strstr(line, "GenuineIntel")) vendor = 'I';
            break;
        }
    }
    fclose(f);
    return vendor;
}

/*
 * Read the NPS mode from /sys for AMD Zen 4+ systems.
 * Returns the number of NUMA nodes exposed per socket (1/2/4),
 * or -1 if it cannot be determined.
 *
 * Under NPS4 each IOD quadrant is a separate NUMA domain and PCIe
 * devices are local to the domain whose quadrant owns the root complex.
 */
static int amd_detect_nps(void)
{
    /* numactl exposes total nodes; derive NPS by dividing by socket count */
    int total_nodes = 0;
    int total_sockets = 0;

    /* Count NUMA nodes */
    FILE *f = fopen("/sys/devices/system/node/possible", "r");
    if (f) {
        int lo, hi;
        if (fscanf(f, "%d-%d", &lo, &hi) == 2)
            total_nodes = hi - lo + 1;
        else if (fscanf(f, "%d", &lo) == 1)
            total_nodes = 1;
        fclose(f);
    }

    /* Count physical sockets */
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[128];
        int max_pkg = -1;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "physical id", 11) == 0) {
                int id;
                if (sscanf(line, "physical id : %d", &id) == 1 && id > max_pkg)
                    max_pkg = id;
            }
        }
        fclose(f);
        if (max_pkg >= 0) total_sockets = max_pkg + 1;
    }

    if (total_sockets > 0 && total_nodes > 0)
        return total_nodes / total_sockets;
    return -1;
}

static void validate_topology(uint16_t tx_port_id, uint16_t rx_port_id)
{
    int nic_node  = rte_eth_dev_socket_id(tx_port_id);
    int core_node = rte_socket_id();
    char vendor   = detect_cpu_vendor();

    printf("\n[Topology]\n");
    printf("  CPU vendor     : %s\n",
           vendor == 'A' ? "AMD" : vendor == 'I' ? "Intel" : "Unknown");
    if (g_cfg.two_port) {
        printf("  TX port        : %u (NUMA node %d)\n",
               tx_port_id, rte_eth_dev_socket_id(tx_port_id));
        printf("  RX port        : %u (NUMA node %d)\n",
               rx_port_id, rte_eth_dev_socket_id(rx_port_id));
    } else {
        printf("  NIC NUMA node  : %d\n", nic_node);
    }
    printf("  Core NUMA node : %d\n", core_node);

    /* AMD-specific: report NPS mode and IOD quadrant locality context */
    if (vendor == 'A') {
        int nps = amd_detect_nps();
        if (nps > 0) {
            printf("  AMD NPS mode   : NPS%d (%d NUMA node%s per socket)\n",
                   nps, nps, nps > 1 ? "s" : "");
            if (nps >= 2) {
                printf("  Note           : Under NPS%d each IOD quadrant is a "
                       "separate NUMA domain.\n"
                       "                   PCIe device locality is determined by "
                       "which quadrant owns\n"
                       "                   the root complex for that slot. Check "
                       "/sys/bus/pci/devices/<bdf>/numa_node\n", nps);
            }
        } else {
            printf("  AMD NPS mode   : could not determine (check numactl --hardware)\n");
        }
    }

    /* Validate expected node from -s flag (Intel SNC node / AMD NPS domain) */
    if (g_cfg.snc_node >= 0) {
        printf("  Expected node  : %d\n", g_cfg.snc_node);
        if (core_node != g_cfg.snc_node) {
            fprintf(stderr,
                "WARN: core is on node %d but expected node %d. "
                "Re-run with numactl --cpunodebind=%d --membind=%d\n",
                core_node, g_cfg.snc_node,
                g_cfg.snc_node, g_cfg.snc_node);
        }
    }

    if (nic_node != core_node) {
        fprintf(stderr,
            "WARN: NIC is on NUMA node %d but core is on node %d. "
            "Cross-%s path — results will reflect remote root complex latency.\n",
            nic_node, core_node,
            vendor == 'A' ? "NPS domain (IOD quadrant)" : "SNC domain");
    } else {
        printf("  Status         : LOCAL root complex (optimal)\n");
    }
    printf("\n");
}

/* --------------------------------------------------------------------------
 * Clock calibration: derive NIC clock → nanoseconds conversion factor
 * -------------------------------------------------------------------------- */

static bool g_hw_nic_clock = false;  /* false if rte_eth_read_clock unsupported */

static void calibrate_clocks(uint16_t port_id)
{
    uint64_t tsc0, tsc1, nic0, nic1;
    const int CAL_MS = 100;

    g_hz_tsc = rte_get_tsc_hz();

    tsc0 = rte_rdtsc_precise();
    int rc0 = rte_eth_read_clock(port_id, &nic0);
    rte_delay_ms(CAL_MS);
    tsc1 = rte_rdtsc_precise();
    int rc1 = rte_eth_read_clock(port_id, &nic1);

    if (rc0 != 0 || rc1 != 0) {
        fprintf(stderr, "WARN: rte_eth_read_clock not supported by this PMD "
                "(sfc/X2522 does not implement it) — "
                "NIC clock unavailable, all measurements will use TSC only.\n");
        g_hz_nic = g_hz_tsc;   /* set equal so nic_cycles_to_ns is a no-op ratio */
        g_hw_nic_clock = false;
        /* Disable HW Rx timestamps too — they need a common clock reference */
        if (g_hw_rx_ts) {
            fprintf(stderr, "WARN: disabling HW Rx timestamps (no NIC clock to "
                    "convert against TSC)\n");
            g_hw_rx_ts = false;
        }
    } else {
        uint64_t tsc_delta = tsc1 - tsc0;
        uint64_t nic_delta = nic1 - nic0;
        g_hz_nic = (uint64_t)((double)nic_delta / tsc_delta * g_hz_tsc);
        g_hw_nic_clock = true;
    }

    printf("[Clock calibration]\n");
    printf("  TSC frequency  : %lu Hz (%.3f GHz)\n", g_hz_tsc, g_hz_tsc / 1e9);
    if (g_hw_nic_clock)
        printf("  NIC frequency  : %lu Hz (%.3f GHz)\n", g_hz_nic, g_hz_nic / 1e9);
    else
        printf("  NIC frequency  : unavailable (TSC-only mode)\n");
    printf("  Cal duration   : %d ms\n\n", CAL_MS);
}

static inline uint64_t nic_cycles_to_ns(uint64_t cycles)
{
    if (!g_hw_nic_clock || g_hz_nic == 0) return 0;
    return (uint64_t)((double)cycles / g_hz_nic * 1e9);
}

static inline uint64_t tsc_cycles_to_ns(uint64_t cycles)
{
    return (uint64_t)((double)cycles / g_hz_tsc * 1e9);
}

/* --------------------------------------------------------------------------
 * Port initialisation
 * -------------------------------------------------------------------------- */

/*
 * port_init - configure a DPDK port.
 *
 * do_tx / do_rx control which directions are enabled:
 *   Single-port loopback / TX-only:  do_tx=true,  do_rx=true or false
 *   Two-port TX port (.0):           do_tx=true,  do_rx=false
 *   Two-port RX port (.1):           do_tx=false, do_rx=true
 *
 * Keeping directions separate avoids advertising offloads the sfc PMD
 * does not support on a given port direction.
 */
static int port_init(uint16_t port_id, struct rte_mempool *pool,
                     bool do_tx, bool do_rx)
{
    struct rte_eth_conf port_conf = {
        .rxmode = { .mq_mode = RTE_ETH_MQ_RX_NONE },
        .txmode = { .mq_mode = RTE_ETH_MQ_TX_NONE },
    };

    struct rte_eth_dev_info dev_info;
    if (rte_eth_dev_info_get(port_id, &dev_info) != 0) {
        fprintf(stderr, "rte_eth_dev_info_get failed for port %u\n", port_id);
        return -1;
    }

    /* HW Rx timestamps — RX port only */
    if (do_rx) {
        if (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_TIMESTAMP) {
            port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TIMESTAMP;
            g_hw_rx_ts = true;
        } else {
            fprintf(stderr, "WARN: port %u does not support HW Rx timestamps — "
                    "using SW TSC for Rx\n", port_id);
        }
    }

    /*
     * HW Tx timestamps — TX port only.
     * sfc (X2522) does not implement timesync_read_tx_timestamp so this
     * will not be set; TSC fallback is used automatically.
     */
    if (do_tx) {
        if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP) {
            port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP;
            g_hw_tx_ts = true;
        } else {
            fprintf(stderr, "INFO: port %u does not support HW Tx timestamps — "
                    "TX latency = TSC(post-burst) - TSC(pre-burst)\n", port_id);
        }
    }

    int ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
    if (ret < 0) {
        fprintf(stderr, "rte_eth_dev_configure port %u: %s\n",
                port_id, strerror(-ret));
        return ret;
    }

    /*
     * Register timestamp dynfield after port configure (DPDK >= 20.11).
     * Only needed on the RX port.
     */
    if (do_rx && g_hw_rx_ts) {
        g_ts_dynfield_offset = rte_mbuf_dynfield_lookup(
                RTE_MBUF_DYNFIELD_TIMESTAMP_NAME, NULL);
        if (g_ts_dynfield_offset < 0) {
            static const struct rte_mbuf_dynfield dynfield_desc = {
                .name  = RTE_MBUF_DYNFIELD_TIMESTAMP_NAME,
                .size  = sizeof(uint64_t),
                .align = __alignof__(uint64_t),
            };
            g_ts_dynfield_offset = rte_mbuf_dynfield_register(&dynfield_desc);
        }
        if (g_ts_dynfield_offset < 0) {
            fprintf(stderr, "WARN: failed to register timestamp dynfield — "
                    "disabling HW Rx timestamps\n");
            g_hw_rx_ts = false;
            port_conf.rxmode.offloads &= ~RTE_ETH_RX_OFFLOAD_TIMESTAMP;
        } else {
            int flag_bit = rte_mbuf_dynflag_lookup(
                               RTE_MBUF_DYNFLAG_RX_TIMESTAMP_NAME, NULL);
            g_ts_dynflag_mask = (flag_bit >= 0) ? RTE_BIT64(flag_bit) : 0;
        }
    }

    int socket_id = rte_eth_dev_socket_id(port_id);

    /* Rx queue */
    struct rte_eth_rxconf rxconf = dev_info.default_rxconf;
    rxconf.offloads = port_conf.rxmode.offloads;
    ret = rte_eth_rx_queue_setup(port_id, 0, NUM_RX_DESC, socket_id, &rxconf, pool);
    if (ret < 0) {
        fprintf(stderr, "rte_eth_rx_queue_setup port %u: %s\n",
                port_id, strerror(-ret));
        return ret;
    }

    /* Tx queue */
    struct rte_eth_txconf txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;
    ret = rte_eth_tx_queue_setup(port_id, 0, NUM_TX_DESC, socket_id, &txconf);
    if (ret < 0) {
        fprintf(stderr, "rte_eth_tx_queue_setup port %u: %s\n",
                port_id, strerror(-ret));
        return ret;
    }

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        fprintf(stderr, "rte_eth_dev_start port %u: %s\n",
                port_id, strerror(-ret));
        return ret;
    }

    rte_eth_promiscuous_enable(port_id);

    printf("[Port %u] started. Socket: %d  TX: %s  RX: %s  HW-Rx-TS: %s  HW-Tx-TS: %s\n",
           port_id, socket_id,
           do_tx ? "yes" : "no",
           do_rx ? "yes" : "no",
           (do_rx && g_hw_rx_ts) ? "yes" : "no",
           (do_tx && g_hw_tx_ts) ? "yes" : "no");
    return 0;
}

/* --------------------------------------------------------------------------
 * Build a minimal test packet
 * -------------------------------------------------------------------------- */

static struct rte_mbuf *build_test_pkt(struct rte_mempool *pool, uint16_t port_id)
{
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    if (!m) {
        fprintf(stderr, "rte_pktmbuf_alloc failed\n");
        return NULL;
    }

    /* Ethernet header + payload */
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)
        rte_pktmbuf_append(m, sizeof(struct rte_ether_hdr) + PKT_DATA_LEN);

    rte_eth_macaddr_get(port_id, &eth->src_addr);
    rte_eth_macaddr_get(port_id, &eth->dst_addr);  /* self for loopback */
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /* Zero payload — we embed a sequence number in first 8 bytes */
    memset(eth + 1, 0xAB, PKT_DATA_LEN);

    return m;
}

/* --------------------------------------------------------------------------
 * Statistics computation
 * -------------------------------------------------------------------------- */

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void compute_stats(sample_buf_t *buf, latency_stats_t *out)
{
    if (buf->count == 0) return;

    qsort(buf->samples, buf->count, sizeof(uint64_t), cmp_u64);

    out->min  = buf->samples[0];
    out->max  = buf->samples[buf->count - 1];
    out->p50  = buf->samples[(uint64_t)buf->count * 50  / 100];
    out->p90  = buf->samples[(uint64_t)buf->count * 90  / 100];
    out->p99  = buf->samples[(uint64_t)buf->count * 99  / 100];
    out->p999 = buf->samples[(uint64_t)buf->count * 999 / 1000];

    uint64_t sum = 0;
    for (uint32_t i = 0; i < buf->count; i++)
        sum += buf->samples[i];
    out->mean = sum / buf->count;

    /* Histogram */
    uint64_t bucket_width = HISTO_MAX_NS / HISTO_BUCKETS;
    memset(out->histo, 0, sizeof(out->histo));
    for (uint32_t i = 0; i < buf->count; i++) {
        uint64_t b = buf->samples[i] / bucket_width;
        if (b >= HISTO_BUCKETS) b = HISTO_BUCKETS - 1;
        out->histo[b]++;
    }
}

/* --------------------------------------------------------------------------
 * Main measurement loop
 * -------------------------------------------------------------------------- */

static void run_measurement(uint16_t tx_port, uint16_t rx_port)
{
    struct rte_mbuf *tx_mbuf;
    struct rte_mbuf *rx_mbufs[32];
    uint32_t seq = 0;

    if (g_cfg.two_port)
        printf("[Mode] Two-port: TX on port %u, RX on port %u\n",
               tx_port, rx_port);
    else if (g_cfg.oneway_tx)
        printf("[Mode] TX-only (no loopback)\n");
    else
        printf("[Mode] Single-port loopback\n");

    /* Warmup */
    printf("[Warmup] sending %u packets...\n", WARMUP_PKTS);
    for (uint32_t i = 0; i < WARMUP_PKTS && g_running; i++) {
        tx_mbuf = build_test_pkt(g_pktmbuf_pool, tx_port);
        if (!tx_mbuf) break;
        rte_eth_tx_burst(tx_port, 0, &tx_mbuf, 1);
        /* drain Rx */
        uint16_t nb = rte_eth_rx_burst(rx_port, 0, rx_mbufs, 32);
        for (uint16_t j = 0; j < nb; j++)
            rte_pktmbuf_free(rx_mbufs[j]);
        rte_delay_us(10);
    }
    printf("[Warmup] complete\n\n");

    printf("[Measurement] collecting %u samples...\n", g_cfg.n_samples);

    while (seq < g_cfg.n_samples && g_running) {

        /* --- TX path ---------------------------------------------------- */
        tx_mbuf = build_test_pkt(g_pktmbuf_pool, tx_port);
        if (!tx_mbuf) {
            fprintf(stderr, "mbuf alloc failed at seq %u\n", seq);
            break;
        }

        /* Embed sequence number */
        uint8_t *payload = rte_pktmbuf_mtod_offset(tx_mbuf, uint8_t *,
                               sizeof(struct rte_ether_hdr));
        *(uint32_t *)payload = seq;

        /*
         * TX timestamp strategy:
         *   HW path (g_hw_tx_ts): set IEEE1588 flag, poll timesync register
         *                          after burst. Supported on i40e, ice, ixgbe.
         *   SW path (fallback):   TSC snapshot immediately after tx_burst.
         *                          Used for sfc (X2522) and any PMD that does
         *                          not implement timesync_read_tx_timestamp.
         *                          Measures descriptor-write→burst-return time.
         */
        if (g_hw_tx_ts)
            tx_mbuf->ol_flags |= RTE_MBUF_F_TX_IEEE1588_TMST;

        uint64_t sw_tx_tsc = rte_rdtsc_precise();   /* SW pre-burst timestamp */

        uint16_t nb_tx = rte_eth_tx_burst(tx_port, 0, &tx_mbuf, 1);
        if (nb_tx == 0) {
            rte_pktmbuf_free(tx_mbuf);
            continue;
        }

        uint64_t sw_tx_post_tsc = rte_rdtsc_precise(); /* SW post-burst timestamp */

        /* Read back HW Tx timestamp if supported, else use SW delta */
        uint64_t hw_tx_time = 0;
        if (g_hw_tx_ts) {
            struct timespec ts = {0, 0};
            int tries = 1000;
            while (tries-- > 0) {
                if (rte_eth_timesync_read_tx_timestamp(port_id, &ts) == 0) {
                    hw_tx_time = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
                    break;
                }
                rte_delay_us(1);
            }
        }

        /* --- RX path (loopback) ----------------------------------------- */
        uint64_t sw_rx_tsc   = 0;
        uint64_t hw_rx_time  = 0;
        bool     rx_matched  = false;

        if (g_cfg.loopback || g_cfg.two_port) {
            int rx_tries = 10000;
            while (rx_tries-- > 0 && !rx_matched) {
                uint16_t nb_rx = rte_eth_rx_burst(rx_port, 0, rx_mbufs, 32);
                sw_rx_tsc = rte_rdtsc_precise();

                for (uint16_t j = 0; j < nb_rx; j++) {
                    struct rte_mbuf *m = rx_mbufs[j];

                    uint32_t *pld = rte_pktmbuf_mtod_offset(m, uint32_t *,
                                        sizeof(struct rte_ether_hdr));
                    if (*pld == seq) {
                        /*
                         * Extract HW Rx timestamp via dynfield (DPDK >= 20.11).
                         * RTE_MBUF_DYNFLAG_RX_TIMESTAMP_NAME replaces the old
                         * RTE_MBUF_F_RX_IEEE1588_TMST flag for the generic
                         * timestamp offload path used by sfc and others.
                         */
                        if (g_hw_rx_ts && g_ts_dynfield_offset >= 0) {
                            if (!g_ts_dynflag_mask) {
                                /* PMD registers flag after port start; look up once */
                                int bit = rte_mbuf_dynflag_lookup(
                                    RTE_MBUF_DYNFLAG_RX_TIMESTAMP_NAME, NULL);
                                g_ts_dynflag_mask = (bit >= 0) ? RTE_BIT64(bit) : 0;
                            }
                            if (g_ts_dynflag_mask && (m->ol_flags & g_ts_dynflag_mask)) {
                                hw_rx_time = *RTE_MBUF_DYNFIELD(m,
                                                g_ts_dynfield_offset, uint64_t *);
                            }
                        }
                        rx_matched = true;
                    }
                    rte_pktmbuf_free(m);
                }
                if (!rx_matched)
                    rte_delay_us(1);
            }
        }

        /* --- Record samples --------------------------------------------- */

        if (g_tx_samples.count < MAX_SAMPLES) {
            uint64_t tx_ns;
            if (g_hw_tx_ts && hw_tx_time > 0) {
                /*
                 * HW path: hw_tx_time is wall-clock ns from timespec.
                 * sw_tx_tsc is a TSC value — convert to wall-clock ns
                 * for the delta. This gives time from SW descriptor post
                 * to NIC wire transmit.
                 */
                uint64_t sw_tx_ns = tsc_cycles_to_ns(sw_tx_tsc);
                tx_ns = (hw_tx_time > sw_tx_ns) ? hw_tx_time - sw_tx_ns : 0;
            } else {
                /* SW fallback: time from pre-burst TSC to post-burst TSC */
                tx_ns = tsc_cycles_to_ns(sw_tx_post_tsc - sw_tx_tsc);
            }
            if (tx_ns > 0 && tx_ns < HISTO_MAX_NS * 10)
                g_tx_samples.samples[g_tx_samples.count++] = tx_ns;
        }

        if (rx_matched && g_rx_samples.count < MAX_SAMPLES) {
            uint64_t rx_ns;
            if (g_hw_rx_ts && hw_rx_time > 0) {
                /* HW: time from NIC receive to SW poll completion */
                rx_ns = tsc_cycles_to_ns(sw_rx_tsc) - nic_cycles_to_ns(hw_rx_time);
            } else {
                /* SW fallback: not meaningful in absolute terms but still
                 * captures relative node differences */
                rx_ns = 0;   /* omit SW-only Rx samples — no hw_rx_time to anchor */
            }
            if (rx_ns > 0 && rx_ns < HISTO_MAX_NS * 10)
                g_rx_samples.samples[g_rx_samples.count++] = rx_ns;
        }

        seq++;

        /* Progress indicator every 10k packets */
        if (seq % 10000 == 0)
            printf("  ... %u / %u\n", seq, g_cfg.n_samples);
    }
}

/* --------------------------------------------------------------------------
 * Results output
 * -------------------------------------------------------------------------- */

static void print_stats(const char *label, latency_stats_t *s, uint32_t count)
{
    printf("\n=== %s (%u samples) ===\n", label, count);
    printf("  min   : %7lu ns\n", s->min);
    printf("  p50   : %7lu ns\n", s->p50);
    printf("  p90   : %7lu ns\n", s->p90);
    printf("  p99   : %7lu ns\n", s->p99);
    printf("  p999  : %7lu ns\n", s->p999);
    printf("  max   : %7lu ns\n", s->max);
    printf("  mean  : %7lu ns\n", s->mean);
}

static void write_csv(const char *path, sample_buf_t *buf)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    fprintf(f, "sample_ns\n");
    for (uint32_t i = 0; i < buf->count; i++)
        fprintf(f, "%lu\n", buf->samples[i]);
    fclose(f);
    printf("  Raw samples written to: %s\n", path);
}

static void write_histo(const char *path, latency_stats_t *s)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    uint64_t bucket_width = HISTO_MAX_NS / HISTO_BUCKETS;
    fprintf(f, "bucket_ns,count\n");
    for (int i = 0; i < HISTO_BUCKETS; i++)
        fprintf(f, "%lu,%lu\n", (uint64_t)i * bucket_width, s->histo[i]);
    fclose(f);
    printf("  Histogram written to: %s\n", path);
}

/* --------------------------------------------------------------------------
 * Argument parsing
 * -------------------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [EAL options] -- "
        "[-p TX_PORT] [-q RX_PORT] [-n SAMPLES] [-s SNC_NODE] [-T] [-R]\n"
        "  -p TX_PORT   DPDK TX port ID (default 0)\n"
        "  -q RX_PORT   DPDK RX port ID for two-port mode (e.g. -p 0 -q 1)\n"
        "               When -q is given, TX and RX use separate ports.\n"
        "               Without -q, single-port loopback mode is used.\n"
        "  -n SAMPLES   number of packets (default %d)\n"
        "  -s SNC_NODE  expected SNC/NPS NUMA node (for validation)\n"
        "  -T           TX-only (no Rx measurement, no loopback needed)\n"
        "  -R           RX-only (no Tx measurement)\n",
        prog, DEFAULT_SAMPLES);
}

static int parse_args(int argc, char **argv)
{
    int opt;
    bool rx_port_set = false;
    while ((opt = getopt(argc, argv, "p:q:n:s:TRh")) != -1) {
        switch (opt) {
        case 'p': g_cfg.port_id    = (uint16_t)atoi(optarg); break;
        case 'q': g_cfg.rx_port_id = (uint16_t)atoi(optarg);
                  rx_port_set = true;                         break;
        case 'n': g_cfg.n_samples  = (uint32_t)atoi(optarg); break;
        case 's': g_cfg.snc_node   = atoi(optarg);           break;
        case 'T': g_cfg.oneway_tx  = true;                   break;
        case 'R': g_cfg.oneway_rx  = true;                   break;
        case 'h': usage(argv[0]); return -1;
        default:  usage(argv[0]); return -1;
        }
    }
    if (rx_port_set) {
        g_cfg.two_port = true;
        g_cfg.loopback = false;
    }
    if (g_cfg.oneway_tx || g_cfg.oneway_rx) {
        g_cfg.loopback  = false;
        g_cfg.two_port  = false;
    }
    if (g_cfg.two_port && g_cfg.port_id == g_cfg.rx_port_id) {
        fprintf(stderr, "ERROR: TX port and RX port must be different\n");
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    int ret = rte_eal_init(argc, argv);
    if (ret < 0) rte_exit(EXIT_FAILURE, "rte_eal_init failed\n");
    argc -= ret;
    argv += ret;

    if (parse_args(argc, argv) < 0)
        rte_exit(EXIT_FAILURE, "argument parse failed\n");

    printf("\n%s: PCIe sub-NUMA latency harness\n", APP_NAME);
    printf("========================================\n");

    /* Mempool allocated on local NUMA socket */
    int local_socket = rte_socket_id();
    g_pktmbuf_pool = rte_pktmbuf_pool_create(
        "MBUF_POOL",
        8192,
        MEMPOOL_CACHE_SZ,
        0,
        RTE_MBUF_DEFAULT_BUF_SIZE,
        local_socket);

    if (!g_pktmbuf_pool)
        rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create failed (socket %d)\n",
                 local_socket);

    printf("[Mempool] allocated on socket %d\n", local_socket);

    /*
     * Port initialisation.
     * Two-port mode:    TX port gets do_tx=true/do_rx=false,
     *                   RX port gets do_tx=false/do_rx=true.
     * Single-port:      one port handles both directions.
     */
    if (g_cfg.two_port) {
        if (port_init(g_cfg.port_id,    g_pktmbuf_pool, true,  false) < 0)
            rte_exit(EXIT_FAILURE, "port_init failed for TX port %u\n",
                     g_cfg.port_id);
        if (port_init(g_cfg.rx_port_id, g_pktmbuf_pool, false, true) < 0)
            rte_exit(EXIT_FAILURE, "port_init failed for RX port %u\n",
                     g_cfg.rx_port_id);
    } else {
        bool do_rx = !g_cfg.oneway_tx;
        bool do_tx = !g_cfg.oneway_rx;
        if (port_init(g_cfg.port_id, g_pktmbuf_pool, do_tx, do_rx) < 0)
            rte_exit(EXIT_FAILURE, "port_init failed\n");
    }

    validate_topology(g_cfg.port_id, g_cfg.rx_port_id);
    calibrate_clocks(g_cfg.port_id);

    memset(&g_tx_samples, 0, sizeof(g_tx_samples));
    memset(&g_rx_samples, 0, sizeof(g_rx_samples));

    run_measurement(g_cfg.port_id, g_cfg.rx_port_id);

    /* Compute and print statistics */
    latency_stats_t tx_stats = {0}, rx_stats = {0};

    if (g_tx_samples.count > 0) {
        compute_stats(&g_tx_samples, &tx_stats);
        print_stats("TX latency (CPU→NIC descriptor→HW transmit)", &tx_stats,
                    g_tx_samples.count);
        write_csv("/tmp/tx_samples.csv", &g_tx_samples);
        write_histo("/tmp/tx_histo.csv", &tx_stats);
    }

    if (g_rx_samples.count > 0) {
        compute_stats(&g_rx_samples, &rx_stats);
        print_stats("RX latency (HW receive→SW poll completion)", &rx_stats,
                    g_rx_samples.count);
        write_csv("/tmp/rx_samples.csv", &g_rx_samples);
        write_histo("/tmp/rx_histo.csv", &rx_stats);
    }

    /* Stop and close all initialised ports */
    if (rte_eth_dev_stop(g_cfg.port_id) != 0)
        fprintf(stderr, "WARN: rte_eth_dev_stop failed for port %u\n",
                g_cfg.port_id);
    rte_eth_dev_close(g_cfg.port_id);
    if (g_cfg.two_port) {
        if (rte_eth_dev_stop(g_cfg.rx_port_id) != 0)
            fprintf(stderr, "WARN: rte_eth_dev_stop failed for port %u\n",
                    g_cfg.rx_port_id);
        rte_eth_dev_close(g_cfg.rx_port_id);
    }
    rte_eal_cleanup();

    printf("\nDone.\n");
    return 0;
}
