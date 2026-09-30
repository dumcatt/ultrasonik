#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "list.h"
#include "snd-buffer.h"

#define SND_NOTIFY_STOP ((size_t) -1)

/*  Position notification. pos is a sample index into the stream's buffer
    (i.e. frame * 2), or SND_NOTIFY_STOP to fire when playback stops. event is
    a Win32 event HANDLE. */

struct snd_notify {
    size_t pos;
    void *event;
};

struct snd_stream;

int snd_stream_alloc(struct snd_stream **out, const struct snd_buffer *buf);
void snd_stream_free(struct snd_stream *stm);
void snd_stream_set_looping(struct snd_stream *stm, bool value);
void snd_stream_set_volume(
        struct snd_stream *stm,
        size_t channel,
        uint16_t value);
void snd_stream_set_position(struct snd_stream *stm, size_t sample_pos);
void snd_stream_prepare_play(struct snd_stream *stm, bool looping);
void snd_stream_ack_play(struct snd_stream *stm);
int snd_stream_set_notifications(
        struct snd_stream *stm,
        const struct snd_notify *items,
        size_t count);
void snd_stream_disable_notifications(struct snd_stream *stm);
void snd_stream_notify_stop(struct snd_stream *stm);
bool snd_stream_render(
        struct snd_stream *stm,
        int32_t *dest_samples,
        size_t dest_nsamples);
void snd_stream_rewind(struct snd_stream *stm);
bool snd_stream_is_finished(const struct snd_stream *stm);
size_t snd_stream_peek_position(const struct snd_stream *stm);
struct list_node *snd_stream_list_upcast(struct snd_stream *node);
struct snd_stream *snd_stream_list_downcast(struct list_node *node);
