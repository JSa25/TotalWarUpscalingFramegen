/* P3: Streamline / DLSS-G present path of the 64-bit helper (present12.cpp). */
#pragma once
#include <d3d11.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int  p12_init(ID3D11Device *d11, ID3D11DeviceContext *ctx11, HWND hwnd, uint32_t w, uint32_t h,
              const wchar_t *sl_dir, const wchar_t *log_dir);
void p12_set_camera(const float clip_to_prev[16], const float prev_to_clip[16]);   /* SL convention: row-major, row vectors */
void p12_set_view(const float vp[16]);   /* FSR build only: this frame's view-projection (AMD FSR's camera values) */
void p12_set_mode(uint32_t frames_to_generate);   /* 0 = frame generation off, 1 = 2x, 3 = 4x */
void p12_set_frame_limit(uint32_t us);            /* NVIDIA Reflex frame limit, microseconds per frame; 0 = none */
int  p12_present(uint32_t frame, ID3D11Texture2D *color, ID3D11Texture2D *depth, ID3D11Texture2D *mvec, int reset);
int p12_vrr_flags(void);                         /* G-Sync / VRR of the window's display (NVAPI flags, bit 4 = in VRR mode); -1 unknown */
void p12_set_sr_preset(uint32_t preset);             /* DLSS preset for the next p12_set_sr (NVIDIA's number, 0 default) */
void p12_set_sr(int on, uint32_t render_w, uint32_t render_h, float jitter_sign);   /* DLSS SR (Quality) */
void p12_set_jitter(float x, float y);                 /* this frame's jitter in render pixels (x right, y down) */
void p12_set_dump(uint32_t frame, const char *path);   /* write <path>_out.ppm (and _in.ppm with SR) at that frame */
int  p12_bridge_init(uint32_t luid_lo, int32_t luid_hi, HWND hwnd, uint32_t w, uint32_t h, const wchar_t *const names[9],
                     const wchar_t *sl_dir, const wchar_t *log_dir);   /* names: fences game / SR / done, final, HUD-less, depth, motion vectors, SR output 0 / 1 */
int  p12_bridge_frame(uint32_t frame, int reset, int sr);   /* SR thread: start frame n; with sr, DLSS SR (game 2n+1 -> SR fence n+1) */
void p12_set_fg_test_limit(uint32_t limit);   /* before init: dev test, 0 none, else the GPU is treated as allowing limit - 1 */
uint32_t p12_fg_max(void);                    /* after init: generated frames this GPU allows (0 = no frame generation) */
int  p12_bridge_present(uint32_t frame, int reset, int sr_done, uint32_t in_w, uint32_t in_h);   /* present thread: game 2n+2 -> FG, present fence n+1; in_w x in_h: the frame's depth / motion vector size */
uint32_t p12_shown_total(void);   /* frames shown so far, generated ones included */
void p12_stats(char *buf, size_t len, int reset);
void p12_shutdown(void);
#ifdef __cplusplus
}
#endif
