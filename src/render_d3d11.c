#define COBJMACROS
#define CINTERFACE
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <stdio.h>
#include <string.h>
#include "render_d3d11.h"
#include "shaders_embedded.h"

/* MinGW sometimes ships the dxgi1_5 header without exporting this IID
   symbol. Define it locally — value from the DXGI 1.5 SDK. */
DEFINE_GUID(ME_IID_IDXGIFactory5, 0x7632e1f5, 0xee65, 0x4dca, 0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d);
#define IID_IDXGIFactory5 ME_IID_IDXGIFactory5

static ID3D11Device             *g_dev = NULL;
static ID3D11DeviceContext      *g_ctx = NULL;
static IDXGISwapChain1          *g_sc  = NULL;
static ID3D11RenderTargetView   *g_rtv = NULL;
static ID3D11Texture2D          *g_tex = NULL;
static ID3D11ShaderResourceView *g_srv = NULL;
/* Second texture/SRV used only by the GL interop path (Step 7b). Created
   with MISC_SHARED so WGL_NV_DX_interop2 can register it. me_d3d11_present
   binds g_srv_shared when g_use_shared is set; otherwise g_srv. */
static ID3D11Texture2D          *g_tex_shared = NULL;
static ID3D11ShaderResourceView *g_srv_shared = NULL;
static int                       g_use_shared = 0;
static ID3D11SamplerState       *g_smp = NULL;
static ID3D11VertexShader       *g_vs  = NULL;
static ID3D11PixelShader        *g_ps  = NULL;
static ID3D11Buffer             *g_cb  = NULL;
static ID3D11RasterizerState    *g_rs  = NULL;

static UINT g_tex_w = 0, g_tex_h = 0;
static UINT g_client_w = 0, g_client_h = 0;
static int  g_allow_tearing = 0;
/* DXGI 1.3 waitable swap chain. When non-NULL, the main loop can block on
   this handle in place of the QPC spin-wait tail: the OS wakes us when the
   swap chain is ready for the next Present (queue depth <= max frame
   latency), which we set to 1 — that pins CPU one frame ahead of GPU
   instead of letting it race ahead. NULL on pre-Win8.1 systems or if
   QueryInterface(IDXGISwapChain2) fails; callers must keep a fallback. */
static HANDLE g_frame_latency_waitable = NULL;

static int chk(HRESULT hr, const char *where) {
    if (FAILED(hr)) {
        fprintf(stderr, "[d3d11] %s failed hr=0x%08lx\n", where, (unsigned long)hr);
        return 0;
    }
    return 1;
}

static int create_rtv(void) {
    ID3D11Texture2D *backbuf = NULL;
    if (!chk(IDXGISwapChain1_GetBuffer(g_sc, 0, &IID_ID3D11Texture2D, (void **)&backbuf),
             "GetBuffer")) return 0;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)backbuf, NULL, &g_rtv);
    ID3D11Texture2D_Release(backbuf);
    return chk(hr, "CreateRenderTargetView");
}

int me_d3d11_init(HWND hwnd, unsigned max_w, unsigned max_h) {
    g_tex_w = max_w;
    g_tex_h = max_h;

    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                   fl, sizeof(fl)/sizeof(fl[0]),
                                   D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
    if (!chk(hr, "D3D11CreateDevice")) return 1;

    IDXGIDevice  *dxgi_dev = NULL;
    IDXGIAdapter *adapter  = NULL;
    IDXGIFactory2 *factory = NULL;
    IDXGIFactory5 *factory5 = NULL;
    if (!chk(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dxgi_dev), "QI IDXGIDevice")) return 1;
    if (!chk(IDXGIDevice_GetAdapter(dxgi_dev, &adapter), "GetAdapter")) { IDXGIDevice_Release(dxgi_dev); return 1; }
    IDXGIDevice_Release(dxgi_dev);
    if (!chk(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory), "GetParent(IDXGIFactory2)")) {
        IDXGIAdapter_Release(adapter); return 1;
    }
    IDXGIAdapter_Release(adapter);

    if (SUCCEEDED(IDXGIFactory2_QueryInterface(factory, &IID_IDXGIFactory5, (void **)&factory5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                        &allow, sizeof(allow)))) {
            g_allow_tearing = allow ? 1 : 0;
        }
        IDXGIFactory5_Release(factory5);
    }
    printf("[d3d11] allow_tearing=%d\n", g_allow_tearing);

    RECT cr; GetClientRect(hwnd, &cr);
    g_client_w = (UINT)(cr.right - cr.left); if (g_client_w == 0) g_client_w = 1;
    g_client_h = (UINT)(cr.bottom - cr.top); if (g_client_h == 0) g_client_h = 1;

    DXGI_SWAP_CHAIN_DESC1 scd = {0};
    scd.Width       = g_client_w;
    scd.Height      = g_client_h;
    scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling     = DXGI_SCALING_STRETCH;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    scd.Flags       = (g_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)
                    | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)g_dev, hwnd, &scd, NULL, NULL, &g_sc);
    if (FAILED(hr)) {
        /* Win8.0 and some VM/RDP paths reject the waitable flag. Retry
           without it so we still get a working FLIP_DISCARD chain — the
           main loop's QPC spin-wait remains the fallback pacing path. */
        fprintf(stderr, "[d3d11] CreateSwapChainForHwnd with waitable flag failed hr=0x%08lx; retrying without\n",
                (unsigned long)hr);
        scd.Flags = g_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)g_dev, hwnd, &scd, NULL, NULL, &g_sc);
    }
    IDXGIFactory2_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
    IDXGIFactory2_Release(factory);
    if (!chk(hr, "CreateSwapChainForHwnd")) return 1;

    /* If the swap chain supports IDXGISwapChain2, pin max frame latency to 1
       and grab the waitable handle. SetMaximumFrameLatency only takes effect
       on chains created with the WAITABLE flag — otherwise it's a no-op. */
    if (scd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        IDXGISwapChain2 *sc2 = NULL;
        if (SUCCEEDED(IDXGISwapChain1_QueryInterface(g_sc, &IID_IDXGISwapChain2, (void **)&sc2))) {
            if (SUCCEEDED(IDXGISwapChain2_SetMaximumFrameLatency(sc2, 1))) {
                g_frame_latency_waitable = IDXGISwapChain2_GetFrameLatencyWaitableObject(sc2);
            }
            IDXGISwapChain2_Release(sc2);
        }
        printf("[d3d11] frame_latency_waitable=%s\n",
               g_frame_latency_waitable ? "on (max_latency=1)" : "unavailable");
    }

    if (!create_rtv()) return 1;

    D3D11_TEXTURE2D_DESC td = {0};
    td.Width  = g_tex_w;
    td.Height = g_tex_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!chk(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_tex), "CreateTexture2D")) return 1;

    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {0};
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (!chk(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_tex, &svd, &g_srv),
             "CreateShaderResourceView")) return 1;

    D3D11_SAMPLER_DESC sd = {0};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MinLOD = 0; sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (!chk(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_smp), "CreateSamplerState")) return 1;

    if (!chk(ID3D11Device_CreateVertexShader(g_dev, quad_vs_bytecode, quad_vs_bytecode_len, NULL, &g_vs),
             "CreateVertexShader")) return 1;
    if (!chk(ID3D11Device_CreatePixelShader(g_dev, quad_ps_bytecode, quad_ps_bytecode_len, NULL, &g_ps),
             "CreatePixelShader")) return 1;

    /* Constant buffer: two float4 (rect, uvscale). */
    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = 32;
    bd.Usage     = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!chk(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb), "CreateBuffer(cb)")) return 1;

    D3D11_RASTERIZER_DESC rd = {0};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (!chk(ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs), "CreateRasterizerState")) return 1;

    printf("[d3d11] initialized: client=%ux%u tex=%ux%u\n", g_client_w, g_client_h, g_tex_w, g_tex_h);
    return 0;
}

void *me_d3d11_get_device(void)  { return g_dev; }
void *me_d3d11_get_texture(void) { return g_tex; }

void *me_d3d11_create_shared_texture(unsigned w, unsigned h) {
    if (!g_dev) return NULL;
    if (g_tex_shared) return g_tex_shared;  /* idempotent */

    D3D11_TEXTURE2D_DESC td = {0};
    td.Width  = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    /* GL renders RGBA8 into this; WGL_NV_DX_interop2 swizzles to/from
       D3D11's BGRA8 transparently when sampling. */
    td.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    /* MISC_SHARED (the legacy kind, not _NTHANDLE) is what WGL interop
       expects on Windows. */
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (!chk(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_tex_shared),
             "CreateTexture2D(shared)")) return NULL;

    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {0};
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (!chk(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_tex_shared,
                                                   &svd, &g_srv_shared),
             "CreateShaderResourceView(shared)")) {
        ID3D11Texture2D_Release(g_tex_shared); g_tex_shared = NULL;
        return NULL;
    }
    /* Bump g_tex_w/h so present's u_scale/v_scale math (which uses these
       as the texture extents) is correct when sampling the shared tex. */
    g_tex_w = w; g_tex_h = h;
    return g_tex_shared;
}

void me_d3d11_use_shared(int yes) { g_use_shared = yes; }

void me_d3d11_shutdown(void) {
    if (g_srv_shared) { ID3D11ShaderResourceView_Release(g_srv_shared); g_srv_shared = NULL; }
    if (g_tex_shared) { ID3D11Texture2D_Release(g_tex_shared);           g_tex_shared = NULL; }
    if (g_rs)  { ID3D11RasterizerState_Release(g_rs);   g_rs  = NULL; }
    if (g_cb)  { ID3D11Buffer_Release(g_cb);            g_cb  = NULL; }
    if (g_ps)  { ID3D11PixelShader_Release(g_ps);       g_ps  = NULL; }
    if (g_vs)  { ID3D11VertexShader_Release(g_vs);      g_vs  = NULL; }
    if (g_smp) { ID3D11SamplerState_Release(g_smp);     g_smp = NULL; }
    if (g_srv) { ID3D11ShaderResourceView_Release(g_srv); g_srv = NULL; }
    if (g_tex) { ID3D11Texture2D_Release(g_tex);        g_tex = NULL; }
    if (g_rtv) { ID3D11RenderTargetView_Release(g_rtv); g_rtv = NULL; }
    /* Waitable handle is owned by the swap chain — releasing the chain
       invalidates it. Just drop the reference. */
    g_frame_latency_waitable = NULL;
    if (g_sc)  { IDXGISwapChain1_Release(g_sc);         g_sc  = NULL; }
    if (g_ctx) { ID3D11DeviceContext_Release(g_ctx);    g_ctx = NULL; }
    if (g_dev) { ID3D11Device_Release(g_dev);           g_dev = NULL; }
}

void me_d3d11_upload(const u32 *pixels, unsigned frame_w, unsigned frame_h, unsigned max_w) {
    if (!g_tex || frame_w == 0 || frame_h == 0) return;
    if (frame_w > g_tex_w) frame_w = g_tex_w;
    if (frame_h > g_tex_h) frame_h = g_tex_h;

    D3D11_MAPPED_SUBRESOURCE m = {0};
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_tex, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    BYTE *dst = (BYTE *)m.pData;
    for (unsigned y = 0; y < frame_h; y++) {
        memcpy(dst + y * m.RowPitch, pixels + y * max_w, frame_w * 4);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_tex, 0);
}

static void resize_if_needed(int cw, int ch) {
    if (cw <= 0 || ch <= 0) return;
    if ((UINT)cw == g_client_w && (UINT)ch == g_client_h) return;
    /* Unbind RTV from the pipeline before releasing it, otherwise the swap
       chain still holds a reference and ResizeBuffers fails silently. */
    ID3D11RenderTargetView *null_rtv = NULL;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &null_rtv, NULL);
    if (g_rtv) { ID3D11RenderTargetView_Release(g_rtv); g_rtv = NULL; }
    /* Keep the same flags the chain was created with — dropping the waitable
       flag on resize would invalidate g_frame_latency_waitable. */
    UINT flags = (g_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)
               | (g_frame_latency_waitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0);
    HRESULT hr = IDXGISwapChain1_ResizeBuffers(g_sc, 0, (UINT)cw, (UINT)ch, DXGI_FORMAT_UNKNOWN, flags);
    if (FAILED(hr)) {
        fprintf(stderr, "[d3d11] ResizeBuffers(%d,%d) failed hr=0x%08lx\n",
                cw, ch, (unsigned long)hr);
        return;
    }
    g_client_w = (UINT)cw;
    g_client_h = (UINT)ch;
    create_rtv();
}

void me_d3d11_present(int client_w, int client_h, int dx, int dy, int dw, int dh,
                      unsigned frame_w, unsigned frame_h) {
    if (!g_sc) return;
    resize_if_needed(client_w, client_h);
    if (!g_rtv || dw <= 0 || dh <= 0 || frame_w == 0 || frame_h == 0) return;

    float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, black);

    /* dst rect (pixels, top-left origin) → NDC (bottom-left origin).
       The shader lerps UV alongside NDC position (uv.y=0 at rect.y,
       uv.y=1 at rect.w). For top-down source textures (which is what
       both the software cores and the GL readback produce), we need
       uv.y=0 at the TOP of the screen — i.e. put the higher NDC y in
       rect.y. So feed (top, bottom) into (rect.y, rect.w), not the
       intuitive (bottom, top). */
    float x0 =        (float)dx              / (float)g_client_w * 2.0f - 1.0f;
    float x1 =        (float)(dx + dw)       / (float)g_client_w * 2.0f - 1.0f;
    float y_top    = 1.0f - (float)dy        / (float)g_client_h * 2.0f;
    float y_bottom = 1.0f - (float)(dy + dh) / (float)g_client_h * 2.0f;
    float y0 = y_top;     /* uv.y = 0 lands here  (texture top row) */
    float y1 = y_bottom;  /* uv.y = 1 lands here  (texture bottom row) */

    float u_scale = (float)frame_w / (float)g_tex_w;
    float v_scale = (float)frame_h / (float)g_tex_h;

    /* Interop path samples a GL-rendered (bottom-up) texture; flip by
       swapping the NDC y-rect so uv.y=0 lands on screen-bottom and
       uv.y=1 on screen-top. */
    if (g_use_shared && g_srv_shared) {
        float tmp = y0; y0 = y1; y1 = tmp;
    }

    D3D11_MAPPED_SUBRESOURCE m = {0};
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float cb[8] = { x0, y0, x1, y1, u_scale, v_scale, 0.0f, 0.0f };
        memcpy(m.pData, cb, sizeof(cb));
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cb, 0);
    }

    D3D11_VIEWPORT vp = {0};
    vp.Width  = (FLOAT)g_client_w;
    vp.Height = (FLOAT)g_client_h;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    ID3D11ShaderResourceView *srv = (g_use_shared && g_srv_shared) ? g_srv_shared : g_srv;
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_smp);
    ID3D11DeviceContext_Draw(g_ctx, 4, 0);

    UINT flags = g_allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    IDXGISwapChain1_Present(g_sc, 0, flags);
}

int me_d3d11_wait_for_present(unsigned timeout_ms) {
    if (!g_frame_latency_waitable) return 0;
    /* bAlertable=FALSE: we don't queue APCs to this thread, so alertable
       waits just add overhead. WAIT_OBJECT_0 is the only success code we
       care about; WAIT_TIMEOUT and WAIT_FAILED both fall through to the
       caller's fallback pacing. */
    WaitForSingleObjectEx(g_frame_latency_waitable, timeout_ms, FALSE);
    return 1;
}
