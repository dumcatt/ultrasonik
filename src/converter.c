#include <windows.h>
#include <mmreg.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "converter.h"
#include "trace.h"

/*  Simple built-in sample format converter.

    This used to be implemented with the Windows ACM API, converting the
    entire buffer on every Unlock() with a stateful ACM stream. ACM rejects
    formats with slightly-off nBlockAlign/nAvgBytesPerSec fields, does not
    handle IEEE float or WAVE_FORMAT_EXTENSIBLE, and its stateful resampler
    is not designed to be re-run over the same data repeatedly. Any of those
    would leave a buffer silent.

    Output is always the system format: interleaved stereo s16 at the system
    sample rate. Each destination frame is a pure function of the source
    buffer, so arbitrary sub-ranges can be (re-)converted independently. */

struct converter {
    WAVEFORMATEX src;
    WAVEFORMATEX dest;
    const uint8_t *src_bytes;
    size_t src_nframes;
    size_t src_bpf;
    int16_t *dest_samples;
    size_t dest_nframes;
};

#define WAVE_FORMAT_EXTENSIBLE_ 0xFFFE
#define WAVE_FORMAT_IEEE_FLOAT_ 0x0003

HRESULT converter_normalize_format(const WAVEFORMATEX *in, WAVEFORMATEX *out)
{
    WORD tag;
    DWORD subformat;

    assert(in != NULL);
    assert(out != NULL);

    memset(out, 0, sizeof(*out));
    tag = in->wFormatTag;

    if (tag == WAVE_FORMAT_EXTENSIBLE_) {
        if (in->cbSize < 22) {
            trace("EXTENSIBLE format with short cbSize %u", in->cbSize);

            return E_INVALIDARG;
        }

        /*  WAVEFORMATEXTENSIBLE: WAVEFORMATEX (18 bytes, packed), then
            WORD Samples, DWORD dwChannelMask, GUID SubFormat. The sub-format
            GUIDs for plain tags are {0000xxxx-0000-0010-8000-00AA00389B71}
            where xxxx is the equivalent wFormatTag. */

        memcpy(&subformat, ((const uint8_t *) in) + 18 + 2 + 4, 4);
        tag = (WORD) subformat;
    }

    if (tag != WAVE_FORMAT_PCM && tag != WAVE_FORMAT_IEEE_FLOAT_) {
        trace("Unsupported format tag %04x", tag);

        return E_INVALIDARG;
    }

    if (in->nChannels == 0 || in->nSamplesPerSec == 0) {
        trace("Invalid channel count / sample rate");

        return E_INVALIDARG;
    }

    if (tag == WAVE_FORMAT_PCM) {
        if (    in->wBitsPerSample != 8 &&
                in->wBitsPerSample != 16 &&
                in->wBitsPerSample != 24 &&
                in->wBitsPerSample != 32) {
            trace("Unsupported PCM bit depth %u", in->wBitsPerSample);

            return E_INVALIDARG;
        }
    } else if (in->wBitsPerSample != 32) {
        trace("Unsupported float bit depth %u", in->wBitsPerSample);

        return E_INVALIDARG;
    }

    out->wFormatTag = tag;
    out->nChannels = in->nChannels;
    out->nSamplesPerSec = in->nSamplesPerSec;
    out->wBitsPerSample = in->wBitsPerSample;
    out->nBlockAlign = in->nChannels * (in->wBitsPerSample / 8);
    out->nAvgBytesPerSec = out->nBlockAlign * in->nSamplesPerSec;
    out->cbSize = 0;

    return S_OK;
}

HRESULT converter_calculate_dest_nbytes(
        const WAVEFORMATEX *src,
        const WAVEFORMATEX *dest,
        size_t src_nbytes,
        size_t *out)
{
    uint64_t num;
    uint64_t den;
    uint64_t dest_bpf;
    uint64_t dest_nframes;
    uint64_t src_bpf;
    uint64_t src_nframes;

    assert(src != NULL);
    assert(dest != NULL);
    assert(out != NULL);

    *out = 0;

    if (    src->nSamplesPerSec == 0 ||
            src->nChannels == 0 ||
            src->wBitsPerSample == 0 ||
            src->wBitsPerSample % 8 != 0) {
        trace("Source format is invalid");

        return E_INVALIDARG;
    }

    if (    dest->nSamplesPerSec == 0 ||
            dest->nChannels == 0 ||
            dest->wBitsPerSample == 0 ||
            dest->wBitsPerSample % 8 != 0) {
        trace("Destination format is invalid");

        return E_INVALIDARG;
    }

    src_bpf = src->nChannels * (src->wBitsPerSample / 8);

    /* Round partial trailing frames down rather than failing outright */

    src_nframes = src_nbytes / src_bpf;

    /*  Do an integer quotient rounding upwards here, because I am definitely
        not about to go calculating a buffer size using floating-point
        arithmetic. */

    num = dest->nSamplesPerSec;
    den = src->nSamplesPerSec;
    dest_nframes = (src_nframes * num + (den - 1)) / den;

    dest_bpf = dest->nChannels * (dest->wBitsPerSample / 8);

    *out = dest_nframes * dest_bpf;

    return S_OK;
}

HRESULT converter_alloc(
        struct converter **out,
        const WAVEFORMATEX *src,
        const WAVEFORMATEX *dest,
        void *src_bytes,
        size_t src_nbytes,
        void *dest_bytes,
        size_t dest_nbytes)
{
    struct converter *conv;

    assert(out != NULL);
    assert(src != NULL);
    assert(dest != NULL);
    assert(src_bytes != NULL);
    assert(dest_bytes != NULL);

    *out = NULL;

    if (    dest->wFormatTag != WAVE_FORMAT_PCM ||
            dest->nChannels != 2 ||
            dest->wBitsPerSample != 16) {
        trace("Unsupported destination format");

        return E_NOTIMPL;
    }

    conv = calloc(1, sizeof(*conv));

    if (conv == NULL) {
        return E_OUTOFMEMORY;
    }

    memcpy(&conv->src, src, sizeof(*src));
    memcpy(&conv->dest, dest, sizeof(*dest));
    conv->src_bytes = src_bytes;
    conv->src_bpf = src->nChannels * (src->wBitsPerSample / 8);
    conv->src_nframes = src_nbytes / conv->src_bpf;
    conv->dest_samples = dest_bytes;
    conv->dest_nframes = dest_nbytes / 4;

    if (conv->src_nframes == 0 || conv->dest_nframes == 0) {
        free(conv);

        return E_INVALIDARG;
    }

    *out = conv;

    return S_OK;
}

void converter_free(struct converter *conv)
{
    free(conv);
}

/* Read one source sample, scaled to the s16 range but kept in an int32 */

static int32_t converter_read(
        const struct converter *conv,
        size_t frame,
        unsigned int channel)
{
    const uint8_t *p;
    int32_t v;
    float f;

    p = conv->src_bytes + frame * conv->src_bpf;

    switch (conv->src.wBitsPerSample) {
    case 8:
        return ((int32_t) p[channel] - 128) << 8;

    case 16:
        p += channel * 2;

        return (int16_t) (p[0] | (p[1] << 8));

    case 24:
        p += channel * 3;
        v = (int32_t) ((uint32_t) p[0] << 8 |
                       (uint32_t) p[1] << 16 |
                       (uint32_t) p[2] << 24);

        return v >> 16;

    case 32:
        p += channel * 4;

        if (conv->src.wFormatTag == WAVE_FORMAT_IEEE_FLOAT_) {
            memcpy(&f, p, 4);

            if (!(f == f)) {
                return 0; /* NaN */
            }

            if (f >= 1.0f) {
                return 32767;
            } else if (f <= -1.0f) {
                return -32768;
            }

            return (int32_t) (f * 32767.0f);
        }

        memcpy(&v, p, 4);

        return v >> 16;

    default:
        return 0;
    }
}

static void converter_frame(
        const struct converter *conv,
        size_t j,
        int16_t *out)
{
    uint64_t num;
    uint64_t rate_src;
    uint64_t rate_dest;
    size_t i0;
    size_t i1;
    int64_t frac;
    int32_t s0;
    int32_t s1;
    int32_t v;
    unsigned int ch;
    unsigned int src_ch;

    rate_src = conv->src.nSamplesPerSec;
    rate_dest = conv->dest.nSamplesPerSec;

    num = (uint64_t) j * rate_src;
    i0 = (size_t) (num / rate_dest);
    frac = (int64_t) (num % rate_dest);

    if (i0 >= conv->src_nframes) {
        i0 = conv->src_nframes - 1;
        frac = 0;
    }

    i1 = i0 + 1;

    if (i1 >= conv->src_nframes) {
        i1 = 0; /* Wrap: nicer for looped buffers, irrelevant otherwise */
    }

    for (ch = 0 ; ch < 2 ; ch++) {
        /* Mono is duplicated, >2 channels take front L/R */
        src_ch = conv->src.nChannels == 1 ? 0 : ch;

        s0 = converter_read(conv, i0, src_ch);

        if (frac != 0) {
            s1 = converter_read(conv, i1, src_ch);
            v = s0 + (int32_t) (((int64_t) (s1 - s0) * frac) /
                    (int64_t) rate_dest);
        } else {
            v = s0;
        }

        if (v > INT16_MAX) {
            v = INT16_MAX;
        } else if (v < INT16_MIN) {
            v = INT16_MIN;
        }

        out[ch] = (int16_t) v;
    }
}

HRESULT converter_convert_range(
        struct converter *conv,
        size_t src_offset,
        size_t src_nbytes)
{
    uint64_t rate_src;
    uint64_t rate_dest;
    size_t fa;
    size_t fb;
    size_t ja;
    size_t jb;
    size_t j;

    assert(conv != NULL);

    if (src_nbytes == 0) {
        return S_OK;
    }

    rate_src = conv->src.nSamplesPerSec;
    rate_dest = conv->dest.nSamplesPerSec;

    /* Source frame range touched by the write */

    fa = src_offset / conv->src_bpf;
    fb = (src_offset + src_nbytes + conv->src_bpf - 1) / conv->src_bpf;

    if (fb > conv->src_nframes) {
        fb = conv->src_nframes;
    }

    if (fa >= fb) {
        return S_OK;
    }

    /*  Destination frames whose interpolation window [i0, i0 + 1] overlaps
        [fa, fb). Be generous by a frame either side. */

    ja = (size_t) (((uint64_t) (fa > 0 ? fa - 1 : 0) * rate_dest) / rate_src);
    jb = (size_t) (((uint64_t) fb * rate_dest + rate_src - 1) / rate_src) + 1;

    if (jb > conv->dest_nframes) {
        jb = conv->dest_nframes;
    }

    for (j = ja ; j < jb ; j++) {
        converter_frame(conv, j, &conv->dest_samples[j * 2]);
    }

    /* The last output frame interpolates towards source frame 0 */

    if (fa == 0 && jb < conv->dest_nframes) {
        j = conv->dest_nframes - 1;
        converter_frame(conv, j, &conv->dest_samples[j * 2]);
    }

    return S_OK;
}

HRESULT converter_convert(
        struct converter *conv,
        size_t *src_nprocessed,
        size_t *dest_nprocessed)
{
    HRESULT hr;

    assert(conv != NULL);

    hr = converter_convert_range(
            conv,
            0,
            conv->src_nframes * conv->src_bpf);

    if (src_nprocessed != NULL) {
        *src_nprocessed = conv->src_nframes * conv->src_bpf;
    }

    if (dest_nprocessed != NULL) {
        *dest_nprocessed = conv->dest_nframes * 4;
    }

    return hr;
}
