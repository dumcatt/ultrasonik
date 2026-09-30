#include <windows.h>

#include <mmreg.h>
#include <objbase.h>
#include <process.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asio.h"
#include "asio-iface.h"
#include "config.h"
#include "defs.h"
#include "hr.h"
#include "snd-mixer.h"
#include "snd-service.h"
#include "trace.h"

struct asio_backend {
    IASIO *driver;
    struct snd_service *svc;
    struct snd_mixer *mixer;
    struct config cfg;
    WAVEFORMATEX sys_wfx;
    ASIOBufferInfo buffer_info[2]; /* stereo: output L, output R */
    ASIOSampleType sample_type;
    long buffer_size;
    HANDLE started;
    HANDLE stop;
    volatile int running;
    bool com_initialized;
};

/*  We need a global pointer because ASIO callbacks don't carry a context
    parameter. The ASIO spec assumes a single driver instance per process,
    so this is consistent with the ASIO design. */

static struct asio_backend *g_asio;

static void asio_buffer_switch(long index, ASIOBool direct_process);
static void asio_sample_rate_changed(ASIOSampleRate rate);
static long asio_message(long selector, long value, void *message, double *opt);

static ASIOCallbacks asio_callbacks = {
    .bufferSwitch           = asio_buffer_switch,
    .sampleRateDidChange    = asio_sample_rate_changed,
    .asioMessage            = asio_message,
    .bufferSwitchTimeInfo   = NULL,
};

/* ASIO message selectors */
#define kAsioSelectorSupported  1
#define kAsioEngineVersion      2
#define kAsioSupportsTimeInfo   4
#define kAsioSupportsTimeCode   5

static ASIOSampleType asio_sample_type_from_depth(uint32_t bit_depth)
{
    switch (bit_depth) {
    case 16:    return ASIOSTInt16LSB;
    case 24:    return ASIOSTInt24LSB;
    case 32:    return ASIOSTInt32LSB;
    default:    return ASIOSTInt24LSB;
    }
}

static size_t asio_sample_byte_size(ASIOSampleType type)
{
    switch (type) {
    case ASIOSTInt16LSB:    return 2;
    case ASIOSTInt24LSB:    return 3;
    case ASIOSTInt32LSB:    return 4;
    case ASIOSTFloat32LSB:  return 4;
    case ASIOSTFloat64LSB:  return 8;
    default:                return 4;
    }
}

static void asio_convert_s16_to_asio(
        const int16_t *src,
        void *left,
        void *right,
        long nframes,
        ASIOSampleType type)
{
    long i;
    uint8_t *lp = (uint8_t *) left;
    uint8_t *rp = (uint8_t *) right;

    switch (type) {
    case ASIOSTInt16LSB:
        for (i = 0; i < nframes; i++) {
            int16_t l = src[i * 2 + 0];
            int16_t r = src[i * 2 + 1];

            lp[i * 2 + 0] = (uint8_t) (l & 0xFF);
            lp[i * 2 + 1] = (uint8_t) ((l >> 8) & 0xFF);
            rp[i * 2 + 0] = (uint8_t) (r & 0xFF);
            rp[i * 2 + 1] = (uint8_t) ((r >> 8) & 0xFF);
        }

        break;

    case ASIOSTInt24LSB:
        for (i = 0; i < nframes; i++) {
            /* Sign-extend 16-bit to 24-bit by shifting left 8 bits */
            int32_t l = ((int32_t) src[i * 2 + 0]) << 8;
            int32_t r = ((int32_t) src[i * 2 + 1]) << 8;

            lp[i * 3 + 0] = (uint8_t) (l & 0xFF);
            lp[i * 3 + 1] = (uint8_t) ((l >> 8) & 0xFF);
            lp[i * 3 + 2] = (uint8_t) ((l >> 16) & 0xFF);
            rp[i * 3 + 0] = (uint8_t) (r & 0xFF);
            rp[i * 3 + 1] = (uint8_t) ((r >> 8) & 0xFF);
            rp[i * 3 + 2] = (uint8_t) ((r >> 16) & 0xFF);
        }

        break;

    case ASIOSTInt32LSB:
        for (i = 0; i < nframes; i++) {
            /* Sign-extend 16-bit to 32-bit by shifting left 16 bits */
            int32_t l = ((int32_t) src[i * 2 + 0]) << 16;
            int32_t r = ((int32_t) src[i * 2 + 1]) << 16;

            memcpy(lp + i * 4, &l, 4);
            memcpy(rp + i * 4, &r, 4);
        }

        break;

    case ASIOSTFloat32LSB:
        for (i = 0; i < nframes; i++) {
            float l = src[i * 2 + 0] / 32768.0f;
            float r = src[i * 2 + 1] / 32768.0f;

            memcpy(lp + i * 4, &l, 4);
            memcpy(rp + i * 4, &r, 4);
        }

        break;

    default:
        /* Unsupported format: silence */
        memset(left, 0, nframes * asio_sample_byte_size(type));
        memset(right, 0, nframes * asio_sample_byte_size(type));

        break;
    }
}

static void asio_buffer_switch(long index, ASIOBool direct_process)
{
    struct asio_backend *asio = g_asio;
    int16_t *mix_buf;
    size_t mix_nbytes;

    if (asio == NULL || !asio->running) {
        return;
    }

    mix_nbytes = asio->buffer_size * 2 * sizeof(int16_t);
    mix_buf = (int16_t *) _alloca(mix_nbytes);

    snd_service_intake(asio->svc, asio->mixer);
    snd_mixer_mix(asio->mixer, mix_buf);
    snd_service_exhaust(asio->svc);

    asio_convert_s16_to_asio(
            mix_buf,
            asio->buffer_info[0].buffers[index],
            asio->buffer_info[1].buffers[index],
            asio->buffer_size,
            asio->sample_type);

    asio->driver->lpVtbl->outputReady(asio->driver);
}

static void asio_sample_rate_changed(ASIOSampleRate rate)
{
    trace("ASIO: Sample rate changed to %g Hz", rate);
}

static long asio_message(long selector, long value, void *message, double *opt)
{
    switch (selector) {
    case kAsioSelectorSupported:
        switch (value) {
        case kAsioEngineVersion:
            return 1;

        default:
            return 0;
        }

    case kAsioEngineVersion:
        return 2;

    default:
        return 0;
    }
}

HRESULT asio_alloc(struct asio_backend **out)
{
    struct asio_backend *asio;
    HRESULT hr;
    int r;

    trace_enter();
    assert(out != NULL);

    *out = NULL;
    asio = calloc(sizeof(*asio), 1);

    if (asio == NULL) {
        hr = E_OUTOFMEMORY;

        goto end;
    }

    config_load(&asio->cfg);

    trace("ASIO config: device='%s' rate=%u depth=%u bufsize=%u",
            asio->cfg.device,
            asio->cfg.sample_rate,
            asio->cfg.bit_depth,
            asio->cfg.buffer_size);

    asio->sample_type = asio_sample_type_from_depth(asio->cfg.bit_depth);

    /* Set up the WAVEFORMATEX that the rest of Ultrasonik will use */
    asio->sys_wfx.wFormatTag        = WAVE_FORMAT_PCM;
    asio->sys_wfx.nChannels         = 2;
    asio->sys_wfx.nSamplesPerSec    = asio->cfg.sample_rate;
    asio->sys_wfx.wBitsPerSample    = 16;
    asio->sys_wfx.nBlockAlign       = 4;
    asio->sys_wfx.nAvgBytesPerSec   = asio->cfg.sample_rate * 4;
    asio->sys_wfx.cbSize            = 0;

    asio->started = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (asio->started == NULL) {
        hr = hr_from_win32();
        hr_trace("CreateEvent", hr);

        goto end;
    }

    asio->stop = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (asio->stop == NULL) {
        hr = hr_from_win32();
        hr_trace("CreateEvent", hr);

        goto end;
    }

    r = snd_service_alloc(&asio->svc);

    if (r < 0) {
        hr = hr_from_errno(r);

        goto end;
    }

    *out = asio;
    asio = NULL;
    hr = S_OK;

end:
    asio_free(asio);
    trace_exit();

    return hr;
}

void asio_free(struct asio_backend *asio)
{
    BOOL ok;

    if (asio == NULL) {
        return;
    }

    asio_stop(asio);

    snd_mixer_free(asio->mixer);
    snd_service_free(asio->svc);

    if (asio->stop != NULL) {
        ok = CloseHandle(asio->stop);

        if (!ok) {
            hr_trace("CloseHandle(asio->stop)", hr_from_win32());
        }
    }

    if (asio->started != NULL) {
        ok = CloseHandle(asio->started);

        if (!ok) {
            hr_trace("CloseHandle(asio->started)", hr_from_win32());
        }
    }

    free(asio);
}

HRESULT asio_start(struct asio_backend *asio)
{
    IASIO *drv;
    ASIOError ae;
    ASIOChannelInfo ch_info;
    char driver_name[256];
    long min_size, max_size, preferred_size, granularity;
    long in_channels, out_channels;
    long in_latency, out_latency;
    HRESULT hr;
    int r;

    assert(asio != NULL);

    trace("ASIO: Initializing COM");

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (SUCCEEDED(hr)) {
        asio->com_initialized = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        trace("ASIO: Thread already in MTA mode (RPC_E_CHANGED_MODE), continuing");
        hr = S_OK;
    } else {
        hr_trace("CoInitializeEx", hr);

        goto end;
    }

    /* Open the ASIO driver */

    if (asio->cfg.device[0] != '\0') {
        trace("ASIO: Opening driver '%s'", asio->cfg.device);
        hr = asio_driver_open_by_name(&drv, asio->cfg.device);
    } else {
        trace("ASIO: Opening first available driver");
        hr = asio_driver_open_first(&drv, driver_name, sizeof(driver_name));

        if (SUCCEEDED(hr)) {
            trace("ASIO: Found driver '%s'", driver_name);
        }
    }

    if (FAILED(hr)) {
        trace("ASIO: Failed to open driver, hr=%08x", hr);

        goto end;
    }

    asio->driver = drv;

    /* Initialize the driver */

    if (!drv->lpVtbl->init(drv, NULL)) {
        trace("ASIO: Driver init failed");
        hr = E_FAIL;

        goto end;
    }

    drv->lpVtbl->getDriverName(drv, driver_name);
    trace("ASIO: Driver name: %s", driver_name);
    trace("ASIO: Driver version: %ld", drv->lpVtbl->getDriverVersion(drv));

    /* Query channel count */

    ae = drv->lpVtbl->getChannels(drv, &in_channels, &out_channels);

    if (ae != ASE_OK) {
        trace("ASIO: getChannels failed: %ld", ae);
        hr = E_FAIL;

        goto end;
    }

    trace("ASIO: Channels: %ld in, %ld out", in_channels, out_channels);

    if (out_channels < 2) {
        trace("ASIO: Need at least 2 output channels, got %ld", out_channels);
        hr = E_FAIL;

        goto end;
    }

    /* Set sample rate */

    ae = drv->lpVtbl->canSampleRate(drv, (ASIOSampleRate) asio->cfg.sample_rate);

    if (ae != ASE_OK) {
        trace("ASIO: Driver does not support %u Hz", asio->cfg.sample_rate);
        hr = E_FAIL;

        goto end;
    }

    ae = drv->lpVtbl->setSampleRate(drv, (ASIOSampleRate) asio->cfg.sample_rate);

    if (ae != ASE_OK) {
        trace("ASIO: setSampleRate(%u) failed: %ld",
                asio->cfg.sample_rate, ae);
        hr = E_FAIL;

        goto end;
    }

    /* Query buffer sizes */

    ae = drv->lpVtbl->getBufferSize(
            drv,
            &min_size,
            &max_size,
            &preferred_size,
            &granularity);

    if (ae != ASE_OK) {
        trace("ASIO: getBufferSize failed: %ld", ae);
        hr = E_FAIL;

        goto end;
    }

    trace("ASIO: Buffer sizes: min=%ld max=%ld preferred=%ld granularity=%ld",
            min_size, max_size, preferred_size, granularity);

    if (asio->cfg.buffer_size == 0) {
        asio->buffer_size = preferred_size;
        trace("ASIO: Using preferred buffer size: %ld", asio->buffer_size);
    } else {
        asio->buffer_size = (long) asio->cfg.buffer_size;

        /* Clamp to driver's supported range */
        if (asio->buffer_size < min_size) {
            trace("ASIO: Requested buffer %ld < min %ld, clamping",
                    asio->buffer_size, min_size);
            asio->buffer_size = min_size;
        }

        if (asio->buffer_size > max_size) {
            trace("ASIO: Requested buffer %ld > max %ld, clamping",
                    asio->buffer_size, max_size);
            asio->buffer_size = max_size;
        }

        /* Align to granularity if required */
        if (granularity > 1) {
            long aligned = (asio->buffer_size / granularity) * granularity;

            if (aligned < min_size) {
                aligned += granularity;
            }

            if (aligned != asio->buffer_size) {
                trace("ASIO: Aligned buffer size from %ld to %ld",
                        asio->buffer_size, aligned);
                asio->buffer_size = aligned;
            }
        }

        trace("ASIO: Using requested buffer size: %ld", asio->buffer_size);
    }

    trace("ASIO: Negotiated latency: %ld frames (%f ms)",
            asio->buffer_size,
            asio->buffer_size * 1000.0 / asio->cfg.sample_rate);

    /* Query output channel format */

    memset(&ch_info, 0, sizeof(ch_info));
    ch_info.channel = 0;
    ch_info.isInput = ASIOFalse;

    ae = drv->lpVtbl->getChannelInfo(drv, &ch_info);

    if (ae != ASE_OK) {
        trace("ASIO: getChannelInfo failed: %ld", ae);
        hr = E_FAIL;

        goto end;
    }

    trace("ASIO: Output channel 0: type=%ld name='%s'",
            (long) ch_info.type, ch_info.name);

    /*  Use the device's native sample type. Override our configured type
        if the device reports something different. */

    asio->sample_type = ch_info.type;
    trace("ASIO: Using sample type %ld from device", (long) asio->sample_type);

    /* Create output buffers (stereo: channels 0 and 1) */

    memset(asio->buffer_info, 0, sizeof(asio->buffer_info));
    asio->buffer_info[0].isInput = ASIOFalse;
    asio->buffer_info[0].channelNum = 0;
    asio->buffer_info[1].isInput = ASIOFalse;
    asio->buffer_info[1].channelNum = 1;

    ae = drv->lpVtbl->createBuffers(
            drv,
            asio->buffer_info,
            2,
            asio->buffer_size,
            &asio_callbacks);

    if (ae != ASE_OK) {
        trace("ASIO: createBuffers failed: %ld", ae);
        hr = E_FAIL;

        goto end;
    }

    /* Allocate the mixer */

    r = snd_mixer_alloc(&asio->mixer, asio->buffer_size, 2);

    if (r < 0) {
        trace("ASIO: snd_mixer_alloc failed: r=%i", r);
        hr = hr_from_errno(r);

        goto end;
    }

    /* Set the global pointer before starting */
    g_asio = asio;
    asio->running = 1;

    /* Query latencies */

    ae = drv->lpVtbl->getLatencies(drv, &in_latency, &out_latency);

    if (ae == ASE_OK) {
        trace("ASIO: Latencies: input=%ld output=%ld", in_latency, out_latency);
    }

    /* Start streaming */

    ae = drv->lpVtbl->start(drv);

    if (ae != ASE_OK) {
        trace("ASIO: start() failed: %ld", ae);
        asio->running = 0;
        g_asio = NULL;
        hr = E_FAIL;

        goto end;
    }

    trace("ASIO: Streaming started successfully");

    SetEvent(asio->started);
    hr = S_OK;

end:
    return hr;
}

HRESULT asio_snd_client_alloc(
        struct asio_backend *asio,
        struct snd_client **out)
{
    int r;

    assert(asio != NULL);

    r = snd_client_alloc(out, asio->svc);

    return hr_from_errno(r);
}

size_t asio_get_period_frames(const struct asio_backend *asio)
{
    assert(asio != NULL);

    return asio->buffer_size > 0 ? (size_t) asio->buffer_size : 512;
}

const WAVEFORMATEX *asio_get_sys_format(const struct asio_backend *asio)
{
    assert(asio != NULL);

    return &asio->sys_wfx;
}

HRESULT asio_stop(struct asio_backend *asio)
{
    assert(asio != NULL);

    if (asio->driver == NULL) {
        return S_FALSE;
    }

    trace("ASIO: Stopping");

    asio->running = 0;
    g_asio = NULL;

    asio->driver->lpVtbl->stop(asio->driver);
    asio->driver->lpVtbl->disposeBuffers(asio->driver);
    asio->driver->lpVtbl->Release(asio->driver);
    asio->driver = NULL;

    if (asio->com_initialized) {
        CoUninitialize();
        asio->com_initialized = false;
    }

    trace("ASIO: Stopped");

    return S_OK;
}
