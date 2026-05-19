User: "If I'm making a libretro core compatible emulator frontend, do I implement this in each core, or in the whole frontend and not have to edit any cores?"
Answer: Frontend, not the cores. None of this needs core changes.

Why it lives in the frontend:

Adaptive sync is purely about how the frontend drives its video/audio backends. Cores just call video_refresh_cb(framebuffer, w, h, pitch) and audio_sample_batch_cb(samples, frames) whenever they finish a frame — they don't know or care about vsync, present modes, or VRR. The frontend owns:

The swapchain / GL context (so it sets swap_interval = 0).
The audio device (so it decides whether audio_sample_batch_cb blocks when the queue is full — that callback returning slowly is exactly what paces the core's retro_run() loop).
Fullscreen mode selection.
RetroArch's "Sync to Exact Content Framerate" + vsync off + audio sync on is literally this preset. You'd expose the same toggle.

Integer scaling is a viewport calculation done after the core hands you a framebuffer. The core reports its base/max geometry via retro_get_system_av_info (base_width, base_height, aspect_ratio); you take that plus your window size and compute the destination rect. Core never sees it.

One subtlety worth knowing: the audio-as-clock trick only paces correctly if the core actually produces audio at the rate declared in av_info.timing.sample_rate and runs one retro_run() per emulated frame (timing.fps). Essentially every libretro core does, but a misbehaving one (variable samples per frame, or retro_run that returns without producing audio) will pace badly. That's a core-quality issue, not something you implement per-core — you just rely on the libretro timing contract.

So: implement once in the frontend, works for every well-behaved core.