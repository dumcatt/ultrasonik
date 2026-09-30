#include <windows.h>

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "defs.h"
#include "list.h"
#include "snd-buffer.h"
#include "snd-stream.h"

/*  A set of position notifications. Sets are swapped in atomically and old
    sets are only freed when the stream itself is freed (which the reaper
    guarantees happens after the audio thread has finished with the stream),
    so the audio thread never observes a dangling pointer. */

struct snd_notify_set {
    struct snd_notify_set *prev;
    size_t count;
    struct snd_notify items[];
};

struct snd_stream {
    struct list_node node;
    const struct snd_buffer *buf;
    atomic_uint pos;
    uint16_t volumes[2];
    atomic_bool looping;
    atomic_bool play_pending;
    atomic_bool notify_enabled;
    _Atomic(struct snd_notify_set *) notify;
};

static void snd_stream_fire_range(
        struct snd_stream *stm,
        size_t start,
        size_t end);

int snd_stream_alloc(struct snd_stream **out, const struct snd_buffer *buf)
{
    struct snd_stream *stm;

    assert(out != NULL);
    assert(buf != NULL);

    *out = NULL;
    stm = calloc(sizeof(*stm), 1);

    if (stm == NULL) {
        return -ENOMEM;
    }

    list_node_init(&stm->node);
    stm->buf = buf;
    stm->volumes[0] = 0x100;
    stm->volumes[1] = 0x100;
    atomic_store(&stm->notify_enabled, true);
    atomic_store(&stm->notify, NULL);

    *out = stm;

    return 0;
}

void snd_stream_free(struct snd_stream *stm)
{
    struct snd_notify_set *set;
    struct snd_notify_set *prev;

    if (stm == NULL) {
        return;
    }

    for (set = atomic_load(&stm->notify) ; set != NULL ; set = prev) {
        prev = set->prev;
        free(set);
    }

    list_node_fini(&stm->node);
    free(stm);
}

void snd_stream_set_looping(struct snd_stream *stm, bool value)
{
    assert(stm != NULL);

    atomic_store(&stm->looping, value);
}

void snd_stream_set_volume(
        struct snd_stream *stm,
        size_t channel,
        uint16_t value)
{
    assert(stm != NULL);
    assert(channel < lengthof(stm->volumes));

    stm->volumes[channel] = value;
}

void snd_stream_set_position(struct snd_stream *stm, size_t sample_pos)
{
    size_t nsamples;

    assert(stm != NULL);

    nsamples = snd_buffer_nsamples(stm->buf);

    if (sample_pos >= nsamples) {
        sample_pos = 0;
    }

    sample_pos &= ~(size_t) 1; /* Keep stereo frames aligned */
    atomic_store(&stm->pos, sample_pos);
}

void snd_stream_prepare_play(struct snd_stream *stm, bool looping)
{
    assert(stm != NULL);

    /*  DirectSound semantics: Play() resumes from the current position. A
        non-looping buffer that has already run off its end restarts from
        the beginning. This runs on the client thread so that GetStatus()
        and GetCurrentPosition() are consistent immediately after Play()
        returns, without waiting for the audio thread to pick up the command.
        The audio thread uses a CAS to publish its position so it will not
        clobber a position that we store here. */

    atomic_store(&stm->looping, looping);

    if (atomic_load(&stm->pos) >= snd_buffer_nsamples(stm->buf)) {
        atomic_store(&stm->pos, 0);
    }

    atomic_store(&stm->play_pending, true);
}

void snd_stream_ack_play(struct snd_stream *stm)
{
    assert(stm != NULL);

    atomic_store(&stm->play_pending, false);
}

int snd_stream_set_notifications(
        struct snd_stream *stm,
        const struct snd_notify *items,
        size_t count)
{
    struct snd_notify_set *set;
    size_t nsamples;
    size_t i;

    assert(stm != NULL);
    assert(items != NULL || count == 0);

    set = calloc(1, sizeof(*set) + count * sizeof(set->items[0]));

    if (set == NULL) {
        return -ENOMEM;
    }

    nsamples = snd_buffer_nsamples(stm->buf);
    set->count = count;

    for (i = 0 ; i < count ; i++) {
        set->items[i] = items[i];

        if (    set->items[i].pos != SND_NOTIFY_STOP &&
                set->items[i].pos >= nsamples) {
            set->items[i].pos = nsamples - 2;
        }
    }

    set->prev = atomic_load(&stm->notify);
    atomic_store(&stm->notify, set);

    return 0;
}

void snd_stream_disable_notifications(struct snd_stream *stm)
{
    assert(stm != NULL);

    atomic_store(&stm->notify_enabled, false);
}

static void snd_stream_fire_range(
        struct snd_stream *stm,
        size_t start,
        size_t end)
{
    const struct snd_notify_set *set;
    size_t i;

    if (!atomic_load(&stm->notify_enabled)) {
        return;
    }

    set = atomic_load(&stm->notify);

    if (set == NULL) {
        return;
    }

    for (i = 0 ; i < set->count ; i++) {
        if (    set->items[i].pos != SND_NOTIFY_STOP &&
                set->items[i].pos >= start &&
                set->items[i].pos < end &&
                set->items[i].event != NULL) {
            SetEvent(set->items[i].event);
        }
    }
}

void snd_stream_notify_stop(struct snd_stream *stm)
{
    const struct snd_notify_set *set;
    size_t i;

    assert(stm != NULL);

    if (!atomic_load(&stm->notify_enabled)) {
        return;
    }

    set = atomic_load(&stm->notify);

    if (set == NULL) {
        return;
    }

    for (i = 0 ; i < set->count ; i++) {
        if (    set->items[i].pos == SND_NOTIFY_STOP &&
                set->items[i].event != NULL) {
            SetEvent(set->items[i].event);
        }
    }
}

bool snd_stream_render(
        struct snd_stream *stm,
        int32_t *dest,
        size_t dest_nsamples)
{
    const int16_t *src;
    const int16_t *src_end;
    const int16_t *buf_samples;
    unsigned int pos_orig;
    size_t buf_nsamples;
    size_t pos;
    size_t seg_start;
    size_t count;
    int32_t vol_l;
    int32_t vol_r;
    bool looping;
    bool finished;

    assert(dest_nsamples % 2 == 0);

    buf_samples = snd_buffer_samples_ro(stm->buf);
    buf_nsamples = snd_buffer_nsamples(stm->buf);
    looping = atomic_load(&stm->looping);
    vol_l = stm->volumes[0];
    vol_r = stm->volumes[1];

    pos_orig = atomic_load(&stm->pos);
    pos = pos_orig;

    if (pos > buf_nsamples) {
        pos = buf_nsamples;
    }

    if (looping && pos == buf_nsamples) {
        pos = 0;
    }

    for (;;) {
        seg_start = pos;
        count = buf_nsamples - pos;

        if (count > dest_nsamples) {
            count = dest_nsamples;
        }

        src = &buf_samples[pos];
        src_end = src + count;

        while (src < src_end) {
            *dest++ += *src++ * vol_l;
            *dest++ += *src++ * vol_r;
        }

        pos += count;
        dest_nsamples -= count;

        if (count > 0) {
            snd_stream_fire_range(stm, seg_start, pos);
        }

        if (dest_nsamples == 0 || !looping) {
            break;
        }

        pos = 0;
    }

    /*  Previously a looping stream whose position happened to land exactly
        on the end of the buffer at the end of a mix period was reported as
        "finished" and dropped from the mixer. Depending on the buffer length
        vs. the ASIO period this could happen after the very first pass or
        after several seconds, and was one way for looping BGM to go silent.
        Wrap it explicitly instead. */

    if (looping && pos >= buf_nsamples) {
        pos = 0;
    }

    finished = !looping && pos >= buf_nsamples;

    /*  Publish our new position, unless the client repositioned the stream
        (SetCurrentPosition / Play-after-end) while we were rendering. In that
        case the client's value wins and we keep the stream alive. */

    if (!atomic_compare_exchange_strong(
            &stm->pos,
            &pos_orig,
            (unsigned int) pos)) {
        return true;
    }

    if (finished) {
        snd_stream_notify_stop(stm);
    }

    return !finished;
}

void snd_stream_rewind(struct snd_stream *stm)
{
    assert(stm != NULL);
    atomic_store(&stm->pos, 0);
}

bool snd_stream_is_finished(const struct snd_stream *stm)
{
    assert(stm != NULL);

    return  atomic_load(&stm->play_pending) == false &&
            atomic_load(&stm->looping) == false &&
            atomic_load(&stm->pos) >= snd_buffer_nsamples(stm->buf);
}

size_t snd_stream_peek_position(const struct snd_stream *stm)
{
    size_t pos;
    size_t nsamples;

    assert(stm != NULL);

    pos = atomic_load(&stm->pos);
    nsamples = snd_buffer_nsamples(stm->buf);

    if (pos >= nsamples) {
        pos = atomic_load(&stm->looping) ? 0 : nsamples;
    }

    /* Convert result from samples (not very meaningful) to frames */

    return pos / 2;
}

struct list_node *snd_stream_list_upcast(struct snd_stream *stm)
{
    assert(stm != NULL);

    return &stm->node;
}

struct snd_stream *snd_stream_list_downcast(struct list_node *node)
{
    assert(node != NULL);

    return containerof(node, struct snd_stream, node);
}
