#pragma once

#include <windows.h>
#include <mmreg.h>

#include <stddef.h>

struct converter;

/*  Validate a client-supplied format and rewrite it into a canonical
    WAVEFORMATEX (PCM or IEEE float, cbSize 0, consistent nBlockAlign and
    nAvgBytesPerSec). Handles WAVE_FORMAT_EXTENSIBLE. */

HRESULT converter_normalize_format(const WAVEFORMATEX *in, WAVEFORMATEX *out);

HRESULT converter_calculate_dest_nbytes(
        const WAVEFORMATEX *src,
        const WAVEFORMATEX *dest,
        size_t src_nbytes,
        size_t *out);

HRESULT converter_alloc(
        struct converter **out,
        const WAVEFORMATEX *src,
        const WAVEFORMATEX *dest,
        void *src_bytes,
        size_t src_nbytes,
        void *dest_bytes,
        size_t dest_nbytes);

void converter_free(struct converter *conv);

/* Re-convert the output affected by a write to [src_offset, +src_nbytes) */

HRESULT converter_convert_range(
        struct converter *conv,
        size_t src_offset,
        size_t src_nbytes);

HRESULT converter_convert(
        struct converter *conv,
        size_t *src_nprocessed,
        size_t *dest_nprocessed);
