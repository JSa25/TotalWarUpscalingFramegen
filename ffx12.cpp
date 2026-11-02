// AMD FSR backend of the 64-bit helper (FSR_PLAN F2 / F3). Designed from the public FSR SDK 2.4.0 docs
// (docs/techniques/super-resolution-upscaler.md, frame-interpolation-api.md, frame-interpolation-swap-chain.md) and its MIT
// headers; AMD's signed DLLs are loaded at run time, unmodified. Every FSR call of the helper is in this file.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _WINDOWS   // ffx_api_loader.h fills ffxFunctions only for this platform macro
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include "ffx12.h"
#ifdef FGB_DEV   // dev builds only: the FGB_* test switches (release helpers carry none of them, AGENT_LOG 187)
#define dev_getenv(n) getenv(n)
#else
#define dev_getenv(n) ((char *)nullptr)
#endif

static ffxFunctions fx;
static ffxContext sr_ctx, fg_ctx, sc_ctx;
static IDXGISwapChain4 *fg_sc;
static char sr_ver[64] = "?", fg_ver[64] = "?";
static float mvs = 1, meters = 1;   // dev test FGB_FSR_MVSIGN: the motion vector sign
static float rcas = -1;            // FSR's own sharpening (RCAS) 0..1, off below 0 (FGB_FSR_RCAS, AGENT_LOG 183)

static void msg(uint32_t type, const wchar_t *m)
{
    fprintf(stderr, "  [FSR %s] %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warn", m);
}

int ffx_load(const wchar_t *dir, int (*verify)(const wchar_t *path))
{
    static const wchar_t *const dlls[] = { L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_upscaler_dx12.dll",
                                           L"amd_fidelityfx_framegeneration_dx12.dll" };
    wchar_t p[MAX_PATH];
    for (const wchar_t *d : dlls) {
        swprintf(p, MAX_PATH, L"%ls\\%ls", dir, d);
        if (!verify(p)) { fprintf(stderr, "ffx: %ls missing or its signature check failed\n", p); return 0; }
    }
    // the loader takes the effect DLLs from the exe's folder first, then its own (FSR SDK ffx-api.md "Effect DLL lookup")
    swprintf(p, MAX_PATH, L"%ls\\%ls", dir, dlls[0]);
    HMODULE m = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { fprintf(stderr, "ffx: cannot load %ls (%lu)\n", p, GetLastError()); return 0; }
    ffxLoadFunctions(&fx, m);
    if (!fx.CreateContext || !fx.DestroyContext || !fx.Configure || !fx.Query || !fx.Dispatch) { fprintf(stderr, "ffx: loader exports missing\n"); return 0; }
    return 1;
}

static void version_of(ffxContext *c, char *out, size_t n)
{
    ffxQueryGetProviderVersion q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (fx.Query(c, &q.header) == FFX_API_RETURN_OK && q.versionName) snprintf(out, n, "%s", q.versionName);
}

const char *ffx_sr_version(void) { return sr_ver; }
const char *ffx_fg_version(void) { return fg_ver; }

int ffx_sr_create(ID3D12Device *dev, uint32_t w, uint32_t h)
{
    ffxCreateBackendDX12Desc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.device = dev;
    ffxCreateContextDescUpscaleVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.header.pNext = &be.header;
    ver.version = FFX_UPSCALER_VERSION;
    ffxCreateContextDescUpscale cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.header.pNext = &ver.header;
    // the game's colour is 8-bit, after tonemapping (gamma-encoded): FSR 4 asks for linear or this flag; exposure automatic
    cd.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    if (dev_getenv("FGB_FSR_FLAGS")) cd.flags |= (uint32_t)atoi(dev_getenv("FGB_FSR_FLAGS"));   // dev test: e.g. 4 = MV jitter cancellation
    cd.maxRenderSize = { w, h };
    cd.maxUpscaleSize = { w, h };
    cd.fpMessage = msg;
    if (dev_getenv("FGB_FSR_MVSIGN")) mvs = (float)atof(dev_getenv("FGB_FSR_MVSIGN"));
    ffxReturnCode_t r = fx.CreateContext(&sr_ctx, &cd.header, nullptr);
    if (r != FFX_API_RETURN_OK) { fprintf(stderr, "ffx: upscaler context failed (%u)\n", r); sr_ctx = nullptr; return 0; }
#ifdef FGB_DEV
    // dev test (the moving-object trail, AGENT_LOG 161): AMD's tuning keys, each from an environment variable
    static const struct { const char *env; uint64_t key; } keys[] = { {"FGB_FSR_VELOCITY", FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR},
        {"FGB_FSR_SHADING", FFX_API_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE}, {"FGB_FSR_ACCUM", FFX_API_CONFIGURE_UPSCALE_KEY_FACCUMULATIONADDEDPERFRAME},
        {"FGB_FSR_MINDISOCC", FFX_API_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION} };
    for (auto &k : keys) if (getenv(k.env)) {
        static float v[4];
        float *pv = &v[&k - keys];
        *pv = (float)atof(getenv(k.env));
        ffxConfigureDescUpscaleKeyValue kv{};
        kv.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
        kv.key = k.key; kv.ptr = pv;
        fprintf(stderr, "  ffx: %s %.3f (%s)\n", k.env, *pv, fx.Configure(&sr_ctx, &kv.header) == FFX_API_RETURN_OK ? "set" : "REFUSED");
    }
#endif
    if (dev_getenv("FGB_FSR_METERS")) meters = (float)atof(dev_getenv("FGB_FSR_METERS"));
    if (dev_getenv("FGB_FSR_RCAS")) rcas = (float)atof(dev_getenv("FGB_FSR_RCAS"));
    if (rcas >= 0) fprintf(stderr, "  ffx: FSR sharpening (RCAS) %.2f\n", rcas);
    version_of(&sr_ctx, sr_ver, sizeof sr_ver);
    fprintf(stderr, "  ffx: AMD FSR upscaling ready (%s)\n", sr_ver);
    return 1;
}

int ffx_sr_dispatch(ID3D12GraphicsCommandList *cl, ID3D12Resource *color, ID3D12Resource *depth, ID3D12Resource *mvec,
                               ID3D12Resource *react, ID3D12Resource *out, uint32_t rw, uint32_t rh, float jx, float jy, int reset, const CamVp *cam, float dt_ms)
{
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cl;
    d.color = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_GENERIC_READ);
    d.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_GENERIC_READ);
    d.motionVectors = ffxApiGetResourceDX12(mvec, FFX_API_RESOURCE_STATE_GENERIC_READ);
    if (react) d.reactive = ffxApiGetResourceDX12(react, FFX_API_RESOURCE_STATE_GENERIC_READ);   // R8: 0 as usual .. 0.9 trust this frame
    d.output = ffxApiGetResourceDX12(out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = { jx, jy };
    d.motionVectorScale = { mvs, mvs };   // our vectors: render pixels, current -> previous (FSR's own convention)
    d.renderSize = { rw, rh };
    D3D12_RESOURCE_DESC od = out->GetDesc();
    d.upscaleSize = { (uint32_t)od.Width, od.Height };
    d.frameTimeDelta = dt_ms;
    d.preExposure = 1;
    d.reset = reset != 0;
    d.cameraNear = cam->nearz; d.cameraFar = cam->farz; d.cameraFovAngleVertical = cam->fov_y;
    d.viewSpaceToMetersFactor = meters;   // shortcut: Attila's units taken as metres (guess, FGB_FSR_METERS to test)
    d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    if (rcas >= 0) { d.enableSharpening = true; d.sharpness = rcas; }
    return fx.Dispatch(&sr_ctx, &d.header) == FFX_API_RETURN_OK;
}

static ffxReturnCode_t fg_dispatch(ffxDispatchDescFrameGeneration *p, void *ctx)
{
    return fx.Dispatch((ffxContext *)ctx, &p->header);
}

IDXGISwapChain4 *ffx_fg_create(ID3D12Device *dev, ID3D12CommandQueue *queue, IDXGIFactory *fac, HWND hwnd, uint32_t w,
                                          uint32_t h, DXGI_FORMAT bb_fmt, DXGI_FORMAT hud_fmt, uint32_t buffers)
{
    DXGI_SWAP_CHAIN_DESC1 sd = { w, h, bb_fmt, FALSE, { 1, 0 }, DXGI_USAGE_RENDER_TARGET_OUTPUT, buffers, DXGI_SCALING_STRETCH,
                                 DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED, 0 };
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 sv{};
    sv.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    sv.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 sc{};
    sc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    sc.header.pNext = &sv.header;
    sc.swapchain = &fg_sc;
    sc.hwnd = hwnd;
    sc.desc = &sd;
    sc.fullscreenDesc = nullptr;   // borderless / windowed only, as with Streamline
    sc.dxgiFactory = fac;
    sc.gameQueue = queue;
    ffxReturnCode_t r = fx.CreateContext(&sc_ctx, &sc.header, nullptr);
    if (r != FFX_API_RETURN_OK || !fg_sc) { fprintf(stderr, "ffx: frame generation swapchain failed (%u)\n", r); sc_ctx = nullptr; return nullptr; }

    ffxCreateBackendDX12Desc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.device = dev;
    ffxCreateContextDescFrameGenerationVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.header.pNext = &be.header;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGenerationHudless hl{};   // the HUD-less image may have another format than the swapchain
    hl.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hl.header.pNext = &ver.header;
    hl.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(hud_fmt);
    ffxCreateContextDescFrameGeneration cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    cd.header.pNext = hud_fmt != bb_fmt ? &hl.header : &ver.header;
    cd.flags = 0;   // no async compute (v1): interpolation runs on the present queue inside Present
    cd.displaySize = { w, h };
    cd.maxRenderSize = { w, h };
    cd.backBufferFormat = ffxApiGetSurfaceFormatDX12(bb_fmt);
    r = fx.CreateContext(&fg_ctx, &cd.header, nullptr);
    if (r != FFX_API_RETURN_OK) { fprintf(stderr, "ffx: frame generation context failed (%u)\n", r); fg_ctx = nullptr; return nullptr; }
    version_of(&fg_ctx, fg_ver, sizeof fg_ver);
    char scv[64] = "?";
    version_of(&sc_ctx, scv, sizeof scv);
    fprintf(stderr, "  ffx: AMD FSR frame generation ready (%s, swapchain %s)\n", fg_ver, scv);
    return fg_sc;
}

int ffx_fg_frame(ID3D12GraphicsCommandList *cl, uint64_t frame_id, int on, ID3D12Resource *hudless, ID3D12Resource *depth,
                            ID3D12Resource *mvec, uint32_t rw, uint32_t rh, float jx, float jy, int reset, const CamVp *cam, float dt_ms)
{
    D3D12_RESOURCE_DESC hd = hudless->GetDesc();
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = fg_sc;
    c.frameGenerationCallback = fg_dispatch;
    c.frameGenerationCallbackUserContext = &fg_ctx;
    c.frameGenerationEnabled = on != 0;
    c.allowAsyncWorkloads = false;
    // HUD-less mode: FSR finds the UI as the difference between the back buffer and this image (helper-owned copy, double
    // buffered: AMD's swapchain reads it after Present returns)
    c.HUDLessColor = ffxApiGetResourceDX12(hudless, FFX_API_RESOURCE_STATE_GENERIC_READ);
    c.generationRect = { 0, 0, (int32_t)hd.Width, (int32_t)hd.Height };
    c.frameID = frame_id;
    if (fx.Configure(&fg_ctx, &c.header) != FFX_API_RETURN_OK) return 0;
    if (!on) return 1;
    ffxDispatchDescFrameGenerationPrepareV2 p{};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = frame_id;
    p.commandList = cl;
    p.renderSize = { rw, rh };
    p.jitterOffset = { jx, jy };
    p.motionVectorScale = { 1, 1 };
    p.frameTimeDelta = dt_ms;
    p.reset = reset != 0;
    p.cameraNear = cam->nearz; p.cameraFar = cam->farz; p.cameraFovAngleVertical = cam->fov_y;
    p.viewSpaceToMetersFactor = 1;
    p.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_GENERIC_READ);
    p.motionVectors = ffxApiGetResourceDX12(mvec, FFX_API_RESOURCE_STATE_GENERIC_READ);
    memcpy(p.cameraPosition, cam->pos, 12); memcpy(p.cameraUp, cam->up, 12);
    memcpy(p.cameraRight, cam->right, 12); memcpy(p.cameraForward, cam->fwd, 12);
    return fx.Dispatch(&fg_ctx, &p.header) == FFX_API_RETURN_OK;
}

void ffx_shutdown(void)
{
    if (fg_ctx) {   // AMD: frame generation off and its context destroyed before the swapchain
        ffxConfigureDescFrameGeneration c{};
        c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        c.swapChain = fg_sc;
        c.frameGenerationEnabled = false;
        fx.Configure(&fg_ctx, &c.header);
        fx.DestroyContext(&fg_ctx, nullptr);
        fg_ctx = nullptr;
    }
    if (sc_ctx) { fx.DestroyContext(&sc_ctx, nullptr); sc_ctx = nullptr; }
    if (fg_sc) { fg_sc->Release(); fg_sc = nullptr; }
    if (sr_ctx) { fx.DestroyContext(&sr_ctx, nullptr); sr_ctx = nullptr; }
}
