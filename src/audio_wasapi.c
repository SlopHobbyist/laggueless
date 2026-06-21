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
DEFINE_GUID(ME_IID_IAudioClient3,        0x7ED4EE07, 0x8E67, 0x4CD4, 0x8C,0x1A,0x2B,0x7A,0x59,0x87,0xAD,0x42);
DEFINE_GUID(ME_KSDATAFORMAT_SUBTYPE_PCM,0x00000001, 0x0000, 0x0010, 0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71);

/* Ring buffer holds float32 interleaved stereo at the device rate.
   Capacity comes from the header so callers can reason about latency. */
#define ME_RING_FRAMES ME_RING_TOTAL
static float g_ring[ME_RING_FRAMES * 2];
static volatile unsigned g_ring_read = 0;
static volatile unsigned g_ring_write = 0;
static CRITICAL_SECTION g_ring_cs;

static size_t ring_used(void) {
    return (size_t)(g_ring_write - g_ring_read);
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
static int g_audio_mode = ME_AUDIO_MODE_SHARED;
static int g_exclusive_pcm16 = 0;   /* exclusive negotiated 16-bit PCM */
static int g_exclusive_pcm32 = 0;   /* exclusive negotiated 24-in-32 or 32-bit PCM */
static int g_exclusive_bps   = 0;   /* bits per sample in exclusive mode */

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

        UINT32 avail;
        if (g_audio_mode == ME_AUDIO_MODE_EXCLUSIVE) {
            avail = g_buffer_frames;
        } else {
            UINT32 padding = 0;
            if (FAILED(IAudioClient_GetCurrentPadding(g_ac, &padding))) continue;
            avail = g_buffer_frames - padding;
        }
        if (avail == 0) continue;

        BYTE *dst = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(g_rc, avail, &dst))) continue;

        unsigned ch = g_device_channels;
        EnterCriticalSection(&g_ring_cs);
        size_t used = ring_used();
        size_t take = used < avail ? used : avail;

        if (g_exclusive_pcm32) {
            /* 24-bit audio in 32-bit container (most common exclusive format). */
            int32_t *out32 = (int32_t *)dst;
            for (size_t i = 0; i < take; i++) {
                unsigned idx = (g_ring_read + (unsigned)i) & (ME_RING_FRAMES - 1);
                float l = g_ring[idx*2 + 0];
                float r = g_ring[idx*2 + 1];
                out32[i*ch + 0] = (int32_t)(l * 2147483647.0f);
                if (ch > 1) out32[i*ch + 1] = (int32_t)(r * 2147483647.0f);
                for (unsigned c = 2; c < ch; c++) out32[i*ch + c] = 0;
            }
            g_ring_read += (unsigned)take;
            LeaveCriticalSection(&g_ring_cs);
            for (size_t i = take; i < avail; i++)
                for (unsigned c = 0; c < ch; c++) out32[i*ch + c] = 0;
        } else if (g_exclusive_pcm16) {
            int16_t *out16 = (int16_t *)dst;
            for (size_t i = 0; i < take; i++) {
                unsigned idx = (g_ring_read + (unsigned)i) & (ME_RING_FRAMES - 1);
                float l = g_ring[idx*2 + 0];
                float r = g_ring[idx*2 + 1];
                int il = (int)(l * 32767.0f); if (il > 32767) il = 32767; else if (il < -32768) il = -32768;
                int ir = (int)(r * 32767.0f); if (ir > 32767) ir = 32767; else if (ir < -32768) ir = -32768;
                out16[i*ch + 0] = (int16_t)il;
                if (ch > 1) out16[i*ch + 1] = (int16_t)ir;
                for (unsigned c = 2; c < ch; c++) out16[i*ch + c] = 0;
            }
            g_ring_read += (unsigned)take;
            LeaveCriticalSection(&g_ring_cs);
            for (size_t i = take; i < avail; i++)
                for (unsigned c = 0; c < ch; c++) out16[i*ch + c] = 0;
        } else {
            float *out = (float *)dst;
            for (size_t i = 0; i < take; i++) {
                unsigned idx = (g_ring_read + (unsigned)i) & (ME_RING_FRAMES - 1);
                float l = g_ring[idx*2 + 0];
                float r = g_ring[idx*2 + 1];
                out[i*ch + 0] = l;
                if (ch > 1) out[i*ch + 1] = r;
                for (unsigned c = 2; c < ch; c++) out[i*ch + c] = 0.0f;
            }
            g_ring_read += (unsigned)take;
            LeaveCriticalSection(&g_ring_cs);
            for (size_t i = take; i < avail; i++)
                for (unsigned c = 0; c < ch; c++) out[i*ch + c] = 0.0f;
        }

        IAudioRenderClient_ReleaseBuffer(g_rc, avail, 0);
    }
    if (mm_task) AvRevertMmThreadCharacteristics(mm_task);
    return 0;
}

static const char *mode_name(int m) {
    switch (m) {
        case ME_AUDIO_MODE_EXCLUSIVE:   return "exclusive";
        case ME_AUDIO_MODE_LOW_LATENCY: return "shared low-latency";
        default:                        return "shared";
    }
}

/* Re-activate IAudioClient (needed after a failed Initialize burns the client). */
static int reactivate_client(void) {
    if (g_ac) { IAudioClient_Release(g_ac); g_ac = NULL; }
    return com_ok(IMMDevice_Activate(g_dev, &ME_IID_IAudioClient, CLSCTX_ALL,
                                     NULL, (void **)&g_ac),
                  "IMMDevice::Activate (re-activate)");
}

/* Try to Initialize exclusive mode with a given format. Handles the
   BUFFER_SIZE_NOT_ALIGNED retry loop. Returns the HRESULT. */
static HRESULT try_exclusive_init(WAVEFORMATEX *fmt, REFERENCE_TIME minPeriod) {
    HRESULT hr = IAudioClient_Initialize(g_ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         minPeriod, minPeriod, fmt, NULL);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned = 0;
        IAudioClient_GetBufferSize(g_ac, &aligned);
        if (!reactivate_client()) return hr;
        REFERENCE_TIME alignedPeriod = (REFERENCE_TIME)(
            (10000.0 * 1000.0 / fmt->nSamplesPerSec * aligned) + 0.5);
        hr = IAudioClient_Initialize(g_ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     alignedPeriod, alignedPeriod, fmt, NULL);
    }
    return hr;
}

/* Try WASAPI exclusive mode. Returns 0 on success, nonzero to fall back.
   Tries the mix format first, then falls back to common PCM formats. */
static int init_exclusive(WAVEFORMATEX *mix) {
    REFERENCE_TIME minPeriod = 0;
    HRESULT hr = IAudioClient_GetDevicePeriod(g_ac, NULL, &minPeriod);
    if (FAILED(hr)) {
        fprintf(stderr, "[audio] GetDevicePeriod failed hr=0x%08lx; exclusive unavailable\n",
                (unsigned long)hr);
        return 1;
    }

    /* 1) Try the mix format (usually float32 WAVEFORMATEXTENSIBLE). */
    WAVEFORMATEX *closest = NULL;
    hr = IAudioClient_IsFormatSupported(g_ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                        mix, &closest);
    if (hr == S_OK || (hr == S_FALSE && closest)) {
        WAVEFORMATEX *use = (hr == S_FALSE) ? closest : mix;
        hr = try_exclusive_init(use, minPeriod);
        if (closest) CoTaskMemFree(closest);
        if (SUCCEEDED(hr)) {
            g_device_rate     = (unsigned)use->nSamplesPerSec;
            g_device_channels = (unsigned)use->nChannels;
            g_audio_mode = ME_AUDIO_MODE_EXCLUSIVE;
            return 0;
        }
        fprintf(stderr, "[audio] exclusive init with mix format failed hr=0x%08lx; trying PCM\n",
                (unsigned long)hr);
        if (!reactivate_client()) return 1;
    } else {
        if (closest) CoTaskMemFree(closest);
        fprintf(stderr, "[audio] mix format not supported in exclusive mode; trying PCM\n");
    }

    /* 2) Try PCM fallback formats at the device's native rate.
       Most devices accept 16-bit or 24-bit PCM in exclusive mode. */
    unsigned rate = (unsigned)mix->nSamplesPerSec;
    unsigned nch  = (unsigned)mix->nChannels;
    struct { WORD bits; WORD valid; } pcm_fmts[] = {
        { 16, 16 },
        { 32, 24 },
    };
    for (int i = 0; i < (int)(sizeof(pcm_fmts)/sizeof(pcm_fmts[0])); i++) {
        WORD bits  = pcm_fmts[i].bits;
        WORD valid = pcm_fmts[i].valid;
        WORD block = (WORD)(nch * bits / 8);

        /* Build a WAVEFORMATEXTENSIBLE for PCM. */
        WAVEFORMATEXTENSIBLE wfx = {0};
        wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.nChannels       = (WORD)nch;
        wfx.Format.nSamplesPerSec  = rate;
        wfx.Format.wBitsPerSample  = bits;
        wfx.Format.nBlockAlign     = block;
        wfx.Format.nAvgBytesPerSec = rate * block;
        wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfx.Samples.wValidBitsPerSample = valid;
        wfx.dwChannelMask = (nch >= 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                        : SPEAKER_FRONT_CENTER;
        wfx.SubFormat = ME_KSDATAFORMAT_SUBTYPE_PCM;

        hr = IAudioClient_IsFormatSupported(g_ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                            (WAVEFORMATEX *)&wfx, NULL);
        if (FAILED(hr)) continue;

        hr = try_exclusive_init((WAVEFORMATEX *)&wfx, minPeriod);
        if (SUCCEEDED(hr)) {
            g_device_rate     = rate;
            g_device_channels = nch;
            g_audio_mode = ME_AUDIO_MODE_EXCLUSIVE;
            g_exclusive_pcm16 = (bits == 16);
            g_exclusive_pcm32 = (bits == 32 || bits == 24);
            g_exclusive_bps   = bits;
            fprintf(stderr, "[audio] exclusive mode using PCM %u-bit (%u valid)\n", bits, valid);
            return 0;
        }
        /* Initialize burns the client — need a fresh one for next attempt. */
        if (!reactivate_client()) return 1;
    }

    fprintf(stderr, "[audio] no supported exclusive format found; falling back\n");
    if (!reactivate_client()) return 1;
    return 1;
}

/* Try IAudioClient3 shared low-latency mode. Returns 0 on success. */
static int init_low_latency(WAVEFORMATEX *mix) {
    IAudioClient3 *ac3 = NULL;
    HRESULT hr = IAudioClient_QueryInterface(g_ac, &ME_IID_IAudioClient3, (void **)&ac3);
    if (FAILED(hr) || !ac3) {
        fprintf(stderr, "[audio] IAudioClient3 not available (hr=0x%08lx); "
                "falling back to shared mode\n", (unsigned long)hr);
        return 1;
    }

    UINT32 defaultPeriod = 0, fundamentalPeriod = 0, minPeriod = 0, maxPeriod = 0;
    hr = IAudioClient3_GetSharedModeEnginePeriod(ac3, mix,
            &defaultPeriod, &fundamentalPeriod, &minPeriod, &maxPeriod);
    if (FAILED(hr)) {
        fprintf(stderr, "[audio] GetSharedModeEnginePeriod failed hr=0x%08lx\n",
                (unsigned long)hr);
        IAudioClient3_Release(ac3);
        return 1;
    }

    fprintf(stderr, "[audio] IAudioClient3 periods: default=%u fundamental=%u "
            "min=%u max=%u frames\n", defaultPeriod, fundamentalPeriod, minPeriod, maxPeriod);

    hr = IAudioClient3_InitializeSharedAudioStream(ac3,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minPeriod, mix, NULL);
    IAudioClient3_Release(ac3);

    if (FAILED(hr)) {
        fprintf(stderr, "[audio] InitializeSharedAudioStream failed hr=0x%08lx; "
                "falling back to shared mode\n", (unsigned long)hr);
        /* Re-activate so the shared fallback has a clean client. */
        IAudioClient_Release(g_ac);
        g_ac = NULL;
        IMMDevice_Activate(g_dev, &ME_IID_IAudioClient, CLSCTX_ALL,
                           NULL, (void **)&g_ac);
        return 1;
    }

    g_audio_mode = ME_AUDIO_MODE_LOW_LATENCY;
    return 0;
}

/* Regular WASAPI shared mode (existing behavior). */
static int init_shared(WAVEFORMATEX *mix) {
    REFERENCE_TIME dur = 20 * 10000;
    HRESULT hr = IAudioClient_Initialize(g_ac, AUDCLNT_SHAREMODE_SHARED,
                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         dur, 0, mix, NULL);
    if (!com_ok(hr, "IAudioClient::Initialize (shared)")) return 1;
    g_audio_mode = ME_AUDIO_MODE_SHARED;
    return 0;
}

int me_audio_init(unsigned *out_device_rate, int mode) {
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

    g_device_rate     = (unsigned)mix->nSamplesPerSec;
    g_device_channels = (unsigned)mix->nChannels;
    printf("[audio] device mix: %u Hz, %u ch, %u bps, tag=0x%04x\n",
           g_device_rate, g_device_channels,
           (unsigned)mix->wBitsPerSample, (unsigned)mix->wFormatTag);

    /* Fallback chain: exclusive → low-latency → shared */
    int ok = 1;
    if (mode == ME_AUDIO_MODE_EXCLUSIVE)
        ok = init_exclusive(mix);
    if (ok && mode >= ME_AUDIO_MODE_LOW_LATENCY)
        ok = init_low_latency(mix);
    if (ok)
        ok = init_shared(mix);
    CoTaskMemFree(mix);
    if (ok) return 1;

    if (!com_ok(IAudioClient_GetBufferSize(g_ac, &g_buffer_frames), "GetBufferSize")) return 1;

    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_ev) return 1;
    if (!com_ok(IAudioClient_SetEventHandle(g_ac, g_ev), "SetEventHandle")) return 1;
    if (!com_ok(IAudioClient_GetService(g_ac, &ME_IID_IAudioRenderClient, (void **)&g_rc),
                "GetService(RenderClient)")) return 1;

    if (!com_ok(IAudioClient_Start(g_ac), "IAudioClient::Start")) return 1;
    g_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    if (!g_thread) return 1;

    if (out_device_rate) *out_device_rate = g_device_rate;
    printf("[audio] WASAPI %s mode at %u Hz, buffer=%u frames (%.2f ms)\n",
           mode_name(g_audio_mode), g_device_rate, (unsigned)g_buffer_frames,
           (double)g_buffer_frames * 1000.0 / g_device_rate);
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
    EnterCriticalSection(&g_ring_cs);
    size_t can = ring_free();
    if (can > frames) can = frames;
    for (size_t i = 0; i < can; i++) {
        unsigned idx = (g_ring_write + (unsigned)i) & (ME_RING_FRAMES - 1);
        g_ring[idx*2 + 0] = data[i*2 + 0] * (1.0f / 32768.0f);
        g_ring[idx*2 + 1] = data[i*2 + 1] * (1.0f / 32768.0f);
    }
    g_ring_write += (unsigned)can;
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
