#pragma once

#include <winerror.h>
#include <mmreg.h>

#include <stddef.h>

#include "snd-service.h"

struct asio_backend;

HRESULT asio_alloc(struct asio_backend **out);
void asio_free(struct asio_backend *asio);
HRESULT asio_start(struct asio_backend *asio);
HRESULT asio_snd_client_alloc(
        struct asio_backend *asio,
        struct snd_client **out);
const WAVEFORMATEX *asio_get_sys_format(const struct asio_backend *asio);
size_t asio_get_period_frames(const struct asio_backend *asio);
HRESULT asio_stop(struct asio_backend *asio);
