/* AMD FSR backend of the 64-bit helper (ffx12.cpp, FSR_PLAN): FSR upscaling and FSR frame generation through AMD's
   signed FSR SDK DLLs (amd_fidelityfx_loader / upscaler / framegeneration_dx12.dll, SDK 2.4.0, unmodified). */
#pragma once
#include <d3d12.h>
#include <dxgi1_5.h>
#include <stdint.h>
#include "camvp.h"

/* loads amd_fidelityfx_loader_dx12.dll from dir once verify() accepted all three DLLs there */
int ffx_load(const wchar_t *dir, int (*verify)(const wchar_t *path));
/* upscaling: one context for the display size; inputs in D3D12_RESOURCE_STATE_GENERIC_READ, output in UNORDERED_ACCESS */
int ffx_sr_create(ID3D12Device *dev, uint32_t w, uint32_t h);
int ffx_sr_dispatch(ID3D12GraphicsCommandList *cl, ID3D12Resource *color, ID3D12Resource *depth, ID3D12Resource *mvec, ID3D12Resource *react,
                    ID3D12Resource *out,
                    uint32_t rw, uint32_t rh, float jx, float jy, int reset, const CamVp *cam, float dt_ms);
/* frame generation: AMD's swapchain (replaces the DXGI one on hwnd) + the frame generation context */
IDXGISwapChain4 *ffx_fg_create(ID3D12Device *dev, ID3D12CommandQueue *queue, IDXGIFactory *fac, HWND hwnd, uint32_t w, uint32_t h,
                               DXGI_FORMAT bb_fmt, DXGI_FORMAT hud_fmt, uint32_t buffers);
/* per presented frame, before Present, on the present command list: configure (HUD-less image for the UI) + prepare
   (depth and motion vectors, inputs in GENERIC_READ). on = 0: frame generation off for this frame */
int ffx_fg_frame(ID3D12GraphicsCommandList *cl, uint64_t frame_id, int on, ID3D12Resource *hudless, ID3D12Resource *depth,
                 ID3D12Resource *mvec, uint32_t rw, uint32_t rh, float jx, float jy, int reset, const CamVp *cam, float dt_ms);
const char *ffx_sr_version(void);   /* AMD's provider name, e.g. "FSR 3.1.6" (after create) */
const char *ffx_fg_version(void);
void ffx_shutdown(void);
