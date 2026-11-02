/* Camera values AMD FSR needs (near, far, vertical FOV, position, axes), taken from the game's view-projection alone
   (row-major, clip = M x [x y z 1], D3D depth 0..1, the bridge's FgbShared.vp). With P x V and V's rows right / up /
   forward: row 0 = sx * right (+ off-centre forward), row 1 = sy * up (+ ...), row 2 = A * forward + (0 0 0 B),
   row 3 = forward. A whole-matrix scale cancels (everything is measured against |row 3|). Checked by camtest.c. */
#pragma once
#include <math.h>

typedef struct { float nearz, farz, fov_y, pos[3], right[3], up[3], fwd[3]; } CamVp;

static inline int cam_from_vp(const float *m, CamVp *c)
{
    const float *r0 = m, *r1 = m + 4, *r2 = m + 8, *r3 = m + 12;
    float s = sqrtf(r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2]);
    float l0 = sqrtf(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]), l1 = sqrtf(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
    if (s < 1e-20f || l0 < 1e-20f || l1 < 1e-20f) return 0;
    double a = ((double)r2[0] * r3[0] + (double)r2[1] * r3[1] + (double)r2[2] * r3[2]) / ((double)s * s);   /* depth scale A */
    double b = (r2[3] - a * r3[3]) / s;                                     /* depth offset B (double: far needs 1 - A) */
    if (fabs(a) < 1e-20 || fabs(1 - a) < 1e-20) return 0;
    c->nearz = (float)(-b / a); c->farz = (float)(b / (1 - a));
    c->fov_y = 2 * atanf(s / l1);
    for (int i = 0; i < 3; i++) { c->right[i] = r0[i] / l0; c->up[i] = r1[i] / l1; c->fwd[i] = r3[i] / s; }
    /* the eye is where clip x, y and w are 0: rows 0, 1, 3 . [p 1] = 0 (Cramer) */
    const float *e[3] = { r0, r1, r3 };
    float det = e[0][0] * (e[1][1] * e[2][2] - e[1][2] * e[2][1]) - e[0][1] * (e[1][0] * e[2][2] - e[1][2] * e[2][0]) +
                e[0][2] * (e[1][0] * e[2][1] - e[1][1] * e[2][0]);
    if (fabsf(det) < 1e-30f) return 0;
    for (int k = 0; k < 3; k++) {
        float t[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) t[i][j] = j == k ? -e[i][3] : e[i][j];
        c->pos[k] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
                     t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / det;
    }
    return c->nearz > 0 && c->farz > c->nearz;
}
