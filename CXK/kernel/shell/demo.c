/* /CXLite/kernel/shell/demo.c */
/* Aurora Tejeda */
/*
 * Spinning wireframe dodecahedron - a showcase for the framebuffer + FPU/SSE.
 *
 * 20 vertices (built from the golden ratio phi), 30 edges. Each frame we rotate
 * every vertex about the X and Y axes (sin/cos rotation matrices), project to
 * 2D with a simple perspective divide, and draw the edges with Bresenham lines.
 * Runs until Ctrl+C. Requires fpu_init() (done in kmain) - uses double math.
 */

#include "demo.h"
#include "fb.h"
#include "console.h"
#include "keyboard.h"
#include "timer.h"
#include "kmath.h"

#define NVERT 20
#define NEDGE 30

/* The 20 dodecahedron vertices. phi = (1+sqrt5)/2; 1/phi = phi-1.
   Groups: the 8 cube corners (+/-1,+/-1,+/-1) and 12 from cyclic
   (0, +/-1/phi, +/-phi). Filled in at runtime since they need phi. */
static double vx[NVERT], vy[NVERT], vz[NVERT];

/* edge list: pairs of vertex indices (computed once by distance) */
static int edges[NEDGE][2];
static int edge_count = 0;

static void build_dodecahedron(void) {
    double phi = (1.0 + km_sqrt(5.0)) / 2.0;
    double inv = phi - 1.0;          /* 1/phi */
    int n = 0;

    /* 8 cube corners (+/-1, +/-1, +/-1) */
    for (int sx = -1; sx <= 1; sx += 2)
    for (int sy = -1; sy <= 1; sy += 2)
    for (int sz = -1; sz <= 1; sz += 2) {
        vx[n] = sx; vy[n] = sy; vz[n] = sz; n++;
    }
    /* (0, +/-inv, +/-phi) */
    for (int a = -1; a <= 1; a += 2)
    for (int b = -1; b <= 1; b += 2) {
        vx[n] = 0;        vy[n] = a * inv;  vz[n] = b * phi;  n++;
    }
    /* (+/-inv, +/-phi, 0) */
    for (int a = -1; a <= 1; a += 2)
    for (int b = -1; b <= 1; b += 2) {
        vx[n] = a * inv;  vy[n] = b * phi;  vz[n] = 0;        n++;
    }
    /* (+/-phi, 0, +/-inv) */
    for (int a = -1; a <= 1; a += 2)
    for (int b = -1; b <= 1; b += 2) {
        vx[n] = a * phi;  vy[n] = 0;        vz[n] = b * inv;  n++;
    }

    /* edges connect vertices exactly the edge-length apart. For this
       construction the edge length squared is (2/phi)^2 = (2*inv)^2.
       Compare squared distances with a tolerance. */
    double elen = 2.0 * inv;
    double target = elen * elen;
    edge_count = 0;
    for (int i = 0; i < NVERT; i++)
    for (int j = i + 1; j < NVERT; j++) {
        double dx = vx[i]-vx[j], dy = vy[i]-vy[j], dz = vz[i]-vz[j];
        double d2 = dx*dx + dy*dy + dz*dz;
        double diff = d2 - target;
        if (diff < 0) diff = -diff;
        if (diff < 0.01 && edge_count < NEDGE) {
            edges[edge_count][0] = i;
            edges[edge_count][1] = j;
            edge_count++;
        }
    }
}

/* clamp helper for color channels */
static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void demo_spin(void) {
    if (!fb_active()) {
        console_print("The spin demo needs graphics mode (VBE framebuffer).\n");
        return;
    }

    build_dodecahedron();

    int cx = (int)(fb_width()  / 2);
    int cy = (int)(fb_height() / 2);
    double scale = (double)fb_height() / 5.0;   /* fit nicely on screen */
    double dist  = 5.0;                          /* camera distance for projection */

    uint32_t bg = fb_rgb(0, 0, 20);

    /* clear the screen ONCE; after that we erase the previous frame by redrawing
       its edges in the background color, instead of clearing all ~786K pixels
       every frame. The wireframe touches only a tiny fraction of the screen, so
       this is dramatically faster. */
    fb_clear(bg);

    /* per-vertex projected coords + per-vertex depth, this frame and last */
    int px[NVERT], py[NVERT];
    int lpx[NVERT], lpy[NVERT];
    double vz_cam[NVERT];        /* camera-space z per vertex, for depth shading */
    int have_last = 0;

    double ax = 0.0, ay = 0.0;   /* rotation angles */

    for (;;) {
        char k = keyboard_getchar();
        if (k == KEY_CTRL_C) break;

        double sa = km_sin(ax), ca = km_cos(ax);
        double sb = km_sin(ay), cb = km_cos(ay);

        /* track min/max camera-z across vertices to normalize depth shading */
        double zmin = 1e9, zmax = -1e9;

        for (int i = 0; i < NVERT; i++) {
            double x = vx[i], y = vy[i], z = vz[i];
            double y1 = y * ca - z * sa;
            double z1 = y * sa + z * ca;
            double x2 = x * cb + z1 * sb;
            double z2 = -x * sb + z1 * cb;
            double f = dist / (dist + z2);
            px[i] = cx + (int)(x2 * scale * f);
            py[i] = cy - (int)(y1 * scale * f);
            vz_cam[i] = z2;
            if (z2 < zmin) zmin = z2;
            if (z2 > zmax) zmax = z2;
        }

        /* erase last frame's wireframe (redraw its edges in bg) */
        if (have_last) {
            for (int e = 0; e < edge_count; e++) {
                int a = edges[e][0], b = edges[e][1];
                fb_draw_line(lpx[a], lpy[a], lpx[b], lpy[b], bg);
            }
        }

        /* draw this frame's edges with DEPTH-CUED brightness: edges nearer the
           camera (smaller z) are bright cyan; farther edges fade to dark blue.
           This is the depth cue that makes near faces clearly "pop" forward. */
        double zrange = zmax - zmin;
        if (zrange < 0.0001) zrange = 0.0001;
        for (int e = 0; e < edge_count; e++) {
            int a = edges[e][0], b = edges[e][1];
            /* edge depth = average of its endpoints' camera-z */
            double zedge = (vz_cam[a] + vz_cam[b]) * 0.5;
            /* t = 0 (nearest) .. 1 (farthest) */
            double t = (zedge - zmin) / zrange;
            /* brightness 1.0 near -> 0.30 far */
            double bright = 1.0 - 0.70 * t;
            int r = clampi((int)(120 * bright), 0, 255);
            int g = clampi((int)(230 * bright), 0, 255);
            int bl= clampi((int)(255 * bright), 0, 255);
            fb_draw_line(px[a], py[a], px[b], py[b], fb_rgb(r, g, bl));
        }

        /* save this frame's points to erase next time */
        for (int i = 0; i < NVERT; i++) { lpx[i] = px[i]; lpy[i] = py[i]; }
        have_last = 1;

        ax += 0.03;
        ay += 0.02;
        if (ax > KM_2PI) ax -= KM_2PI;
        if (ay > KM_2PI) ay -= KM_2PI;

        timer_sleep(16);   /* ~60 fps pacing */
    }

    console_clear();
}