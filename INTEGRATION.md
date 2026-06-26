# Integration Guide

This document describes the exact changes required to integrate `nr_pdsch_capture` into the OAI nrUE source tree.

Two files in the OAI repository must be modified:

1. `openair1/SCHED_NR_UE/phy_procedures_nr_ue.c` — plugin loader and hook call
2. `CMakeLists.txt` (root) — build and path definitions

---

## 1. `phy_procedures_nr_ue.c`

### 1a. Plugin loader — add after the existing `#include` block (around line 80)

```c
/* ---------- PDSCH capture plugin (optional) ----------------------------- */
#include <dlfcn.h>

typedef int (*pdsch_capture_fn_t)(PHY_VARS_NR_UE *,
                                  const UE_nr_rxtx_proc_t *,
                                  NR_UE_DLSCH_t *,
                                  c16_t *,    /* rxdataF        */
                                  uint32_t,   /* rx_size_symbol */
                                  int,        /* nbRx           */
                                  int);       /* nbLayers       */

static pdsch_capture_fn_t  g_pdsch_capture_fn   = NULL;
static pthread_once_t      g_pdsch_capture_once  = PTHREAD_ONCE_INIT;

static void pdsch_capture_loader_init(void)
{
#ifndef PDSCH_CAPTURE_SO_PATH
#define PDSCH_CAPTURE_SO_PATH "libpdsch_capture.so"
#endif
  void *h = dlopen(PDSCH_CAPTURE_SO_PATH, RTLD_LAZY | RTLD_GLOBAL);
  if (!h) {
    fprintf(stderr, "[PDSCH_CAPTURE] dlopen failed: %s\n", dlerror());
    return;
  }
  /* Call the plugin's own init so it opens the dataset file */
  typedef int (*init_fn_t)(void *);
  init_fn_t init_fn = (init_fn_t)dlsym(h, "loader_init");
  if (init_fn && init_fn(NULL) != 0) {
    fprintf(stderr, "[PDSCH_CAPTURE] loader_init failed\n");
    return;
  }
  g_pdsch_capture_fn = (pdsch_capture_fn_t)dlsym(h, "pdsch_capture_compute");
  if (!g_pdsch_capture_fn)
    fprintf(stderr, "[PDSCH_CAPTURE] dlsym failed: %s\n", dlerror());
  else
    fprintf(stderr, "[PDSCH_CAPTURE] plugin loaded OK\n");
}
/* ----------------------------------------------------------------------- */
```

### 1b. Hook call — insert inside `nr_ue_pdsch_procedures()`, after channel estimation, before equalization

The correct insertion point is immediately after the `chest_time` averaging loop and before the `first_symbol_with_data` processing block. Search for the DMRS channel estimation call sequence and insert after it:

```c
    /* PDSCH capture plugin hook — post channel-estimation, pre equalization.
     * rxdataF is unchanged by CE (CE writes to pdsch_dl_ch_estimates only),
     * so captured IQ is identical to what would be captured pre-CE. */
    pthread_once(&g_pdsch_capture_once, pdsch_capture_loader_init);
    if (g_pdsch_capture_fn != NULL) {
      g_pdsch_capture_fn(ue,
                         proc,
                         dlsch,
                         (c16_t *)rxdataF,
                         rx_size_symbol,
                         ue->frame_parms.nb_antennas_rx,
                         dlsch[0].Nl);
    }
```

> **Important:** The hook must be placed **post** channel estimation, not pre. A pre-CE hook places file I/O on the real-time receive path, causing HARQ timing violations in SA mode. `rxdataF` is not modified by channel estimation (CE writes only to `pdsch_dl_ch_estimates`), so the captured IQ is identical either way.

---

## 2. Root `CMakeLists.txt`

Add the following after the `dfts` library target (search for `add_library(dfts MODULE`):

```cmake
# PDSCH capture plugin (clone nr_pdsch_capture_oai into plugins/nr_pdsch_capture)
add_subdirectory(${CMAKE_SOURCE_DIR}/plugins/nr_pdsch_capture
                 ${CMAKE_BINARY_DIR}/nr_pdsch_capture)

# Embed the .so build path so dlopen loads directly without a system install step
target_compile_definitions(SCHED_NR_UE_LIB PRIVATE
    PDSCH_CAPTURE_SO_PATH="${CMAKE_BINARY_DIR}/libpdsch_capture.so"
)
```

The `PDSCH_CAPTURE_SO_PATH` definition tells the loader the absolute path to the built `.so`, so no `sudo cp` or `ldconfig` step is needed after a build.

---

## Build

```bash
cd openairinterface5g/cmake_targets/ran_build/build
cmake ..
make nr-uesoftmodem pdsch_capture -j$(nproc)
```

On success, `libpdsch_capture.so` appears in the build directory and `[PDSCH_CAPTURE] plugin loaded OK` is printed to stderr when the UE starts.
