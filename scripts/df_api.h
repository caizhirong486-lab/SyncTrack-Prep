/*
 * Minimal DeepFilterNet libDF C API surface used by SyncTrack Prep.
 * Mirrors libDF/src/capi.rs (Rikorose/DeepFilterNet, MIT/Apache-2.0).
 * Full header can be regenerated with cbindgen; only this subset is linked.
 */
#ifndef STP_DFN_H
#define STP_DFN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct DFState DFState;

/** Create a DeepFilterNet state from a tar.gz onnx model bundle.
    atten_lim: attenuation limit in dB (0 = no attenuation / bypass,
    >= 100 = unlimited). Mono: create one state per channel. */
DFState* df_create (const char* path, float atten_lim, const char* log_level);

void df_free (DFState* st);

/** Frame (hop) size in samples, e.g. 480 at 48 kHz for DFN3. */
size_t df_get_frame_length (DFState* st);

/** Process one frame of hop_size samples in place; returns local SNR. */
float df_process_frame (DFState* st, float* input, float* output);

/** Change the attenuation limit (dB) on a live state. */
void df_set_atten_lim (DFState* st, float lim_db);

#ifdef __cplusplus
}
#endif

#endif
