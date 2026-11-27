/* SpideyTentacles - Iron Spider tentacle overlay for Spider-Man 3 (PSP) on PPSSPP.
 * Press L+R to extend / retract the tentacles on Spider-Man's back.
 *
 * HOW IT WORKS: a plugin thread waits for each vblank, and software-draws the
 * (decimated) tentacle mesh straight into the visible framebuffer.
 * Spider-Man is NOT tracked in 3D (that needs game-specific memory addresses);
 * the tentacles are anchored to a fixed screen point (default = screen centre,
 * where the third-person camera keeps Spider-Man). Tune with the .ini file. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "tentacles_mesh.h"

PSP_MODULE_INFO("SpideyTentacles", 0, 1, 1);

#define LOG_PATH "ms0:/spidey_tentacles.log"
#define INI_PATH "ms0:/PSP/PLUGINS/SpideyTentacles.ini"

/* ---- settings (override in INI_PATH, one "key=value" per line) ---- */
static int   cfg_buttons = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER; /* buttons=  (bitmask) */
static float cfg_cx = 240.f, cfg_cy = 150.f;   /* cx= cy=   screen point of the tentacle hub (back) */
static float cfg_scale = 260.f;                /* scale=    pixels per mesh unit (mesh height = 1) */
static float cfg_yaw = 0.f;                    /* yaw=      degrees; 180 flips the tentacles around */
static float cfg_tilt = 0.f;                   /* tilt=     degrees, pitch */
static unsigned cfg_color = 0xD8A838;          /* color=    hex RRGGBB */

static void log_line(const char *m)
{
    int fd = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, m, strlen(m)); sceIoClose(fd);
}

static void load_ini(void)
{
    char buf[512]; int fd = sceIoOpen(INI_PATH, PSP_O_RDONLY, 0);
    char *p, *line;
    if (fd < 0) return;
    int n = sceIoRead(fd, buf, sizeof(buf) - 1); sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        p = strchr(line, '='); if (!p) continue; *p++ = 0;
        if (!strcmp(line, "cx")) cfg_cx = (float)atof(p);
        else if (!strcmp(line, "cy")) cfg_cy = (float)atof(p);
        else if (!strcmp(line, "scale")) cfg_scale = (float)atof(p);
        else if (!strcmp(line, "yaw")) cfg_yaw = (float)atof(p);
        else if (!strcmp(line, "tilt")) cfg_tilt = (float)atof(p);
        else if (!strcmp(line, "color")) cfg_color = (unsigned)strtoul(p, NULL, 16);
        else if (!strcmp(line, "buttons")) cfg_buttons = (int)strtoul(p, NULL, 0);
    }
}

/* ---- framebuffer ---- */
static unsigned char *fb; static int fb_w, fb_fmt;

static inline void put(int x, int y, int r, int g, int b)
{
    if ((unsigned)x >= 480u || (unsigned)y >= 272u) return;
    if (fb_fmt == PSP_DISPLAY_PIXEL_FORMAT_8888)
        ((unsigned *)fb)[y * fb_w + x] = 0xFF000000u | (b << 16) | (g << 8) | r;
    else if (fb_fmt == PSP_DISPLAY_PIXEL_FORMAT_565)
        ((unsigned short *)fb)[y * fb_w + x] = (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
}

static void tri(const float *a, const float *b, const float *c, int r, int g, int bl)
{
    float x0 = a[0], y0 = a[1], x1 = b[0], y1 = b[1], x2 = c[0], y2 = c[1];
    int miny = (int)floorf(fminf(y0, fminf(y1, y2))), maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    int y;
    if (miny < 0) miny = 0; if (maxy > 271) maxy = 271;
    for (y = miny; y <= maxy; y++) {
        float fy = y + 0.5f, xl = 1e9f, xr = -1e9f, t, x;
        const float e[3][4] = {{x0,y0,x1,y1},{x1,y1,x2,y2},{x2,y2,x0,y0}};
        int i, xs, xe;
        for (i = 0; i < 3; i++) {
            float ay = e[i][1], by = e[i][3];
            if ((fy < ay && fy < by) || (fy >= ay && fy >= by) || ay == by) continue;
            t = (fy - ay) / (by - ay); x = e[i][0] + t * (e[i][2] - e[i][0]);
            if (x < xl) xl = x; if (x > xr) xr = x;
        }
        if (xl > xr) continue;
        xs = (int)floorf(xl); xe = (int)ceilf(xr);
        if (xs < 0) xs = 0; if (xe > 479) xe = 479;
        for (; xs <= xe; xs++) put(xs, y, r, g, bl);
    }
}

/* ---- scene ---- */
static float sx[TENT_NV], sy[TENT_NV], sz[TENT_NV], wx[TENT_NV], wy[TENT_NV], wz[TENT_NV];
static unsigned short order[TENT_NT]; static float depth[TENT_NT];
static int cmp(const void *a, const void *b)
{ float d = depth[*(const unsigned short *)b] - depth[*(const unsigned short *)a]; return (d > 0) - (d < 0); }

static void draw_tentacles(float ext, float t)
{
    const float D2R = 0.01745329f;
    float cy = cosf(cfg_yaw * D2R), sy_ = sinf(cfg_yaw * D2R);
    float ct = cosf(cfg_tilt * D2R), st = sinf(cfg_tilt * D2R);
    int i; const float hub_y = 0.55f;
    for (i = 0; i < TENT_NV; i++) {
        float x = tent_v[i][0], y = tent_v[i][1] - hub_y, z = tent_v[i][2];
        float k = (y > 0 ? y : -y);                       /* sway grows toward the tips */
        float sw = 0.04f * k * sinf(t * 3.f + y * 6.f);
        float e = ext, rx, rz, ry;
        x = (x + sw) * e; y *= e; z *= e;                 /* grow out of the hub */
        rx = x * cy + z * sy_; rz = -x * sy_ + z * cy;    /* yaw */
        ry = y * ct - rz * st; rz = y * st + rz * ct;     /* tilt */
        wx[i] = rx; wy[i] = ry; wz[i] = rz;
        sx[i] = cfg_cx + rx * cfg_scale; sy[i] = cfg_cy - ry * cfg_scale; sz[i] = rz;
    }
    for (i = 0; i < TENT_NT; i++) {
        order[i] = i;
        depth[i] = (sz[tent_f[i][0]] + sz[tent_f[i][1]] + sz[tent_f[i][2]]) * (1.f / 3.f);
    }
    qsort(order, TENT_NT, sizeof(order[0]), cmp);        /* painter's algorithm */
    for (i = 0; i < TENT_NT; i++) {
        int f = order[i], a = tent_f[f][0], b = tent_f[f][1], c = tent_f[f][2];
        float ux = wx[b]-wx[a], uy = wy[b]-wy[a], uz = wz[b]-wz[a];
        float vx = wx[c]-wx[a], vy = wy[c]-wy[a], vz = wz[c]-wz[a];
        float nx = uy*vz-uz*vy, ny = uz*vx-ux*vz, nz = ux*vy-uy*vx;
        float l = sqrtf(nx*nx+ny*ny+nz*nz) + 1e-6f, sh;
        float p[3][2]; (void)p;
        sh = fabsf((-0.4f*nx + 0.6f*ny + 0.7f*nz) / l); /* two-sided light */
        sh = 0.30f + 0.70f * sh;
        {
            float A[2] = {sx[a], sy[a]}, B[2] = {sx[b], sy[b]}, C[2] = {sx[c], sy[c]};
            int r = (int)(((cfg_color >> 16) & 255) * sh), g = (int)(((cfg_color >> 8) & 255) * sh),
                bl = (int)((cfg_color & 255) * sh);
            tri(A, B, C, r, g, bl);
        }
    }
}

/* ---- main thread ---- */
static int worker(SceSize args, void *argp)
{
    SceCtrlData pad; int prev_combo = 0, on = 0; float ext = 0.f, t = 0.f;
    unsigned char *top; int bw, pf;
    log_line("thread running\n");
    for (;;) {
        sceDisplayWaitVblankStart();
        sceCtrlPeekBufferPositive(&pad, 1);
        {   int combo = (pad.Buttons & cfg_buttons) == cfg_buttons;
            if (combo && !prev_combo) { on = !on; log_line(on ? "tentacles ON\n" : "tentacles OFF\n"); }
            prev_combo = combo; }
        ext += on ? 0.06f : -0.06f;                       /* ~0.3s grow / retract */
        if (ext > 1.f) ext = 1.f; if (ext < 0.f) ext = 0.f;
        t += 1.f / 60.f;
        if (ext <= 0.f) continue;
        sceDisplayGetFrameBuf((void **)&top, &bw, &pf, PSP_DISPLAY_SETBUF_IMMEDIATE);
        if (!top) continue;
        fb = (unsigned char *)((unsigned)top | 0x40000000u); fb_w = bw; fb_fmt = pf;
        {   float e = ext * ext * (3.f - 2.f * ext);      /* smoothstep */
            draw_tentacles(e, t); }
        sceKernelDcacheWritebackAll();
    }
    return 0;
}

int module_start(SceSize args, void *argp)
{
    SceUID th;
    log_line("SpideyTentacles: module_start\n");
    load_ini();
    th = sceKernelCreateThread("SpideyTentacles", worker, 0x30, 0x10000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    return 0;
}
int module_stop(SceSize args, void *argp) { return 0; }
