#ifndef ME_AUDIO_WASAPI_H
#define ME_AUDIO_WASAPI_H

#include "types.h"
#include <stddef.h>

/* Initialize a WASAPI render stream. If want_exclusive is nonzero the function
   attempts AUDCLNT_SHAREMODE_EXCLUSIVE (target ~3 ms latency); on failure it
   falls back to shared mode automatically. The actual mode used is printed to
   stdout. *out_device_rate receives the device sample rate. Returns 0 on
   success, nonzero on failure. */
int  me_audio_init(unsigned *out_device_rate, int want_exclusive);
void me_audio_shutdown(void);

unsigned me_audio_device_rate(void);

/* Total usable frames in the ring buffer (so callers can compute "how much
   should be buffered for X ms of latency"). */
size_t me_audio_ring_capacity(void);

#define ME_RING_TOTAL 16384u

/* Push interleaved stereo s16 frames into the ring buffer. Returns frames
   actually accepted (may be < frames if buffer full). */
size_t me_audio_push(const int16_t *data, size_t frames);

/* How many frames the ring buffer can still accept right now. */
size_t me_audio_writable_frames(void);

/* Block until at least `frames` of writable space is available, or the
   timeout expires. Returns 1 if space is available, 0 on timeout. */
int me_audio_wait_writable(size_t frames, unsigned timeout_ms);

#endif
