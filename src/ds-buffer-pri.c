#include <windows.h>
#include <dsound.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "defs.h"
#include "ds-buffer-pri.h"
#include "refcount.h"
#include "trace.h"

struct ds_buffer_pri {
    IDirectSoundBuffer com;
    refcount_t rc;
    WAVEFORMATEX format;
    LONG volume;
    LONG pan;
};

static IDirectSoundBufferVtbl ds_buffer_pri_vtbl;

HRESULT ds_buffer_pri_alloc(struct ds_buffer_pri **out)
{
    struct ds_buffer_pri *self;

    assert(out != NULL);

    *out = NULL;
    self = calloc(sizeof(*self), 1);

    if (self == NULL) {
        return E_OUTOFMEMORY;
    }

    self->com.lpVtbl = &ds_buffer_pri_vtbl;
    self->rc = 1;
    self->format.wFormatTag = WAVE_FORMAT_PCM;
    self->format.nChannels = 2;
    self->format.nSamplesPerSec = 44100;
    self->format.wBitsPerSample = 16;
    self->format.nBlockAlign = 4;
    self->format.nAvgBytesPerSec = 44100 * 4;

    *out = self;

    return S_OK;
}

struct ds_buffer_pri *ds_buffer_pri_downcast(IDirectSoundBuffer *com)
{
    if (com == NULL) {
        return NULL;
    }

    return containerof(com, struct ds_buffer_pri, com);
}

IDirectSoundBuffer *ds_buffer_pri_upcast(struct ds_buffer_pri *self)
{
    if (self == NULL) {
        return NULL;
    }

    return &self->com;
}

struct ds_buffer_pri *ds_buffer_pri_ref(struct ds_buffer_pri *self)
{
    assert(self != NULL);
    refcount_inc(&self->rc);

    return self;
}

struct ds_buffer_pri *ds_buffer_pri_unref(struct ds_buffer_pri *self)
{
    if (self == NULL || refcount_dec(&self->rc) > 0) {
        return NULL;
    }

    free(self);

    return NULL;
}

static __stdcall HRESULT ds_buffer_pri_query_interface(
        IDirectSoundBuffer *com,
        const IID *iid,
        void **out)
{
    struct ds_buffer_pri *self;

    if (iid == NULL || out == NULL) {
        return E_POINTER;
    }

    *out = NULL;
    self = ds_buffer_pri_downcast(com);

    if (    memcmp(iid, &IID_IDirectSoundBuffer8, sizeof(*iid)) == 0 ||
            memcmp(iid, &IID_IDirectSoundBuffer, sizeof(*iid)) == 0 ||
            memcmp(iid, &IID_IUnknown, sizeof(*iid)) == 0) {
        ds_buffer_pri_ref(self);
        *out = com;

        return S_OK;
    } else {
        trace("%s: Unsupported interface %08lx", __func__, iid->Data1);

        return E_NOINTERFACE;
    }
}

static __stdcall ULONG ds_buffer_pri_add_ref(IDirectSoundBuffer *com)
{
    ds_buffer_pri_ref(ds_buffer_pri_downcast(com));

    return 0;
}

static __stdcall ULONG ds_buffer_pri_release(IDirectSoundBuffer *com)
{
    ds_buffer_pri_unref(ds_buffer_pri_downcast(com));

    return 0;
}

static __stdcall HRESULT ds_buffer_pri_set_format(
        IDirectSoundBuffer *com,
        const WAVEFORMATEX *format)
{
    struct ds_buffer_pri *self;

    if (format == NULL) {
        return E_POINTER;
    }

    trace("%s: tag %04x, %u ch, %u Hz, %u bit (mixing format is fixed)",
            __func__,
            format->wFormatTag,
            format->nChannels,
            (unsigned int) format->nSamplesPerSec,
            format->wBitsPerSample);

    self = ds_buffer_pri_downcast(com);
    memcpy(&self->format, format, sizeof(self->format));
    self->format.cbSize = 0;

    return S_OK;
}

/*  Everything below used to be a NULL vtbl slot, i.e. an instant crash for
    any engine that, say, calls Play() on the primary buffer to keep the
    mixer running, or queries its caps/format during initialisation. */

static __stdcall HRESULT ds_buffer_pri_get_caps(
        IDirectSoundBuffer *com,
        DSBCAPS *out)
{
    if (out == NULL) {
        return E_POINTER;
    }

    if (out->dwSize < sizeof(*out)) {
        return DSERR_INVALIDPARAM;
    }

    out->dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_LOCSOFTWARE |
            DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN;
    out->dwBufferBytes = 4096;
    out->dwUnlockTransferRate = 0;
    out->dwPlayCpuOverhead = 0;

    return S_OK;
}

static __stdcall HRESULT ds_buffer_pri_get_current_position(
        IDirectSoundBuffer *com,
        DWORD *play,
        DWORD *write)
{
    if (play != NULL) {
        *play = 0;
    }

    if (write != NULL) {
        *write = 0;
    }

    return S_OK;
}

static __stdcall HRESULT ds_buffer_pri_get_format(
        IDirectSoundBuffer *com,
        WAVEFORMATEX *out,
        DWORD nbytes,
        DWORD *nbytes_out)
{
    struct ds_buffer_pri *self;

    self = ds_buffer_pri_downcast(com);

    if (out == NULL) {
        if (nbytes_out == NULL) {
            return DSERR_INVALIDPARAM;
        }

        *nbytes_out = sizeof(*out);

        return S_OK;
    }

    nbytes = nbytes < sizeof(*out) ? nbytes : sizeof(*out);
    memcpy(out, &self->format, nbytes);

    if (nbytes_out != NULL) {
        *nbytes_out = nbytes;
    }

    return S_OK;
}

static __stdcall HRESULT ds_buffer_pri_get_frequency(
        IDirectSoundBuffer *com,
        DWORD *out)
{
    if (out == NULL) {
        return E_POINTER;
    }

    *out = ds_buffer_pri_downcast(com)->format.nSamplesPerSec;

    return S_OK;
}

static __stdcall HRESULT ds_buffer_pri_get_pan(
        IDirectSoundBuffer *com,
        LONG *out)
{
    if (out == NULL) {
        return E_POINTER;
    }

    *out = ds_buffer_pri_downcast(com)->pan;

    return S_OK;
}

static __stdcall HRESULT ds_buffer_pri_get_status(
        IDirectSoundBuffer *com,
        DWORD *out)
{
    if (out == NULL) {
        return E_POINTER;
    }

    *out = DSBSTATUS_PLAYING | DSBSTATUS_LOOPING;

    return S_OK;
}

static __stdcall HRESULT ds_buffer_pri_get_volume(
        IDirectSoundBuffer *com,
        LONG *out)
{
    if (out == NULL) {
        return E_POINTER;
    }

    *out = ds_buffer_pri_downcast(com)->volume;

    return S_OK;
}

static __stdcall HRESULT ds_buffer_pri_initialize(
        IDirectSoundBuffer *com,
        IDirectSound *api,
        const DSBUFFERDESC *desc)
{
    return DSERR_ALREADYINITIALIZED;
}

static __stdcall HRESULT ds_buffer_pri_lock(
        IDirectSoundBuffer *com,
        DWORD pos,
        DWORD nbytes,
        void **ptr1,
        DWORD *nbytes1,
        void **ptr2,
        DWORD *nbytes2,
        DWORD flags)
{
    trace("%s: direct primary buffer access is not supported", __func__);

    return DSERR_PRIOLEVELNEEDED;
}

static __stdcall HRESULT ds_buffer_pri_play(
        IDirectSoundBuffer *com,
        DWORD reserved1,
        DWORD reserved2,
        DWORD flags)
{
    return S_OK;
}

static __stdcall HRESULT ds_buffer_pri_restore(IDirectSoundBuffer *com)
{
    return S_OK;
}

static __stdcall HRESULT ds_buffer_pri_set_current_position(
        IDirectSoundBuffer *com,
        DWORD pos)
{
    return DSERR_INVALIDCALL;
}

static __stdcall HRESULT ds_buffer_pri_set_frequency(
        IDirectSoundBuffer *com,
        DWORD freq)
{
    return DSERR_CONTROLUNAVAIL;
}

static __stdcall HRESULT ds_buffer_pri_set_pan(
        IDirectSoundBuffer *com,
        LONG pan)
{
    ds_buffer_pri_downcast(com)->pan = pan;

    return S_OK;
}

static __stdcall HRESULT ds_buffer_pri_set_volume(
        IDirectSoundBuffer *com,
        LONG volume)
{
    ds_buffer_pri_downcast(com)->volume = volume;

    return S_OK;
}

static __stdcall HRESULT ds_buffer_pri_stop(IDirectSoundBuffer *com)
{
    return S_OK;
}

static __stdcall HRESULT ds_buffer_pri_unlock(
        IDirectSoundBuffer *com,
        void *ptr1,
        DWORD nbytes1,
        void *ptr2,
        DWORD nbytes2)
{
    return DSERR_INVALIDCALL;
}

static struct IDirectSoundBufferVtbl ds_buffer_pri_vtbl = {
    .QueryInterface     = ds_buffer_pri_query_interface,
    .AddRef             = ds_buffer_pri_add_ref,
    .Release            = ds_buffer_pri_release,
    .GetCaps            = ds_buffer_pri_get_caps,
    .GetCurrentPosition = ds_buffer_pri_get_current_position,
    .GetFormat          = ds_buffer_pri_get_format,
    .GetFrequency       = ds_buffer_pri_get_frequency,
    .GetPan             = ds_buffer_pri_get_pan,
    .GetStatus          = ds_buffer_pri_get_status,
    .GetVolume          = ds_buffer_pri_get_volume,
    .Initialize         = ds_buffer_pri_initialize,
    .Lock               = ds_buffer_pri_lock,
    .Play               = ds_buffer_pri_play,
    .Restore            = ds_buffer_pri_restore,
    .SetCurrentPosition = ds_buffer_pri_set_current_position,
    .SetFormat          = ds_buffer_pri_set_format,
    .SetFrequency       = ds_buffer_pri_set_frequency,
    .SetPan             = ds_buffer_pri_set_pan,
    .SetVolume          = ds_buffer_pri_set_volume,
    .Stop               = ds_buffer_pri_stop,
    .Unlock             = ds_buffer_pri_unlock,
};
