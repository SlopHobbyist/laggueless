#define COBJMACROS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <avrt.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_wasapi.h"

/* PKEY_AudioEngine_DeviceFormat: holds the exact WAVEFORMATEXTENSIBLE the
   device's audio engine is configured for. Using this format for exclusive
   Initialize() is guaranteed to succeed (it's what the device wants natively),
   which avoids the IsFormatSupported guessing game that loses to driver-side
   format conversions (e.g. accepting 16-bit while the device runs 24-bit). */
DEFINE_PROPERTYKEY(ME_PKEY_AudioEngine_DeviceFormat,
    0xf19f064d, 0x82c, 0x4e27, 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c, 0);

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

/* Free-running indices: never mask the indices themselves, only mask when
   indexing into g_ring[]. This keeps (write - read) valid across wrap-around
   as long as the ring never holds more than ME_RING_FRAMES frames. */
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
        g_ring_read += (unsigned)take;
        LeaveCriticalSection(&g_ring_cs);

        for (size_t i = take; i < avail; i++) {
            for (unsigned c = 0; c < ch; c++) out[i*ch + c] = 0.0f;
        }

        IAudioRenderClient_ReleaseBuffer(g_rc, avail, 0);
    }
    if (mm_task) AvRevertMmThreadCharacteristics(mm_task);
    return 0;
}

/* Format dispatch: derived from SubFormat.Data1 (mode: 1=PCM, 3=IEEE float)
   and wBitsPerSample. */
typedef enum {
    ME_EXCL_PCM16 = 0,  /* mode=1, precision=16 — int16 samples */
    ME_EXCL_PCM24 = 1,  /* mode=1, precision=24 — packed 3-byte samples */
    ME_EXCL_PCM32 = 2,  /* mode=1, precision=32 — int32 samples */
    ME_EXCL_F32   = 3   /* mode=3, precision=32 — float samples */
} me_excl_fmt;

static int         g_exclusive     = 0;
static me_excl_fmt g_exclusive_fmt = ME_EXCL_F32;

/* Try exclusive mode. Reads the device's configured "Default Format" from
   PKEY_AudioEngine_DeviceFormat (the format the user selected in the
   Sound control panel's Advanced tab) and uses that verbatim — this is
   guaranteed to work in exclusive mode since it's what the device's audio
   engine is already configured for. No probing, no guessing.
   Returns 0 on success and updates g_device_rate / g_device_channels / g_exclusive_fmt. */
static int try_exclusive(void) {
    IPropertyStore *ps = NULL;
    HRESULT hr = IMMDevice_OpenPropertyStore(g_dev, STGM_READ, &ps);
    if (FAILED(hr) || !ps) {
        fprintf(stderr, "[audio] OpenPropertyStore failed hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    PROPVARIANT pv;
    PropVariantInit(&pv);
    hr = IPropertyStore_GetValue(ps, &ME_PKEY_AudioEngine_DeviceFormat, &pv);
    IPropertyStore_Release(ps);
    if (FAILED(hr) || pv.vt != VT_BLOB || !pv.blob.pBlobData) {
        fprintf(stderr, "[audio] PKEY_AudioEngine_DeviceFormat get failed hr=0x%08lx vt=%u\n",
                (unsigned long)hr, (unsigned)pv.vt);
        PropVariantClear(&pv);
        return 1;
    }

    /* Copy the format out — the blob memory is owned by the PROPVARIANT. */
    WAVEFORMATEXTENSIBLE wfx;
    memset(&wfx, 0, sizeof(wfx));
    size_t copy_n = pv.blob.cbSize > sizeof(wfx) ? sizeof(wfx) : pv.blob.cbSize;
    memcpy(&wfx, pv.blob.pBlobData, copy_n);
    PropVariantClear(&pv);

    REFERENCE_TIME default_period = 0, min_period = 0;
    if (FAILED(IAudioClient_GetDevicePeriod(g_ac, &default_period, &min_period))) return 1;

    /* 10 ms (default shared period) is what the driver is guaranteed to support
       and is large enough that our ~16.6 ms emulation-frame push granularity
       can keep the ring fed. */
    REFERENCE_TIME period = default_period;
    if (period < min_period) period = min_period;

    hr = IAudioClient_Initialize(g_ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 period, period, &wfx.Format, NULL);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        /* Driver wants a different buffer size. Per MSDN, after this error you
           must release the IAudioClient, re-Activate it, compute the period
           that matches the driver's preferred buffer size, and try again. */
        UINT32 aligned = 0;
        if (FAILED(IAudioClient_GetBufferSize(g_ac, &aligned))) return 1;
        IAudioClient_Release(g_ac);
        g_ac = NULL;
        if (FAILED(IMMDevice_Activate(g_dev, &ME_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&g_ac)))
            return 1;
        period = (REFERENCE_TIME)(10000000.0 * aligned / wfx.Format.nSamplesPerSec + 0.5);
        if (period < min_period) period = min_period;
        hr = IAudioClient_Initialize(g_ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     period, period, &wfx.Format, NULL);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[audio] exclusive Initialize failed hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    g_device_rate     = (unsigned)wfx.Format.nSamplesPerSec;
    g_device_channels = (unsigned)wfx.Format.nChannels;

    /* Decide format dispatch from SubFormat.Data1 + wBitsPerSample. For plain
       WAVEFORMATEX (no EXTENSIBLE), wFormatTag is WAVE_FORMAT_PCM (1) or
       WAVE_FORMAT_IEEE_FLOAT (3); for EXTENSIBLE the SubFormat GUID's Data1
       carries the same 1/3 encoding. */
    unsigned mode = wfx.Format.wFormatTag;
    if (wfx.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        mode = wfx.SubFormat.Data1;

    const char *fmt_name = "unknown";
    if (mode == 1 && wfx.Format.wBitsPerSample == 16) {
        g_exclusive_fmt = ME_EXCL_PCM16; fmt_name = "PCM16";
    } else if (mode == 1 && wfx.Format.wBitsPerSample == 24) {
        g_exclusive_fmt = ME_EXCL_PCM24; fmt_name = "PCM24";
    } else if (mode == 1 && wfx.Format.wBitsPerSample == 32) {
        g_exclusive_fmt = ME_EXCL_PCM32; fmt_name = "PCM32";
    } else if (mode == 3 && wfx.Format.wBitsPerSample == 32) {
        g_exclusive_fmt = ME_EXCL_F32;   fmt_name = "F32";
    } else {
        fprintf(stderr, "[audio] unsupported device format mode=%u bps=%u — bailing\n",
                mode, (unsigned)wfx.Format.wBitsPerSample);
        return 1;
    }

    printf("[audio] exclusive: %s %u Hz %u ch (bps=%u valid=%u), period=%lld us\n",
           fmt_name, g_device_rate, g_device_channels,
           (unsigned)wfx.Format.wBitsPerSample,
           (unsigned)wfx.Samples.wValidBitsPerSample,
           (long long)(period / 10));
    return 0;
}

/* Convert `take` frames from the ring (starting at g_ring_read) into the
   WASAPI buffer at `dst`, in whatever format the device negotiated. Caller
   holds g_ring_cs. */
static void excl_convert(BYTE *dst, size_t take) {
    unsigned ch = g_device_channels;
    me_excl_fmt fmt = g_exclusive_fmt;
    if (fmt == ME_EXCL_F32) {
        float *out = (float *)dst;
        for (size_t i = 0; i < take; i++) {
            unsigned idx = (g_ring_read + (unsigned)i) & (ME_RING_FRAMES - 1);
            out[i*ch + 0] = g_ring[idx*2 + 0];
            if (ch > 1) out[i*ch + 1] = g_ring[idx*2 + 1];
            for (unsigned c = 2; c < ch; c++) out[i*ch + c] = 0.0f;
        }
    } else if (fmt == ME_EXCL_PCM32) {
        int32_t *out = (int32_t *)dst;
        const double scale = 2147483647.0;
        for (size_t i = 0; i < take; i++) {
            unsigned idx = (g_ring_read + (unsigned)i) & (ME_RING_FRAMES - 1);
            double l = (double)g_ring[idx*2 + 0];
            double r = (double)g_ring[idx*2 + 1];
            if (l >  1.0) l =  1.0; else if (l < -1.0) l = -1.0;
            if (r >  1.0) r =  1.0; else if (r < -1.0) r = -1.0;
            out[i*ch + 0] = (int32_t)(l * scale);
            if (ch > 1) out[i*ch + 1] = (int32_t)(r * scale);
            for (unsigned c = 2; c < ch; c++) out[i*ch + c] = 0;
        }
    } else if (fmt == ME_EXCL_PCM24) {
        BYTE *out = (BYTE *)dst;
        const double scale = 8388607.0;
        for (size_t i = 0; i < take; i++) {
            unsigned idx = (g_ring_read + (unsigned)i) & (ME_RING_FRAMES - 1);
            for (unsigned c = 0; c < ch; c++) {
                int32_t s;
                if (c < 2) {
                    double v = (double)g_ring[idx*2 + c];
                    if (v >  1.0) v =  1.0; else if (v < -1.0) v = -1.0;
                    s = (int32_t)(v * scale);
                } else {
                    s = 0;
                }
                BYTE *p = out + (i*ch + c) * 3;
                p[0] = (BYTE)(s & 0xFF);
                p[1] = (BYTE)((s >> 8) & 0xFF);
                p[2] = (BYTE)((s >> 16) & 0xFF);
            }
        }
    } else { /* PCM16 */
        int16_t *out = (int16_t *)dst;
        for (size_t i = 0; i < take; i++) {
            unsigned idx = (g_ring_read + (unsigned)i) & (ME_RING_FRAMES - 1);
            float l = g_ring[idx*2 + 0];
            float r = g_ring[idx*2 + 1];
            if (l >  1.0f) l =  1.0f; else if (l < -1.0f) l = -1.0f;
            if (r >  1.0f) r =  1.0f; else if (r < -1.0f) r = -1.0f;
            out[i*ch + 0] = (int16_t)(l * 32767.0f);
            if (ch > 1) out[i*ch + 1] = (int16_t)(r * 32767.0f);
            for (unsigned c = 2; c < ch; c++) out[i*ch + c] = 0;
        }
    }
    g_ring_read += (unsigned)take;
}

/* Bytes per device frame for the negotiated exclusive format. */
static size_t excl_frame_bytes(void) {
    size_t sample_bytes;
    switch (g_exclusive_fmt) {
        case ME_EXCL_PCM16: sample_bytes = 2; break;
        case ME_EXCL_PCM24: sample_bytes = 3; break;
        case ME_EXCL_PCM32: sample_bytes = 4; break;
        case ME_EXCL_F32:   sample_bytes = 4; break;
        default:            sample_bytes = 4; break;
    }
    return g_device_channels * sample_bytes;
}

/* Producer-driven write for exclusive mode. Called by me_audio_push after the
   ring has been updated. Drains ring → WASAPI buffer as long as the device's
   event is signaled (i.e. has space). Non-blocking: returns immediately if
   the device buffer isn't ready.

   This replaces the dedicated audio thread that was causing 60Hz callback
   throttling — apparently the WASAPI event on this system wasn't reliably
   waking the audio thread at the 100Hz device period, but the emu thread
   IS running fast enough to feed the device when it polls the event itself. */
static void excl_drain_to_device(void) {
    UINT32 buf_frames = g_buffer_frames;
    if (!buf_frames) return;

    for (;;) {
        /* Non-blocking event check. In exclusive event-driven mode the event
           is signaled when the device buffer is drained and ready for refill. */
        if (WaitForSingleObject(g_ev, 0) != WAIT_OBJECT_0) return;

        BYTE *dst = NULL;
        HRESULT ghr = IAudioRenderClient_GetBuffer(g_rc, buf_frames, &dst);
        if (FAILED(ghr) || !dst) return;

        EnterCriticalSection(&g_ring_cs);
        size_t used = ring_used();
        size_t take = used < buf_frames ? used : buf_frames;
        DWORD release_flags = 0;
        if (take == 0) {
            /* No samples available — let WASAPI play silence rather than
               stale memory. Cheaper than zeroing the buffer ourselves. */
            release_flags = AUDCLNT_BUFFERFLAGS_SILENT;
        } else {
            excl_convert(dst, take);
            if (take < buf_frames) {
                size_t fb = excl_frame_bytes();
                memset(dst + take * fb, 0, (buf_frames - take) * fb);
            }
        }
        LeaveCriticalSection(&g_ring_cs);

        IAudioRenderClient_ReleaseBuffer(g_rc, buf_frames, release_flags);

        /* Loop: if more buffers are queued, drain those too in this same call. */
    }
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
    if (want_exclusive && try_exclusive() == 0) {
        used_exclusive = 1;
    } else {
        if (want_exclusive)
            fprintf(stderr, "[audio] exclusive mode failed; falling back to shared\n");

        /* try_exclusive may have left g_ac in an Initialized state (or even
           a partly-set-up state after a failed retry). Release + re-Activate
           so the shared Initialize below starts from a clean client. */
        if (g_ac) { IAudioClient_Release(g_ac); g_ac = NULL; }
        if (!com_ok(IMMDevice_Activate(g_dev, &ME_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&g_ac),
                    "IMMDevice::Activate (shared retry)")) { CoTaskMemFree(mix); return 1; }

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
        /* Producer-driven model: no dedicated audio thread. me_audio_push
           drains the ring into the device buffer from the emu thread,
           checking the WASAPI event non-blocking. Start() now; the first
           push() will populate the buffer when the event signals. */
        if (!com_ok(IAudioClient_Start(g_ac), "IAudioClient::Start (exclusive)")) return 1;
        g_thread = NULL;
    } else {
        g_exclusive = 0;
        if (!com_ok(IAudioClient_Start(g_ac), "IAudioClient::Start")) return 1;
        g_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
        if (!g_thread) return 1;
    }

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
    g_ring_write += (unsigned)can;
    LeaveCriticalSection(&g_ring_cs);

    /* Exclusive mode runs the producer-driven model: drain ring → device
       buffer right here on the emu thread whenever the WASAPI event is
       signaled. Shared mode has a dedicated audio thread that handles this. */
    if (g_exclusive) excl_drain_to_device();

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
