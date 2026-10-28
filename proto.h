/* Passthrough protocol: shared by the 32-bit front end (rec32.c) and the 64-bit replay helper (helper64.c).
   Fixed-size fields only (no pointers, no size_t) so both bitnesses see the same layout. */
#pragma once
#include <stdint.h>

#define P1_PROTO     2
#define P1_RING_SIZE (64u << 20)   /* command ring, power of two */
#define P1_HEAP_SIZE (256u << 20)  /* upload heap for Map data, initial data, shaders */
#define P1_ARENA_SIZE (128u << 20) /* P1b: shadows of ring buffers (big dynamic VB/IB) + readback copies of staging */
#define P1_RING_MIN (64u << 10)    /* dynamic VB/IB at least this big are rings (NO_OVERWRITE, shadowed) */
#define P1_RING_SEGS 4             /* shadow copies per ring: DISCARD moves to the next one (renaming) */
#define P1_MAX_OBJ 65536           /* object ids 1..65535, reused after release (Attila: ~35k creations a session) */
#define P1_RESULT_SIZE 128u        /* per object id: query result / readback sequence */
#define P1_CAPS_SIZE (64u << 10)   /* device answers measured by the helper at startup */

/* Positions are monotonic uint32 byte counters; offset = pos % size (sizes divide 2^32). */
typedef struct {
    uint32_t proto, ring_size, heap_size, pad0[13];
    /* producer line */
    volatile uint32_t write_pos;
    uint32_t pad1[15];
    /* consumer line */
    volatile uint32_t read_pos;
    volatile uint32_t heap_read;
    volatile uint32_t frames_done;   /* frames replayed with the helper's GPU <= 2 frames behind */
    volatile uint32_t sleeping;      /* helper is waiting on the wake event */
    volatile uint32_t readback_done;
    volatile uint32_t error;         /* nonzero: helper failed (message on its stderr) */
    volatile uint32_t caps_ready;    /* helper filled the caps area */
    uint32_t pad2[9];
} P1Header;
_Static_assert(sizeof(P1Header) == 192, "header layout");

#define P1_HDR_SIZE 4096u  /* ring follows, then heap, arena, results, caps */
#define P1_SECTION_SIZE (P1_HDR_SIZE + P1_RING_SIZE + P1_HEAP_SIZE + P1_ARENA_SIZE + P1_MAX_OBJ * P1_RESULT_SIZE + P1_CAPS_SIZE)

/* Results of queries and staging readbacks, by object id: written by the helper, read by the front end.
   seq = number of the End (query) or copy-into (staging) whose result is in data / in the staging's arena copy. */
typedef struct { volatile uint32_t seq; uint32_t size; uint32_t pad[2]; uint8_t data[112]; } P1Result;
_Static_assert(sizeof(P1Result) == P1_RESULT_SIZE, "result layout");

/* Device answers that need the real device (no D3D11 in the game process). */
#define P1_NFMT 192
typedef struct {
    uint32_t feature_level, creation_flags, pad[2];
    struct { uint32_t support, support2, msaa[4]; } fmt[P1_NFMT];   /* CheckFormatSupport / ..Support2 / quality levels 1,2,4,8 */
    struct { uint32_t ok, size; uint8_t data[56]; } feature[16];     /* CheckFeatureSupport for features 0..15 without input */
} P1Caps;
_Static_assert(sizeof(P1Caps) <= P1_CAPS_SIZE, "caps layout");

enum {
    OP_PAD = 1, OP_CREATE_BUFFER, OP_CREATE_TEX2D, OP_CREATE_VIEW, OP_CREATE_SHADER, OP_CREATE_LAYOUT,
    OP_CLEAR_RTV, OP_CLEAR_DSV, OP_SET_RT, OP_SET_VP, OP_SET_LAYOUT, OP_SET_TOPO, OP_SET_VB, OP_SET_IB,
    OP_SET_SHADER, OP_SET_CB, OP_SET_SRV, OP_UPDATE, OP_DRAW_II, OP_FRAME_END, OP_READBACK, OP_QUIT,
    OP_CREATE_SWAPCHAIN, OP_PRESENT, OP_RESIZE, OP_RELEASE,
    OP_FG_TAGS, OP_FG_MODE,  /* P3 proof only: which textures are depth / motion vectors; frame generation mode */
    OP_SR_CONFIG, OP_FRAME_INFO,  /* P3b: scene colour + render size + DLSS SR on/off; per-frame jitter */
    OP_RING_SEG, OP_DRAW, OP_DRAW_IDX,  /* P1b: ring switched to shadow segment N (DISCARD); Draw; DrawIndexed */
    /* M3 slice 1 */
    OP_CREATE_TEX3D, OP_CREATE_STATE, OP_CREATE_QUERY, OP_SET_SMP, OP_SET_UAV_CS, OP_SET_RT_UAV, OP_SET_SC,
    OP_SET_RS, OP_SET_BLEND, OP_SET_DSS, OP_SET_SO, OP_SET_PRED, OP_CLEAR_STATE, OP_DISPATCH, OP_COPY_RES,
    OP_COPY_SUB, OP_UPDATE_SUB, OP_BEGIN, OP_END, OP_FLUSH
};
enum { VIEW_RTV, VIEW_DSV, VIEW_SRV, VIEW_UAV };
enum { STAGE_VS, STAGE_PS, STAGE_GS, STAGE_HS, STAGE_DS, STAGE_CS, NSTAGE };
enum { STATE_BLEND, STATE_DSS, STATE_RS, STATE_SAMPLER };

/* Every record: uint32 op, uint32 size (bytes incl. these 8, multiple of 8), then uint32 payload words.
   Object ids are 1-based (0 = NULL). Heap references are (pos, size) pairs. */
typedef struct { uint32_t op, size; } P1Rec;

typedef struct {   /* OP_CREATE_LAYOUT element, inline */
    char     name[16];
    uint32_t index, format, slot, offset, slot_class, step_rate;
} P1Elem;
_Static_assert(sizeof(P1Elem) == 40, "elem layout");

typedef struct {   /* initial data / update of one subresource, in the heap (or the arena for readback copies) */
    uint32_t pos, size, row_pitch, depth_pitch;
} P1Sub;

/* Bytes per element (or per 4x4 block for block-compressed formats) of a DXGI format; 0 = not supported here.
   block = 4 for BC formats, 1 otherwise. */
static inline uint32_t p1_fmt_bytes(uint32_t f, uint32_t *block)
{
    *block = 1;
    if (f >= 1 && f <= 4) return 16;
    if (f >= 5 && f <= 8) return 12;
    if (f >= 9 && f <= 22) return 8;
    if (f >= 23 && f <= 47) return 4;
    if (f >= 48 && f <= 59) return 2;
    if (f >= 60 && f <= 65) return 1;
    if (f == 67) return 4;
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) { *block = 4; return 8; }
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) { *block = 4; return 16; }
    if (f == 85 || f == 86 || f == 115) return 2;
    if (f >= 87 && f <= 93) return 4;
    return 0;
}

/* Row pitch / rows / slice size of a tightly packed mip level (w, h already reduced to the mip). */
static inline void p1_mip_layout(uint32_t fmt, uint32_t w, uint32_t h, uint32_t *row, uint32_t *rows)
{
    uint32_t block, bytes = p1_fmt_bytes(fmt, &block);
    *row = (w + block - 1) / block * bytes;
    *rows = (h + block - 1) / block;
}
