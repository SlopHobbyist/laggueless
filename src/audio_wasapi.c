#define COBJMACROS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_wasapi.h"

DEFINE_GUID(ME_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
DEFINE_GUID(ME_IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
DEFINE_GUID(ME_IID_IAudioClient,         0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
DEFINE_GUID(ME_IID_IAudioRenderClient,   0xF294ACFC, 0x3146, 0x4483, 0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

/* Ring buffer holds float32 interleaved stereo at the device rate. */
#define ME_RING_FRAMES 16384u  /* ~340 ms at 48 kHz */
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
    return 0;
}

int me_audio_init(unsigned *out_device_rate) {
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

    REFERENCE_TIME dur = 50 * 10000;
    hr = IAudioClient_Initialize(g_ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 dur, 0, mix, NULL);
    CoTaskMemFree(mix);
    if (!com_ok(hr, "IAudioClient::Initialize")) return 1;

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
    printf("[audio] WASAPI shared mode running at device rate %u Hz, buffer=%u frames\n",
           g_device_rate, (unsigned)g_buffer_frames);
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

unsigned me_audio_device_rate(void) { return g_device_rate; }

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
