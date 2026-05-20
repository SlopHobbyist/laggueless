#ifndef ME_AUDIO_WASAPI_H
#define ME_AUDIO_WASAPI_H

#include "types.h"
#include <stddef.h>

/* Initialize a WASAPI shared-mode render stream at the given sample rate
   (stereo s16). Returns 0 on success, nonzero on failure. */
int  me_audio_init(unsigned sample_rate);
void me_audio_shutdown(void);

/* Push interleaved stereo s16 frames into the ring buffer. Returns frames
   actually accepted (may be < frames if buffer full). */
size_t me_audio_push(const int16_t *data, size_t frames);

/* How many frames the ring buffer can still accept right now. */
size_t me_audio_writable_frames(void);

/* Block until at least `frames` of writable space is available, or the
   timeout expires. Returns 1 if space is available, 0 on timeout. */
int me_audio_wait_writable(size_t frames, unsigned timeout_ms);

#endif
