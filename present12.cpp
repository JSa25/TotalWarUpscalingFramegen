// P3 (64-bit helper): presents the helper's replayed D3D11 frame through a D3D12 swapchain managed by NVIDIA
// Streamline, with DLSS Frame Generation and Reflex. Inputs (colour = HUD-less, depth, motion vectors) are D3D11
// textures created with SHARED_NTHANDLE, opened in D3D12; one shared fence orders D3D11 -> D3D12 -> next D3D11 frame.
// Designed from Streamline's public programming guides (docs/ProgrammingGuide*.md of the SDK release).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include "present12.h"
#include "ffx12.h"
#ifdef FGB_DEV   // dev builds only: the FGB_* test switches (release helpers carry none of them, AGENT_LOG 187)
#define dev_getenv(n) getenv(n)
#else
#define dev_getenv(n) ((char *)nullptr)
#endif
// FSR build (FSR_PLAN F2 / F3): FGB_FSR=1 in the environment runs AMD FSR upscaling + frame generation instead of
// Streamline (Streamline is then never loaded); the dev test on NVIDIA GPUs. Mixed mode (F4): on an RTX GPU without
// DLSS-G (RTX 20 / 30, or the test limit fglimit=1) DLSS upscaling stays and frame generation is AMD FSR's
static bool sl_on = true;
static bool ffx_fg;   // frame generation and Present through AMD's swapchain (FSR mode or mixed mode)

using namespace sl;

// Core SL functions come from sl.interposer.dll at run time (no import library: llvm-mingw, and the DLL path is ours).
static decltype(&slInit) p_slInit;
static decltype(&slShutdown) p_slShutdown;
static decltype(&slSetD3DDevice) p_slSetD3DDevice;
static decltype(&slIsFeatureSupported) p_slIsFeatureSupported;
static decltype(&slSetTagForFrame) p_slSetTagForFrame;
static decltype(&slSetConstants) p_slSetConstants;
static decltype(&slGetNewFrameToken) p_slGetNewFrameToken;
static decltype(&slGetFeatureFunction) p_slGetFeatureFunction;
static decltype(&slEvaluateFeature) p_slEvaluateFeature;
// The feature helpers in sl_dlss_g.h / sl_reflex.h / sl_pcl.h call slGetFeatureFunction by name.
extern "C" sl::Result slGetFeatureFunction(sl::Feature f, const char *name, void *&fn) { return p_slGetFeatureFunction(f, name, fn); }

static ID3D12Device *d12;
static ID3D12CommandQueue *queue;
static IDXGISwapChain3 *sc;
static ID3D12Resource *bufs[3];
static ID3D12CommandAllocator *alloc[3];
static ID3D12GraphicsCommandList *cl;
static ID3D12Fence *fence12;
static ID3D11Fence *fence11;
static ID3D11DeviceContext4 *ctx4;
static HANDLE fence_event;
static uint64_t slot_done[3];       // fence value that frees each allocator
static ID3D12CommandAllocator *sr_alloc[3];   // bridge: the mid-frame DLSS SR pass of each frame
static uint64_t sr_slot_done[3];
static uint32_t width, height, fg_frames;
// frame generation as NVIDIA allows it on this GPU (Streamline): DLSS SR needs an RTX GPU (20 series up), frame
// generation an RTX 40 / 50 (fg_ok), and how many frames it may generate comes from DLSSGState::numFramesToGenerateMax
// (1 = 2x on RTX 40; more on RTX 50). Nothing beyond what Streamline reports is ever turned on.
static bool fg_ok;
// V-Sync on (Rome 2 DLSS_LOG 50, AGENT_LOG 140): with sync interval 0, frames Windows sends straight to the display
// (independent flip, also with the NVIDIA overlay on a hardware plane) carried DXGI_PRESENT_ALLOW_TEARING and tore; a player
// saw it with G-Sync in Attila and Rome 2. The helper's frames never get windowed G-Sync (NVIDIA's driver gives it to the
// window's own process only), so V-Sync is what keeps them whole. With frame generation only where DLSS-G supports it
// (ProgrammingGuideDLSS_G 22).
static bool vsync_fg;
static bool vs_user_off;   // the player turned V-Sync off (the game's settings / F11 panel): no cap, frames may tear
extern "C" void p12_set_vsync_off(int off) { vs_user_off = off != 0; }
static HWND p_hwnd;
static uint32_t st_vsync_on, st_vsync_off;
// The V-Sync guard: once (Rome 2), with windowed G-Sync and the NVIDIA overlay on, Windows composed the frames and V-Sync
// held them to 75 per second on a 240 Hz screen (each Present waited ~27 ms; not reproduced since). When Presents with
// V-Sync wait longer on average than 1.5 refreshes per frame shown (counted per Present: frame generation can change
// within the second), two seconds in a row, V-Sync goes off for 30 s (the frames are composed then, and composed frames
// never tear), then is tried again. (A one-second version false-triggered at a 4x -> off switch.)
static bool vs_held;
static int64_t vs_t0, vs_wait, vs_off_until;
static uint32_t vs_n, vs_shown, vs_bad;
static double vs_refresh_ms(void)
{
    MONITORINFOEXW mi{};
    DEVMODEW dm{};
    mi.cbSize = sizeof mi; dm.dmSize = sizeof dm;
    HMONITOR m = p_hwnd ? MonitorFromWindow(p_hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!m || !GetMonitorInfoW(m, (MONITORINFO *)&mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) ||
        dm.dmDisplayFrequency < 2) return 0;
    return 1000.0 / dm.dmDisplayFrequency;
}
static void vsync_guard(UINT sync, int64_t now, int64_t waited)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    if (vs_held) {
        if (now < vs_off_until) return;
        vs_held = false; vs_t0 = now; vs_wait = 0; vs_n = vs_shown = vs_bad = 0;
        fprintf(stderr, "  p12: V-Sync on again (30 s passed)\n");
        return;
    }
    if (!sync) return;
    vs_wait += waited; vs_n++; vs_shown += fg_frames + 1;
    if (now - vs_t0 < f.QuadPart) return;
    double avg_ms = (double)vs_wait * 1000.0 / f.QuadPart / vs_n, refresh = vs_refresh_ms(), limit = 1.5 * refresh * vs_shown / vs_n;
    vs_bad = refresh > 0 && vs_n >= 10 && avg_ms > limit ? vs_bad + 1 : 0;
    if (vs_bad >= 2) {
        vs_held = true; vs_off_until = now + 30 * f.QuadPart; vs_bad = 0;
        fprintf(stderr, "  p12: V-Sync held the frames back (each Present waited %.1f ms, limit %.1f ms at %.0f Hz) - V-Sync off for 30 s "
                        "(Windows composes the frames then; composed frames never tear)\n", avg_ms, limit, 1000.0 / refresh);
    }
    vs_t0 = now; vs_wait = 0; vs_n = vs_shown = 0;
}
static uint32_t fg_max, fg_test_limit;   // test limit: 0 none, else the GPU is treated as allowing limit - 1
extern "C" void p12_set_fg_test_limit(uint32_t limit) { fg_test_limit = limit; }
extern "C" uint32_t p12_fg_max(void) { return fg_max; }
static FrameToken *token;
static FrameToken *tokens[4];   // frame n uses tokens[n % 4]
// bridge (frame bridge with the game, AGENT_LOG 95/96)
static ID3D12Resource *br[6];
static ID3D12Resource *br_react;   // AMD FSR's reactive mask from the game (FSR build of the engine fix), else null
static ID3D12Fence *f_game, *f_sr, *f_done;   // game: 2n+1 inputs, 2n+2 final; SR queue: n+1; present queue: n+1
static ID3D12CommandQueue *queue_sr;          // the mid-frame DLSS SR pass, so it never waits behind a Present
static ID3D12GraphicsCommandList *cl_sr;
static HANDLE sr_event;
static const ViewportHandle viewport{0};

static struct { ID3D11Texture2D *t11; ID3D12Resource *t12; } shared_cache[16];

static uint32_t st_presents, st_frames_shown, st_bad_status, st_last_status, st_tag_fail, st_present_fail, st_hud_post;
static uint32_t st_shown_hist[6];
static volatile uint32_t st_shown_total;   // never reset (the bridge panel's shown frame rate)
static double st_cpu_ms;            // helper CPU time inside p12_present
static double st_slot_t, st_present_t, st_tick_ms;   // of it (QPC ticks): waiting for the GPU to finish the slot's earlier frame / inside Present

// ---- DLSS Super Resolution (P3b) ----
static bool sr_on;
static uint32_t rw, rh;               // render (input) size
static uint32_t br_iw, br_ih;         // bridge: the presented frame's input size (the SR thread may already be on the next size)
static float jit_x, jit_y, jit_sign = 1;
static ID3D12Resource *sr_out;        // display-size output, R8G8B8A8, UAV
static ID3D12QueryHeap *ts_heap;
static ID3D12Resource *ts_buf;        // 3 slots x 2 timestamps
static bool ts_pending[3];
static uint64_t ts_freq;
static double st_dlss_gpu_ms;
static uint32_t st_dlss_n;
static uint32_t dump_frame = UINT32_MAX;
static char dump_path[MAX_PATH];


static void logcb(LogType type, const char *msg)
{
    if (type != LogType::eInfo) fprintf(stderr, "  [SL %s] %s", type == LogType::eError ? "error" : "warn", msg);
}

static bool signed_by_nvidia(const wchar_t *path)
{
    WINTRUST_FILE_INFO fi = { sizeof fi, path, nullptr, nullptr };
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd = {};
    wd.cbStruct = sizeof wd; wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE; wd.pFile = &fi; wd.dwStateAction = WTD_STATEACTION_VERIFY;
    LONG r = WinVerifyTrust(nullptr, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    return r == 0;
}

static ID3D12Resource *open_shared(ID3D11Texture2D *t)
{
    for (auto &e : shared_cache) if (e.t11 == t) return e.t12;
    IDXGIResource1 *r1; HANDLE h; ID3D12Resource *r = nullptr;
    if (FAILED(t->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1))) return nullptr;
    HRESULT hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    r1->Release();
    if (FAILED(hr)) return nullptr;
    hr = d12->OpenSharedHandle(h, __uuidof(ID3D12Resource), (void **)&r);
    CloseHandle(h);
    if (FAILED(hr)) return nullptr;
    for (auto &e : shared_cache) if (!e.t11) { e.t11 = t; e.t12 = r; return r; }
    return nullptr;
}

static void begin_frame(uint32_t n)
{
    p_slGetNewFrameToken(token, &n);
    tokens[n % 4] = token;
    slReflexSleep(*token);
    slPCLSetMarker(PCLMarker::eSimulationStart, *token);
    slPCLSetMarker(PCLMarker::eSimulationEnd, *token);
    slPCLSetMarker(PCLMarker::eRenderSubmitStart, *token);
}

static ID3D12Resource *hud_copy[2];   // the HUD-less image of frame n in [n % 2]: AMD's swapchain reads it after Present
static uint32_t frame_limit_us;       // the frame cap as Reflex counts it (microseconds per shown frame)
static float view_vp[16], fsr_jsign = 1;
static bool no_hud;   // dev test FGB_FSR_NOHUD (uicap's control): the final image given as the HUD-less one
static LARGE_INTEGER fsr_last;
static struct { float jx, jy, dt; CamVp cam; bool reset; } fr_info[8];   // frame n's values in [n % 8], set by the SR thread
static CamVp cam_ok = { 0.1f, 1000, 1, { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };   // last valid camera
extern "C" void p12_set_view(const float vp[16]) { memcpy(view_vp, vp, sizeof view_vp); }
static uint32_t p_tech;   // the game's FgbShared::tech: 0 NVIDIA only (older game side), 1 auto, 2 AMD FSR for both, 3 DLSS + FSR frame generation
extern "C" void p12_set_tech(uint32_t tech) { p_tech = tech; }
extern "C" void p12_backends(uint32_t *sr, uint32_t *fg) { *sr = sl_on ? 1 : 2; *fg = !fg_max ? 0 : ffx_fg ? 2 : 1; }
static int verify_signed(const wchar_t *p) { return signed_by_nvidia(p); }   // a valid Authenticode signature (any signer)

// AMD's DLLs from the helper's folder (signature checked) and the dev test switches
static int load_ffx(void)
{
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    *wcsrchr(dir, L'\\') = 0;
    if (!ffx_load(dir, verify_signed)) return 0;
    const char *js = dev_getenv("FGB_FSR_JSIGN");   // dev test: the jitter sign (FSR_PLAN 2.2)
    if (js) fsr_jsign = (float)atof(js);
    no_hud = dev_getenv("FGB_FSR_NOHUD") != nullptr;
    return 1;
}

// AMD FSR for both upscaling and frame generation: the device from Windows' own D3D12, no NVIDIA file loaded
static int init_fsr(const LUID *luid, uint32_t w, uint32_t h)
{
    sl_on = false;
    if (!(ffx_fg = load_ffx())) return 0;
    IDXGIFactory4 *fac;
    IDXGIAdapter1 *ad;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&fac)) ||
        FAILED(fac->EnumAdapterByLuid(*luid, __uuidof(IDXGIAdapter1), (void **)&ad))) { fprintf(stderr, "p12: no adapter for the game's LUID\n"); return 0; }
    HRESULT hr = D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void **)&d12);
    ad->Release(); fac->Release();
    if (FAILED(hr)) { fprintf(stderr, "p12: D3D12CreateDevice 0x%08lx\n", hr); return 0; }
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    d12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&queue);
    for (auto &a : alloc) d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&a);
    for (auto &a : sr_alloc) d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&a);
    d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&cl);
    cl->Close();
    width = w; height = h;
    if (!ffx_sr_create(d12, w, h)) return 0;
    fg_ok = true; fg_max = 1;   // AMD FSR frame generation: one generated frame per rendered frame (2x)
    fprintf(stderr, "  p12: AMD FSR mode (jitter sign %+.0f), frame generation allowed: 2x\n", fsr_jsign);
    return 1;
}

// after the game's textures are open: AMD's swapchain on the game window (its HUD-less format is known now)
static void queue_idle(ID3D12CommandQueue *q);
static int init_fsr_fg(HWND hwnd)
{
    IDXGIFactory2 *fac;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void **)&fac))) return 0;
    D3D12_RESOURCE_DESC hd = br[1]->GetDesc();
    if (dev_getenv("FGB_SWAP_PROBE")) {   /* dev test (AGENT_LOG 166): can any swapchain be made on this window now? */
        DXGI_SWAP_CHAIN_DESC1 pd = { width, height, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, { 1, 0 }, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
                                     DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED, 0 };
        IDXGISwapChain1 *ps = nullptr;
        HRESULT ph = fac->CreateSwapChainForHwnd(queue, hwnd, &pd, nullptr, nullptr, &ps);
        fprintf(stderr, "  p12: probe: a plain flip swapchain on the game window: 0x%08lx\n", ph);
        if (ps) { ps->Release(); queue_idle(queue); }
    }
    IDXGISwapChain4 *s4 = ffx_fg_create(d12, queue, fac, hwnd, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, hd.Format, 2);
    if (s4) fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    fac->Release();
    if (!s4) return 0;
    sc = s4;
    p_hwnd = hwnd;
    for (uint32_t i = 0; i < 2; i++) sc->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&bufs[i]);
    hd.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    for (auto &r : hud_copy)
        if (FAILED(d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &hd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                __uuidof(ID3D12Resource), (void **)&r))) return 0;
    return 1;
}

static int init_core(const LUID *luid, HWND hwnd, uint32_t w, uint32_t h, const wchar_t *sl_dir, const wchar_t *log_dir)
{
    if (p_tech == 2 || dev_getenv("FGB_FSR")) return init_fsr(luid, w, h);
    {   // NVIDIA only: the adapter the inputs come from, checked with Windows' own DXGI before any NVIDIA file is loaded
        IDXGIFactory4 *f = nullptr;
        IDXGIAdapter1 *a = nullptr;
        DXGI_ADAPTER_DESC1 ad{};
        bool known = SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&f)) &&
                     SUCCEEDED(f->EnumAdapterByLuid(*luid, __uuidof(IDXGIAdapter1), (void **)&a)) && SUCCEEDED(a->GetDesc1(&ad));
        if (a) a->Release();
        if (f) f->Release();
        if (!known || ad.VendorId != 0x10DE) {
            if (known && p_tech) {   // AMD, Intel: no NVIDIA file is loaded
                fprintf(stderr, "  p12: the game's GPU is not NVIDIA (vendor 0x%04X) - AMD FSR for upscaling and frame generation\n", ad.VendorId);
                return init_fsr(luid, w, h);
            }
            fprintf(stderr, "p12: the game's GPU is %s (vendor 0x%04X) - DLSS needs an NVIDIA RTX GPU, helper not started\n",
                    known ? "not NVIDIA" : "unknown", ad.VendorId);
            return 0;
        }
    }
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\sl.interposer.dll", sl_dir);
    if (!signed_by_nvidia(path)) { fprintf(stderr, "p12: sl.interposer.dll signature check failed\n"); return 0; }
    HMODULE m = LoadLibraryW(path);
    if (!m) { fprintf(stderr, "p12: cannot load %ls\n", path); return 0; }
#define GET(x) if (!(p_##x = (decltype(p_##x))GetProcAddress(m, #x))) { fprintf(stderr, "p12: missing %s\n", #x); return 0; }
    GET(slInit) GET(slShutdown) GET(slSetD3DDevice) GET(slIsFeatureSupported) GET(slSetTagForFrame)
    GET(slSetConstants) GET(slGetNewFrameToken) GET(slGetFeatureFunction) GET(slEvaluateFeature)
    auto slD3D12CreateDevice = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(m, "D3D12CreateDevice");
    auto slCreateDXGIFactory2 = (HRESULT(WINAPI *)(UINT, REFIID, void **))GetProcAddress(m, "CreateDXGIFactory2");
    if (!slD3D12CreateDevice || !slCreateDXGIFactory2) { fprintf(stderr, "p12: interposer exports missing\n"); return 0; }

    Preferences pref{};
    const wchar_t *paths[] = { sl_dir };
    Feature features[] = { kFeatureDLSS_G, kFeatureReflex, kFeaturePCL, kFeatureDLSS };
    pref.showConsole = false;
    pref.logLevel = LogLevel::eDefault;
    pref.pathsToPlugins = paths; pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = log_dir;
    pref.logMessageCallback = logcb;
    // NVIDIA's over-the-air updates (Streamline programming guide 2.2.5): Streamline / NGX fetch newer NVIDIA-signed DLSS
    // and plugin versions themselves and load them (also keeps DLSS working on GPUs newer than this SDK); we download
    // nothing ourselves. Frame-based tagging is required by slSetTagForFrame.
    pref.flags = PreferenceFlags::eDisableCLStateTracking | PreferenceFlags::eUseFrameBasedResourceTagging | PreferenceFlags::eAllowOTA |
                 PreferenceFlags::eLoadDownloadedPlugins;
    pref.featuresToLoad = features; pref.numFeaturesToLoad = 4;
    pref.engine = EngineType::eCustom; pref.engineVersion = "1.0";
    pref.projectId = "5b0f6f3c-2d4e-4d8a-9a51-7a3c9e2f1b64";   // our own GUID for this proof
    pref.renderAPI = RenderAPI::eD3D12;
    Result r = p_slInit(pref, kSDKVersion);
    if (r != Result::eOk) { fprintf(stderr, "p12: slInit failed (%d)\n", (int)r); return 0; }

    IDXGIFactory4 *fac;
    if (FAILED(slCreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&fac))) return 0;
    // the adapter of the D3D11 device the inputs come from (shared resources need the same one)
    IDXGIAdapter1 *ad12;
    if (FAILED(fac->EnumAdapterByLuid(*luid, __uuidof(IDXGIAdapter1), (void **)&ad12))) return 0;
    AdapterInfo ai{};
    ai.deviceLUID = (uint8_t *)luid; ai.deviceLUIDSizeInBytes = sizeof(LUID);
    Result sup = p_slIsFeatureSupported(kFeatureDLSS_G, ai), sup_sr = p_slIsFeatureSupported(kFeatureDLSS, ai);
    fprintf(stderr, "  p12: DLSS SR supported: %s (%d), DLSS-G supported: %s (%d)\n", sup_sr == Result::eOk ? "yes" : "NO", (int)sup_sr,
            sup == Result::eOk ? "yes" : "NO", (int)sup);
    if (sup_sr != Result::eOk && sup != Result::eOk) {   // neither (not an RTX GPU): nothing for the helper to do
        if (p_tech) {   // GTX 10 / 16: AMD FSR for both, Streamline closed again
            ad12->Release(); fac->Release();
            p_slShutdown();
            fprintf(stderr, "  p12: no DLSS on this NVIDIA GPU - AMD FSR for upscaling and frame generation\n");
            return init_fsr(luid, w, h);
        }
        return 0;
    }
    fg_ok = sup == Result::eOk && fg_test_limit != 1;
    if (p_tech == 3) fg_ok = false;   // the player picked AMD FSR frame generation (mixed mode below)
    // mixed mode: no DLSS-G here, so AMD FSR frame generation on AMD's own swapchain (Windows' DXGI, not Streamline's);
    // DLSS upscaling as always. Only for a game side that knows FSR (tech != 0): an older one would show it as DLSS.
    // FGB_FSR_NOFG=1: no frame generation, as the release helper on RTX 20 / 30
    if (p_tech && !fg_ok && sup_sr == Result::eOk && !dev_getenv("FGB_FSR_NOFG")) ffx_fg = load_ffx();

    HRESULT hr = slD3D12CreateDevice(ad12, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void **)&d12);
    if (FAILED(hr)) { fprintf(stderr, "p12: D3D12CreateDevice 0x%08lx\n", hr); return 0; }
    p_slSetD3DDevice(d12);
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    d12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&queue);
    for (auto &a : alloc) d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&a);
    for (auto &a : sr_alloc) d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&a);
    d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&cl);
    cl->Close();
    if (ffx_fg) {   // AMD's swapchain comes in init_fsr_fg, once the game's textures are open
        fac->Release(); ad12->Release();
        width = w; height = h; fg_max = 1;
        fprintf(stderr, "  p12: DLSS upscaling with AMD FSR frame generation (no DLSS-G on this GPU%s), frame generation allowed: 2x\n",
                fg_test_limit == 1 ? ": test limit" : "");
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC1 d = { w, h, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, { 1, 0 }, DXGI_USAGE_RENDER_TARGET_OUTPUT, 3,
                                DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED, 0 };
    IDXGISwapChain1 *sc1;
    hr = fac->CreateSwapChainForHwnd(queue, hwnd, &d, nullptr, nullptr, &sc1);
    if (FAILED(hr)) { fprintf(stderr, "p12: CreateSwapChainForHwnd 0x%08lx\n", hr); return 0; }
    p_hwnd = hwnd;
    fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&sc);
    for (uint32_t i = 0; i < 3; i++) sc->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&bufs[i]);
    width = w; height = h;
    if (fg_ok) {   // how many generated frames this GPU allows; 4x (3 generated) is the most offered here
        DLSSGState st{};
        bool got = slDLSSGGetState(viewport, st, nullptr) == Result::eOk;
        fg_max = got && st.numFramesToGenerateMax ? st.numFramesToGenerateMax : 1;
        if (fg_max > 3) fg_max = 3;
        vsync_fg = got && st.bIsVsyncSupportAvailable == Boolean::eTrue;
        fprintf(stderr, "  p12: V-Sync with frame generation: %s\n", vsync_fg ? "supported" : "not supported (frame generation presents without it)");
    }
    if (fg_test_limit && fg_max > fg_test_limit - 1) fg_max = fg_test_limit - 1;
    fprintf(stderr, "  p12: frame generation allowed: %s%s\n", !fg_max ? "no" : fg_max == 1 ? "2x" : fg_max == 2 ? "up to 3x" : "up to 4x",
            fg_test_limit ? " (test limit)" : "");
    return 1;
}

static void init_tail(void)
{
    fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 6, 0 };
    d12->CreateQueryHeap(&qh, __uuidof(ID3D12QueryHeap), (void **)&ts_heap);
    D3D12_HEAP_PROPERTIES rbh = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC tbd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 48, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                                D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    d12->CreateCommittedResource(&rbh, D3D12_HEAP_FLAG_NONE, &tbd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                 __uuidof(ID3D12Resource), (void **)&ts_buf);
    queue->GetTimestampFrequency(&ts_freq);

    if (!sl_on) return;
    ReflexOptions ro{};
    ro.mode = ReflexMode::eLowLatency;
    ro.frameLimitUs = 0;   // Reflex limits shown frames, not rendered ones; the test app caps its own rate
    slReflexSetOptions(ro);
    if (!br[0]) begin_frame(0);
}

/* proof path (P3): inputs are the helper's own D3D11 textures; the helper creates the fence and opens it in D3D11 */
extern "C" int p12_init(ID3D11Device *d11, ID3D11DeviceContext *ctx11, HWND hwnd, uint32_t w, uint32_t h,
                        const wchar_t *sl_dir, const wchar_t *log_dir)
{
    IDXGIDevice *xd; IDXGIAdapter *ad; DXGI_ADAPTER_DESC adesc;
    d11->QueryInterface(__uuidof(IDXGIDevice), (void **)&xd); xd->GetAdapter(&ad); ad->GetDesc(&adesc);
    if (!init_core(&adesc.AdapterLuid, hwnd, w, h, sl_dir, log_dir)) return 0;
    // one shared fence: D3D11 signals 2n+1, D3D12 waits it and signals 2n+2, D3D11 waits 2n+2 before frame n+1
    HANDLE fh;
    d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), (void **)&fence12);
    d12->CreateSharedHandle(fence12, nullptr, GENERIC_ALL, nullptr, &fh);
    ID3D11Device5 *d5;
    if (FAILED(d11->QueryInterface(__uuidof(ID3D11Device5), (void **)&d5)) ||
        FAILED(d5->OpenSharedFence(fh, __uuidof(ID3D11Fence), (void **)&fence11)) ||
        FAILED(ctx11->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4))) { fprintf(stderr, "p12: shared fence\n"); return 0; }
    CloseHandle(fh);
    init_tail();
    return 1;
}

/* bridge path: the game's D3D11 device (another process) created the fence and the textures under these names:
   fence, final colour (with UI), HUD-less colour, depth (R32F), motion vectors (RG16F, pixels) */
extern "C" int p12_bridge_init(uint32_t luid_lo, int32_t luid_hi, HWND hwnd, uint32_t w, uint32_t h, const wchar_t *const names[9], const HANDLE imported[10],
                               const wchar_t *sl_dir, const wchar_t *log_dir)
{
    LUID luid = { luid_lo, luid_hi };
    if (!init_core(&luid, hwnd, w, h, sl_dir, log_dir)) return 0;
    // imported handles are borrowed for this call; helper64 closes them on every outcome.
    ID3D12Fence **fences[3] = { &f_game, &f_sr, &f_done };
    const char *resource[10] = { "fence", "fsr", "fdone", "final", "hudless", "depth", "mvec", "srout0", "srout1", "react" };
    auto release_imports = [&]() {
        for (auto f : fences) if (*f) { (*f)->Release(); *f = nullptr; }
        for (auto &r : br) if (r) { r->Release(); r = nullptr; }
        if (br_react) { br_react->Release(); br_react = nullptr; }
    };
    for (int i = 0; i < 10; ++i) {
        if (i == 9 && dev_getenv("FGB_FSR_NOREACT")) continue;
        HANDLE hh = imported ? imported[i] : nullptr;
        HRESULT hr = S_OK;
        const char *op = "OpenSharedHandle";
        if (!imported) {
            wchar_t rn[64];
            const wchar_t *name = i < 9 ? names[i] : rn;
            if (i == 9) {
                lstrcpynW(rn, names[3], 64);
                wchar_t *suffix = wcsstr(rn, L"_final");
                if (!suffix) continue;
                lstrcpyW(suffix, L"_react");
            }
            op = "OpenSharedHandleByName";
            hr = d12->OpenSharedHandleByName(name, GENERIC_ALL, &hh);
        }
        if (SUCCEEDED(hr) && hh) {
            op = "OpenSharedHandle";
            hr = i < 3 ? d12->OpenSharedHandle(hh, __uuidof(ID3D12Fence), (void **)fences[i])
                        : d12->OpenSharedHandle(hh, __uuidof(ID3D12Resource), i == 9 ? (void **)&br_react : (void **)&br[i - 3]);
        } else if (SUCCEEDED(hr) && i < 9) hr = E_HANDLE;
        if (!imported && hh) CloseHandle(hh);
        if (FAILED(hr)) {
            fprintf(stderr, "p12: resource=%s operation=%s HRESULT=0x%08lx%s\n", resource[i], op, (unsigned long)hr, i == 9 ? " (optional)" : "");
            if (i != 9) { release_imports(); return 0; }
        }
    }
    fprintf(stderr, "  p12: AMD FSR reactive mask: %s\n", br_react ? "from the game" : "none");
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    if (FAILED(d12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&queue_sr)) ||
        FAILED(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, sr_alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList),
                                      (void **)&cl_sr))) { fprintf(stderr, "p12: SR queue\n"); release_imports(); return 0; }
    cl_sr->Close();
    sr_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ffx_fg && !init_fsr_fg(hwnd)) { release_imports(); return 0; }
    init_tail();
    return 1;
}

// NVIDIA Reflex's frame limiter (the game's frame cap; 0 = none). It keeps the low-latency mode set in init_tail.
// G-Sync / VRR on the game window's display, from NVIDIA's driver: NvAPI_Disp_GetVRRInfo's flags (bit 0 VRR enabled, 1
// possible, 2 requested, 3 indicator, 4 display in VRR mode), or -1 when unknown. Function IDs and NV_GET_VRR_INFO_V1's
// layout from Rome 2's port (the NVAPI headers, MIT licence); nvapi64.dll is the driver's own.
static void *(*nvapi_qi(void))(uint32_t)   // nvapi_QueryInterface once NvAPI_Initialize worked, else null
{
    static void *(*qi)(uint32_t);
    static bool tried;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryW(L"nvapi64.dll");
        auto q = m ? (void *(*)(uint32_t))(void *)GetProcAddress(m, "nvapi_QueryInterface") : nullptr;
        auto init = q ? (int (*)(void))q(0x0150e828) : nullptr;   // NvAPI_Initialize
        if (init && init() == 0) qi = q;
    }
    return qi;
}

extern "C" int p12_vrr_flags(void)
{
    auto qi = nvapi_qi();
    auto by_name = qi ? (int (*)(const char *, uint32_t *))qi(0xae457190) : nullptr;   // NvAPI_DISP_GetDisplayIdByDisplayName
    auto vrr_info = qi ? (int (*)(uint32_t, void *))qi(0xdf8fda57) : nullptr;          // NvAPI_Disp_GetVRRInfo
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof mi;
    HMONITOR mon = p_hwnd ? MonitorFromWindow(p_hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    uint32_t id;
    if (!by_name || !vrr_info || !mon || !GetMonitorInfoA(mon, &mi) || by_name(mi.szDevice, &id) != 0) return -1;
    struct { uint32_t version, flags, reserved[4]; } v{};
    v.version = sizeof v | 1u << 16;   // NV_GET_VRR_INFO_VER1
    return vrr_info(id, &v) == 0 ? (int)(v.flags & 31) : -1;
}

// The driver's Vertical sync for this process (NVIDIA Control Panel / app: helper64.exe's own profile if one names it,
// else the global one): it overrides the Present interval, so a forced On costs the V-Sync fps whatever the player picks
// in F11 (AGENT_LOG 157). 0 the application decides, 1 forced on (or every 2nd-4th refresh), 2 forced off, 3 Fast Sync,
// -1 unknown. IDs, values and layouts from Streamline's NVAPI headers (NvApiDriverSettings.h, nvapi.h: NVDRS_SETTING_V1
// 12320 bytes, current value at 8220, NVDRS_APPLICATION_V4 20492 bytes, pack 4).
extern "C" int p12_driver_vsync(void)
{
    auto qi = nvapi_qi();
    if (!qi) return -1;
    auto create = (int (*)(void **))qi(0x0694d52e);
    auto load = (int (*)(void *))qi(0x375dbd6b), destroy = (int (*)(void *))qi(0xdad9cff8);
    auto global = (int (*)(void *, void **))qi(0x617bff9f);
    auto find = (int (*)(void *, const uint16_t *, void **, void *))qi(0xeee566b2);
    auto get = (int (*)(void *, void *, uint32_t, void *))qi(0x73bf8338);
    void *s = nullptr, *prof = nullptr;
    if (!create || !load || !destroy || !global || !find || !get || create(&s) != 0) return -1;
    static uint8_t app[20492], set[12320];
    static uint16_t name[2048];   // NvAPI_UnicodeString: the driver may read all 2048
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t *base = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
    for (int i = 0; n && base[i] && i < 2047; i++) name[i] = base[i];
    *(uint32_t *)app = sizeof app | 4u << 16;   // NVDRS_APPLICATION_VER_V4
    *(uint32_t *)set = sizeof set | 1u << 16;   // NVDRS_SETTING_VER1
    int r = load(s) != 0 ? -1 : find(s, name, &prof, app) == 0 || global(s, &prof) == 0 ? get(s, prof, 0x00A879CF, set) : -1;
    destroy(s);
    uint32_t v = r == 0 ? *(uint32_t *)(set + 8220) : r == -160 ? 0x60925292u : 0;   // -160 NVAPI_SETTING_NOT_FOUND: default
    switch (v) {
    case 0x60925292: return 0;   // VSYNCMODE_PASSIVE (the application decides, the default)
    case 0x47814940: case 0x32610244: case 0x71271021: case 0x13245256: return 1;   // FORCEON, FLIPINTERVAL2..4
    case 0x08416747: return 2;   // FORCEOFF
    case 0x18888888: return 3;   // VIRTUAL (Fast Sync)
    default: return -1;
    }
}

extern "C" void p12_set_frame_limit(uint32_t us)
{
    if (ffx_fg) { frame_limit_us = us; return; }   // our own limiter in present_fsr
    ReflexOptions ro{};
    ro.mode = ReflexMode::eLowLatency;
    ro.frameLimitUs = us;
    if (slReflexSetOptions(ro) != Result::eOk) fprintf(stderr, "p12: slReflexSetOptions (frame limit %u us) failed\n", us);
}

extern "C" void p12_set_mode(uint32_t frames_to_generate)
{
    if (frames_to_generate > fg_max) frames_to_generate = fg_max;   // never more than the GPU allows
    fg_frames = frames_to_generate;
    if (!fg_ok || ffx_fg) return;   // no frame generation on this GPU: DLSS-G stays off (its default); FSR reads fg_frames
    DLSSGOptions o{};
    o.mode = frames_to_generate ? DLSSGMode::eOn : DLSSGMode::eOff;
    o.numFramesToGenerate = frames_to_generate ? frames_to_generate : 1;
    Result r = slDLSSGSetOptions(viewport, o);
    if (r != Result::eOk) fprintf(stderr, "p12: slDLSSGSetOptions failed (%d)\n", (int)r);
}

static float4x4 identity()
{
    float4x4 m;
    for (int i = 0; i < 4; i++) m.row[i] = float4(i == 0, i == 1, i == 2, i == 3);
    return m;
}

// bridge: the game's camera, already in SL's convention (row-major, row vectors); identity until set
static float4x4 cam_c2p = identity(), cam_p2c = identity();
extern "C" void p12_set_camera(const float clip_to_prev[16], const float prev_to_clip[16])
{
    for (int i = 0; i < 4; i++) {
        cam_c2p.row[i] = float4(clip_to_prev[i * 4], clip_to_prev[i * 4 + 1], clip_to_prev[i * 4 + 2], clip_to_prev[i * 4 + 3]);
        cam_p2c.row[i] = float4(prev_to_clip[i * 4], prev_to_clip[i * 4 + 1], prev_to_clip[i * 4 + 2], prev_to_clip[i * 4 + 3]);
    }
}

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER rb = {};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = r; rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = a; rb.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &rb);
}

static DLSSPreset sr_preset = DLSSPreset::eDefault;
extern "C" void p12_set_sr_preset(uint32_t preset)
{
    // from the game's memory: J, K, L, M only (the others are deprecated or fall back to the default anyway)
    sr_preset = preset >= 10 && preset <= 13 ? (DLSSPreset)preset : DLSSPreset::eDefault;
}

extern "C" void p12_set_sr(int on, uint32_t render_w, uint32_t render_h, float jitter_sign)
{
    sr_on = on; rw = render_w; rh = render_h; jit_sign = jitter_sign;
    if (!sl_on) {
        fprintf(stderr, "  p12: AMD FSR upscaling %s, render %ux%u of %ux%u\n", on ? "on" : "off", rw, rh, width, height);
        return;
    }
    DLSSOptions o{};
    float r = (float)render_w / width;   // the game picks the render size per preset (F12 panel); the mode follows it
    o.mode = !on ? DLSSMode::eOff : r >= 0.99f ? DLSSMode::eDLAA : r >= 0.64f ? DLSSMode::eMaxQuality : r >= 0.55f ? DLSSMode::eBalanced
           : r >= 0.45f ? DLSSMode::eMaxPerformance : DLSSMode::eUltraPerformance;
    o.outputWidth = width; o.outputHeight = height;
    o.colorBuffersHDR = Boolean::eFalse;   // the test scene is plain 8-bit colour
    o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset = o.ultraPerformancePreset = sr_preset;
    if (on) fprintf(stderr, "  p12: DLSS preset %c\n", sr_preset == DLSSPreset::eDefault ? '-' : (char)('A' + (int)sr_preset - 1));
    DLSSOptimalSettings best{};
    if (on && slDLSSGetOptimalSettings(o, best) == Result::eOk)
        fprintf(stderr, "  p12: DLSS %s for %ux%u: optimal render %ux%u (range %ux%u..%ux%u), we render %ux%u\n",
                o.mode == DLSSMode::eDLAA ? "DLAA" : o.mode == DLSSMode::eMaxQuality ? "Quality" : o.mode == DLSSMode::eBalanced ? "Balanced"
                : o.mode == DLSSMode::eMaxPerformance ? "Performance" : "Ultra Performance",
                width, height, best.optimalRenderWidth, best.optimalRenderHeight, best.renderWidthMin, best.renderHeightMin,
                best.renderWidthMax, best.renderHeightMax, rw, rh);
    if (slDLSSSetOptions(viewport, o) != Result::eOk) fprintf(stderr, "p12: slDLSSSetOptions failed\n");
    if (on && !sr_out && !br[4]) {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
        d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                     __uuidof(ID3D12Resource), (void **)&sr_out);
    }
}

extern "C" void p12_set_jitter(float x, float y) { jit_x = x; jit_y = y; }

extern "C" void p12_set_dump(uint32_t frame, const char *path)
{
    dump_frame = frame;
    snprintf(dump_path, sizeof dump_path, "%s", path);
}

// copies one RGBA8 texture to a readback buffer on cl; the buffer is written to a PPM after the fence
static ID3D12Resource *readback(ID3D12Resource *tex, D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp)
{
    D3D12_RESOURCE_DESC d = tex->GetDesc();
    UINT64 total;
    d12->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource *buf;
    d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                 __uuidof(ID3D12Resource), (void **)&buf);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = buf; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    return buf;
}

static void write_ppm(ID3D12Resource *buf, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp, const char *path)
{
    uint8_t *p;
    HRESULT mh = buf->Map(0, nullptr, (void **)&p);   // whole buffer: the last row has no pitch padding
    if (FAILED(mh)) { fprintf(stderr, "p12: readback Map failed 0x%08lx for %s\n", mh, path); return; }
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", fp.Footprint.Width, fp.Footprint.Height);
        for (UINT y = 0; y < fp.Footprint.Height; y++)
            for (UINT x = 0; x < fp.Footprint.Width; x++) fwrite(p + (size_t)y * fp.Footprint.RowPitch + x * 4, 1, 3, f);
        long sz = ftell(f);
        int cr = fclose(f);
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        BOOL ok = GetFileAttributesExA(path, GetFileExInfoStandard, &fa);
        fprintf(stderr, "  p12: wrote %s (%ld bytes, fclose %d, exists %d size %lu)\n", path, sz, cr, ok, fa.nFileSizeLow);
    }
    buf->Unmap(0, nullptr);
    buf->Release();
}

static void set_constants(FrameToken &tk, int reset)
{
    uint32_t iw = sr_on ? rw : width, ih = sr_on ? rh : height;   // size of the depth / motion vector inputs
    Constants k{};
    k.cameraViewToClip = k.clipToCameraView = k.clipToLensClip = identity();
    k.clipToPrevClip = cam_c2p; k.prevClipToClip = cam_p2c;
    k.jitterOffset = sr_on ? float2(jit_sign * jit_x, jit_sign * jit_y) : float2(0, 0);
    k.mvecScale = float2(1.0f / iw, 1.0f / ih);   // motion vectors are in pixels of the input size
    k.cameraPinholeOffset = float2(0, 0);
    k.cameraPos = float3(0, 0, 0); k.cameraUp = float3(0, 1, 0); k.cameraRight = float3(1, 0, 0); k.cameraFwd = float3(0, 0, 1);
    k.cameraNear = 0.1f; k.cameraFar = 1000.0f; k.cameraFOV = 1.0f; k.cameraAspectRatio = (float)width / height;
    k.depthInverted = Boolean::eFalse; k.cameraMotionIncluded = Boolean::eTrue; k.motionVectors3D = Boolean::eFalse;
    k.reset = reset ? Boolean::eTrue : Boolean::eFalse;
    p_slSetConstants(k, tk, viewport);
}

static void wait_value(ID3D12Fence *f, uint64_t v, HANDLE ev)
{
    if (f->GetCompletedValue() < v) {
        f->SetEventOnCompletion(v, ev);
        WaitForSingleObject(ev, INFINITE);
    }
}

static void collect_ts(uint32_t slot)   // DLSS GPU time of the frame that last used this slot (its pass is done)
{
    if (!ts_pending[slot]) return;
    uint64_t *t;
    D3D12_RANGE r = { slot * 16, slot * 16 + 16 };
    if (SUCCEEDED(ts_buf->Map(0, &r, (void **)&t))) {
        st_dlss_gpu_ms += (double)(t[slot * 2 + 1] - t[slot * 2]) * 1000.0 / ts_freq;
        st_dlss_n++;
        D3D12_RANGE none = { 0, 0 };
        ts_buf->Unmap(0, &none);
    }
    ts_pending[slot] = false;
}

/* bridge, SR thread: starts frame n (token, markers, constants) and, with sr, runs DLSS SR on its own queue from the
   game's HUD-less colour (render size, top-left) into SR output n % 2, which the game copies into its back buffer before
   drawing the UI. Waits game fence 2n+1 (and present fence n-1: output n % 2 was last read by frame n-2), signals SR
   fence n+1. Only this thread makes frame tokens and calls slEvaluateFeature (Streamline: not thread safe). */
extern "C" int p12_bridge_frame(uint32_t n, int reset, int sr)
{
    FrameToken *tk = nullptr;
    if (sl_on) {
        p_slGetNewFrameToken(tokens[n % 4], &n);
        tk = tokens[n % 4];
        slPCLSetMarker(PCLMarker::eSimulationStart, *tk);
        slPCLSetMarker(PCLMarker::eSimulationEnd, *tk);
        slPCLSetMarker(PCLMarker::eRenderSubmitStart, *tk);
        set_constants(*tk, reset);
    }
    if (ffx_fg) {   // frame n's camera, jitter and frame time, for its upscaling here and its frame generation on the present thread
        auto &fi = fr_info[n % 8];
        CamVp c;
        if (cam_from_vp(view_vp, &c)) cam_ok = c;
        fi.cam = cam_ok;
        fi.jx = sr ? fsr_jsign * jit_x : 0; fi.jy = sr ? fsr_jsign * jit_y : 0;
        LARGE_INTEGER now, qf;
        QueryPerformanceCounter(&now); QueryPerformanceFrequency(&qf);
        fi.dt = fsr_last.QuadPart ? (float)((now.QuadPart - fsr_last.QuadPart) * 1000.0 / qf.QuadPart) : 16.7f;
        if (fi.dt > 100) fi.dt = 100;
        fsr_last = now;
        fi.reset = reset != 0;   // camera cut / stall (helper64.c): frame generation must not blend across it either
    }
    if (!sr) return 1;
    uint32_t slot = n % 3;
    wait_value(f_sr, sr_slot_done[slot], sr_event);
    collect_ts(slot);
    sr_alloc[slot]->Reset();
    cl_sr->Reset(sr_alloc[slot], nullptr);
    ID3D12GraphicsCommandList *c = cl_sr;
    ID3D12Resource *out = br[4 + n % 2];
    const D3D12_RESOURCE_STATES rd = D3D12_RESOURCE_STATE_GENERIC_READ, ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto bar = [c](ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = r; rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        rb.Transition.StateBefore = a; rb.Transition.StateAfter = b;
        c->ResourceBarrier(1, &rb);
    };
    for (int i = 1; i < 4; i++) bar(br[i], D3D12_RESOURCE_STATE_COMMON, rd);
    if (!sl_on && br_react) bar(br_react, D3D12_RESOURCE_STATE_COMMON, rd);
    bar(out, D3D12_RESOURCE_STATE_COMMON, ua);
    Resource rin(ResourceType::eTex2d, br[1], nullptr, nullptr, (uint32_t)rd);
    Resource rdp(ResourceType::eTex2d, br[2], nullptr, nullptr, (uint32_t)rd);
    Resource rm(ResourceType::eTex2d, br[3], nullptr, nullptr, (uint32_t)rd);
    Resource rout(ResourceType::eTex2d, out, nullptr, nullptr, (uint32_t)ua);
    Extent in_ext{ 0, 0, rw, rh }, out_ext{ 0, 0, width, height };
    ResourceTag tags[] = {
        ResourceTag(&rdp, kBufferTypeDepth, ResourceLifecycle::eOnlyValidNow, &in_ext),
        ResourceTag(&rm, kBufferTypeMotionVectors, ResourceLifecycle::eOnlyValidNow, &in_ext),
        ResourceTag(&rin, kBufferTypeScalingInputColor, ResourceLifecycle::eValidUntilEvaluate, &in_ext),
        ResourceTag(&rout, kBufferTypeScalingOutputColor, ResourceLifecycle::eValidUntilEvaluate, &out_ext),
    };
    if (sl_on && p_slSetTagForFrame(*tk, viewport, tags, 4, (CommandBuffer *)c) != Result::eOk) st_tag_fail++;
    c->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    const BaseStructure *inputs[] = { &viewport };
    if (!sl_on) {
        const auto &fi = fr_info[n % 8];
        if (!ffx_sr_dispatch(c, br[1], br[2], br[3], br_react, out, rw, rh, fi.jx, fi.jy, reset, &fi.cam, fi.dt)) st_tag_fail++;
    } else
    if (p_slEvaluateFeature(kFeatureDLSS, *tk, inputs, 1, (CommandBuffer *)c) != Result::eOk) st_tag_fail++;
    c->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    c->ResolveQueryData(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, ts_buf, slot * 16);
    ts_pending[slot] = true;
    for (int i = 1; i < 4; i++) bar(br[i], rd, D3D12_RESOURCE_STATE_COMMON);
    if (!sl_on && br_react) bar(br_react, rd, D3D12_RESOURCE_STATE_COMMON);
    bar(out, ua, D3D12_RESOURCE_STATE_COMMON);
    c->Close();
    queue_sr->Wait(f_game, 2ull * n + 1);
    if (n >= 2) queue_sr->Wait(f_done, n - 1);
    queue_sr->ExecuteCommandLists(1, (ID3D12CommandList **)&c);
    queue_sr->Signal(f_sr, n + 1);
    sr_slot_done[slot] = n + 1;
    return 1;
}

/* one frame: c12 = the image to show (with UI), hud12 = HUD-less colour, d12r = depth, m12 = motion vectors. proof: the
   inputs come from this process's D3D11 device (signal / wait it here); bridge: the game signals and waits itself. */
static int present_core(uint32_t n, ID3D12Resource *c12, ID3D12Resource *hud12, ID3D12Resource *d12r, ID3D12Resource *m12,
                        int reset, int proof, ID3D12Fence *fw, uint64_t wait_v, ID3D12Fence *fs, uint64_t signal_v, int sr_done)
{
    const bool bridge = br[0] != nullptr;
    FrameToken &tk = *tokens[n % 4];
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    slPCLSetMarker(PCLMarker::eRenderSubmitEnd, tk);
    uint32_t iw = bridge ? br_iw : sr_on ? rw : width, ih = bridge ? br_ih : sr_on ? rh : height;   // size of the depth / motion vector inputs
    if (!bridge) set_constants(tk, reset);   // the bridge set them when it started the frame (p12_bridge_frame)

    // D3D11 finished frame n -> D3D12 may read
    if (proof) { ctx4->Signal(fence11, wait_v); ctx4->Flush(); }
    queue->Wait(fw, wait_v);

    uint32_t slot = n % 3;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    wait_value(fs, slot_done[slot], fence_event);
    QueryPerformanceCounter(&t1);
    st_slot_t += (t1.QuadPart - t0.QuadPart);
    if (!bridge) collect_ts(slot);
    alloc[slot]->Reset();
    cl->Reset(alloc[slot], nullptr);
    ID3D12Resource *bb = bufs[sc->GetCurrentBackBufferIndex()];
    const D3D12_RESOURCE_STATES rd = D3D12_RESOURCE_STATE_GENERIC_READ;
    barrier(c12, D3D12_RESOURCE_STATE_COMMON, rd);
    if (hud12 != c12) barrier(hud12, D3D12_RESOURCE_STATE_COMMON, rd);
    barrier(d12r, D3D12_RESOURCE_STATE_COMMON, rd);
    barrier(m12, D3D12_RESOURCE_STATE_COMMON, rd);

    // the inputs are overwritten by the next D3D11 frame, so SL copies depth / motion vectors now on this command list
    Resource rdp(ResourceType::eTex2d, d12r, nullptr, nullptr, (uint32_t)rd);
    Resource rm(ResourceType::eTex2d, m12, nullptr, nullptr, (uint32_t)rd);
    Extent in_ext{ 0, 0, iw, ih }, out_ext{ 0, 0, width, height };
    ID3D12Resource *shown;               // what goes to the back buffer; also the HUD-less input of DLSS-G
    D3D12_RESOURCE_STATES shown_state;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp_in, fp_out;
    ID3D12Resource *dump_in = nullptr, *dump_out = nullptr;
    if (sr_on && !sr_done && !br[4]) {   // the proof does its own SR here; the bridge in p12_bridge_frame
        Resource rin(ResourceType::eTex2d, c12, nullptr, nullptr, (uint32_t)rd);
        Resource rout(ResourceType::eTex2d, sr_out, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ResourceTag tags[] = {
            ResourceTag(&rdp, kBufferTypeDepth, ResourceLifecycle::eOnlyValidNow, &in_ext),
            ResourceTag(&rm, kBufferTypeMotionVectors, ResourceLifecycle::eOnlyValidNow, &in_ext),
            ResourceTag(&rin, kBufferTypeScalingInputColor, ResourceLifecycle::eValidUntilEvaluate, &in_ext),
            ResourceTag(&rout, kBufferTypeScalingOutputColor, ResourceLifecycle::eValidUntilEvaluate, &out_ext),
        };
        if (p_slSetTagForFrame(tk, viewport, tags, 4, (CommandBuffer *)cl) != Result::eOk) st_tag_fail++;
        cl->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
        const BaseStructure *inputs[] = { &viewport };
        if (p_slEvaluateFeature(kFeatureDLSS, tk, inputs, 1, (CommandBuffer *)cl) != Result::eOk) st_tag_fail++;
        cl->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
        cl->ResolveQueryData(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, ts_buf, slot * 16);
        ts_pending[slot] = true;
        barrier(sr_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        shown = sr_out; shown_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (n == dump_frame) dump_in = readback(c12, fp_in);
    } else {
        ResourceTag tags[] = {
            ResourceTag(&rdp, kBufferTypeDepth, ResourceLifecycle::eOnlyValidNow, &in_ext),
            ResourceTag(&rm, kBufferTypeMotionVectors, ResourceLifecycle::eOnlyValidNow, &in_ext),
        };
        if (p_slSetTagForFrame(tk, viewport, tags, 2, (CommandBuffer *)cl) != Result::eOk) st_tag_fail++;
        shown = c12; shown_state = rd;
    }
    if (n == dump_frame) dump_out = readback(shown, fp_out);
    barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    // the reference run renders larger than the window: show its top-left corner
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = bb; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = shown; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { 0, 0, 0, width, height, 1 };
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    bool own_sr = sr_on && !sr_done && !br[4];   // bridge with SR: hud12 is the shared SR output (full size)
    Resource rhud(ResourceType::eTex2d, own_sr ? shown : hud12, nullptr, nullptr, (uint32_t)(own_sr ? shown_state : rd));
    ResourceTag hud(&rhud, kBufferTypeHUDLessColor, ResourceLifecycle::eOnlyValidNow, &out_ext);
    if (p_slSetTagForFrame(tk, viewport, &hud, 1, (CommandBuffer *)cl) != Result::eOk) st_tag_fail++;
    if (own_sr) barrier(sr_out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(c12, rd, D3D12_RESOURCE_STATE_COMMON);
    if (hud12 != c12) barrier(hud12, rd, D3D12_RESOURCE_STATE_COMMON);
    barrier(d12r, rd, D3D12_RESOURCE_STATE_COMMON);
    barrier(m12, rd, D3D12_RESOURCE_STATE_COMMON);
    cl->Close();
    queue->ExecuteCommandLists(1, (ID3D12CommandList **)&cl);
    queue->Signal(fs, signal_v);
    slot_done[slot] = signal_v;

    slPCLSetMarker(PCLMarker::ePresentStart, tk);
    QueryPerformanceCounter(&t0);
    UINT sync = !vs_user_off && !vs_held && (!fg_ok || !fg_frames || vsync_fg) ? 1 : 0;
    (sync ? st_vsync_on : st_vsync_off)++;
    HRESULT hr = sc->Present(sync, 0);
    QueryPerformanceCounter(&t1);
    st_present_t += (t1.QuadPart - t0.QuadPart);
    vsync_guard(sync, t1.QuadPart, t1.QuadPart - t0.QuadPart);
    slPCLSetMarker(PCLMarker::ePresentEnd, tk);
    if (FAILED(hr)) st_present_fail++;
    st_presents++;

    DLSSGState s{};
    if (!fg_ok) { st_frames_shown++; st_shown_total++; st_shown_hist[1]++; }   // every rendered frame is shown once
    else if (slDLSSGGetState(viewport, s, nullptr) == Result::eOk) {
        st_frames_shown += s.numFramesActuallyPresented;
        st_shown_total += s.numFramesActuallyPresented;
        st_shown_hist[s.numFramesActuallyPresented < 5 ? s.numFramesActuallyPresented : 5]++;
        if (s.status != DLSSGStatus::eOk) { st_bad_status++; st_last_status = (uint32_t)s.status; }
    }
    if (dump_out) {   // wait for this frame on the GPU, then write the images
        fs->SetEventOnCompletion(signal_v, fence_event);
        WaitForSingleObject(fence_event, INFINITE);
        char path[MAX_PATH + 8];
        if (dump_in) { snprintf(path, sizeof path, "%s_lowres.ppm", dump_path); write_ppm(dump_in, fp_in, path); }
        snprintf(path, sizeof path, "%s_out.ppm", dump_path);
        write_ppm(dump_out, fp_out, path);
    }

    // next D3D11 frame may overwrite the inputs once D3D12 has copied them
    if (proof) ctx4->Wait(fence11, signal_v);
    QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
    st_cpu_ms += (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;   // before Reflex sleep (begin_frame)
    st_tick_ms = 1000.0 / qf.QuadPart;
    if (!bridge) begin_frame(n + 1);
    return 1;
}

extern "C" int p12_present(uint32_t n, ID3D11Texture2D *color, ID3D11Texture2D *depth, ID3D11Texture2D *mvec, int reset)
{
    ID3D12Resource *c12 = open_shared(color), *d12r = open_shared(depth), *m12 = open_shared(mvec);
    if (!c12 || !d12r || !m12) { fprintf(stderr, "p12: cannot open shared inputs\n"); return 0; }
    return present_core(n, c12, c12, d12r, m12, reset, 1, fence12, 2ull * n + 1, fence12, 2ull * n + 2, 0);
}

/* the same frame through AMD FSR frame generation: final colour into AMD's back buffer, the HUD-less image into our copy
   n % 2 (AMD's swapchain composes after Present returns, the game may overwrite its texture after _fdone n+1), configure +
   prepare (depth / motion vectors are consumed on this command list), then Present: AMD's swapchain interpolates and paces */
static int present_fsr(uint32_t n, int reset, int sr_done, int hud_post)
{
    ID3D12Resource *c12 = br[0], *hud12 = sr_done && !hud_post ? br[4 + n % 2] : br[1], *d12r = br[2], *m12 = br[3], *hc = hud_copy[n % 2];
    LARGE_INTEGER q0, q1, qf, t0, t1;
    QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&qf);
    queue->Wait(f_game, 2ull * n + 2);
    uint32_t slot = n % 3;
    QueryPerformanceCounter(&t0);
    wait_value(f_done, slot_done[slot], fence_event);
    QueryPerformanceCounter(&t1);
    st_slot_t += (t1.QuadPart - t0.QuadPart);
    alloc[slot]->Reset();
    cl->Reset(alloc[slot], nullptr);
    const D3D12_RESOURCE_STATES rd = D3D12_RESOURCE_STATE_GENERIC_READ, cm = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource *in[4] = { c12, hud12, d12r, m12 };
    for (auto r : in) barrier(r, cm, rd);
    ID3D12Resource *bb = bufs[sc->GetCurrentBackBufferIndex()];
    barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = bb; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = c12; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { 0, 0, 0, width, height, 1 };
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    barrier(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    barrier(hc, rd, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(hc, no_hud ? c12 : hud12);
    barrier(hc, D3D12_RESOURCE_STATE_COPY_DEST, rd);
    const auto &fi = fr_info[n % 8];
    bool on = fg_frames > 0;
    reset |= fi.reset;
    if (!ffx_fg_frame(cl, n, on, hc, d12r, m12, br_iw, br_ih, fi.jx, fi.jy, reset, &fi.cam, fi.dt)) st_tag_fail++;
    for (auto r : in) barrier(r, rd, cm);
    cl->Close();
    queue->ExecuteCommandLists(1, (ID3D12CommandList **)&cl);
    queue->Signal(f_done, n + 1);
    slot_done[slot] = n + 1;

    if (frame_limit_us) {   // the frame cap on presents (a present shows 2 frames with frame generation on)
        static LARGE_INTEGER next;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        int64_t iv = (int64_t)frame_limit_us * (on ? 2 : 1) * qf.QuadPart / 1000000;
        // Sleep() wakes on the 15.6 ms system tick: a 120 cap gave 92 fps with frame generation off. A high-resolution
        // timer (Windows 10 1803+) wakes ~1 ms early, the spin does the rest; Sleep only if it cannot be created
        static HANDLE cap_timer;   // set here, not in the declaration: a guarded static would need libc++.dll
        if (!cap_timer) cap_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        int64_t left_ms = (next.QuadPart - now.QuadPart) * 1000 / qf.QuadPart - 1;
        if (left_ms > 0 && cap_timer) {
            LARGE_INTEGER due = { .QuadPart = -left_ms * 10000 };
            if (SetWaitableTimer(cap_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(cap_timer, INFINITE);
        } else if (left_ms > 1) Sleep((DWORD)left_ms);
        do QueryPerformanceCounter(&now); while (now.QuadPart < next.QuadPart);
        next.QuadPart = (next.QuadPart + iv > now.QuadPart ? next.QuadPart : now.QuadPart) + iv;
    }
    // the player's V-Sync, as on the DLSS path: AMD's swapchain presents with the interval given here (with 0 and frame
    // generation on it adds DXGI_PRESENT_ALLOW_TEARING itself and tore: 473 display changes/s on 240 Hz, PresentMon); off
    // asks for tearing with frame generation off too. No guard: AMD's swapchain paces the frames itself
    UINT sync = vs_user_off ? 0 : 1;
    (sync ? st_vsync_on : st_vsync_off)++;
    QueryPerformanceCounter(&t0);
    HRESULT hr = sc->Present(sync, sync ? 0 : DXGI_PRESENT_ALLOW_TEARING);
    QueryPerformanceCounter(&t1);
    st_present_t += (t1.QuadPart - t0.QuadPart);
    if (FAILED(hr)) {
        st_present_fail++;
        fprintf(stderr, "p12: AMD Present frame %u reset %d failed (0x%08lx)\n", n, reset, (unsigned long)hr);
    }
    st_presents++;
    uint32_t shown = on && !reset ? 2 : 1;   // estimate: AMD's swapchain shows the generated frame before each real one
    st_frames_shown += shown; st_shown_total += shown; st_shown_hist[shown]++;
    QueryPerformanceCounter(&q1);
    st_cpu_ms += (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
    st_tick_ms = 1000.0 / qf.QuadPart;
    return 1;
}

/* bridge, present thread: frame generation. With SR on this frame the HUD-less image is SR output n % 2. Waits game
   fence 2n+2 (final colour), signals present fence n+1 once frame n's inputs are read. */
extern "C" int p12_bridge_present(uint32_t n, int reset, int sr_done, uint32_t in_w, uint32_t in_h, int hud_post)
{
    br_iw = in_w; br_ih = in_h;
    /* hud_post: the game wrote its own HUD-less image after SR into _hudless (full size: SR output + ReShade's effects) */
    if (sr_done && hud_post) st_hud_post++;
    if (ffx_fg) return present_fsr(n, reset, sr_done, hud_post);
    return present_core(n, br[0], sr_done && !hud_post ? br[4 + n % 2] : br[1], br[2], br[3], reset, 0, f_game, 2ull * n + 2, f_done, n + 1, sr_done);
}

extern "C" uint32_t p12_shown_total(void) { return st_shown_total; }

extern "C" void p12_stats(char *buf, size_t len, int reset)
{
    snprintf(buf, len, "presents %u, %s %u (%.2f per present; per-present count 0:%u 1:%u 2:%u 3:%u 4:%u >4:%u), "
             "bad status %u (last 0x%x), tag failures %u, present failures %u, mode %u, helper CPU %.3f ms/present "
             "(waiting for the GPU %.3f, inside Present %.3f), DLSS SR GPU %.3f ms (%u), V-Sync on %u / off %u, HUD-less from the game after SR %u",
             st_presents, ffx_fg ? "estimated frames shown" : "frames shown", st_frames_shown, st_presents ? (double)st_frames_shown / st_presents : 0,
             st_shown_hist[0], st_shown_hist[1], st_shown_hist[2], st_shown_hist[3], st_shown_hist[4], st_shown_hist[5],
             st_bad_status, st_last_status, st_tag_fail, st_present_fail, fg_frames, st_presents ? st_cpu_ms / st_presents : 0,
             st_presents ? st_slot_t * st_tick_ms / st_presents : 0, st_presents ? st_present_t * st_tick_ms / st_presents : 0,
             st_dlss_n ? st_dlss_gpu_ms / st_dlss_n : 0, st_dlss_n, st_vsync_on, st_vsync_off, st_hud_post);
    if (reset) {
        st_presents = st_frames_shown = st_bad_status = st_tag_fail = st_present_fail = 0;
        st_vsync_on = st_vsync_off = st_hud_post = 0;
        st_cpu_ms = st_dlss_gpu_ms = st_slot_t = st_present_t = 0; st_dlss_n = 0;
        memset(st_shown_hist, 0, sizeof st_shown_hist);
    }
}

static void queue_idle(ID3D12CommandQueue *q)   // everything submitted to q finished (2 s at most)
{
    ID3D12Fence *f = nullptr;
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (q && e && SUCCEEDED(d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&f)) && SUCCEEDED(q->Signal(f, 1)) &&
        SUCCEEDED(f->SetEventOnCompletion(1, e)))
        WaitForSingleObject(e, 2000);
    if (f) f->Release();
    if (e) CloseHandle(e);
}

// An orderly end: frame generation off, both queues idle, the back buffers and the swapchain released before the process
// ends. Left to the process exit, Windows kept the game window tied to the old swapchain for seconds, and the next helper's
// swapchain on it was refused ("Access is denied" / AMD's "swapchain failed (3)", F11 switch, AGENT_LOG 166).
extern "C" void p12_shutdown(void)
{
    if (!queue) return;
    if (sl_on) p12_set_mode(0);
    queue_idle(queue);
    queue_idle(queue_sr);
    for (auto &b : bufs) if (b) { b->Release(); b = nullptr; }
    if (ffx_fg) { ffx_shutdown(); sc = nullptr; }   // AMD's swapchain: AMD's code releases it
    if (sc) { sc->Release(); sc = nullptr; }
    if (sl_on) p_slShutdown();
    for (auto &r : br) if (r) { r->Release(); r = nullptr; }
    if (br_react) { br_react->Release(); br_react = nullptr; }
    if (f_game) { f_game->Release(); f_game = nullptr; }
    if (f_sr) { f_sr->Release(); f_sr = nullptr; }
    if (f_done) { f_done->Release(); f_done = nullptr; }
}
