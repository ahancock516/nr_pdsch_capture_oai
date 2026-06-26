/*
 * nr_pdsch_capture.c
 *
 * OAI nrUE plugin: captures PDSCH frequency-domain IQ per slot, writing a
 * labelled binary dataset for ML/neural-receiver research.
 * Hook point: pre channel-estimation (raw IQ only, same layer as PUSCH plugin).
 *
 * Captures are accepted only when:
 *   1. At least one DMRS symbol falls within the allocation window.
 *   2. The DMRS RE comb is strongly visible in the received IQ
 *      (active-bin power / quiet-bin power >= DMRS_COMB_POWER_RATIO_THRESHOLD).
 *   3. The IQ payload is not a duplicate of a previously accepted capture.
 *
 * Dataset layout (v1):
 *   [pdsch_capture_hdr_t] [IQ block]
 *   IQ block: nb_rx * nb_symbols * nb_re_per_sym c16_t (all allocated REs, all symbols)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#include "openair1/PHY/defs_nr_UE.h"
#include "openair1/PHY/NR_UE_TRANSPORT/nr_transport_ue.h"
#include "openair1/PHY/TOOLS/tools_defs.h"
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"
#include "nfapi/open-nFAPI/nfapi/public_inc/nfapi_nr_interface.h"
#include "SCHED_NR_UE/defs.h"

/* -------------------------------------------------------------------------
 * Dataset format
 * ---------------------------------------------------------------------- */

#define CAPTURE_MAGIC   "PDSC"
#define CAPTURE_VERSION 1

#pragma pack(push, 1)
typedef struct {
    uint8_t  magic[4];
    uint8_t  version;
    uint32_t capture_index;
    uint16_t frame;
    uint8_t  slot;
    uint16_t rnti;
    uint16_t bwp_start;
    uint16_t rb_start;
    uint16_t nb_rbs;
    uint8_t  start_symbol;
    uint8_t  nb_symbols;
    uint16_t dmrs_symb_pos;
    uint8_t  dmrs_config_type;
    uint8_t  n_dmrs_cdm_groups;
    uint8_t  qam_mod_order;
    uint8_t  nb_layers;
    uint8_t  nb_rx;
    uint32_t nb_re_per_sym;
    /* Immediately followed by:
     *   IQ: nb_rx * nb_symbols * nb_re_per_sym * sizeof(c16_t) */
} pdsch_capture_hdr_t;
#pragma pack(pop)

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

#ifndef PDSCH_CAPTURE_REPO_DIR
#define PDSCH_CAPTURE_REPO_DIR "."
#endif
#define DEFAULT_OUTPUT_FILE   PDSCH_CAPTURE_REPO_DIR "/data/pdsch_dataset.bin"
#define DEFAULT_MAX_CAPTURES  5000
#define CONFIG_FILE           PDSCH_CAPTURE_REPO_DIR "/capture_config.txt"

#define DMRS_COMB_POWER_RATIO_THRESHOLD 1.50

#define FNV1A64_OFFSET_BASIS  UINT64_C(14695981039346656037)
#define FNV1A64_PRIME         UINT64_C(1099511628211)

typedef struct {
    char     output_file[512];
    uint32_t max_captures;
} capture_cfg_t;

static capture_cfg_t g_cfg;

static void load_config(void)
{
    strncpy(g_cfg.output_file, DEFAULT_OUTPUT_FILE, sizeof(g_cfg.output_file) - 1);
    g_cfg.max_captures = DEFAULT_MAX_CAPTURES;

    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp)
        return;
    char line[640];
    while (fgets(line, sizeof(line), fp)) {
        char key[64], val[512];
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        if (sscanf(line, "%63s %511s", key, val) != 2)
            continue;
        if (strcmp(key, "output_file") == 0)
            strncpy(g_cfg.output_file, val, sizeof(g_cfg.output_file) - 1);
        else if (strcmp(key, "max_captures") == 0)
            g_cfg.max_captures = (uint32_t)atoi(val);
    }
    fclose(fp);
}

/* -------------------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------------- */

static FILE           *g_out         = NULL;
static uint32_t        g_count       = 0;
static pthread_mutex_t g_mutex       = PTHREAD_MUTEX_INITIALIZER;
static int             g_initialised = 0;

static uint64_t       *g_seen_hashes      = NULL;
static uint32_t        g_seen_hash_count  = 0;
static uint32_t        g_dup_skip_count   = 0;
static uint32_t        g_dmrs_skip_count  = 0;
static uint32_t        g_comb_skip_count  = 0;

static int open_output(void)
{
    char tmp[512];
    strncpy(tmp, g_cfg.output_file, sizeof(tmp) - 1);
    char *slash = strrchr(tmp, '/');
    if (slash) {
        *slash = '\0';
        char cmd[560];
        snprintf(cmd, sizeof(cmd), "mkdir -p %s", tmp);
        (void)system(cmd);
    }
    g_out = fopen(g_cfg.output_file, "ab");
    if (!g_out) {
        fprintf(stderr, "[PDSCH_CAPTURE] cannot open %s: %s\n",
                g_cfg.output_file, strerror(errno));
        return -1;
    }
    fprintf(stderr, "[PDSCH_CAPTURE] writing dataset to %s (max %u captures)\n",
            g_cfg.output_file, g_cfg.max_captures);
    return 0;
}

/* -------------------------------------------------------------------------
 * Loader init
 * ---------------------------------------------------------------------- */

int loader_init(void *arg)
{
    (void)arg;
    load_config();
    if (open_output() != 0)
        return -1;

    g_seen_hashes = calloc(g_cfg.max_captures, sizeof(*g_seen_hashes));
    if (!g_seen_hashes) {
        fprintf(stderr, "[PDSCH_CAPTURE] cannot allocate hash table\n");
        return -1;
    }

    g_initialised = 1;
    fprintf(stderr, "[PDSCH_CAPTURE] plugin initialised\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * DMRS filter helpers  (ported from nr_pusch_capture by Jose)
 * ---------------------------------------------------------------------- */

static int has_dmrs_in_window(uint16_t dmrs_symb_pos,
                              uint8_t  start_symbol,
                              uint8_t  nb_symbols)
{
    for (int s = 0; s < nb_symbols; s++) {
        if ((dmrs_symb_pos >> (start_symbol + s)) & 0x1)
            return 1;
    }
    return 0;
}

static int dmrs_type_is_type1(uint8_t dmrs_config_type)
{
    return dmrs_config_type == NFAPI_NR_DMRS_TYPE1;
}

static void mark_dmrs_group_bins(uint8_t dmrs_config_type,
                                 uint8_t group_index,
                                 uint8_t bins[12])
{
    if (dmrs_type_is_type1(dmrs_config_type)) {
        for (int k = group_index; k < 12; k += 2)
            bins[k] = 1;
        return;
    }
    static const uint8_t type2_group_bins[3][4] = {
        {0, 1, 6, 7}, {2, 3, 8, 9}, {4, 5, 10, 11},
    };
    if (group_index >= 3) return;
    for (int i = 0; i < 4; i++)
        bins[type2_group_bins[group_index][i]] = 1;
}

static int get_dmrs_port_delta(uint8_t dmrs_config_type,
                               uint8_t port,
                               uint8_t *delta_out)
{
    static const uint8_t type1_deltas[8]  = {0, 0, 1, 1, 0, 0, 1, 1};
    static const uint8_t type2_deltas[12] = {0, 0, 2, 2, 4, 4, 0, 0, 2, 2, 4, 4};
    if (dmrs_type_is_type1(dmrs_config_type)) {
        if (port >= 8) return 0;
        *delta_out = type1_deltas[port];
        return 1;
    }
    if (dmrs_config_type != NFAPI_NR_DMRS_TYPE2 || port >= 12)
        return 0;
    *delta_out = type2_deltas[port];
    return 1;
}

static void mark_dmrs_active_bins(uint8_t dmrs_config_type,
                                  uint8_t delta,
                                  uint8_t bins[12])
{
    if (dmrs_type_is_type1(dmrs_config_type)) {
        for (int k = delta; k < 12; k += 2)
            bins[k] = 1;
        return;
    }
    static const uint8_t type2_offsets[4] = {0, 1, 6, 7};
    for (int i = 0; i < 4; i++) {
        uint8_t bin = delta + type2_offsets[i];
        if (bin < 12) bins[bin] = 1;
    }
}

/* Returns 1 if active/quiet bins are determinable, fills both arrays.
 *
 * Returns:
 *   0  — config unsupported, skip capture
 *   1  — inter-CDM-group quiet bins found; power ratio check applies
 *   2  — data bins used as fallback quiet reference; skip power ratio check
 *         (signal-vs-signal comparison is not meaningful)
 *
 * dmrs_ports==0 defaults to port 0 (single-layer phy-test default). */
static int build_dmrs_bins(uint8_t  dmrs_config_type,
                            uint8_t  n_dmrs_cdm_groups,
                            uint16_t dmrs_ports,
                            uint8_t  active_bins[12],
                            uint8_t  quiet_bins[12])
{
    uint8_t reserved_bins[12] = {0};
    memset(active_bins, 0, 12);
    memset(quiet_bins,  0, 12);

    if (dmrs_config_type != NFAPI_NR_DMRS_TYPE1 &&
        dmrs_config_type != NFAPI_NR_DMRS_TYPE2)
        return 0;

    int max_groups = dmrs_type_is_type1(dmrs_config_type) ? 2 : 3;
    if (n_dmrs_cdm_groups < 1 || n_dmrs_cdm_groups > max_groups)
        return 0;

    for (uint8_t g = 0; g < n_dmrs_cdm_groups; g++)
        mark_dmrs_group_bins(dmrs_config_type, g, reserved_bins);

    /* Default to port 0 when field is unpopulated (phy-test mode) */
    uint16_t ports = (dmrs_ports != 0) ? dmrs_ports : 0x1;

    int max_ports = dmrs_type_is_type1(dmrs_config_type) ? 8 : 12;
    for (int p = 0; p < max_ports; p++) {
        if (!((ports >> p) & 0x1)) continue;
        uint8_t delta = 0;
        if (!get_dmrs_port_delta(dmrs_config_type, (uint8_t)p, &delta))
            return 0;
        mark_dmrs_active_bins(dmrs_config_type, delta, active_bins);
    }

    /* Validate: every active bin must be in a reserved CDM group */
    int active_count = 0;
    for (int b = 0; b < 12; b++) {
        if (active_bins[b]) {
            if (!reserved_bins[b]) return 0;
            active_count++;
        }
    }
    if (active_count == 0)
        return 0;

    /* Priority 1: reserved-but-inactive bins (inter-CDM-group quiet) */
    int quiet_count = 0;
    for (int b = 0; b < 12; b++) {
        if (!active_bins[b] && reserved_bins[b]) {
            quiet_bins[b] = 1;
            quiet_count++;
        }
    }
    if (quiet_count > 0)
        return 1;  /* power ratio check meaningful */

    /* Priority 2: non-reserved data bins — power ratio check not applicable */
    for (int b = 0; b < 12; b++) {
        if (!reserved_bins[b]) {
            quiet_bins[b] = 1;
            quiet_count++;
        }
    }
    return (quiet_count > 0) ? 2 : 0;
}

/* Returns 1 if the DMRS comb power ratio meets the threshold. */
static int dmrs_comb_is_strong(uint16_t dmrs_symb_pos,
                                uint8_t  start_symbol,
                                uint8_t  nb_symbols,
                                uint32_t nb_re_per_sym,
                                const c16_t *iq,   /* (nb_symbols, nb_re_per_sym) */
                                const uint8_t active_bins[12],
                                const uint8_t quiet_bins[12])
{
    double active_sum = 0.0, quiet_sum = 0.0;
    uint32_t active_n = 0, quiet_n = 0;

    for (int s = 0; s < nb_symbols; s++) {
        if (!((dmrs_symb_pos >> (start_symbol + s)) & 0x1))
            continue;
        const c16_t *sym = iq + (size_t)s * nb_re_per_sym;
        for (uint32_t re = 0; re < nb_re_per_sym; re++) {
            double i = (double)sym[re].r;
            double q = (double)sym[re].i;
            double p = i * i + q * q;
            uint8_t bin = (uint8_t)(re % 12);
            if (active_bins[bin]) { active_sum += p; active_n++; }
            else if (quiet_bins[bin]) { quiet_sum += p; quiet_n++; }
        }
    }

    if (active_n == 0 || quiet_n == 0)
        return 0;

    double active_mean = active_sum / (double)active_n;
    double quiet_mean  = quiet_sum  / (double)quiet_n;
    if (quiet_mean <= 0.0)
        return active_mean > 0.0;

    return active_mean >= quiet_mean * DMRS_COMB_POWER_RATIO_THRESHOLD;
}

/* -------------------------------------------------------------------------
 * Duplicate detection (FNV1a-64 over IQ payload)
 * ---------------------------------------------------------------------- */

static uint64_t fnv1a64(const void *data, size_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    uint64_t hash = FNV1A64_OFFSET_BASIS;
    for (size_t i = 0; i < len; i++) {
        hash ^= b[i];
        hash *= FNV1A64_PRIME;
    }
    return hash;
}

static int hash_seen(uint64_t hash)
{
    for (uint32_t i = 0; i < g_seen_hash_count; i++)
        if (g_seen_hashes[i] == hash) return 1;
    return 0;
}

static void hash_remember(uint64_t hash)
{
    if (g_seen_hash_count < g_cfg.max_captures)
        g_seen_hashes[g_seen_hash_count++] = hash;
}

static void maybe_log_skip(const char *reason, uint32_t count)
{
    if (count <= 5 || count % 100 == 0)
        fprintf(stderr, "[PDSCH_CAPTURE] skip %s (n=%u)\n", reason, count);
}

/* -------------------------------------------------------------------------
 * Main capture function
 * ---------------------------------------------------------------------- */

int pdsch_capture_compute(PHY_VARS_NR_UE        *ue,
                          const UE_nr_rxtx_proc_t *proc,
                          NR_UE_DLSCH_t          *dlsch,
                          c16_t                 *rxdataF,
                          uint32_t               rx_size_symbol,
                          int                    nbRx,
                          int                    nbLayers)
{
    if (!g_initialised)
        return 0;

    fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg = &dlsch[0].dlsch_config;
    NR_DL_FRAME_PARMS *fp = &ue->frame_parms;

    if (cfg->number_rbs == 0 || cfg->number_symbols == 0)
        return 0;
    if (cfg->dlDmrsSymbPos == 0)
        return 0;

    pthread_mutex_lock(&g_mutex);

    if (g_count >= g_cfg.max_captures) {
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    /* --- Filter 1: DMRS window check --- */
    if (!has_dmrs_in_window((uint16_t)cfg->dlDmrsSymbPos,
                             cfg->start_symbol,
                             cfg->number_symbols)) {
        g_dmrs_skip_count++;
        maybe_log_skip("no DMRS in allocation window", g_dmrs_skip_count);
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    /* Extract IQ for the allocated RBs into a contiguous buffer */
    const int nb_re    = cfg->number_rbs * 12;
    const int sps      = fp->samples_per_slot_wCP;
    const int ofdm     = fp->ofdm_symbol_size;
    const int re_start = cfg->BWPStart + cfg->start_rb;
    const int fc_off   = fp->first_carrier_offset;

    /* Stack buffer: single antenna, all symbols */
    c16_t iq_buf[cfg->number_symbols * nb_re];

    for (int s = 0; s < cfg->number_symbols; s++) {
        int sym = cfg->start_symbol + s;
        c16_t *sym_ptr = (c16_t *)rxdataF + (size_t)0 * sps + sym * ofdm;
        int carrier_idx = (fc_off + re_start) % ofdm;
        c16_t *dst = iq_buf + s * nb_re;
        for (int re = 0; re < nb_re; re++)
            dst[re] = sym_ptr[(carrier_idx + re) % ofdm];
    }

    /* --- Filter 2: DMRS comb visibility --- */
    uint8_t active_bins[12], quiet_bins[12];
    int comb_mode = build_dmrs_bins(cfg->dmrsConfigType,
                                     cfg->n_dmrs_cdm_groups,
                                     cfg->dmrs_ports,
                                     active_bins, quiet_bins);
    if (comb_mode == 0) {
        g_comb_skip_count++;
        maybe_log_skip("unsupportable DMRS comb config", g_comb_skip_count);
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    /* Power ratio check only when inter-CDM-group quiet bins are available */
    if (comb_mode == 1 &&
        !dmrs_comb_is_strong((uint16_t)cfg->dlDmrsSymbPos,
                              cfg->start_symbol,
                              cfg->number_symbols,
                              (uint32_t)nb_re,
                              iq_buf,
                              active_bins, quiet_bins)) {
        g_comb_skip_count++;
        maybe_log_skip("weak DMRS comb", g_comb_skip_count);
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    /* --- Filter 3: duplicate detection --- */
    uint64_t hash = fnv1a64(iq_buf, (size_t)cfg->number_symbols * nb_re * sizeof(c16_t));
    if (hash_seen(hash)) {
        g_dup_skip_count++;
        maybe_log_skip("duplicate IQ payload", g_dup_skip_count);
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }
    hash_remember(hash);

    /* --- Write header --- */
    pdsch_capture_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, CAPTURE_MAGIC, 4);
    hdr.version           = CAPTURE_VERSION;
    hdr.capture_index     = g_count;
    hdr.frame             = (uint16_t)proc->frame_rx;
    hdr.slot              = (uint8_t)proc->nr_slot_rx;
    hdr.rnti              = dlsch[0].rnti;
    hdr.bwp_start         = cfg->BWPStart;
    hdr.rb_start          = cfg->start_rb;
    hdr.nb_rbs            = cfg->number_rbs;
    hdr.start_symbol      = cfg->start_symbol;
    hdr.nb_symbols        = cfg->number_symbols;
    hdr.dmrs_symb_pos     = (uint16_t)cfg->dlDmrsSymbPos;
    hdr.dmrs_config_type  = cfg->dmrsConfigType;
    hdr.n_dmrs_cdm_groups = cfg->n_dmrs_cdm_groups;
    hdr.qam_mod_order     = cfg->qamModOrder;
    hdr.nb_layers         = (uint8_t)nbLayers;
    hdr.nb_rx             = (uint8_t)nbRx;
    hdr.nb_re_per_sym     = (uint32_t)nb_re;

    if (fwrite(&hdr, sizeof(hdr), 1, g_out) != 1)
        goto write_error;

    /* --- Write IQ for all antennas --- */
    for (int ant = 0; ant < nbRx; ant++) {
        if (ant == 0) {
            /* already extracted into iq_buf */
            if (fwrite(iq_buf, sizeof(c16_t), cfg->number_symbols * nb_re, g_out)
                    != (size_t)(cfg->number_symbols * nb_re))
                goto write_error;
        } else {
            c16_t *rxF_ant = (c16_t *)rxdataF + (size_t)ant * sps;
            for (int s = 0; s < cfg->number_symbols; s++) {
                int sym = cfg->start_symbol + s;
                c16_t *sym_ptr = rxF_ant + sym * ofdm;
                int carrier_idx = (fc_off + re_start) % ofdm;
                c16_t re_buf[nb_re];
                for (int re = 0; re < nb_re; re++)
                    re_buf[re] = sym_ptr[(carrier_idx + re) % ofdm];
                if (fwrite(re_buf, sizeof(c16_t), nb_re, g_out) != (size_t)nb_re)
                    goto write_error;
            }
        }
    }

    fflush(g_out);
    g_count++;

    if (g_count % 50 == 0 || g_count == g_cfg.max_captures)
        fprintf(stderr, "[PDSCH_CAPTURE] captured %u / %u  "
                "(skipped: dmrs=%u comb=%u dup=%u)\n",
                g_count, g_cfg.max_captures,
                g_dmrs_skip_count, g_comb_skip_count, g_dup_skip_count);

    if (g_count >= g_cfg.max_captures)
        fprintf(stderr, "[PDSCH_CAPTURE] dataset complete — plugin now passthrough\n");

    pthread_mutex_unlock(&g_mutex);
    return 0;

write_error:
    fprintf(stderr, "[PDSCH_CAPTURE] write error at capture %u: %s\n",
            g_count, strerror(errno));
    pthread_mutex_unlock(&g_mutex);
    return -1;
}
