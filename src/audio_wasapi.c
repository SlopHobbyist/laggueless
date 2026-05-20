#define COBJMACROS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_wasapi.h"

DEFINE_GUID(ME_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
DEFINE_GUID(ME_IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
DEFINE_GUID(ME_IID_IAudioClient,         0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
DEFINE_GUID(ME_IID_IAudioRenderClient,   0xF294ACFC, 0x3146, 0x4483, 0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

/* Ring buffer holds float32 interleaved stereo at the device rate.
   Capacity comes from the header so callers can reason about latency. */
#define ME_RING_FRAMES ME_RING_TOTAL
static float g_ring[ME_RING_FRAMES * 2];
static volatile unsigned g_ring_read = 0;
static volatile unsigned g_ring_write = 0;
static CRITICAL_SECTION g_ring_cs;

static size_t ring_used(void) {
    return (size_t)((g_ring_write - g_ring_read) & (ME_RING_FRAMES - 1));
}
static size_t ring_free(void) { return ME_RING_FRAMES - 1 - ring_used(); }

static IMMDeviceEnumerator *g_enum = NULL;
static IMMDevice           *g_dev  = NULL;
static IAudioClient        *g_ac   = NULL;
static IAudioRenderClient  *g_rc   = NULL;
static HANDLE  g_ev = NULL;
static HANDLE  g_thread = NULL;
static volatile int g_quit = 0;
static UINT32 g_buffer_frames = 0;
static unsigned g_device_rate = 0;
static unsigned g_device_channels = 2;

static int com_ok(HRESULT hr, const char *where) {
    if (FAILED(hr)) {
        fprintf(stderr, "[audio] %s failed hr=0x%08lx\n", where, (unsigned long)hr);
        return 0;
    }
    return 1;
}

static DWORD WINAPI audio_thread(LPVOID arg) {
    (void)arg;
    DWORD mm_task_idx = 0;
    HANDLE mm_task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mm_task_idx);
    if (mm_task) AvSetMmThreadPriority(mm_task, AVRT_PRIORITY_HIGH);
    while (!g_quit) {
        DWORD w = WaitForSingleObject(g_ev, 200);
        if (w != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(g_ac, &padding))) continue;
        UINT32 avail = g_buffer_frames - padding;
        if (avail == 0) continue;

        BYTE *dst = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(g_rc, avail, &dst))) continue;

        float *out = (float *)dst;
        unsigned ch = g_device_channels;
        EnterCriticalSection(&g_ring_cs);
        size_t used = ring_used();
        size_t take = used < avail ? used : avail;
        for (size_t i = 0; i < take; i++) {
            unsigned idx = (g_ring_read + (unsigned)i) & (ME_RING_FRAMES - 1);
            float l = g_ring[idx*2 + 0];
            float r = g_ring[idx*2 + 1];
            out[i*ch + 0] = l;
            if (ch > 1) out[i*ch + 1] = r;
            for (unsigned c = 2; c < ch; c++) out[i*ch + c] = 0.0f;
        }
        g_ring_read = (g_ring_read + (unsigned)take) & (ME_RING_FRAMES - 1);
        LeaveCriticalSection(&g_ring_cs);

        for (size_t i = take; i < avail; i++) {
            for (unsigned c = 0; c < ch; c++) out[i*ch + c] = 0.0f;
        }

        IAudioRenderClient_ReleaseBuffer(g_rc, avail, 0);
    }
    if (mm_task) AvRevertMmThreadCharacteristics(mm_task);
    return 0;
}

typedef enum { ME_EXCL_S16 = 0, ME_EXCL_F32 = 1 } me_excl_fmt;

static int         g_exclusive     = 0;
static me_excl_fmt g_exclusive_fmt = ME_EXCL_S16;

/* Attempt IAudioClient_Initialize in exclusive mode with fmt/period, handling
   the BUFFER_SIZE_NOT_ALIGNED retry. Returns 0 on success. */
static int try_exclusive_init(WAVEFORMATEX *fmt, REFERENCE_TIME period, REFERENCE_TIME min_period) {
    HRESULT hr = IAudioClient_Initialize(g_ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         period, period, fmt, NULL);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned = 0;
        if (FAILED(IAudioClient_GetBufferSize(g_ac, &aligned))) return 1;
        period = (REFERENCE_TIME)(10000000.0 * aligned / fmt->nSamplesPerSec + 0.5);
        if (period < min_period) period = min_period;
        hr = IAudioClient_Initialize(g_ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     period, period, fmt, NULL);
    }
    return FAILED(hr) ? 1 : 0;
}

/* Try exclusive mode. Probes two formats in order:
   1. Plain 16-bit PCM at the device's native rate/channels.
   2. EXTENSIBLE IEEE-float 32-bit at the device's native rate/channels,
      built from scratch (not the mix format verbatim) so we know exactly
      what we're writing.
   The mix format is intentionally NOT passed directly to avoid accepting
   unexpected container formats (24-in-32, surround, etc.).
   Returns 0 on success and updates g_device_rate / g_device_channels / g_exclusive_fmt. */
static int try_exclusive(WAVEFORMATEX *mix_fmt) {
    REFERENCE_TIME default_period = 0, min_period = 0;
    if (FAILED(IAudioClient_GetDevicePeriod(g_ac, &default_period, &min_period))) return 1;

    /* Use the device's default shared-mode period as our exclusive period.
       This is typically 10 ms, which the driver is guaranteed to support.
       Smaller periods (3 ms) cause starvation because our push granularity
       is one emulation frame (~16.6 ms) — we can't feed a 3 ms drain. */
    REFERENCE_TIME period = default_period;
    if (period < min_period) period = min_period;

    WORD ch   = (WORD)mix_fmt->nChannels;
    DWORD rate = mix_fmt->nSamplesPerSec;

    /* --- candidate 1: 16-bit PCM --- */
    {
        WAVEFORMATEX pcm = {0};
        pcm.wFormatTag      = WAVE_FORMAT_PCM;
        pcm.nChannels       = ch;
        pcm.nSamplesPerSec  = rate;
        pcm.wBitsPerSample  = 16;
        pcm.nBlockAlign     = (WORD)(ch * 2);
        pcm.nAvgBytesPerSec = rate * pcm.nBlockAlign;

        WAVEFORMATEX *closest = NULL;
        HRESULT hr = IAudioClient_IsFormatSupported(g_ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                    &pcm, &closest);
        if (closest) { CoTaskMemFree(closest); closest = NULL; }
        if (hr == S_OK && try_exclusive_init(&pcm, period, min_period) == 0) {
            g_device_rate     = (unsigned)rate;
            g_device_channels = (unsigned)ch;
            g_exclusive_fmt   = ME_EXCL_S16;
            printf("[audio] exclusive: 16-bit PCM %lu Hz %u ch, period=%lld us\n",
                   (unsigned long)rate, (unsigned)ch, (long long)(period / 10));
            return 0;
        }
    }

    /* --- candidate 2: 32-bit IEEE float EXTENSIBLE --- */
    {
        static const GUID float_guid = {0x00000003, 0x0000, 0x0010,
                                        {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
        WAVEFORMATEXTENSIBLE ext = {0};
        ext.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        ext.Format.nChannels       = ch;
        ext.Format.nSamplesPerSec  = rate;
        ext.Format.wBitsPerSample  = 32;
        ext.Format.nBlockAlign     = (WORD)(ch * 4);
        ext.Format.nAvgBytesPerSec = rate * ext.Format.nBlockAlign;
        ext.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        ext.Samples.wValidBitsPerSample = 32;
        ext.dwChannelMask          = (ch == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                                : SPEAKER_FRONT_CENTER;
        ext.SubFormat              = float_guid;

        WAVEFORMATEX *closest = NULL;
        HRESULT hr = IAudioClient_IsFormatSupported(g_ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                    &ext.Format, &closest);
        if (closest) { CoTaskMemFree(closest); closest = NULL; }
        if (hr == S_OK && try_exclusive_init(&ext.Format, period, min_period) == 0) {
            g_device_rate     = (unsigned)rate;
            g_device_channels = (unsigned)ch;
            g_exclusive_fmt   = ME_EXCL_F32;
            printf("[audio] exclusive: 32-bit float EXTENSIBLE %lu Hz %u ch, period=%lld us\n",
                   (unsigned long)rate, (unsigned)ch, (long long)(period / 10));
            return 0;
        }
    }

    return 1;
}

/* Set by try_exclusive() before the thread launches. */

static DWORD WINAPI audio_thread_exclusive(LPVOID arg) {
    (void)arg;
    DWORD mm_task_idx = 0;
    HANDLE mm_task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mm_task_idx);
    if (mm_task) AvSetMmThreadPriority(mm_task, AVRT_PRIORITY_HIGH);

    UINT32 buf_frames = g_buffer_frames;
    int use_f32 = (g_exclusive_fmt == ME_EXCL_F32);
    size_t frame_bytes = g_device_channels * (use_f32 ? sizeof(float) : sizeof(int16_t));

    /* In exclusive mode the driver fires the event immediately after Start()
       to request the first buffer — do not pre-fill; let the loop handle it
       uniformly so the ring read pointer stays in sync from frame 0. */
    IAudioClient_Start(g_ac);

    while (!g_quit) {
        DWORD w = WaitForSingleObject(g_ev, 200);
        if (w != WAIT_OBJECT_0) continue;

        BYTE *dst = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(g_rc, buf_frames, &dst)) || !dst) continue;

        unsigned ch = g_device_channels;
        EnterCriticalSection(&g_ring_cs);
        size_t used = ring_used();
        size_t take = used < buf_frames ? used : buf_frames;
        if (use_f32) {
            float *out32 = (float *)dst;
            for (size_t i = 0; i < take; i++) {
                unsigned idx = (g_ring_read + (unsigned)i) & (ME_RING_FRAMES - 1);
                out32[i*ch + 0] = g_ring[idx*2 + 0];
                if (ch > 1) out32[i*ch + 1] = g_ring[idx*2 + 1];
                for (unsigned c = 2; c < ch; c++) out32[i*ch + c] = 0.0f;
            }
            if (take < buf_frames)
                memset((float *)dst + take * ch, 0, (buf_frames - take) * frame_bytes);
        } else {
            int16_t *out16 = (int16_t *)dst;
            for (size_t i = 0; i < take; i++) {
                unsigned idx = (g_ring_read + (unsigned)i) & (ME_RING_FRAMES - 1);
                out16[i*ch + 0] = (int16_t)(g_ring[idx*2 + 0] * 32767.0f);
                if (ch > 1) out16[i*ch + 1] = (int16_t)(g_ring[idx*2 + 1] * 32767.0f);
                for (unsigned c = 2; c < ch; c++) out16[i*ch + c] = 0;
            }
            if (take < buf_frames)
                memset((int16_t *)dst + take * ch, 0, (buf_frames - take) * frame_bytes);
        }
        g_ring_read = (g_ring_read + (unsigned)take) & (ME_RING_FRAMES - 1);
        LeaveCriticalSection(&g_ring_cs);

        IAudioRenderClient_ReleaseBuffer(g_rc, buf_frames, 0);
    }
    IAudioClient_Stop(g_ac);
    if (mm_task) AvRevertMmThreadCharacteristics(mm_task);
    return 0;
}

int me_audio_init(unsigned *out_device_rate, int want_exclusive) {
    InitializeCriticalSection(&g_ring_cs);

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[audio] CoInitializeEx failed hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    if (!com_ok(CoCreateInstance(&ME_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                 &ME_IID_IMMDeviceEnumerator, (void **)&g_enum),
                "CoCreateInstance")) return 1;
    if (!com_ok(IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_enum, eRender, eConsole, &g_dev),
                "GetDefaultAudioEndpoint")) return 1;
    if (!com_ok(IMMDevice_Activate(g_dev, &ME_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&g_ac),
                "IMMDevice::Activate")) return 1;

    WAVEFORMATEX *mix = NULL;
    if (!com_ok(IAudioClient_GetMixFormat(g_ac, &mix), "GetMixFormat") || !mix) return 1;

    /* Stash mix-format rate/channels as defaults; try_exclusive may override them. */
    g_device_rate     = (unsigned)mix->nSamplesPerSec;
    g_device_channels = (unsigned)mix->nChannels;
    printf("[audio] device mix: %u Hz, %u ch, %u bps, tag=0x%04x\n",
           g_device_rate, g_device_channels,
           (unsigned)mix->wBitsPerSample, (unsigned)mix->wFormatTag);

    int used_exclusive = 0;
    if (want_exclusive && try_exclusive(mix) == 0) {
        used_exclusive = 1;
    } else {
        if (want_exclusive)
            fprintf(stderr, "[audio] exclusive mode failed; falling back to shared\n");

        REFERENCE_TIME dur = 20 * 10000;
        hr = IAudioClient_Initialize(g_ac, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     dur, 0, mix, NULL);
        if (!com_ok(hr, "IAudioClient::Initialize (shared)")) { CoTaskMemFree(mix); return 1; }
    }
    CoTaskMemFree(mix);

    if (!com_ok(IAudioClient_GetBufferSize(g_ac, &g_buffer_frames), "GetBufferSize")) return 1;

    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_ev) return 1;
    if (!com_ok(IAudioClient_SetEventHandle(g_ac, g_ev), "SetEventHandle")) return 1;
    if (!com_ok(IAudioClient_GetService(g_ac, &ME_IID_IAudioRenderClient, (void **)&g_rc),
                "GetService(RenderClient)")) return 1;

    if (used_exclusive) {
        g_exclusive = 1;
        /* Exclusive mode: thread does Start() after the first pre-fill. */
        g_thread = CreateThread(NULL, 0, audio_thread_exclusive, NULL, 0, NULL);
    } else {
        g_exclusive = 0;
        if (!com_ok(IAudioClient_Start(g_ac), "IAudioClient::Start")) return 1;
        g_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    }
    if (!g_thread) return 1;

    if (out_device_rate) *out_device_rate = g_device_rate;
    printf("[audio] WASAPI %s mode running at %u Hz, buffer=%u frames%s\n",
           used_exclusive ? "exclusive" : "shared",
           g_device_rate, (unsigned)g_buffer_frames,
           (used_exclusive && g_exclusive_fmt == ME_EXCL_F32) ? " (f32)" : "");
    return 0;
}

void me_audio_shutdown(void) {
    g_quit = 1;
    if (g_ev) SetEvent(g_ev);
    if (g_thread) { WaitForSingleObject(g_thread, 1000); CloseHandle(g_thread); g_thread = NULL; }
    if (g_ac) IAudioClient_Stop(g_ac);
    if (g_rc) { IAudioRenderClient_Release(g_rc); g_rc = NULL; }
    if (g_ac) { IAudioClient_Release(g_ac); g_ac = NULL; }
    if (g_dev) { IMMDevice_Release(g_dev); g_dev = NULL; }
    if (g_enum) { IMMDeviceEnumerator_Release(g_enum); g_enum = NULL; }
    if (g_ev) { CloseHandle(g_ev); g_ev = NULL; }
    DeleteCriticalSection(&g_ring_cs);
}

void me_audio_set_thread_affinity(unsigned long long affinity_mask, int priority) {
    if (!g_thread) return;
    if (affinity_mask) SetThreadAffinityMask(g_thread, (DWORD_PTR)affinity_mask);
    if (priority != -1) SetThreadPriority(g_thread, priority);
}

unsigned me_audio_device_rate(void) { return g_device_rate; }
size_t   me_audio_ring_capacity(void) { return ME_RING_FRAMES; }

size_t me_audio_push(const int16_t *data, size_t frames) {
    /* Direct push at device rate (resampling happens in caller). */
    EnterCriticalSection(&g_ring_cs);
    size_t can = ring_free();
    if (can > frames) can = frames;
    for (size_t i = 0; i < can; i++) {
        unsigned idx = (g_ring_write + (unsigned)i) & (ME_RING_FRAMES - 1);
        g_ring[idx*2 + 0] = data[i*2 + 0] * (1.0f / 32768.0f);
        g_ring[idx*2 + 1] = data[i*2 + 1] * (1.0f / 32768.0f);
    }
    g_ring_write = (g_ring_write + (unsigned)can) & (ME_RING_FRAMES - 1);
    LeaveCriticalSection(&g_ring_cs);
    return can;
}

size_t me_audio_writable_frames(void) {
    EnterCriticalSection(&g_ring_cs);
    size_t f = ring_free();
    LeaveCriticalSection(&g_ring_cs);
    return f;
}

int me_audio_wait_writable(size_t frames, unsigned timeout_ms) {
    DWORD start = GetTickCount();
    while (me_audio_writable_frames() < frames) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeout_ms) return 0;
        Sleep(1);
    }
    return 1;
}
