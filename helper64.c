/* P1 replay helper (64-bit): opens the 32-bit app's shared memory and replays its recorded D3D11 calls on a
   D3D11On12 device (D3D12 underneath, as Streamline needs later). Prints its own timing to stderr at exit.
   Usage (started by rec32.c): helper64.exe <app pid> */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "proto.h"
#include "present12.h"
#include "fgb_proto.h"

static P1Header *hdr;
static uint8_t *ring, *heap, *arena;
static P1Result *results;
static P1Caps *caps;
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;

#define MAX_OBJ (P1_MAX_OBJ - 1)
static IUnknown *obj[MAX_OBJ + 1];
static uint32_t tex_w[MAX_OBJ + 1], tex_h[MAX_OBJ + 1];
/* staging resources the app reads back: layout of their arena copy */
typedef struct { uint32_t kind, fmt, w, h, d, mips, nsub, rb_pos; } ResInfo;   /* kind 1 buffer, 2 tex2d, 3 tex3d */
static ResInfo *rinfo[MAX_OBJ + 1];

static void fail(const char *m, HRESULT hr)
{
    fprintf(stderr, "helper64: %s (0x%08lx)\n", m, (unsigned long)hr);
    if (hdr) hdr->error = 1;
    exit(2);
}
#define CHECK(x, m) do { HRESULT hr_ = (x); if (FAILED(hr_)) fail(m, hr_); } while (0)

static void *at(uint32_t pos) { return heap + pos % P1_HEAP_SIZE; }
static void release_heap(uint32_t pos, uint32_t size)
{
    if (size) __atomic_store_n(&hdr->heap_read, pos + ((size + 15) & ~15u), __ATOMIC_RELEASE);
}
static void *o(uint32_t id) { if (id > MAX_OBJ || (id && !obj[id])) fail("bad object id", id); return id ? obj[id] : NULL; }
static void **oa(const uint32_t *ids, uint32_t n, void **out) { for (uint32_t i = 0; i < n; i++) out[i] = o(ids[i]); return out; }
static void put(uint32_t id, void *p) { if (!id || id > MAX_OBJ) fail("bad new id", id); obj[id] = p; }

static LARGE_INTEGER qpf;
static double busy_ms, gpu_wait_ms, *frame_busy;
/* P3: P3_SL_DIR=<Streamline bind> switches presenting to Streamline / DLSS-G (present12.cpp) */
static int sl_mode;
static uint32_t fg_depth, fg_mvec, sl_frame, sr_color;
static uint32_t nframes, max_frames = 100000;
static ID3D11Query *q[3];
static IDXGISwapChain1 *sc;
static uint32_t sc_bb, presents, occluded, present_fail;
static HRESULT last_fail;
static double present_ms;

static void fetch_backbuffer(void)
{
    ID3D11Texture2D *t;
    CHECK(IDXGISwapChain1_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&t), "GetBuffer");
    put(sc_bb, t);
}
static const char *backend;

/* ---- P1b: rings. The app writes into shadow segments in the arena; at each draw the helper works out which bytes of
   each bound ring the draw reads (vertex / instance range, index range for indexed draws) and uploads only bytes it
   has not uploaded since the ring last moved to a new segment (the app's DISCARD). ---- */
typedef struct { uint32_t size, seg_pos[P1_RING_SEGS], cur, lo, hi; int fresh; } RingSt;
static RingSt *rings[MAX_OBJ + 1];
static uint8_t *ib_cpu[MAX_OBJ + 1];                 /* CPU copy of static index buffers (for index ranges) */
static uint32_t lay_inst[MAX_OBJ + 1], cur_layout;   /* per-instance slots of each input layout (bit per slot) */
static uint32_t vb_id[16], vb_stride[16], vb_off[16], ib_id, ib_fmt, ib_off;
static uint64_t up_bytes, up_maps, idx_scans;

static void ring_ensure(uint32_t id, uint64_t a, uint64_t b)
{
    RingSt *rs = rings[id];
    if (b > rs->size) b = rs->size;
    if (a >= b || (!rs->fresh && a >= rs->lo && b <= rs->hi)) return;
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(ID3D11DeviceContext_Map(ctx, o(id), 0, rs->fresh ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m),
          "ring Map");
    const uint8_t *src = arena + rs->seg_pos[rs->cur];
    if (rs->fresh || b <= rs->lo || a >= rs->hi) {   /* nothing usable uploaded: copy all of [a,b) */
        memcpy((uint8_t *)m.pData + a, src + a, b - a);
        up_bytes += b - a;
        if (rs->fresh || a > rs->hi || b < rs->lo) { rs->lo = (uint32_t)a; rs->hi = (uint32_t)b; }
        else { if (a < rs->lo) rs->lo = (uint32_t)a; if (b > rs->hi) rs->hi = (uint32_t)b; }
    } else {                                         /* overlaps the uploaded interval: copy the parts outside it */
        if (a < rs->lo) { memcpy((uint8_t *)m.pData + a, src + a, rs->lo - a); up_bytes += rs->lo - a; rs->lo = (uint32_t)a; }
        if (b > rs->hi) { memcpy((uint8_t *)m.pData + rs->hi, src + rs->hi, b - rs->hi); up_bytes += b - rs->hi; rs->hi = (uint32_t)b; }
    }
    rs->fresh = 0;
    up_maps++;
    ID3D11DeviceContext_Unmap(ctx, o(id), 0);
}

/* vertex range [v0, v0+vn) for per-vertex slots, instance range [i0, i0+in) for per-instance slots */
static void ring_draw(uint64_t v0, uint64_t vn, uint64_t i0, uint64_t in)
{
    uint32_t inst = lay_inst[cur_layout];
    for (int s = 0; s < 16; s++) {
        uint32_t id = vb_id[s];
        if (!id || !rings[id] || !vb_stride[s]) continue;
        int pi = inst >> s & 1;
        uint64_t first = pi ? i0 : v0, n = pi ? in : vn;
        ring_ensure(id, vb_off[s] + first * vb_stride[s], vb_off[s] + (first + n) * vb_stride[s]);
    }
}

/* min/max index of [si, si+ic) of the bound index buffer; static buffers are cached by range */
static int index_range(uint32_t si, uint32_t ic, uint32_t *mn, uint32_t *mx)
{
    static struct { uint32_t id, off, si, ic, mn, mx; } cache[4096];
    uint32_t isz = ib_fmt == DXGI_FORMAT_R32_UINT ? 4 : 2;
    const uint8_t *src;
    if (rings[ib_id]) {
        ring_ensure(ib_id, ib_off + (uint64_t)si * isz, ib_off + (uint64_t)(si + ic) * isz);
        src = arena + rings[ib_id]->seg_pos[rings[ib_id]->cur];
    } else {
        uint32_t h = (ib_id * 2654435761u ^ ib_off * 40503u ^ si * 2246822519u ^ ic * 3266489917u) & 4095;
        if (cache[h].id == ib_id && cache[h].off == ib_off && cache[h].si == si && cache[h].ic == ic) {
            *mn = cache[h].mn; *mx = cache[h].mx; return 1;
        }
        if (!(src = ib_cpu[ib_id])) return 0;
        uint32_t a = 0xFFFFFFFF, b = 0;
        const uint8_t *q = src + ib_off + (size_t)si * isz;
        for (uint32_t i = 0; i < ic; i++) { uint32_t v = isz == 4 ? ((const uint32_t *)q)[i] : ((const uint16_t *)q)[i]; if (v < a) a = v; if (v > b) b = v; }
        cache[h].id = ib_id; cache[h].off = ib_off; cache[h].si = si; cache[h].ic = ic; cache[h].mn = a; cache[h].mx = b;
        *mn = a; *mx = b; idx_scans++;
        return ic > 0;
    }
    uint32_t a = 0xFFFFFFFF, b = 0;
    const uint8_t *q = src + ib_off + (size_t)si * isz;
    for (uint32_t i = 0; i < ic; i++) { uint32_t v = isz == 4 ? ((const uint32_t *)q)[i] : ((const uint16_t *)q)[i]; if (v < a) a = v; if (v > b) b = v; }
    *mn = a; *mx = b; idx_scans++;
    return ic > 0;
}

static void ring_draw_indexed(uint32_t ic, uint32_t si, int32_t bv, uint32_t inst, uint32_t sinst)
{
    int any = 0;
    for (int s = 0; s < 16; s++) if (vb_id[s] && rings[vb_id[s]]) any = 1;
    if (!any && !rings[ib_id]) return;
    uint32_t mn, mx;
    if (!index_range(si, ic, &mn, &mx)) return;
    ring_draw((uint64_t)((int64_t)bv + mn), (uint64_t)mx - mn + 1, sinst, inst);
}

/* ---- queries and staging readbacks: results go to the shared results table, the app polls them ---- */
typedef struct { uint32_t id, seq; } Pending;
static Pending pend_q[4096], pend_rb[256];
static uint32_t npend_q, npend_rb;

static void poll_results(void)
{
    for (uint32_t i = 0; i < npend_q;) {
        P1Result *r = &results[pend_q[i].id];
        ID3D11Asynchronous *a = (ID3D11Asynchronous *)obj[pend_q[i].id];
        UINT size = a ? ID3D11Asynchronous_GetDataSize(a) : 0;
        HRESULT hr = a ? ID3D11DeviceContext_GetData(ctx, a, r->data, size, D3D11_ASYNC_GETDATA_DONOTFLUSH) : S_OK;
        if (hr == S_FALSE) { i++; continue; }
        r->size = size;
        __atomic_store_n(&r->seq, pend_q[i].seq, __ATOMIC_RELEASE);
        pend_q[i] = pend_q[--npend_q];
    }
    for (uint32_t i = 0; i < npend_rb;) {
        ResInfo *ri = rinfo[pend_rb[i].id];
        ID3D11Resource *res = (ID3D11Resource *)obj[pend_rb[i].id];
        if (!ri || !res) { pend_rb[i] = pend_rb[--npend_rb]; continue; }
        D3D11_MAPPED_SUBRESOURCE m;
        if (ID3D11DeviceContext_Map(ctx, res, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == DXGI_ERROR_WAS_STILL_DRAWING) { i++; continue; }
        ID3D11DeviceContext_Unmap(ctx, res, 0);
        uint8_t *dst = arena + ri->rb_pos;
        for (uint32_t sub = 0; sub < ri->nsub; sub++) {   /* tightly packed copy of every subresource */
            uint32_t mip = sub % ri->mips, w = ri->w >> mip ? ri->w >> mip : 1, h = ri->h >> mip ? ri->h >> mip : 1;
            uint32_t dd = ri->d >> mip ? ri->d >> mip : 1, row, rows;
            if (ri->kind == 1) { row = ri->w; rows = 1; dd = 1; } else p1_mip_layout(ri->fmt, w, h, &row, &rows);
            CHECK(ID3D11DeviceContext_Map(ctx, res, sub, D3D11_MAP_READ, 0, &m), "readback Map");
            for (uint32_t z = 0; z < dd; z++)
                for (uint32_t y = 0; y < rows; y++)
                    memcpy(dst + (z * rows + y) * row, (uint8_t *)m.pData + (size_t)z * m.DepthPitch + (size_t)y * m.RowPitch, row);
            ID3D11DeviceContext_Unmap(ctx, res, sub);
            dst += row * rows * dd;
        }
        __atomic_store_n(&results[pend_rb[i].id].seq, pend_rb[i].seq, __ATOMIC_RELEASE);
        pend_rb[i] = pend_rb[--npend_rb];
    }
}
static void readback_after_copy(uint32_t id, uint32_t seq)
{
    if (!seq) return;
    for (uint32_t i = 0; i < npend_rb; i++) if (pend_rb[i].id == id) { pend_rb[i].seq = seq; return; }
    if (npend_rb == 256) fail("too many pending readbacks", 0);
    pend_rb[npend_rb++] = (Pending){ id, seq };
}

static void set_stage(uint32_t op, uint32_t stage, uint32_t start, uint32_t n, void **v)
{
    if (op == OP_SET_CB) {
        ID3D11Buffer **b = (ID3D11Buffer **)v;
        switch (stage) {
        case STAGE_VS: ID3D11DeviceContext_VSSetConstantBuffers(ctx, start, n, b); break;
        case STAGE_PS: ID3D11DeviceContext_PSSetConstantBuffers(ctx, start, n, b); break;
        case STAGE_GS: ID3D11DeviceContext_GSSetConstantBuffers(ctx, start, n, b); break;
        case STAGE_HS: ID3D11DeviceContext_HSSetConstantBuffers(ctx, start, n, b); break;
        case STAGE_DS: ID3D11DeviceContext_DSSetConstantBuffers(ctx, start, n, b); break;
        default: ID3D11DeviceContext_CSSetConstantBuffers(ctx, start, n, b);
        }
    } else if (op == OP_SET_SRV) {
        ID3D11ShaderResourceView **b = (ID3D11ShaderResourceView **)v;
        switch (stage) {
        case STAGE_VS: ID3D11DeviceContext_VSSetShaderResources(ctx, start, n, b); break;
        case STAGE_PS: ID3D11DeviceContext_PSSetShaderResources(ctx, start, n, b); break;
        case STAGE_GS: ID3D11DeviceContext_GSSetShaderResources(ctx, start, n, b); break;
        case STAGE_HS: ID3D11DeviceContext_HSSetShaderResources(ctx, start, n, b); break;
        case STAGE_DS: ID3D11DeviceContext_DSSetShaderResources(ctx, start, n, b); break;
        default: ID3D11DeviceContext_CSSetShaderResources(ctx, start, n, b);
        }
    } else {
        ID3D11SamplerState **b = (ID3D11SamplerState **)v;
        switch (stage) {
        case STAGE_VS: ID3D11DeviceContext_VSSetSamplers(ctx, start, n, b); break;
        case STAGE_PS: ID3D11DeviceContext_PSSetSamplers(ctx, start, n, b); break;
        case STAGE_GS: ID3D11DeviceContext_GSSetSamplers(ctx, start, n, b); break;
        case STAGE_HS: ID3D11DeviceContext_HSSetSamplers(ctx, start, n, b); break;
        case STAGE_DS: ID3D11DeviceContext_DSSetSamplers(ctx, start, n, b); break;
        default: ID3D11DeviceContext_CSSetSamplers(ctx, start, n, b);
        }
    }
}

static void fill_caps(void)
{
    caps->feature_level = ID3D11Device_GetFeatureLevel(dev);
    caps->creation_flags = 0;   /* what the app asked for (the proof apps ask for none) */
    for (uint32_t f = 0; f < P1_NFMT; f++) {
        UINT s = 0;
        if (SUCCEEDED(ID3D11Device_CheckFormatSupport(dev, f, &s))) caps->fmt[f].support = s;
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s2 = { f, 0 };
        if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_FORMAT_SUPPORT2, &s2, sizeof s2))) caps->fmt[f].support2 = s2.OutFormatSupport2;
        static const UINT counts[4] = { 1, 2, 4, 8 };
        for (int i = 0; i < 4; i++) {
            UINT q = 0;
            if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(dev, f, counts[i], &q))) caps->fmt[f].msaa[i] = q;
        }
    }
    static const UINT sizes[16] = { sizeof(D3D11_FEATURE_DATA_THREADING), sizeof(D3D11_FEATURE_DATA_DOUBLES), 0, 0,
        sizeof(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS), sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS),
        sizeof(D3D11_FEATURE_DATA_ARCHITECTURE_INFO), sizeof(D3D11_FEATURE_DATA_D3D9_OPTIONS),
        sizeof(D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT), sizeof(D3D11_FEATURE_DATA_D3D9_SHADOW_SUPPORT),
        sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS1), sizeof(D3D11_FEATURE_DATA_D3D9_SIMPLE_INSTANCING_SUPPORT),
        sizeof(D3D11_FEATURE_DATA_MARKER_SUPPORT), sizeof(D3D11_FEATURE_DATA_D3D9_OPTIONS1),
        sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS2), sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS3) };
    for (uint32_t f = 0; f < 16; f++) {
        if (!sizes[f] || sizes[f] > sizeof caps->feature[f].data) continue;
        if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(dev, f, caps->feature[f].data, sizes[f]))) {
            caps->feature[f].ok = 1; caps->feature[f].size = sizes[f];
        }
    }
    __atomic_store_n(&hdr->caps_ready, 1, __ATOMIC_RELEASE);
}

static void replay(const uint32_t *r)
{
    const uint32_t *p = r + 2;
    switch (r[0]) {
    case OP_PAD: break;
    case OP_CREATE_BUFFER: {
        D3D11_BUFFER_DESC d = { p[1], p[2], p[3], p[4], p[5], p[6] };
        D3D11_SUBRESOURCE_DATA s = { at(p[7]), 0, 0 };
        ID3D11Buffer *b;
        CHECK(ID3D11Device_CreateBuffer(dev, &d, p[8] ? &s : NULL, &b), "CreateBuffer");
        if (p[9]) {   /* ring: data comes per draw from its shadow segments */
            RingSt *rs = calloc(1, sizeof *rs);
            rs->size = p[1]; memcpy(rs->seg_pos, p + 10, sizeof rs->seg_pos); rs->fresh = 1;
            rings[p[0]] = rs;
        } else if ((p[3] & D3D11_BIND_INDEX_BUFFER) && p[8]) {
            ib_cpu[p[0]] = malloc(p[8]); memcpy(ib_cpu[p[0]], at(p[7]), p[8]);
        }
        if (p[10 + P1_RING_SEGS] != 0xFFFFFFFF) {
            ResInfo *ri = calloc(1, sizeof *ri);
            *ri = (ResInfo){ 1, 0, p[1], 1, 1, 1, 1, p[10 + P1_RING_SEGS] };
            free(rinfo[p[0]]); rinfo[p[0]] = ri;
        }
        put(p[0], b); release_heap(p[7], p[8]);
        break; }
    case OP_CREATE_TEX2D: {
        D3D11_TEXTURE2D_DESC d = { p[1], p[2], p[3], p[4], p[5], { p[6], p[7] }, p[8], p[9], p[10], p[11] };
        uint32_t nsub = p[13];
        const P1Sub *sb = (const P1Sub *)(p + 14);
        D3D11_SUBRESOURCE_DATA s[256];
        if (nsub > 256) fail("too many subresources", nsub);
        for (uint32_t i = 0; i < nsub; i++) s[i] = (D3D11_SUBRESOURCE_DATA){ at(sb[i].pos), sb[i].row_pitch, sb[i].depth_pitch };
        /* shortcut: P3 shares every render target with D3D12; the game version shares only the tagged ones */
        if (sl_mode && (d.BindFlags & D3D11_BIND_RENDER_TARGET))
            d.MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        ID3D11Texture2D *t;
        CHECK(ID3D11Device_CreateTexture2D(dev, &d, nsub ? s : NULL, &t), "CreateTexture2D");
        for (uint32_t i = 0; i < nsub; i++) release_heap(sb[i].pos, sb[i].size);
        if (p[12] != 0xFFFFFFFF) {
            ResInfo *ri = calloc(1, sizeof *ri);
            *ri = (ResInfo){ 2, p[5], p[1], p[2], 1, p[3], p[3] * p[4], p[12] };
            free(rinfo[p[0]]); rinfo[p[0]] = ri;
        }
        put(p[0], t); tex_w[p[0]] = p[1]; tex_h[p[0]] = p[2];
        break; }
    case OP_CREATE_TEX3D: {
        D3D11_TEXTURE3D_DESC d = { p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9] };
        uint32_t nsub = p[11];
        const P1Sub *sb = (const P1Sub *)(p + 12);
        D3D11_SUBRESOURCE_DATA s[32];
        if (nsub > 32) fail("too many 3D subresources", nsub);
        for (uint32_t i = 0; i < nsub; i++) s[i] = (D3D11_SUBRESOURCE_DATA){ at(sb[i].pos), sb[i].row_pitch, sb[i].depth_pitch };
        ID3D11Texture3D *t;
        CHECK(ID3D11Device_CreateTexture3D(dev, &d, nsub ? s : NULL, &t), "CreateTexture3D");
        for (uint32_t i = 0; i < nsub; i++) release_heap(sb[i].pos, sb[i].size);
        put(p[0], t);
        break; }
    case OP_CREATE_STATE: {
        void *v;
        switch (p[1]) {
        case STATE_BLEND: CHECK(ID3D11Device_CreateBlendState(dev, (const D3D11_BLEND_DESC *)(p + 2), (ID3D11BlendState **)&v), "CreateBlendState"); break;
        case STATE_DSS: CHECK(ID3D11Device_CreateDepthStencilState(dev, (const D3D11_DEPTH_STENCIL_DESC *)(p + 2), (ID3D11DepthStencilState **)&v), "CreateDepthStencilState"); break;
        case STATE_RS: CHECK(ID3D11Device_CreateRasterizerState(dev, (const D3D11_RASTERIZER_DESC *)(p + 2), (ID3D11RasterizerState **)&v), "CreateRasterizerState"); break;
        default: CHECK(ID3D11Device_CreateSamplerState(dev, (const D3D11_SAMPLER_DESC *)(p + 2), (ID3D11SamplerState **)&v), "CreateSamplerState");
        }
        put(p[0], v);
        break; }
    case OP_CREATE_QUERY: {
        D3D11_QUERY_DESC d = { p[1], p[2] };
        void *v;
        if (d.Query == D3D11_QUERY_OCCLUSION_PREDICATE || d.Query == D3D11_QUERY_SO_OVERFLOW_PREDICATE)
            CHECK(ID3D11Device_CreatePredicate(dev, &d, (ID3D11Predicate **)&v), "CreatePredicate");
        else CHECK(ID3D11Device_CreateQuery(dev, &d, (ID3D11Query **)&v), "CreateQuery");
        put(p[0], v);
        break; }
    case OP_CREATE_VIEW: {
        void *v;
        ID3D11Resource *res = o(p[2]);
        const void *dd = p[3] ? (const void *)(p + 4) : NULL;
        if (p[1] == VIEW_RTV) CHECK(ID3D11Device_CreateRenderTargetView(dev, res, dd, (ID3D11RenderTargetView **)&v), "RTV");
        else if (p[1] == VIEW_DSV) CHECK(ID3D11Device_CreateDepthStencilView(dev, res, dd, (ID3D11DepthStencilView **)&v), "DSV");
        else if (p[1] == VIEW_UAV) CHECK(ID3D11Device_CreateUnorderedAccessView(dev, res, dd, (ID3D11UnorderedAccessView **)&v), "UAV");
        else CHECK(ID3D11Device_CreateShaderResourceView(dev, res, dd, (ID3D11ShaderResourceView **)&v), "SRV");
        put(p[0], v);
        break; }
    case OP_CREATE_SHADER: {
        void *s;
        switch (p[1]) {
        case STAGE_VS: CHECK(ID3D11Device_CreateVertexShader(dev, at(p[2]), p[3], NULL, (ID3D11VertexShader **)&s), "VS"); break;
        case STAGE_PS: CHECK(ID3D11Device_CreatePixelShader(dev, at(p[2]), p[3], NULL, (ID3D11PixelShader **)&s), "PS"); break;
        case STAGE_GS: CHECK(ID3D11Device_CreateGeometryShader(dev, at(p[2]), p[3], NULL, (ID3D11GeometryShader **)&s), "GS"); break;
        case STAGE_HS: CHECK(ID3D11Device_CreateHullShader(dev, at(p[2]), p[3], NULL, (ID3D11HullShader **)&s), "HS"); break;
        case STAGE_DS: CHECK(ID3D11Device_CreateDomainShader(dev, at(p[2]), p[3], NULL, (ID3D11DomainShader **)&s), "DS"); break;
        default: CHECK(ID3D11Device_CreateComputeShader(dev, at(p[2]), p[3], NULL, (ID3D11ComputeShader **)&s), "CS");
        }
        put(p[0], s); release_heap(p[2], p[3]);
        break; }
    case OP_CREATE_LAYOUT: {
        const P1Elem *pe = (const P1Elem *)(p + 4);
        D3D11_INPUT_ELEMENT_DESC e[32];
        if (p[1] > 32) fail("too many layout elements", p[1]);
        for (uint32_t i = 0; i < p[1]; i++)
            e[i] = (D3D11_INPUT_ELEMENT_DESC){ pe[i].name, pe[i].index, pe[i].format, pe[i].slot, pe[i].offset,
                                               pe[i].slot_class, pe[i].step_rate };
        ID3D11InputLayout *l;
        CHECK(ID3D11Device_CreateInputLayout(dev, e, p[1], at(p[2]), p[3], &l), "CreateInputLayout");
        for (uint32_t i = 0; i < p[1]; i++)
            if (pe[i].slot_class == D3D11_INPUT_PER_INSTANCE_DATA && pe[i].slot < 32) lay_inst[p[0]] |= 1u << pe[i].slot;
        put(p[0], l); release_heap(p[2], p[3]);
        break; }
    case OP_CLEAR_RTV: ID3D11DeviceContext_ClearRenderTargetView(ctx, o(p[0]), (const float *)(p + 1)); break;
    case OP_CLEAR_DSV: { float d; memcpy(&d, p + 2, 4);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, o(p[0]), p[1], d, (UINT8)p[3]); break; }
    case OP_SET_RT: {
        ID3D11RenderTargetView *v[8];
        if (p[0] > 8) fail("too many RTs", p[0]);
        for (uint32_t i = 0; i < p[0]; i++) v[i] = o(p[2 + i]);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, p[0], v, o(p[1]));
        break; }
    case OP_SET_VP: ID3D11DeviceContext_RSSetViewports(ctx, p[0], (const D3D11_VIEWPORT *)(p + 1)); break;
    case OP_SET_LAYOUT: cur_layout = p[0]; ID3D11DeviceContext_IASetInputLayout(ctx, o(p[0])); break;
    case OP_SET_TOPO: ID3D11DeviceContext_IASetPrimitiveTopology(ctx, p[0]); break;
    case OP_SET_VB: {
        ID3D11Buffer *b[16]; UINT st[16], of[16];
        if (p[1] > 16) fail("too many VBs", p[1]);
        for (uint32_t i = 0; i < p[1]; i++) { b[i] = o(p[2 + i * 3]); st[i] = p[3 + i * 3]; of[i] = p[4 + i * 3]; }
        for (uint32_t i = 0; i < p[1] && p[0] + i < 16; i++) {
            vb_id[p[0] + i] = p[2 + i * 3]; vb_stride[p[0] + i] = st[i]; vb_off[p[0] + i] = of[i];
        }
        ID3D11DeviceContext_IASetVertexBuffers(ctx, p[0], p[1], b, st, of);
        break; }
    case OP_SET_IB: ib_id = p[0]; ib_fmt = p[1]; ib_off = p[2]; ID3D11DeviceContext_IASetIndexBuffer(ctx, o(p[0]), p[1], p[2]); break;
    case OP_SET_SHADER:
        switch (p[0]) {
        case STAGE_VS: ID3D11DeviceContext_VSSetShader(ctx, o(p[1]), NULL, 0); break;
        case STAGE_PS: ID3D11DeviceContext_PSSetShader(ctx, o(p[1]), NULL, 0); break;
        case STAGE_GS: ID3D11DeviceContext_GSSetShader(ctx, o(p[1]), NULL, 0); break;
        case STAGE_HS: ID3D11DeviceContext_HSSetShader(ctx, o(p[1]), NULL, 0); break;
        case STAGE_DS: ID3D11DeviceContext_DSSetShader(ctx, o(p[1]), NULL, 0); break;
        default: ID3D11DeviceContext_CSSetShader(ctx, o(p[1]), NULL, 0);
        }
        break;
    case OP_SET_CB: case OP_SET_SRV: case OP_SET_SMP: {
        void *v[128];
        if (p[2] > 128) fail("too many bindings", p[2]);
        set_stage(r[0], p[0], p[1], p[2], oa(p + 3, p[2], v));
        break; }
    case OP_SET_UAV_CS: {
        void *v[64]; UINT c[64];
        if (p[1] > 64) fail("too many UAVs", p[1]);
        oa(p + 2, p[1], v);
        for (uint32_t i = 0; i < p[1]; i++) c[i] = p[2 + p[1] + i];
        ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, p[0], p[1], (ID3D11UnorderedAccessView **)v, c);
        break; }
    case OP_SET_RT_UAV: {
        uint32_t nrt = p[0] == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL ? 0 : p[0];
        uint32_t nu = p[3] == D3D11_KEEP_UNORDERED_ACCESS_VIEWS ? 0 : p[3];
        void *rt[8], *u[64]; UINT c[64];
        if (nrt > 8 || nu > 64) fail("OMSetRenderTargetsAndUnorderedAccessViews out of range", 0);
        oa(p + 4, nrt, rt); oa(p + 4 + nrt, nu, u);
        for (uint32_t i = 0; i < nu; i++) c[i] = p[4 + nrt + nu + i];
        ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(ctx, p[0], (ID3D11RenderTargetView **)rt, o(p[1]), p[2], p[3],
                                                                       (ID3D11UnorderedAccessView **)u, c);
        break; }
    case OP_SET_SC: ID3D11DeviceContext_RSSetScissorRects(ctx, p[0], (const D3D11_RECT *)(p + 1)); break;
    case OP_SET_RS: ID3D11DeviceContext_RSSetState(ctx, o(p[0])); break;
    case OP_SET_BLEND: ID3D11DeviceContext_OMSetBlendState(ctx, o(p[0]), (const float *)(p + 1), p[5]); break;
    case OP_SET_DSS: ID3D11DeviceContext_OMSetDepthStencilState(ctx, o(p[0]), p[1]); break;
    case OP_SET_SO: {
        void *v[4];
        if (p[0] > 4) fail("too many SO targets", p[0]);
        ID3D11DeviceContext_SOSetTargets(ctx, p[0], (ID3D11Buffer **)oa(p + 1, p[0], v), p + 1 + p[0]);
        break; }
    case OP_SET_PRED: ID3D11DeviceContext_SetPredication(ctx, o(p[0]), p[1]); break;
    case OP_CLEAR_STATE:
        ID3D11DeviceContext_ClearState(ctx);
        memset(vb_id, 0, sizeof vb_id); ib_id = 0; cur_layout = 0;
        break;
    case OP_DISPATCH: ID3D11DeviceContext_Dispatch(ctx, p[0], p[1], p[2]); break;
    case OP_COPY_RES:
        ID3D11DeviceContext_CopyResource(ctx, o(p[0]), o(p[1]));
        readback_after_copy(p[0], p[2]);
        break;
    case OP_COPY_SUB:
        ID3D11DeviceContext_CopySubresourceRegion(ctx, o(p[0]), p[1], p[2], p[3], p[4], o(p[5]), p[6],
                                                  p[7] ? (const D3D11_BOX *)(p + 8) : NULL);
        readback_after_copy(p[0], p[14]);
        break;
    case OP_UPDATE_SUB: {
        const P1Sub *sb = (const P1Sub *)(p + 9);
        ID3D11DeviceContext_UpdateSubresource(ctx, o(p[0]), p[1], p[2] ? (const D3D11_BOX *)(p + 3) : NULL, at(sb->pos),
                                              sb->row_pitch, sb->depth_pitch);
        release_heap(sb->pos, sb->size);
        break; }
    case OP_BEGIN: ID3D11DeviceContext_Begin(ctx, o(p[0])); break;
    case OP_END:
        ID3D11DeviceContext_End(ctx, o(p[0]));
        for (uint32_t i = 0; i < npend_q; i++) if (pend_q[i].id == p[0]) { pend_q[i] = pend_q[--npend_q]; break; }
        if (npend_q == 4096) fail("too many pending queries", 0);
        pend_q[npend_q++] = (Pending){ p[0], p[1] };
        break;
    case OP_FLUSH: ID3D11DeviceContext_Flush(ctx); break;
    case OP_UPDATE: {   /* id, sub, map type, P1Sub: data of a Map(WRITE_DISCARD / WRITE) the app wrote into the heap */
        ID3D11Resource *res = o(p[0]);
        const P1Sub *sb = (const P1Sub *)(p + 3);
        D3D11_MAPPED_SUBRESOURCE m;
        CHECK(ID3D11DeviceContext_Map(ctx, res, p[1], p[2], 0, &m), "Map");
        const uint8_t *src = at(sb->pos);
        if (sb->size == sb->row_pitch || (m.RowPitch == sb->row_pitch && m.DepthPitch == sb->depth_pitch)) memcpy(m.pData, src, sb->size);
        else {
            uint32_t rows = sb->depth_pitch / sb->row_pitch, slices = sb->size / sb->depth_pitch;
            for (uint32_t z = 0; z < slices; z++)
                for (uint32_t y = 0; y < rows; y++)
                    memcpy((uint8_t *)m.pData + (size_t)z * m.DepthPitch + (size_t)y * m.RowPitch,
                           src + (size_t)z * sb->depth_pitch + (size_t)y * sb->row_pitch, sb->row_pitch);
        }
        ID3D11DeviceContext_Unmap(ctx, res, p[1]);
        release_heap(sb->pos, sb->size);
        break; }
    case OP_DRAW_II:
        ring_draw_indexed(p[0], p[2], (int32_t)p[3], p[1], p[4]);
        ID3D11DeviceContext_DrawIndexedInstanced(ctx, p[0], p[1], p[2], (INT)p[3], p[4]);
        break;
    case OP_DRAW_IDX:
        ring_draw_indexed(p[0], p[1], (int32_t)p[2], 1, 0);
        ID3D11DeviceContext_DrawIndexed(ctx, p[0], p[1], (INT)p[2]);
        break;
    case OP_DRAW:
        ring_draw(p[1], p[0], 0, 1);
        ID3D11DeviceContext_Draw(ctx, p[0], p[1]);
        break;
    case OP_RING_SEG: {
        RingSt *rs = rings[p[0]];
        if (!rs) fail("RING_SEG for a non-ring buffer", p[0]);
        rs->cur = p[1]; rs->fresh = 1; rs->lo = rs->hi = 0;
        break; }
    case OP_READBACK: {
        ID3D11Texture2D *src = o(p[0]), *st;
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(src, &d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        CHECK(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st), "staging");
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
        D3D11_MAPPED_SUBRESOURCE m;
        CHECK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m), "readback Map");
        uint32_t row = d.Width * 4;
        for (uint32_t y = 0; y < d.Height; y++)
            memcpy((uint8_t *)at(p[1]) + (size_t)y * row, (uint8_t *)m.pData + (size_t)y * m.RowPitch, row);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        ID3D11Texture2D_Release(st);
        release_heap(p[1], p[2]);
        __atomic_store_n(&hdr->readback_done, 1, __ATOMIC_RELEASE);
        break; }
    case OP_CREATE_SWAPCHAIN: {
        if (sl_mode) {
            /* the app's back buffer is a shared D3D11 texture; Streamline's D3D12 swapchain presents it */
            D3D11_TEXTURE2D_DESC td = { p[3], p[4], 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0,
                                        D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE };
            ID3D11Texture2D *t;
            CHECK(ID3D11Device_CreateTexture2D(dev, &td, NULL, &t), "shared back buffer");
            put(p[1], t); sc_bb = p[1];
            wchar_t dir[MAX_PATH], logs[MAX_PATH];
            GetEnvironmentVariableW(L"P3_SL_DIR", dir, MAX_PATH);
            if (!GetEnvironmentVariableW(L"P3_LOG_DIR", logs, MAX_PATH)) logs[0] = 0;
            if (!p12_init(dev, ctx, (HWND)(uintptr_t)p[2], p[3], p[4], dir, logs[0] ? logs : NULL)) fail("p12_init", 0);
            char dump[MAX_PATH], df[16];
            if (GetEnvironmentVariableA("P3_DUMP_PATH", dump, MAX_PATH) && GetEnvironmentVariableA("P3_DUMP_FRAME", df, 16))
                p12_set_dump((uint32_t)atoi(df), dump);
            break;
        }
        /* option 1 of DLSS_PLAN 1.3: our own flip-model swapchain on the 32-bit app's window */
        IDXGIDevice *xd; IDXGIAdapter *ad; IDXGIFactory2 *fac;
        CHECK(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xd), "IDXGIDevice");
        CHECK(IDXGIDevice_GetAdapter(xd, &ad), "GetAdapter");
        CHECK(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac), "IDXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 d = { p[3], p[4], DXGI_FORMAT_B8G8R8A8_UNORM, FALSE, { 1, 0 },
                                    DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, DXGI_SCALING_STRETCH,
                                    DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED, 0 };
        HWND hwnd = (HWND)(uintptr_t)p[2];
        CHECK(IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)dev, hwnd, &d, NULL, NULL, &sc),
              "CreateSwapChainForHwnd on the app's window");
        IDXGIFactory2_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
        put(p[0], sc); sc_bb = p[1];
        fetch_backbuffer();
        break; }
    case OP_PRESENT: {
        LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
        if (sl_mode) {
            if (!p12_present(sl_frame, o(sr_color ? sr_color : sc_bb), o(fg_depth), o(fg_mvec), sl_frame == 0)) fail("p12_present", 0);
            sl_frame++;
            QueryPerformanceCounter(&b);
            present_ms += (b.QuadPart - a.QuadPart) * 1000.0 / qpf.QuadPart;
            presents++;
            break;
        }
        HRESULT hr = IDXGISwapChain1_Present(sc, p[1], p[2]);
        QueryPerformanceCounter(&b);
        present_ms += (b.QuadPart - a.QuadPart) * 1000.0 / qpf.QuadPart;
        presents++;
        if (hr == DXGI_STATUS_OCCLUDED) occluded++;
        else if (FAILED(hr)) { present_fail++; last_fail = hr; }
        break; }
    case OP_RESIZE:
        if (sl_mode) fail("resize is not implemented in the Streamline path (P3 proof)", 0);
        ID3D11Texture2D_Release((ID3D11Texture2D *)obj[sc_bb]); obj[sc_bb] = NULL;
        ID3D11DeviceContext_ClearState(ctx);   /* the app rebinds everything after a resize */
        ID3D11DeviceContext_Flush(ctx);
        CHECK(IDXGISwapChain1_ResizeBuffers(sc, 0, p[1], p[2], DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
        fetch_backbuffer();
        break;
    case OP_RELEASE:
        IUnknown_Release((IUnknown *)o(p[0])); obj[p[0]] = NULL;
        free(rinfo[p[0]]); rinfo[p[0]] = NULL;
        if (rings[p[0]]) { free(rings[p[0]]); rings[p[0]] = NULL; }
        if (ib_cpu[p[0]]) { free(ib_cpu[p[0]]); ib_cpu[p[0]] = NULL; }
        lay_inst[p[0]] = 0;
        break;
    case OP_FG_TAGS: fg_depth = p[0]; fg_mvec = p[1]; break;
    case OP_SR_CONFIG: {
        float sign; memcpy(&sign, p + 4, 4);
        sr_color = p[3];
        if (sl_mode) p12_set_sr(p[0], p[1], p[2], sign);
        break; }
    case OP_FRAME_INFO: {
        float jx, jy; memcpy(&jx, p, 4); memcpy(&jy, p + 1, 4);
        if (sl_mode) p12_set_jitter(jx, jy);
        break; }
    case OP_FG_MODE:
        if (sl_mode) {
            char st[512];
            p12_stats(st, sizeof st, 1);
            fprintf(stderr, "  [phase end] %s\n", st);
            p12_set_mode(p[0]);
        }
        break;
    default: fail("unknown op", r[0]);
    }
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

/* ---- frame bridge (plan B): no command replay; the game shares its finished textures and a fence by name ---- */
static int inverse4(const float *m, float *out)   /* Gauss-Jordan, row-major */
{
    float a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabsf(a[r][c]) > fabsf(a[p][c])) p = r;
        if (fabsf(a[p][c]) < 1e-12f) return 0;
        for (int k = 0; k < 8; k++) { float t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        float d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            float f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][c + 4];
    return 1;
}

/* the game's matrices are column-vector (clip = M x p); SL wants row vectors: clipToPrevClip = (prev_vp x inv(vp))^T */
static void bridge_camera(const float *vp, const float *prev_vp, int check)
{
    float iv[16], ip[16], c2p[16], p2c[16], t1[16], t2[16];
    if (!inverse4(vp, iv) || !inverse4(prev_vp, ip)) return;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float a = 0, b = 0;
            for (int k = 0; k < 4; k++) { a += prev_vp[i * 4 + k] * iv[k * 4 + j]; b += vp[i * 4 + k] * ip[k * 4 + j]; }
            t1[i * 4 + j] = a; t2[i * 4 + j] = b;
        }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) { c2p[i * 4 + j] = t1[j * 4 + i]; p2c[i * 4 + j] = t2[j * 4 + i]; }
    p12_set_camera(c2p, p2c);
    if (check) {   /* self-check: a world point's clip position, as a row vector times clipToPrevClip, lands on its previous clip position */
        const float w[4] = { 3.5f, -1, 27, 1 };
        float c[4], p[4], q[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 4; i++) {
            c[i] = vp[i * 4] * w[0] + vp[i * 4 + 1] * w[1] + vp[i * 4 + 2] * w[2] + vp[i * 4 + 3];
            p[i] = prev_vp[i * 4] * w[0] + prev_vp[i * 4 + 1] * w[1] + prev_vp[i * 4 + 2] * w[2] + prev_vp[i * 4 + 3];
        }
        for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) q[j] += c[k] * c2p[k * 4 + j];
        fprintf(stderr, "  bridge camera check: previous clip (%.4f %.4f %.4f %.4f), via clipToPrevClip (%.4f %.4f %.4f %.4f)\n",
                p[0], p[1], p[2], p[3], q[0], q[1], q[2], q[3]);
    }
}

/* bridge: two threads. The SR thread (main) starts every frame as soon as the game publishes it (mid-frame SR inputs, or
   the final image when the frame has no HUD-less point) and runs DLSS SR on its own queue; the present thread does frame
   generation and Present, which can block on display pacing without delaying the next frame's SR (AGENT_LOG 96). */
static FgbShared *bsh;
static HANDLE bwake, bwake2, bgame;   /* auto-reset events: one per thread, the game sets both */
static volatile uint32_t b_started;   /* SR thread: frames started (p12_bridge_frame done for frame n when > n) */
static volatile uint32_t b_quit;
static uint8_t b_frame_sr[8];         /* frame n had DLSS SR (index n % 8) */
static uint32_t b_frame_iw[8], b_frame_ih[8];   /* frame n's depth / motion vector size (render size with SR on) */
static double b_sr_cpu_ms;            /* SR thread CPU from picking up an SR request to submitting it */
static unsigned b_sr_cpu_n;

static DWORD WINAPI bridge_present_thread(void *arg)
{
    (void)arg;
    uint32_t fg = 0xFFFFFFFF, limit = 0;
    int vrr = -2;
    LARGE_INTEGER last_report = { 0 }, last_vrr = { 0 };
    for (uint32_t n = 0;; n++) {
        uint32_t f;
        for (int i = 0; __atomic_load_n(&b_started, __ATOMIC_ACQUIRE) <= n || (f = __atomic_load_n(&bsh->frames, __ATOMIC_ACQUIRE)) <= n ||
                        f == FGB_QUIT; i++) {
            if (b_quit) return 0;
            if (i < 2000) __builtin_ia32_pause(); else WaitForSingleObject(bwake2, 1);
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (bsh->fg_frames == fg && now.QuadPart - last_report.QuadPart > 30 * qpf.QuadPart) {
            char st[512];
            p12_stats(st, sizeof st, 1);
            fprintf(stderr, "  [30 s] %s, SR submit %.3f ms CPU (%u)\n", st, b_sr_cpu_n ? b_sr_cpu_ms / b_sr_cpu_n : 0, b_sr_cpu_n);
            b_sr_cpu_ms = 0; b_sr_cpu_n = 0;
            last_report = now;
        }
        if (bsh->fg_frames != fg) {
            if (fg != 0xFFFFFFFF) {
                char st[512];
                p12_stats(st, sizeof st, 1);
                fprintf(stderr, "  [phase end] %s, SR submit %.3f ms CPU (%u)\n", st, b_sr_cpu_n ? b_sr_cpu_ms / b_sr_cpu_n : 0, b_sr_cpu_n);
                b_sr_cpu_ms = 0; b_sr_cpu_n = 0;
            }
            QueryPerformanceCounter(&last_report);
            fg = bsh->fg_frames;
            p12_set_mode(fg);
        }
        if (now.QuadPart - last_vrr.QuadPart > qpf.QuadPart) {   /* G-Sync on the game's display, for bug reports only */
            int v = p12_vrr_flags();
            if (v != vrr) {
                vrr = v;
                fprintf(stderr, "  display: %s (NVAPI VRR flags %d; the helper's frames do not get windowed G-Sync, AGENT_LOG 140)\n",
                        v < 0 ? "unknown" : v & 16 ? "G-Sync / VRR on" : "fixed refresh", v);
            }
            last_vrr = now;
        }
        if (bsh->frame_limit_us != limit) {   /* the game's frame cap (NVIDIA Reflex limiter) */
            limit = bsh->frame_limit_us;
            p12_set_frame_limit(limit);
            fprintf(stderr, "  frame limit: %s%u us per shown frame (%.1f fps)\n", limit ? "" : "none, ", limit, limit ? 1e6 / limit : 0.0);
        }
        if (!p12_bridge_present(n, n == 0 || bsh->reset, b_frame_sr[n % 8], b_frame_iw[n % 8], b_frame_ih[n % 8])) { bsh->error = 1; b_quit = 1; return 4; }
        bsh->shown = p12_shown_total();
        __atomic_store_n(&bsh->done, n + 1, __ATOMIC_RELEASE);
    }
}

static int bridge_main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: helper64 bridge <game pid> <streamline dir> <log dir>\n"); return 1; }
    QueryPerformanceFrequency(&qpf);
    DWORD pid = (DWORD)strtoul(argv[2], NULL, 10);
    enum { NN = 11 };
    wchar_t names[NN][64], sl_dir[MAX_PATH], log_dir[MAX_PATH];
    static const wchar_t *const what[NN] = { L"map", L"wake", L"fence", L"fsr", L"fdone", L"final", L"hudless", L"depth", L"mvec",
                                             L"srout0", L"srout1" };
    for (int i = 0; i < NN; i++) swprintf(names[i], 64, L"Local\\fgb_%lu_%ls", (unsigned long)pid, what[i]);
    /* the paths from the wide command line: argv is its ANSI code page copy, '?' for other characters (a folder named in
       Japanese on a code page 1252 PC: no log, no Streamline) */
    int wn = 0;
    wchar_t **wa = CommandLineToArgvW(GetCommandLineW(), &wn);
    if (!wa || wn < 5) { fprintf(stderr, "helper64 bridge: command line not readable\n"); return 1; }
    lstrcpynW(sl_dir, wa[3], MAX_PATH);
    lstrcpynW(log_dir, wa[4], MAX_PATH);
    LocalFree(wa);
    if (log_dir[0]) {   /* started without a window (CREATE_NO_WINDOW): stats go to <log dir>\fgbridge_helper.log unless stderr is a file or pipe */
        wchar_t lp[MAX_PATH];
        swprintf(lp, MAX_PATH, L"%ls\\fgbridge_helper.log", log_dir);
        DWORD ft = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
        if (ft != FILE_TYPE_DISK && ft != FILE_TYPE_PIPE) _wfreopen(lp, L"w", stderr);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, names[0]);
    bwake = OpenEventW(SYNCHRONIZE, FALSE, names[1]);
    wchar_t w2[72];
    swprintf(w2, 72, L"%ls2", names[1]);
    bwake2 = OpenEventW(SYNCHRONIZE, FALSE, w2);
    bgame = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!map || !bwake || !bwake2 || !bgame) { fprintf(stderr, "helper64 bridge: cannot open the game's shared objects (%lu)\n", GetLastError()); return 2; }
    FgbShared *sh = bsh = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FgbShared));
    if (!sh || sh->proto != FGB_PROTO) { fprintf(stderr, "helper64 bridge: protocol mismatch\n"); return 2; }
    const wchar_t *objs[9];
    for (int i = 0; i < 9; i++) objs[i] = names[2 + i];
    p12_set_fg_test_limit(sh->fg_test_limit);
    if (!p12_bridge_init(sh->luid_lo, sh->luid_hi, (HWND)(uintptr_t)sh->hwnd, sh->width, sh->height, objs, sl_dir,
                         log_dir[0] ? log_dir : NULL)) { sh->error = 1; return 3; }
    sh->fg_max = p12_fg_max();   /* the game reads it once helper_ok is set */
    HANDLE pt = CreateThread(NULL, 0, bridge_present_thread, NULL, 0, NULL);
    if (!pt) { sh->error = 1; return 3; }
    sh->helper_caps = FGB_CAP_PRESET;
    __atomic_store_n(&sh->helper_ok, 1, __ATOMIC_RELEASE);
    uint32_t sr_cfg[4] = { 0, 0, 0, 0 }, sr_prev = 0;
    for (uint32_t n = 0;; n++) {
        uint32_t f, s = 0;
        /* frame n starts with its SR inputs (mid-frame) or its final image (Present), whichever comes first */
        for (int i = 0;; i++) {
            f = __atomic_load_n(&sh->frames, __ATOMIC_ACQUIRE);
            if (f == FGB_QUIT || b_quit || WaitForSingleObject(bgame, 0) == WAIT_OBJECT_0) goto end;
            if (f > n || (s = __atomic_load_n(&sh->sr_frames, __ATOMIC_ACQUIRE) > n)) break;
            if (i < 2000) __builtin_ia32_pause(); else WaitForSingleObject(bwake, 1);
        }
        if (s) {
            /* tell the game now that frame n's SR will come (its GPU then waits for _fsr n+1): it does not have to wait for
               Streamline's CPU work. shortcut: a helper crash between here and the submit hangs the game's GPU (TDR) */
            p12_set_jitter(sh->jitter_x, sh->jitter_y);   /* before sr_done: the game may write frame n+1's jitter right after */
            __atomic_store_n(&sh->sr_done, n + 1, __ATOMIC_RELEASE);
        }
        /* SR on / off and the render size, every frame: with SR switched off no frame has s, and the old render size
           would stay on the frame generation inputs (and their motion vector scale) */
        uint32_t rw = sh->render_w, rh = sh->render_h;   /* from the game's memory: within the shared textures */
        if (!rw || rw > sh->width || !rh || rh > sh->height) { rw = sh->width; rh = sh->height; }
        if (sh->sr_on != sr_cfg[0] || rw != sr_cfg[1] || rh != sr_cfg[2] || sh->sr_preset != sr_cfg[3]) {
            sr_cfg[0] = sh->sr_on; sr_cfg[1] = rw; sr_cfg[2] = rh; sr_cfg[3] = sh->sr_preset;
            p12_set_sr_preset(sr_cfg[3]);
            p12_set_sr(sr_cfg[0], sr_cfg[1], sr_cfg[2], 1);
            sr_prev = 0;
        }
        bridge_camera(sh->vp, sh->prev_vp, n == 100);
        LARGE_INTEGER s0, s1;
        QueryPerformanceCounter(&s0);
        if (!p12_bridge_frame(n, n == 0 || (s ? !sr_prev : sh->reset), s)) { sh->error = 1; break; }
        if (s) {
            QueryPerformanceCounter(&s1);
            b_sr_cpu_ms += (double)(s1.QuadPart - s0.QuadPart) * 1000 / qpf.QuadPart; b_sr_cpu_n++;
        }
        b_frame_sr[n % 8] = (uint8_t)s;
        b_frame_iw[n % 8] = sr_cfg[0] ? sr_cfg[1] : sh->width;
        b_frame_ih[n % 8] = sr_cfg[0] ? sr_cfg[2] : sh->height;
        __atomic_store_n(&b_started, n + 1, __ATOMIC_RELEASE);
        sr_prev = s;
    }
end:
    b_quit = 1;
    WaitForSingleObject(pt, 5000);
    char st[512];
    p12_stats(st, sizeof st, 1);
    fprintf(stderr, "  [bridge end] %s\n", st);
    p12_shutdown();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "bridge")) return bridge_main(argc, argv);
    if (argc < 2) return 1;
    QueryPerformanceFrequency(&qpf);
    double ms = 1000.0 / qpf.QuadPart;
    char name[64], ename[64];
    snprintf(name, sizeof name, "Local\\dlssp1_map_%s", argv[1]);
    snprintf(ename, sizeof ename, "Local\\dlssp1_wake_%s", argv[1]);
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    HANDLE wake = OpenEventA(SYNCHRONIZE, FALSE, ename);
    if (!map || !wake) fail("open shared memory / event", GetLastError());
    uint8_t *base = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!base) fail("MapViewOfFile", GetLastError());
    hdr = (P1Header *)base; ring = base + P1_HDR_SIZE; heap = ring + P1_RING_SIZE; arena = heap + P1_HEAP_SIZE;
    results = (P1Result *)(arena + P1_ARENA_SIZE);
    caps = (P1Caps *)((uint8_t *)results + P1_MAX_OBJ * P1_RESULT_SIZE);
    if (hdr->proto != P1_PROTO || hdr->ring_size != P1_RING_SIZE || hdr->heap_size != P1_HEAP_SIZE) fail("protocol mismatch", 0);

    /* plain 64-bit D3D11 (chosen after P1); P1_BACKEND=d3d11on12 (inherited from the app) for the P1 comparison */
    sl_mode = getenv("P3_SL_DIR") != NULL;
    const char *be = getenv("P1_BACKEND");
    backend = be && !strcmp(be, "d3d11on12") ? "D3D11On12" : "D3D11";
    if (backend[5] == 0) {
        CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                                D3D11_SDK_VERSION, &dev, NULL, &ctx), "D3D11CreateDevice");
    } else {
        ID3D12Device *d12; ID3D12CommandQueue *queue;
        CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&d12), "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0 };
        CHECK(ID3D12Device_CreateCommandQueue(d12, &qd, &IID_ID3D12CommandQueue, (void **)&queue), "CreateCommandQueue");
        CHECK(D3D11On12CreateDevice((IUnknown *)d12, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                                    (IUnknown *const *)&queue, 1, 0, &dev, &ctx, NULL), "D3D11On12CreateDevice");
    }
    fill_caps();
    D3D11_QUERY_DESC evd = { D3D11_QUERY_EVENT, 0 };
    for (int i = 0; i < 3; i++) CHECK(ID3D11Device_CreateQuery(dev, &evd, &q[i]), "CreateQuery");
    frame_busy = calloc(max_frames, sizeof(double));

    uint32_t rpos = 0;
    LARGE_INTEGER t0, t1;
    double frame_start_busy = 0;
    for (;;) {
        uint32_t w = __atomic_load_n(&hdr->write_pos, __ATOMIC_ACQUIRE);
        if (w == rpos) {
            /* spin ~1 ms before sleeping so mid-frame gaps never cost a wake-up */
            QueryPerformanceCounter(&t0);
            for (;;) {
                __builtin_ia32_pause();
                if (npend_q || npend_rb) poll_results();   /* the app may be polling a query / readback */
                if (__atomic_load_n(&hdr->write_pos, __ATOMIC_ACQUIRE) != rpos) break;
                QueryPerformanceCounter(&t1);
                if ((t1.QuadPart - t0.QuadPart) * ms > 1.0) {
                    hdr->sleeping = 1;
                    __atomic_thread_fence(__ATOMIC_SEQ_CST);
                    if (__atomic_load_n(&hdr->write_pos, __ATOMIC_ACQUIRE) == rpos) WaitForSingleObject(wake, 1);
                    hdr->sleeping = 0;
                    t0 = t1;
                }
            }
            continue;
        }
        QueryPerformanceCounter(&t0);
        while (rpos != w) {
            const uint32_t *r = (const uint32_t *)(ring + (rpos & (P1_RING_SIZE - 1)));
            if (r[1] < 8 || (r[1] & 7)) fail("bad record size", r[1]);
            if (r[0] == OP_QUIT) {
                QueryPerformanceCounter(&t1); busy_ms += (t1.QuadPart - t0.QuadPart) * ms;
                uint32_t n = nframes > 60 ? nframes - 60 : nframes;   /* same warm-up as game32 */
                double *fb = frame_busy + (nframes - n), mean = 0;
                for (uint32_t i = 0; i < n; i++) mean += fb[i];
                qsort(fb, n, sizeof *fb, cmpd);
                char line[256];
                snprintf(line, sizeof line, "  helper replay CPU ms/frame (%s): median %.3f  mean %.3f  p95 %.3f  max %.3f;"
                        " GPU wait %.3f ms/frame (%u frames)\n", backend, n ? fb[n / 2] : 0, n ? mean / n : 0,
                        n ? fb[n * 95 / 100] : 0, n ? fb[n - 1] : 0, nframes ? gpu_wait_ms / nframes : 0, nframes);
                fputs(line, stderr);   /* one write: stderr is unbuffered */
                if (up_maps) {
                    snprintf(line, sizeof line, "  helper ring uploads: %.2f MB/frame in %.0f maps/frame, %.0f index scans/frame\n",
                             up_bytes / 1048576.0 / nframes, (double)up_maps / nframes, (double)idx_scans / nframes);
                    fputs(line, stderr);
                }
                if (sl_mode) {
                    char st[512];
                    p12_stats(st, sizeof st, 1);
                    fprintf(stderr, "  [phase end] %s\n", st);
                    p12_shutdown();
                }
                if (sc) {
                    snprintf(line, sizeof line, "  helper presents %u (occluded %u, failed %u, last error 0x%08lx), "
                             "Present call %.3f ms each\n", presents, occluded, present_fail, (unsigned long)last_fail,
                             presents ? present_ms / presents : 0);
                    fputs(line, stderr);
                }
                return 0;
            }
            if (r[0] == OP_FRAME_END) {
                ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q[nframes % 3]);
                ID3D11DeviceContext_Flush(ctx);
                QueryPerformanceCounter(&t1);
                busy_ms += (t1.QuadPart - t0.QuadPart) * ms;
                if (nframes < max_frames) frame_busy[nframes] = busy_ms - frame_start_busy;
                if (nframes >= 2)   /* GPU at most 2 frames behind (not counted as replay time) */
                    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q[(nframes - 2) % 3], NULL, 0, 0) != S_OK)
                        YieldProcessor();
                QueryPerformanceCounter(&t0);
                gpu_wait_ms += (t0.QuadPart - t1.QuadPart) * ms;
                frame_start_busy = busy_ms;
                nframes++;
                __atomic_store_n(&hdr->frames_done, nframes, __ATOMIC_RELEASE);
            } else replay(r);
            rpos += r[1];
            __atomic_store_n(&hdr->read_pos, rpos, __ATOMIC_RELEASE);
        }
        if (npend_q || npend_rb) poll_results();
        QueryPerformanceCounter(&t1);
        busy_ms += (t1.QuadPart - t0.QuadPart) * ms;
    }
}
