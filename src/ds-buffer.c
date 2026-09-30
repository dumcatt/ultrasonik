#include <windows.h>
#include <dsound.h>

#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "converter.h"
#include "defs.h"
#include "ds-buffer.h"
#include "hr.h"
#include "reaper.h"
#include "refcount.h"
#include "snd-buffer.h"
#include "snd-service.h"
#include "snd-stream.h"
#include "trace.h"

#include "guid.h"

/*  IDirectSoundBuffer8 is IDirectSoundBuffer plus three extra methods. We
    previously claimed to support IID_IDirectSoundBuffer8 while handing out a
    plain IDirectSoundBuffer vtbl, so any call to one of those methods would
    jump through garbage. */

struct ds_buffer_vtbl8 {
    IDirectSoundBufferVtbl base;
    HRESULT (__stdcall *SetFX)(
            IDirectSoundBuffer *com,
            DWORD count,
            DSEFFECTDESC *descs,
            DWORD *results);
    HRESULT (__stdcall *AcquireResources)(
            IDirectSoundBuffer *com,
            DWORD flags,
            DWORD count,
            DWORD *results);
    HRESULT (__stdcall *GetObjectInPath)(
            IDirectSoundBuffer *com,
            const GUID *guid_object,
            DWORD index,
            const GUID *guid_iface,
            void **out);
};

struct ds_buffer {
    IDirectSoundBuffer com;
    IDirectSoundNotify notify_com;
    refcount_t rc;
    CRITICAL_SECTION lock;
    dtor_notify_t dtor_notify;
    void *dtor_notify_ctx;
    struct converter *conv;
    void *conv_bytes;
    size_t conv_nbytes;
    struct reaper *reaper;
    struct reaper_task *rtask;
    struct snd_buffer *buf;
    struct snd_stream *stm;
    struct snd_client *cli;
    WAVEFORMATEX format;
    WAVEFORMATEX format_sys;
    DWORD flags;
    size_t lead_frames;
    LONG volume;
    LONG pan;
    DWORD frequency;
    bool buf_owned;
    bool playing;
    bool looping;
};

static bool ds_buffer_requires_conversion(const struct ds_buffer *self);
static HRESULT ds_buffer_prepare_conversion(struct ds_buffer *self);
static size_t ds_buffer_client_bytes_to_sys_samples(
        const struct ds_buffer *self,
        size_t nbytes);
static size_t ds_buffer_sys_frames_to_client_bytes(
        const struct ds_buffer *self,
        size_t nframes);
static HRESULT ds_buffer_submit_volume(struct ds_buffer *self);
static HRESULT ds_buffer_unlock_span(
        struct ds_buffer *self,
        void *bytes,
        DWORD nbytes);

static const struct ds_buffer_vtbl8 ds_buffer_vtbl;
static const IDirectSoundNotifyVtbl ds_buffer_notify_vtbl;

HRESULT ds_buffer_alloc(
        struct ds_buffer **out,
        dtor_notify_t dtor_notify,
        void *dtor_notify_ctx,
        struct reaper *reaper,
        struct snd_client *cli,
        struct snd_buffer *buf,
        const WAVEFORMATEX *format,
        const WAVEFORMATEX *format_sys,
        size_t nbytes,
        DWORD flags,
        size_t lead_frames)
{
    struct ds_buffer *self;
    size_t sys_nbytes;
    HRESULT hr;
    int r;

    assert(out != NULL);
    assert(cli != NULL);
    assert(reaper != NULL);
    assert(format != NULL);
    assert(format_sys != NULL);

    *out = NULL;
    self = NULL;

    if (format_sys->nChannels != 2 || format_sys->wBitsPerSample != 16) {
        trace("Unsupported system audio format");
        hr = E_NOTIMPL;

        goto end;
    }

    self = calloc(sizeof(*self), 1);

    if (self == NULL) {
        hr = E_OUTOFMEMORY;

        goto end;
    }

    self->com.lpVtbl = (IDirectSoundBufferVtbl *) &ds_buffer_vtbl.base;
    self->notify_com.lpVtbl = (IDirectSoundNotifyVtbl *) &ds_buffer_notify_vtbl;
    self->rc = 1;
    InitializeCriticalSection(&self->lock);

    hr = converter_normalize_format(format, &self->format);

    if (FAILED(hr)) {
        trace("Rejecting buffer: unsupported format (tag %04x, %u ch, "
                "%u Hz, %u bit)",
                format->wFormatTag,
                format->nChannels,
                (unsigned int) format->nSamplesPerSec,
                format->wBitsPerSample);

        goto end;
    }

    memcpy(&self->format_sys, format_sys, sizeof(*format_sys));

    /*  Real DirectSound rounds the buffer size up to a whole number of
        frames. We used to reject such buffers outright. */

    if (nbytes % self->format.nBlockAlign != 0) {
        nbytes += self->format.nBlockAlign - nbytes % self->format.nBlockAlign;
    }

    if (nbytes == 0) {
        trace("Rejecting zero-length buffer");
        hr = E_INVALIDARG;

        goto end;
    }

    self->conv_nbytes = nbytes;
    self->flags = flags;
    self->lead_frames = lead_frames;
    self->volume = DSBVOLUME_MAX;
    self->pan = DSBPAN_CENTER;
    self->frequency = self->format.nSamplesPerSec;

    hr = converter_calculate_dest_nbytes(
            &self->format,
            format_sys,
            nbytes,
            &sys_nbytes);

    if (FAILED(hr)) {
        goto end;
    }

    if (buf != NULL) {
        self->buf = buf;
    } else {
        r = snd_buffer_alloc(&self->buf, sys_nbytes / 2);

        if (r < 0) {
            hr = hr_from_errno(r);
            trace("snd_buffer_alloc failed: %i", r);

            goto end;
        }

        self->buf_owned = true;
    }

    r = snd_stream_alloc(&self->stm, self->buf);

    if (r < 0) {
        hr = hr_from_errno(r);
        trace("snd_stream_alloc failed: %i", r);

        goto end;
    }

    /* Pre-allocate a reaper task to clean up this object */

    self->reaper = reaper;
    hr = reaper_alloc_task(
            reaper,
            &self->rtask,
            self->stm,
            self->buf_owned ? self->buf : NULL);

    if (FAILED(hr)) {
        goto end;
    }

    /*  Commit to constructing this object: Take ownership of passed-in
        resources and store the destructor notification callback. */

    self->cli = cli;
    self->dtor_notify = dtor_notify;
    self->dtor_notify_ctx = dtor_notify_ctx;
    dtor_notify = NULL;

    *out = ds_buffer_ref(self);
    hr = S_OK;

end:
    ds_buffer_unref(self);

    if (dtor_notify != NULL) {
        dtor_notify(dtor_notify_ctx);
    }

    return hr;
}

struct ds_buffer *ds_buffer_downcast(IDirectSoundBuffer *com)
{
    if (com == NULL) {
        return NULL;
    }

    return containerof(com, struct ds_buffer, com);
}

static struct ds_buffer *ds_buffer_notify_downcast(IDirectSoundNotify *com)
{
    if (com == NULL) {
        return NULL;
    }

    return containerof(com, struct ds_buffer, notify_com);
}

IDirectSoundBuffer *ds_buffer_upcast(struct ds_buffer *self)
{
    if (self == NULL) {
        return NULL;
    }

    return &self->com;
}

struct ds_buffer *ds_buffer_ref(struct ds_buffer *self)
{
    assert(self != NULL);
    refcount_inc(&self->rc);

    return self;
}

struct ds_buffer *ds_buffer_ref_checked(IDirectSoundBuffer *com)
{
    IDirectSoundBuffer *checked;
    HRESULT hr;

    if (com == NULL) {
        return NULL;
    }

    hr = IDirectSoundBuffer_QueryInterface(
            com,
            &ds_buffer_private_iid,
            (void **) &checked);

    if (FAILED(hr)) {
        return NULL;
    }

    return ds_buffer_downcast(checked);
}

struct ds_buffer *ds_buffer_unref(struct ds_buffer *self)
{
    if (self == NULL || refcount_dec(&self->rc) > 0) {
        return NULL;
    }

    free(self->conv_bytes);
    converter_free(self->conv);
    snd_client_free(self->cli);

    if (self->rtask != NULL) {
        /*  Make sure the application's event handles are never signalled
            after it has released the buffer, then asynchronously destroy our
            stream and (if we own it) buffer */

        snd_stream_disable_notifications(self->stm);
        reaper_submit_task(self->reaper, self->rtask);
    } else {
        /* Construction failed; nothing was ever submitted to the mixer */

        snd_stream_free(self->stm);

        if (self->buf_owned) {
            snd_buffer_free(self->buf);
        }
    }

    if (self->dtor_notify != NULL) {
        self->dtor_notify(self->dtor_notify_ctx);
    }

    DeleteCriticalSection(&self->lock);
    free(self);

    return NULL;
}

void ds_buffer_unref_notify(void *ptr)
{
    ds_buffer_unref(ptr);
}

struct snd_buffer *ds_buffer_get_snd_buffer(struct ds_buffer *self)
{
    assert(self != NULL);

    return self->buf;
}

const WAVEFORMATEX *ds_buffer_get_format_(const struct ds_buffer *self)
{
    assert(self != NULL);

    return &self->format;
}

size_t ds_buffer_get_nbytes(const struct ds_buffer *self)
{
    assert(self != NULL);

    return self->conv_nbytes;
}

DWORD ds_buffer_get_flags(const struct ds_buffer *self)
{
    assert(self != NULL);

    return self->flags;
}

static bool ds_buffer_requires_conversion(const struct ds_buffer *self)
{
    assert(self != NULL);

    return  self->format.wFormatTag != WAVE_FORMAT_PCM ||
            self->format.nSamplesPerSec != self->format_sys.nSamplesPerSec ||
            self->format.nChannels != self->format_sys.nChannels ||
            self->format.wBitsPerSample != self->format_sys.wBitsPerSample;
}

static HRESULT ds_buffer_prepare_conversion(struct ds_buffer *self)
{
    HRESULT hr;

    assert(self != NULL);

    if (self->conv != NULL) {
        return S_FALSE;
    }

    assert(self->conv_bytes == NULL);

    self->conv_bytes = malloc(self->conv_nbytes);

    if (self->conv_bytes == NULL) {
        hr = E_OUTOFMEMORY;

        goto fail;
    }

    /* Silence is 0x80 for unsigned 8-bit PCM, zero otherwise */

    memset( self->conv_bytes,
            self->format.wBitsPerSample == 8 ? 0x80 : 0x00,
            self->conv_nbytes);

    hr = converter_alloc(
            &self->conv,
            &self->format,
            &self->format_sys,
            self->conv_bytes,
            self->conv_nbytes,
            snd_buffer_samples_rw(self->buf),
            snd_buffer_nbytes(self->buf));

    if (FAILED(hr)) {
        trace("converter_alloc failed: %08x", (unsigned int) hr);

        goto fail;
    }

    return S_OK;

fail:
    converter_free(self->conv);
    self->conv = NULL;
    free(self->conv_bytes);
    self->conv_bytes = NULL;

    return hr;
}

static size_t ds_buffer_client_bytes_to_sys_samples(
        const struct ds_buffer *self,
        size_t nbytes)
{
    uint64_t frames;

    frames = nbytes / self->format.nBlockAlign;
    frames = frames * self->format_sys.nSamplesPerSec
            / self->format.nSamplesPerSec;

    return (size_t) frames * 2;
}

static size_t ds_buffer_sys_frames_to_client_bytes(
        const struct ds_buffer *self,
        size_t nframes)
{
    uint64_t frames;
    size_t nbytes;

    frames = (uint64_t) nframes * self->format.nSamplesPerSec
            / self->format_sys.nSamplesPerSec;
    nbytes = (size_t) frames * self->format.nBlockAlign;

    return nbytes;
}

static __stdcall HRESULT ds_buffer_query_interface(
        IDirectSoundBuffer *com,
        const IID *iid,
        void **out)
{
    struct ds_buffer *self;

    if (iid == NULL || out == NULL) {
        return E_POINTER;
    }

    *out = NULL;
    self = ds_buffer_downcast(com);

    if (    memcmp(iid, &ds_buffer_private_iid, sizeof(*iid)) == 0 ||
            memcmp(iid, &IID_IDirectSoundBuffer8, sizeof(*iid)) == 0 ||
            memcmp(iid, &IID_IDirectSoundBuffer, sizeof(*iid)) == 0 ||
            memcmp(iid, &IID_IUnknown, sizeof(*iid)) == 0) {
        ds_buffer_ref(self);
        *out = com;

        return S_OK;
    }

    if (memcmp(iid, &IID_IDirectSoundNotify, sizeof(*iid)) == 0) {
        ds_buffer_ref(self);
        *out = &self->notify_com;

        return S_OK;
    }

    trace("%s: %p: Unsupported interface "
            "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
            __func__,
            self,
            iid->Data1,
            iid->Data2,
            iid->Data3,
            iid->Data4[0], iid->Data4[1], iid->Data4[2], iid->Data4[3],
            iid->Data4[4], iid->Data4[5], iid->Data4[6], iid->Data4[7]);

    return E_NOINTERFACE;
}

static __stdcall ULONG ds_buffer_add_ref(IDirectSoundBuffer *com)
{
    struct ds_buffer *self;

    self = ds_buffer_downcast(com);
    ds_buffer_ref(self);

    return atomic_load(&self->rc);
}

static __stdcall ULONG ds_buffer_release(IDirectSoundBuffer *com)
{
    struct ds_buffer *self;
    ULONG rc;

    self = ds_buffer_downcast(com);
    rc = atomic_load(&self->rc) - 1;
    ds_buffer_unref(self);

    return rc;
}

static __stdcall HRESULT ds_buffer_get_caps(
        IDirectSoundBuffer *com,
        DSBCAPS *out)
{
    struct ds_buffer *self;

    if (out == NULL) {
        return E_POINTER;
    }

    if (out->dwSize < sizeof(*out)) {
        trace("%s: unexpected out param size: %i", __func__,
                (int) out->dwSize);

        return E_INVALIDARG;
    }

    self = ds_buffer_downcast(com);
    out->dwFlags = (self->flags & ~(DSBCAPS_LOCDEFER | DSBCAPS_LOCHARDWARE))
            | DSBCAPS_LOCSOFTWARE;
    out->dwBufferBytes = self->conv_nbytes;
    out->dwUnlockTransferRate = 0;
    out->dwPlayCpuOverhead = 0;

    return S_OK;
}

static __stdcall HRESULT ds_buffer_get_current_position(
        IDirectSoundBuffer *com,
        DWORD *cur_play_byte_no,
        DWORD *cur_write_byte_no)
{
    struct ds_buffer *self;
    size_t sys_frame_pos;
    size_t play;
    size_t write;
    bool playing;

    self = ds_buffer_downcast(com);

    sys_frame_pos = snd_stream_peek_position(self->stm);
    play = ds_buffer_sys_frames_to_client_bytes(self, sys_frame_pos);

    if (play >= self->conv_nbytes) {
        /* Non-looping buffer that has run off the end */
        play = 0;
    }

    /*  The write cursor marks the point beyond which it is safe to write.
        The audio thread may read up to one ASIO period beyond the current
        play position on its next callback, so report the write cursor that
        far ahead while playing. This used to always be 0, which confuses
        streaming code that fills the region between the cursors. */

    playing = self->playing && !snd_stream_is_finished(self->stm);
    write = play;

    if (playing) {
        write += ds_buffer_sys_frames_to_client_bytes(
                self,
                self->lead_frames) + self->format.nBlockAlign;
        write %= self->conv_nbytes;
    }

    if (cur_play_byte_no != NULL) {
        *cur_play_byte_no = play;
    }

    if (cur_write_byte_no != NULL) {
        *cur_write_byte_no = write;
    }

    return S_OK;
}

static __stdcall HRESULT ds_buffer_get_format(
        IDirectSoundBuffer *com,
        WAVEFORMATEX *out,
        DWORD nbytes,
        DWORD *nbytes_out)
{
    struct ds_buffer *self;

    if (out != NULL) {
        self = ds_buffer_downcast(com);
        nbytes = nbytes < sizeof(*out) ? nbytes : sizeof(*out);
        memcpy(out, &self->format, nbytes);

        if (nbytes_out != NULL) {
            *nbytes_out = nbytes;
        }
    } else {
        if (nbytes_out != NULL) {
            *nbytes_out = sizeof(*out);
        } else {
            trace("%s: ??? Both out ptrs NULL", __func__);

            return E_INVALIDARG;
        }
    }

    return S_OK;
}

static __stdcall HRESULT ds_buffer_get_frequency(
        IDirectSoundBuffer *com,
        DWORD *out)
{
    struct ds_buffer *self;

    if (out == NULL) {
        return E_POINTER;
    }

    self = ds_buffer_downcast(com);
    *out = self->frequency;

    return S_OK;
}

static __stdcall HRESULT ds_buffer_get_pan(
        IDirectSoundBuffer *com,
        LONG *out)
{
    if (out == NULL) {
        return E_POINTER;
    }

    *out = ds_buffer_downcast(com)->pan;

    return S_OK;
}

static __stdcall HRESULT ds_buffer_get_status(
        IDirectSoundBuffer *com,
        DWORD *out)
{
    struct ds_buffer *self;
    DWORD status;

    if (out == NULL) {
        return E_POINTER;
    }

    self = ds_buffer_downcast(com);

    EnterCriticalSection(&self->lock);

    /* Make sure self->playing is up to date */

    if (self->playing && snd_stream_is_finished(self->stm)) {
        self->playing = false;
        self->looping = false;
    }

    status = 0;

    if (self->playing) {
        status |= DSBSTATUS_PLAYING;

        if (self->looping) {
            status |= DSBSTATUS_LOOPING;
        }
    }

    LeaveCriticalSection(&self->lock);

    *out = status;

    return S_OK;
}

static __stdcall HRESULT ds_buffer_get_volume(
        IDirectSoundBuffer *com,
        LONG *out)
{
    if (out == NULL) {
        return E_POINTER;
    }

    *out = ds_buffer_downcast(com)->volume;

    return S_OK;
}

static __stdcall HRESULT ds_buffer_initialize(
        IDirectSoundBuffer *com,
        IDirectSound *api,
        const DSBUFFERDESC *desc)
{
    trace("%s(%p, %p)?", __func__, api, desc);

    return DSERR_ALREADYINITIALIZED;
}

static __stdcall HRESULT ds_buffer_lock(
        IDirectSoundBuffer *com,
        DWORD in_pos,
        DWORD in_nbytes,
        void **out_ptr,
        DWORD *out_nbytes,
        void **out_ptr2,
        DWORD *out_nbytes2,
        DWORD flags)
{
    struct ds_buffer *self;
    DWORD write_pos;
    uint8_t *buf_bytes;
    size_t buf_nbytes;
    size_t first;
    HRESULT hr;

    self = ds_buffer_downcast(com);

    /* Initial parameter validation */

    if (out_ptr == NULL || out_nbytes == NULL) {
        trace("%s: Main span out params are NULL", __func__);

        return DSERR_INVALIDPARAM;
    }

    *out_ptr = NULL;
    *out_nbytes = 0;

    if (out_ptr2 != NULL) {
        *out_ptr2 = NULL;
    }

    if (out_nbytes2 != NULL) {
        *out_nbytes2 = 0;
    }

    EnterCriticalSection(&self->lock);

    /* Acquire a suitable destination buffer */

    if (ds_buffer_requires_conversion(self)) {
        /*  Lazily allocate the conversion buffer. This might be a cloned
            buffer that never actually gets locked, after all. No sense in
            wasting time and memory. */

        hr = ds_buffer_prepare_conversion(self);

        if (FAILED(hr)) {
            goto end;
        }

        buf_bytes = (uint8_t *) self->conv_bytes;
    } else {
        buf_bytes = (uint8_t *) snd_buffer_samples_rw(self->buf);
    }

    buf_nbytes = self->conv_nbytes;

    /* Decode args into a span */

    if (flags & DSBLOCK_ENTIREBUFFER) {
        in_pos = 0;
        in_nbytes = buf_nbytes;
    } else if (flags & DSBLOCK_FROMWRITECURSOR) {
        ds_buffer_get_current_position(com, NULL, &write_pos);
        in_pos = write_pos;
    }

    if (in_pos >= buf_nbytes || in_nbytes == 0 || in_nbytes > buf_nbytes) {
        trace("%s: Invalid lock span: pos %u len %u (buffer %u)",
                __func__,
                (unsigned int) in_pos,
                (unsigned int) in_nbytes,
                (unsigned int) buf_nbytes);
        hr = DSERR_INVALIDPARAM;

        goto end;
    }

    /*  Circular lock. Streaming code (e.g. BGM players) locks a region that
        wraps around the end of the buffer and expects the remainder in the
        second span. This used to fail with E_NOTIMPL whenever the second
        span pointers were even supplied, which is basically always the case
        for streaming code. */

    first = buf_nbytes - in_pos;

    if (first > in_nbytes) {
        first = in_nbytes;
    }

    *out_ptr = buf_bytes + in_pos;
    *out_nbytes = first;

    if (first < in_nbytes && out_ptr2 != NULL && out_nbytes2 != NULL) {
        *out_ptr2 = buf_bytes;
        *out_nbytes2 = in_nbytes - first;
    }

    hr = S_OK;

end:
    LeaveCriticalSection(&self->lock);

    return hr;
}

static __stdcall HRESULT ds_buffer_play(
        IDirectSoundBuffer *com,
        DWORD reserved1,
        DWORD reserved2,
        DWORD flags)
{
    struct ds_buffer *self;
    struct snd_command *cmd;
    bool looping;
    int r;

    self = ds_buffer_downcast(com);
    looping = (flags & DSBPLAY_LOOPING) != 0;

    /*  Why only two reserved parameters? Why not ten?
        You know, just to be sure. Fucking Microsoft. */

    EnterCriticalSection(&self->lock);

    r = snd_client_cmd_alloc(self->cli, &cmd);

    if (r < 0) {
        LeaveCriticalSection(&self->lock);

        return hr_from_errno(r);
    }

    snd_stream_prepare_play(self->stm, looping);
    self->playing = true;
    self->looping = looping;

    snd_command_play(cmd, self->stm, looping);
    snd_client_cmd_submit(self->cli, cmd);

    LeaveCriticalSection(&self->lock);

    return S_OK;
}

static __stdcall HRESULT ds_buffer_restore(IDirectSoundBuffer *com)
{
    return S_OK;
}

static __stdcall HRESULT ds_buffer_set_current_position(
        IDirectSoundBuffer *com,
        DWORD pos)
{
    struct ds_buffer *self;

    self = ds_buffer_downcast(com);

    if (pos >= self->conv_nbytes) {
        trace("%s: position %u out of range (buffer %u)",
                __func__,
                (unsigned int) pos,
                (unsigned int) self->conv_nbytes);

        return DSERR_INVALIDPARAM;
    }

    /*  This used to be a no-op (and its trace() call had a format string bug
        that would dereference the position as a pointer in debug builds). */

    snd_stream_set_position(
            self->stm,
            ds_buffer_client_bytes_to_sys_samples(self, pos));

    return S_OK;
}

static __stdcall HRESULT ds_buffer_set_format(
        IDirectSoundBuffer *com,
        const WAVEFORMATEX *format)
{
    trace("%s(%p): invalid on secondary buffers", __func__, format);

    return DSERR_INVALIDCALL;
}

static __stdcall HRESULT ds_buffer_set_frequency(
        IDirectSoundBuffer *com,
        DWORD freq)
{
    struct ds_buffer *self;

    self = ds_buffer_downcast(com);

    if (freq == DSBFREQUENCY_ORIGINAL) {
        freq = self->format.nSamplesPerSec;
    }

    if (freq != self->format.nSamplesPerSec) {
        trace("%s(%u): frequency changes are not implemented (native %u)",
                __func__,
                (unsigned int) freq,
                (unsigned int) self->format.nSamplesPerSec);
    }

    self->frequency = freq;

    return S_OK;
}

static uint16_t ds_buffer_attenuation_to_linear(LONG millibels)
{
    double gain;

    if (millibels <= DSBVOLUME_MIN) {
        return 0;
    }

    if (millibels >= 0) {
        return 0x100;
    }

    gain = pow(10.0, millibels / 2000.0);

    return (uint16_t) (256.0 * gain + 0.5);
}

static HRESULT ds_buffer_submit_volume(struct ds_buffer *self)
{
    struct snd_command *cmd;
    LONG left;
    LONG right;
    int r;

    /* Pan attenuates the opposite channel */

    left = self->volume;
    right = self->volume;

    if (self->pan > 0) {
        left -= self->pan;
    } else if (self->pan < 0) {
        right += self->pan;
    }

    r = snd_client_cmd_alloc(self->cli, &cmd);

    if (r < 0) {
        return hr_from_errno(r);
    }

    snd_command_set_volume(
            cmd,
            self->stm,
            ds_buffer_attenuation_to_linear(left),
            ds_buffer_attenuation_to_linear(right));
    snd_client_cmd_submit(self->cli, cmd);

    return S_OK;
}

static __stdcall HRESULT ds_buffer_set_pan(
        IDirectSoundBuffer *com,
        LONG pan)
{
    struct ds_buffer *self;
    HRESULT hr;

    if (pan < DSBPAN_LEFT || pan > DSBPAN_RIGHT) {
        trace("%s: Pan param out of range: %li", __func__, (long) pan);

        return DSERR_INVALIDPARAM;
    }

    self = ds_buffer_downcast(com);

    EnterCriticalSection(&self->lock);
    self->pan = pan;
    hr = ds_buffer_submit_volume(self);
    LeaveCriticalSection(&self->lock);

    return hr;
}

static __stdcall HRESULT ds_buffer_set_volume(
        IDirectSoundBuffer *com,
        LONG millibels)
{
    struct ds_buffer *self;
    HRESULT hr;

    if (millibels < DSBVOLUME_MIN || millibels > DSBVOLUME_MAX) {
        trace("%s: Attenuation param out of range: %li",
                __func__,
                (long) millibels);

        return DSERR_INVALIDPARAM;
    }

    self = ds_buffer_downcast(com);

    EnterCriticalSection(&self->lock);
    self->volume = millibels;
    hr = ds_buffer_submit_volume(self);
    LeaveCriticalSection(&self->lock);

    return hr;
}

static __stdcall HRESULT ds_buffer_stop(IDirectSoundBuffer *com)
{
    struct ds_buffer *self;
    struct snd_command *cmd;
    int r;

    self = ds_buffer_downcast(com);

    EnterCriticalSection(&self->lock);

    r = snd_client_cmd_alloc(self->cli, &cmd);

    if (r < 0) {
        LeaveCriticalSection(&self->lock);

        return hr_from_errno(r);
    }

    snd_command_stop(cmd, self->stm);
    snd_client_cmd_submit(self->cli, cmd);

    self->playing = false;
    self->looping = false;

    LeaveCriticalSection(&self->lock);

    return S_OK;
}

static HRESULT ds_buffer_unlock_span(
        struct ds_buffer *self,
        void *bytes,
        DWORD nbytes)
{
    uint8_t *base;
    uint8_t *p;

    if (bytes == NULL || nbytes == 0 || self->conv == NULL) {
        return S_OK;
    }

    base = self->conv_bytes;
    p = bytes;

    if (p < base || p >= base + self->conv_nbytes) {
        trace("%s: pointer %p is not inside this buffer", __func__, bytes);

        return DSERR_INVALIDPARAM;
    }

    if ((size_t) (p - base) + nbytes > self->conv_nbytes) {
        nbytes = self->conv_nbytes - (p - base);
    }

    return converter_convert_range(self->conv, p - base, nbytes);
}

static __stdcall HRESULT ds_buffer_unlock(
        IDirectSoundBuffer *com,
        void *bytes,
        DWORD nbytes,
        void *bytes2,
        DWORD nbytes2)
{
    struct ds_buffer *self;
    HRESULT hr;

    self = ds_buffer_downcast(com);

    EnterCriticalSection(&self->lock);

    hr = ds_buffer_unlock_span(self, bytes, nbytes);

    if (SUCCEEDED(hr)) {
        hr = ds_buffer_unlock_span(self, bytes2, nbytes2);
    }

    LeaveCriticalSection(&self->lock);

    return hr;
}

static __stdcall HRESULT ds_buffer_set_fx(
        IDirectSoundBuffer *com,
        DWORD count,
        DSEFFECTDESC *descs,
        DWORD *results)
{
    if (count == 0) {
        return S_OK;
    }

    trace("%s(%u): effects are not supported", __func__,
            (unsigned int) count);

    return DSERR_CONTROLUNAVAIL;
}

static __stdcall HRESULT ds_buffer_acquire_resources(
        IDirectSoundBuffer *com,
        DWORD flags,
        DWORD count,
        DWORD *results)
{
    return S_OK;
}

static __stdcall HRESULT ds_buffer_get_object_in_path(
        IDirectSoundBuffer *com,
        const GUID *guid_object,
        DWORD index,
        const GUID *guid_iface,
        void **out)
{
    if (out != NULL) {
        *out = NULL;
    }

    return DSERR_OBJECTNOTFOUND;
}

static const struct ds_buffer_vtbl8 ds_buffer_vtbl = {
    .base = {
        .QueryInterface     = ds_buffer_query_interface,
        .AddRef             = ds_buffer_add_ref,
        .Release            = ds_buffer_release,
        .GetCaps            = ds_buffer_get_caps,
        .GetCurrentPosition = ds_buffer_get_current_position,
        .GetFormat          = ds_buffer_get_format,
        .GetFrequency       = ds_buffer_get_frequency,
        .GetPan             = ds_buffer_get_pan,
        .GetStatus          = ds_buffer_get_status,
        .GetVolume          = ds_buffer_get_volume,
        .Initialize         = ds_buffer_initialize,
        .Lock               = ds_buffer_lock,
        .Play               = ds_buffer_play,
        .Restore            = ds_buffer_restore,
        .SetCurrentPosition = ds_buffer_set_current_position,
        .SetFormat          = ds_buffer_set_format,
        .SetFrequency       = ds_buffer_set_frequency,
        .SetPan             = ds_buffer_set_pan,
        .SetVolume          = ds_buffer_set_volume,
        .Stop               = ds_buffer_stop,
        .Unlock             = ds_buffer_unlock,
    },
    .SetFX              = ds_buffer_set_fx,
    .AcquireResources   = ds_buffer_acquire_resources,
    .GetObjectInPath    = ds_buffer_get_object_in_path,
};

/* IDirectSoundNotify, sharing the buffer's reference count */

static __stdcall HRESULT ds_buffer_notify_query_interface(
        IDirectSoundNotify *com,
        const IID *iid,
        void **out)
{
    struct ds_buffer *self;

    self = ds_buffer_notify_downcast(com);

    return ds_buffer_query_interface(&self->com, iid, out);
}

static __stdcall ULONG ds_buffer_notify_add_ref(IDirectSoundNotify *com)
{
    return ds_buffer_add_ref(&ds_buffer_notify_downcast(com)->com);
}

static __stdcall ULONG ds_buffer_notify_release(IDirectSoundNotify *com)
{
    return ds_buffer_release(&ds_buffer_notify_downcast(com)->com);
}

static __stdcall HRESULT ds_buffer_notify_set_positions(
        IDirectSoundNotify *com,
        DWORD count,
        const DSBPOSITIONNOTIFY *items)
{
    struct ds_buffer *self;
    struct snd_notify *tmp;
    DWORD i;
    HRESULT hr;
    int r;

    self = ds_buffer_notify_downcast(com);

    if (count > 0 && items == NULL) {
        return DSERR_INVALIDPARAM;
    }

    tmp = calloc(count > 0 ? count : 1, sizeof(*tmp));

    if (tmp == NULL) {
        return E_OUTOFMEMORY;
    }

    for (i = 0 ; i < count ; i++) {
        if (items[i].dwOffset == DSBPN_OFFSETSTOP) {
            tmp[i].pos = SND_NOTIFY_STOP;
        } else if (items[i].dwOffset >= self->conv_nbytes) {
            trace("%s: offset %u out of range", __func__,
                    (unsigned int) items[i].dwOffset);
            free(tmp);

            return DSERR_INVALIDPARAM;
        } else {
            tmp[i].pos = ds_buffer_client_bytes_to_sys_samples(
                    self,
                    items[i].dwOffset);
        }

        tmp[i].event = items[i].hEventNotify;
    }

    EnterCriticalSection(&self->lock);
    r = snd_stream_set_notifications(self->stm, tmp, count);
    LeaveCriticalSection(&self->lock);

    free(tmp);

    hr = r < 0 ? hr_from_errno(r) : S_OK;

    return hr;
}

static const IDirectSoundNotifyVtbl ds_buffer_notify_vtbl = {
    .QueryInterface             = ds_buffer_notify_query_interface,
    .AddRef                     = ds_buffer_notify_add_ref,
    .Release                    = ds_buffer_notify_release,
    .SetNotificationPositions   = ds_buffer_notify_set_positions,
};
