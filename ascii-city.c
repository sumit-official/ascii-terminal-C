/*
 * STATIC CITY - native Linux terminal edition (C).
 * An abandoned ASCII city you can walk through, drawn with a real 3D raycaster.
 *
 * Build:  gcc -O2 -o ascii-city ascii-city.c -lm        (or just: make)
 * Run:    ./ascii-city
 *
 * Controls: W A S D move | arrows: left/right turn, up/down look | Shift run
 *           F auto-walk | R rain | P or Ctrl+C quit
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define N 77
#define EYE 0.5
#define MAXCOLS 200
#define MAXROWS 60
#define NPAL 17

static int COLS = 100, ROWS = 30, HOR = 15;
static double K = 31;

static const int PAL[NPAL][3] = {
    {240, 100, 50}, {235, 215, 70}, {60, 130, 240}, {70, 210, 120}, {70, 210, 230}, {230, 70, 210},
    {150, 150, 165}, {240, 60, 60}, {110, 110, 125}, {60, 170, 70}, {40, 110, 220}, {90, 220, 220},
    {0, 0, 0}, {255, 225, 120}, {255, 255, 255}, {150, 105, 65}, {175, 235, 60}};
static const double FAC[5] = {1, .75, .52, .34, .2};

static int ci(int p, int l) { return p * 5 + (l > 4 ? 4 : l); }
static int imin(int a, int b) { return a < b ? a : b; }
static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* deterministic hash -> 0..999 (same as the JS version) */
static int hs(int a, int b, int c) {
    uint32_t h = (uint32_t)a * 374761393u ^ (uint32_t)b * 668265263u ^ (uint32_t)c * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (int)((h ^ (h >> 16)) % 1000u);
}

/* ---------- world ---------- */
static float H[N * N], TZ[N * N];
static uint8_t P[N * N], FL[N * N], BLK[N * N], TY[N * N], TV[N * N], LIT[N * N], CAX[N * N], CF[N * N],
    CP[N * N];
static int shards[8];

static void generate(void) {
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int i = y * N + x, rx = x % 11, ry = y % 11, bx = x / 11, by = y / 11, road = rx < 3 || ry < 3;
            int lake = ((x - 46) * (x - 46) + (y - 34) * (y - 34)) < 150;
            if (road) {
                FL[i] = lake ? 4 : 0;
            } else if (lake) {
                FL[i] = 3;
                BLK[i] = 1;
            } else if (rx == 3 || rx == 10 || ry == 3 || ry == 10) {
                FL[i] = 1;
            } else if (hs(bx, by, 7) < 190) {
                FL[i] = 2;
                if (hs(x, y, 3) < 230) { /* a tree: one of five kinds */
                    int q = hs(x, y, 11) % 100, v = q < 32 ? 0 : q < 48 ? 1 : q < 64 ? 2 : q < 80 ? 3 : 4;
                    double u = hs(x, y, 4) / 1000.0;
                    double zs[5] = {1.4 + u * .6, 1.9 + u * .7, .35, 1.8 + u * .6, 1.4 + u * .6};
                    double hh[5] = {.7, 1.15, .45, .75, .7};
                    TV[i] = v;
                    TZ[i] = zs[v];
                    H[i] = zs[v] + hh[v];
                    TY[i] = 1;
                    P[i] = 3;
                    BLK[i] = 1;
                }
            } else { /* a tower */
                int sx = (rx - 4) / 3, sy = (ry - 4) / 3, s = sx * 2 + sy;
                double dc = hypot(x - 38, y - 38);
                H[i] = (5 + hs(bx, by, s) / 1000.0 * 13) * (1 + fmax(0, 1 - dc / 50));
                P[i] = hs(bx, by, 10 + s) % 6;
                BLK[i] = 1;
                FL[i] = 1;
            }
            if (x == 0 || y == 0 || x == N - 1 || y == N - 1) {
                H[i] = 4;
                P[i] = 6;
                BLK[i] = 1;
                TY[i] = 0;
                FL[i] = 1;
            }
        }
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (x && y) {
                int i = y * N + x;
                H[i] = 0;
                BLK[i] = 0;
                TY[i] = 0;
            }
    /* fences along the water's edge */
    for (int y = 1; y < N - 1; y++)
        for (int x = 1; x < N - 1; x++) {
            int i = y * N + x;
            if (BLK[i] || H[i] != 0 || FL[i] == 3) continue;
            if (FL[i - 1] == 3 || FL[i + 1] == 3 || FL[i - N] == 3 || FL[i + N] == 3) {
                H[i] = .3;
                TY[i] = 4;
                BLK[i] = 1;
                P[i] = 6;
            }
        }
    /* close the corners: join diagonal fence cells so fences meet at right angles */
    static int add[N * N];
    int nadd = 0;
    for (int y = 1; y < N - 1; y++)
        for (int x = 1; x < N - 1; x++) {
            if (TY[y * N + x] != 4) continue;
            for (int k = 0; k < 2; k++) {
                int a = 1, b = k ? -1 : 1;
                if (TY[(y + b) * N + x + a] != 4) continue;
                int p = y * N + x + a, q = (y + b) * N + x;
                if (TY[p] == 4 || TY[q] == 4) continue;
                int c[2] = {p, q};
                for (int m = 0; m < 2; m++)
                    if (!BLK[c[m]] && H[c[m]] == 0 && FL[c[m]] != 3) {
                        add[nadd++] = c[m];
                        break;
                    }
            }
        }
    for (int k = 0; k < nadd; k++) {
        int i = add[k];
        H[i] = .3;
        TY[i] = 4;
        BLK[i] = 1;
        P[i] = 6;
    }
    /* street lamps on sidewalks, with a pool of warm light around each */
    for (int y = 2; y < N - 2; y++)
        for (int x = 2; x < N - 2; x++) {
            int i = y * N + x;
            if (FL[i] != 1 || BLK[i] || H[i] != 0 || (x + y) % 4 || hs(x, y, 12) >= 260) continue;
            H[i] = 2.6;
            TY[i] = 5;
            BLK[i] = 1;
            P[i] = 15;
            TV[i] = hs(x, y, 13) % 2;
            for (int b = -3; b <= 3; b++)
                for (int a = -3; a <= 3; a++) {
                    int j = (y + b) * N + x + a, v = 3 - (int)hypot(a, b);
                    if (v > 0 && v > LIT[j]) LIT[j] = v;
                }
        }
    /* parked cars: two cells long, one wide, with a facing */
    for (int y = 1; y < N - 1; y++)
        for (int x = 1; x < N - 1; x++) {
            int rx = x % 11, ry = y % 11, ax = -1, x2 = x, y2 = y;
            if (rx < 3 && ry >= 3 && (rx == 0 || rx == 2) && y % 2 == 0 && (y + 1) % 11 >= 3) {
                ax = 1;
                y2 = y + 1;
            } else if (ry < 3 && rx >= 3 && (ry == 0 || ry == 2) && x % 2 == 0 && (x + 1) % 11 >= 3) {
                ax = 0;
                x2 = x + 1;
            }
            if (ax < 0 || hs(x, y, 5) >= 170) continue;
            int i = y * N + x, j = y2 * N + x2;
            if (FL[i] || FL[j] || H[i] != 0 || H[j] != 0 || y2 >= N - 1 || x2 >= N - 1) continue;
            static const int cols[7] = {0, 1, 2, 5, 7, 4, 3};
            int f = hs(x, y, 8) % 2, c = cols[hs(x, y, 6) % 7], ks[2] = {i, j};
            for (int p = 0; p < 2; p++) {
                int k = ks[p];
                H[k] = .8;
                TY[k] = 2;
                BLK[k] = 1;
                P[k] = c;
                CAX[k] = ax;
                CF[k] = f;
                CP[k] = p;
            }
        }
    /* the eight data shards */
    int ns = 0;
    for (int s = 0; ns < 8; s++) {
        int x = 3 + hs(s, 1, 9) % (N - 6), y = 3 + hs(s, 2, 9) % (N - 6), i = y * N + x;
        if (FL[i] == 0 && !BLK[i] && hypot(x, y) > 8) {
            H[i] = 1.3;
            TY[i] = 3;
            P[i] = 13;
            shards[ns++] = i;
        }
    }
}

/* ---------- state ---------- */
static double px = 1.5, py = 1.5, ang = .78, pitch = 0, T = 0, flash = 0;
static int got = 0, rain = 0;
static uint16_t CHs[MAXCOLS * MAXROWS];
static int16_t CLs[MAXCOLS * MAXROWS];
static int drops[400][2], ndrops = 0;

static void put(int x, int r, int ch, int c) {
    if (x < 0 || x >= COLS || r < 0 || r >= ROWS) return;
    CHs[r * COLS + x] = (uint16_t)ch;
    CLs[r * COLS + x] = (int16_t)c;
}

/* a pixel produced by one of the object shaders */
typedef struct {
    int ch, pal, off, glow;
} Hit;
static void setHit(Hit *h, int ch, int pal, int off, int glow) {
    h->ch = ch;
    h->pal = pal;
    h->off = off;
    h->glow = glow;
}

/* generic ray-vs-box in local space */
static int slab(const double *o, const double *d, const double *b, double *t, int *fa, int *fs) {
    double tn = -1e9, tf = 1e9;
    int a_ = 0, s_ = 0;
    for (int a = 0; a < 3; a++) {
        double lo = b[a * 2], hi = b[a * 2 + 1];
        if (fabs(d[a]) < 1e-9) {
            if (o[a] < lo || o[a] > hi) return 0;
            continue;
        }
        double t1 = (lo - o[a]) / d[a], t2 = (hi - o[a]) / d[a];
        int sg = d[a] > 0 ? -1 : 1;
        if (t1 > t2) {
            double tmp = t1;
            t1 = t2;
            t2 = tmp;
        }
        if (t1 > tn) {
            tn = t1;
            a_ = a;
            s_ = sg;
        }
        if (t2 < tf) tf = t2;
    }
    if (tn <= tf && tn > 0) {
        *t = tn;
        *fa = a_;
        *fs = s_;
        return 1;
    }
    return 0;
}

/* data shard: a small glowing gold crystal floating and bobbing above the street */
static int crystalHit(int i, int x, int r, double rdx, double rdy, double dz, double d, Hit *h) {
    double ox = px - ((i % N) + .5), oy = py - ((i / N) + .5), zc = .75 + .08 * sin(T * 2 + i), R = .3;
    for (double t = d; t < d + 1.6; t += .07) {
        double f = fabs(ox + t * rdx) + fabs(oy + t * rdy) + fabs(EYE + t * dz - zc) * 1.4;
        if (f < R) {
            double e = f / R;
            if (e < .35) setHit(h, '@', 14, 0, 0);
            else if (e > .75) setHit(h, '#', 13, 0, 0);
            else setHit(h, (hs(x, r, 5) & 1) ? '@' : '8', 13, 0, 0);
            return 1;
        }
    }
    return 0;
}

/* waist-high wooden fence: a centre post plus two rails reaching each neighbouring fence cell */
static int fenceHit(int i, double rdx, double rdy, double dz, Hit *h) {
    double o[3] = {px - ((i % N) + .5), py - ((i / N) + .5), EYE}, d[3] = {rdx, rdy, dz};
    double bx[9][6] = {{-.12, .12, -.12, .12, 0, .3}};
    int nb = 1;
    static const int nbr[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    static const double zr[2][2] = {{.07, .14}, {.19, .27}};
    for (int k = 0; k < 4; k++) {
        int ex = nbr[k][0], ey = nbr[k][1], ni = i + ex + ey * N;
        if (TY[ni] != 4) continue;
        for (int m = 0; m < 2; m++) {
            double *b = bx[nb++];
            if (ex) {
                b[0] = ex > 0 ? 0 : -.5; b[1] = ex > 0 ? .5 : 0; b[2] = -.04; b[3] = .04;
            } else {
                b[0] = -.04; b[1] = .04; b[2] = ey > 0 ? 0 : -.5; b[3] = ey > 0 ? .5 : 0;
            }
            b[4] = zr[m][0];
            b[5] = zr[m][1];
        }
    }
    double bt = 1e9, t;
    int bk = 0, bf = 0, fa, fs;
    for (int k = 0; k < nb; k++)
        if (slab(o, d, bx[k], &t, &fa, &fs) && t < bt) {
            bt = t;
            bk = k ? 1 : 0;
            bf = fa;
        }
    if (bt >= 1e9) return 0;
    if (bk == 0) {
        if (bf == 2) setHit(h, '=', 15, 0, 0);
        else setHit(h, '#', 15, bf == 0 ? 0 : 1, 0);
    } else {
        if (bf == 2) setHit(h, '-', 15, 0, 0);
        else setHit(h, '=', 15, 1, 0);
    }
    return 1;
}

/* ornate double street lamp: base, pole, arm with two glowing lanterns. 7th value: 1 = glass */
static double LB[12][7];
static int nLB = 0;
static void initLamp(void) {
    static const double base[4][7] = {{-.2, .2, -.2, .2, 0, .2, 0},
                                      {-.14, .14, -.14, .14, .2, .42, 0},
                                      {-.036, .036, -.036, .036, 2.3, 2.58, 0},
                                      {-.36, .36, -.03, .03, 2.16, 2.22, 0}};
    for (int k = 0; k < 4; k++) memcpy(LB[nLB++], base[k], sizeof base[k]);
    for (int s = 1; s >= -1; s -= 2) {
        double m[4][5] = {{.31, .37, -.03, .03, 2.06}, {.24, .44, -.1, .1, 2.0}, {.28, .4, -.06, .06, 1.66},
                          {.26, .42, -.08, .08, 1.72}};
        double z1[4] = {2.34, 2.06, 1.72, 2.0};
        for (int k = 0; k < 4; k++) {
            double *b = LB[nLB++];
            b[0] = s > 0 ? m[k][0] : -m[k][1];
            b[1] = s > 0 ? m[k][1] : -m[k][0];
            b[2] = m[k][2];
            b[3] = m[k][3];
            b[4] = m[k][4];
            b[5] = z1[k];
            b[6] = k == 3;
        }
    }
}
static int lampHit(int i, int x, int r, double rdx, double rdy, double dz, Hit *h) {
    double ox = px - ((i % N) + .5), oy = py - ((i / N) + .5), dx = rdx, dy = rdy, tmp;
    if (TV[i]) {
        tmp = ox; ox = oy; oy = tmp;
        tmp = dx; dx = dy; dy = tmp;
    }
    double o[3] = {ox, oy, EYE}, d[3] = {dx, dy, dz}, bt = 1e9, t;
    int bb = -1, bf = 0, pole = 0, fa, fs;
    for (int k = 0; k < nLB; k++)
        if (slab(o, d, LB[k], &t, &fa, &fs) && t < bt) {
            bt = t;
            bb = k;
            bf = fa;
        }
    double A = dx * dx + dy * dy, B = 2 * (ox * dx + oy * dy), D = B * B - 4 * A * (ox * ox + oy * oy - .0025);
    if (D >= 0 && A > 1e-9) {
        t = (-B - sqrt(D)) / (2 * A);
        double z = EYE + t * dz;
        if (t > 0 && t < bt && z > .42 && z < 2.3) {
            bt = t;
            pole = 1;
            bb = -1;
        }
    }
    if (bt >= 1e9) return 0;
    if (pole) {
        setHit(h, (r & 1) ? 'I' : '|', 15, 0, 0);
        return 1;
    }
    if (LB[bb][6] != 0) {
        double hx = ox + bt * dx, hy = oy + bt * dy, cx = fabs(hx) - .34;
        if (bf == 2) {
            setHit(h, '8', 1, 0, 1);
            return 1;
        }
        if (bf == 0 ? fabs(hy) > .065 : fabs(cx) > .065) {
            setHit(h, '|', 15, 0, 0);
            return 1;
        }
        int f = hs(x, r, (int)(T * 5));
        setHit(h, "8@O0%"[f % 5], f < 400 ? 1 : 0, 0, 1);
        return 1;
    }
    setHit(h, bf == 2 ? '=' : '#', 15, 1, 0);
    return 1;
}

/* five tree kinds: ellipsoid crowns {dx,dy,dz,radiusXY,radiusZ} + trunk radius
 * 0 oak, 1 tall narrow cypress, 2 low bush, 3 birch (white trunk), 4 autumn tree (orange/red) */
static const struct {
    int n;
    double s[3][5];
    double tr;
} TT[5] = {{3, {{0, 0, 0, .4, .4}, {.2, -.1, .35, .3, .3}, {-.18, .15, .25, .3, .3}}, .08},
           {2, {{0, 0, 0, .25, .8}, {0, 0, .7, .15, .4}}, .06},
           {2, {{0, 0, 0, .42, .3}, {.1, .08, .15, .3, .25}}, 0},
           {3, {{0, 0, 0, .3, .45}, {.12, -.08, .4, .22, .3}, {-.1, .1, .3, .2, .3}}, .05},
           {3, {{0, 0, 0, .42, .42}, {.2, -.1, .35, .3, .3}, {-.18, .15, .25, .3, .3}}, .08}};

static int treeHit(int i, int x, int r, double dx, double dy, double dz, Hit *h) {
    int ty = TV[i];
    double ox = px - ((i % N) + .5), oy = py - ((i / N) + .5), zc = TZ[i];
    double A = dx * dx + dy * dy, B = 2 * (ox * dx + oy * dy), bt = 1e9, nz = 0;
    int kind = 0;
    if (TT[ty].tr > 0) {
        double D = B * B - 4 * A * (ox * ox + oy * oy - TT[ty].tr * TT[ty].tr);
        if (D >= 0 && A > 1e-9) {
            double t = (-B - sqrt(D)) / (2 * A), z = EYE + t * dz;
            if (t > 0 && z >= 0 && z <= zc) {
                bt = t;
                kind = 1;
            }
        }
    }
    for (int k = 0; k < TT[ty].n; k++) {
        const double *s = TT[ty].s[k];
        double q = s[3] / s[4], lx = ox - s[0], ly = oy - s[1], lz = (EYE - (zc + s[2])) * q, e = dz * q;
        double a = dx * dx + dy * dy + e * e, b = 2 * (lx * dx + ly * dy + lz * e),
               c = lx * lx + ly * ly + lz * lz - s[3] * s[3], dd = b * b - 4 * a * c;
        if (dd >= 0) {
            double t = (-b - sqrt(dd)) / (2 * a);
            if (t > 0 && t < bt) {
                bt = t;
                kind = 2;
                nz = (lz + t * e) / s[3];
            }
        }
    }
    if (!kind) return 0;
    if (kind == 1) {
        int f = hs(x, r, 4) < 350;
        if (ty == 3) setHit(h, f ? 'I' : '|', f ? 6 : 14, 0, 0);
        else setHit(h, (r & 1) ? '|' : 'I', 15, 0, 0);
        return 1;
    }
    double br = nz * .5 + .5 + (hs(x, r, 5) - 500) / 1500.0;
    int ch = "B8&@%#*;"[hs(x, r, 6) % 8], o = br < .3 ? 2 : br < .5 ? 1 : 0, p;
    if (ty == 1) p = br > .6 ? 3 : 9;
    else if (ty == 2) p = br > .7 ? 16 : 3;
    else if (ty == 3) p = br > .55 ? 16 : br > .3 ? 3 : 9;
    else if (ty == 4) p = br > .75 ? 1 : br > .4 ? 0 : 7;
    else p = hs(x, r, 7) < 40 ? 4 : br > .78 ? 16 : br > .45 ? 3 : 9;
    setHit(h, ch, p, ty == 1 ? imin(2, o + 1) : o, 0);
    return 1;
}

/* real car geometry in local space: +lx = front, ly = across, z = up */
static const double CBOX[2][6] = {{-.95, .95, -.4, .4, .14, .46}, {-.62, .38, -.36, .36, .46, .8}};
static const double WH[4][2] = {{.6, 1}, {.6, -1}, {-.6, 1}, {-.6, -1}};

static int carHit(int i, int x, int r, double rdx, double rdy, double dz, Hit *out) {
    int ax = CAX[i], cp = CP[i], X = i % N, Y = i / N;
    double cx, cy, tmp;
    if (ax == 0) { cx = X - cp + 1; cy = Y + .5; }
    else { cx = X + .5; cy = Y - cp + 1; }
    double ox = px - cx, oy = py - cy, dx = rdx, dy = rdy;
    if (ax == 1) {
        tmp = ox; ox = oy; oy = tmp;
        tmp = dx; dx = dy; dy = tmp;
    }
    if (!CF[i]) { ox = -ox; dx = -dx; }
    double o[3] = {ox, oy, EYE}, d[3] = {dx, dy, dz}, bt = 1e9, t, bw = 0;
    int bk = -1, bf = 0, bs = 0, fa, fs;
    for (int k = 0; k < 2; k++) {
        double b[6];
        memcpy(b, CBOX[k], sizeof b);
        if (slab(o, d, b, &t, &fa, &fs) && t < bt) {
            bt = t;
            bk = k;
            bf = fa;
            bs = fs;
        }
    }
    for (int w = 0; w < 4; w++) {
        double wc = WH[w][0], sy = WH[w][1];
        double lo = fmin(sy * .33, sy * .43), hi = fmax(sy * .33, sy * .43);
        double ex = ox - wc, ez = EYE - .17, A = dx * dx + dz * dz, B = 2 * (ex * dx + ez * dz),
               C = ex * ex + ez * ez - .0289, D = B * B - 4 * A * C;
        if (D >= 0 && A > 1e-9) {
            t = (-B - sqrt(D)) / (2 * A);
            double yy = oy + t * dy;
            if (t > 0 && t < bt && yy >= lo && yy <= hi) {
                bt = t;
                bk = 2;
                bw = wc;
            }
        }
        if (fabs(dy) > 1e-9) {
            t = ((sy > 0 ? hi : lo) - oy) / dy;
            double e1 = ex + t * dx, e2 = ez + t * dz;
            if (t > 0 && t < bt && e1 * e1 + e2 * e2 < .0289) {
                bt = t;
                bk = 3;
                bw = wc;
            }
        }
    }
    if (bk < 0) return 0;
    double hx = ox + bt * dx, hy = oy + bt * dy, hz = EYE + bt * dz, ay = fabs(hy);
    int col = P[i];
    int glassA = hs(x, r, 3) < 200; /* window: reflection or dark glass */
    if (bk == 2) { setHit(out, 'O', 6, 3, 0); return 1; }
    if (bk == 3) {
        double rr = hypot(hx - bw, hz - .17);
        if (rr < .04) setHit(out, '*', 6, 0, 0);
        else if (rr < .1) setHit(out, 'o', 6, 0, 0);
        else setHit(out, 'O', 6, 3, 0);
        return 1;
    }
    int fr = bf == 0 && bs > 0;
    if (bk == 0) {
        if (bf == 0) {
            if (ay < .13 && hz > .16 && hz < .26) setHit(out, "W115"[x & 3], 14, 0, 0);
            else if (hz < .22) setHit(out, '=', 6, 0, 0);
            else if (hz > .26 && hz < .4 && ay > .17 && ay < .36) { if (fr) setHit(out, '@', 1, 0, 0); else setHit(out, '#', 7, 0, 0); }
            else if (fr && ay < .14 && hz > .26 && hz < .4) setHit(out, (x & 1) ? '#' : '+', 6, 1, 0);
            else setHit(out, (r & 1) ? '=' : '#', col, 0, 0);
            return 1;
        }
        if (bf == 1) {
            if (hz < .2) setHit(out, '_', col, 3, 0);
            else if (hx > .88 && hz > .26 && hz < .4) setHit(out, '@', 1, 0, 0);
            else if (hx < -.88 && hz > .26 && hz < .4) setHit(out, '#', 7, 0, 0);
            else if (fabs(hx - .15) < .03 || fabs(hx + .45) < .03) setHit(out, '|', col, 2, 0);
            else if (hz > .41) setHit(out, '-', 6, 1, 0);
            else setHit(out, (r & 1) ? '=' : '#', col, 1, 0);
            return 1;
        }
        if (hx > .8 && ay < .05) setHit(out, '*', 6, 0, 0);
        else setHit(out, '=', col, 0, 0);
        return 1;
    }
    if (bf == 1) {
        if (hx > -.5 && hx < .28 && hz > .52 && hz < .76 && fabs(hx + .1) > .03) {
            if (glassA) setHit(out, '/', 4, 0, 0); else setHit(out, '#', 4, 3, 0);
        } else setHit(out, '|', col, 1, 0);
        return 1;
    }
    if (bf == 0) {
        if (ay < .3 && hz > .5 && hz < .76) { if (glassA) setHit(out, '/', 4, 0, 0); else setHit(out, '#', 4, 3, 0); }
        else setHit(out, '=', col, 1, 0);
        return 1;
    }
    setHit(out, '=', col, 0, 0);
    return 1;
}

/* the moon: fixed direction in the sky and its tangent basis */
static double MX, MY, MZ, AX_, AY_, BX_, BY_, BZ_;
static void initMoon(void) {
    double MA = 2.4, ME = .45;
    MX = cos(MA) * cos(ME);
    MY = sin(MA) * cos(ME);
    MZ = sin(ME);
    double MC = hypot(MX, MY);
    AX_ = -MY / MC;
    AY_ = MX / MC;
    BX_ = -MZ * MX / MC;
    BY_ = -MZ * MY / MC;
    BZ_ = MC;
}

/* one sky cell: stars on a fixed dome, and the waxing crescent moon */
static void skyCell(int x, int r, double Dx, double Dy, double Dz, double len) {
    double el = asin(clampd(Dz / len, -1, 1));
    int eb = (int)floor(el / .025);
    int nA = (int)floor(6.283 * cos(eb * .025) / .0133);
    if (nA < 1) nA = 1;
    int ab = (int)floor((atan2(Dy, Dx) + 3.14159265358979) / 6.283 * nA) % nA;
    int band = fabs((Dx * .3 + Dy * .5 + Dz * .8) / len) < .12, th = band ? 60 : 20;
    if (hs(ab, eb, 3) < th && !(hs(ab, eb, 6) < 150 && (((int)(T * 3)) + ab) % 3 == 0)) {
        int b = hs(ab, eb, 4), t = hs(ab, eb, 5);
        int pal = t < 200 ? 4 : t < 330 ? 1 : 14, lv = (b < 100 ? 0 : b < 450 ? 1 : 2) + (el < .12 ? 1 : 0);
        put(x, r, b < 100 ? '*' : b < 450 ? '+' : '.', ci(pal, lv));
    }
    if ((Dx * MX + Dy * MY + Dz * MZ) / len > .9) {
        double u = (Dx * AX_ + Dy * AY_) / len, v = (Dx * BX_ + Dy * BY_ + Dz * BZ_) / len, dd = hypot(u, v);
        if (dd <= .125) {
            if (hypot(u + .055, v) > .125) {
                int cr = hs((int)floor(u / .0133), (int)floor(v / .025), 9) < 130;
                put(x, r, cr ? 'o' : dd > .1 ? '0' : '@', ci(13, cr ? 2 : 0));
            } else if (dd > .108) put(x, r, '.', ci(14, 3));
        }
    }
}

/* Every character cell casts its own true 3D ray (yaw + pitch), so looking up rotates the camera. */
static void render(void) {
    for (int i = 0; i < COLS * ROWS; i++) CLs[i] = -1;
    double dx = cos(ang), dy = sin(ang), sp = sin(pitch), cp = cos(pitch), MAXT = 48, MAXH = 40;
    for (int x = 0; x < COLS; x++) {
        double rg = (2.0 * x / COLS - 1) * .8;
        for (int r = 0; r < ROWS; r++) {
            double up = (ROWS / 2.0 - r - .5) / K;
            double Dx = dx * cp - dy * rg - dx * sp * up, Dy = dy * cp + dx * rg - dy * sp * up, Dz = sp + cp * up;
            double len = sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
            double hx = Dx / len, hy = Dy / len, hz = Dz / len, hl = hypot(hx, hy);
            double tlim = MAXT, tg = 0;
            int ground = 0;
            if (hz < -1e-6) {
                tg = -EYE / hz;
                if (tg < tlim) { tlim = tg; ground = 1; }
            } else if (hz > 1e-6) {
                double ts = (MAXH - EYE) / hz;
                if (ts < tlim) tlim = ts;
            }
            int mx = (int)px, my = (int)py;
            double ddx = fabs(1 / hx), ddy = fabs(1 / hy);
            int stx = hx < 0 ? -1 : 1, sty = hy < 0 ? -1 : 1, done = 0;
            double sdx = (hx < 0 ? px - mx : mx + 1 - px) * ddx, sdy = (hy < 0 ? py - my : my + 1 - py) * ddy;
            for (int n = 0; n < 120; n++) {
                double tn;
                int side;
                if (sdx < sdy) { tn = sdx; sdx += ddx; mx += stx; side = 0; }
                else { tn = sdy; sdy += ddy; my += sty; side = 1; }
                if (tn > tlim || mx < 0 || my < 0 || mx >= N || my >= N) break;
                int i = my * N + mx;
                double h = H[i];
                if (h <= 0) continue;
                double tx = fmin(sdx, sdy), zi = EYE + tn * hz, zo = EYE + tx * hz;
                if (fmin(zi, zo) > h) continue;
                int ty = TY[i], l = (int)(tn * hl / 7) + side, ch, c;
                Hit q;
                if (ty == 1) { if (!treeHit(i, x, r, hx, hy, hz, &q)) continue; ch = q.ch; c = ci(q.pal, l + q.off); }
                else if (ty == 2) { if (!carHit(i, x, r, hx, hy, hz, &q)) continue; ch = q.ch; c = ci(q.pal, l + q.off); }
                else if (ty == 3) { if (!crystalHit(i, x, r, hx, hy, hz, tn, &q)) continue; ch = q.ch; c = ci(q.pal, imin(1, l)); }
                else if (ty == 4) { if (!fenceHit(i, hx, hy, hz, &q)) continue; ch = q.ch; c = ci(15, l + q.off); }
                else if (ty == 5) { if (!lampHit(i, x, r, hx, hy, hz, &q)) continue; ch = q.ch; c = ci(q.pal, q.glow ? imin(1, l) : l + q.off); }
                else {
                    if (zi > h) continue;
                    double w = side == 0 ? py + tn * hy : px + tn * hx, u = w - floor(w), iy = zi / 1.3;
                    int ix = (int)(u * 3), jy = (int)iy;
                    if (u < .05 || u > .95) { ch = '|'; c = ci(P[i], l); }
                    else if (iy - jy < .2) { ch = '='; c = ci(P[i], l + 2); }
                    else {
                        int lit = hs(i, side * 7 + ix, jy) < 520;
                        ch = lit ? "OXZ8"[hs(ix, jy, i) % 4] : '.';
                        c = ci(P[i], l + (lit ? 0 : 2));
                    }
                }
                put(x, r, ch, c);
                done = 1;
                break;
            }
            if (done) continue;
            if (!ground) { skyCell(x, r, Dx, Dy, Dz, len); continue; }
            /* the ground */
            double wx = px + hx * tg, wy = py + hy * tg;
            int gx = (int)wx, gy = (int)wy;
            if (wx < 0 || wy < 0 || gx >= N || gy >= N) continue;
            int i = gy * N + gx, l = imin(4, (int)(tg * hl / 8)), f = FL[i], ch, c;
            double fx = wx - gx, fy = wy - gy;
            if (f == 0) {
                int rx = gx % 11, ry = gy % 11;
                if (rx == 1 && ry > 2 && fx > .35 && fx < .65 && ((int)(fy * 2)) == 0) { ch = '|'; c = ci(1, l + 1); }
                else if (ry == 1 && rx > 2 && fy > .35 && fy < .65 && ((int)(fx * 2)) == 0) { ch = '-'; c = ci(1, l + 1); }
                else { ch = hs(gx * 3 + (int)(fx * 3), gy * 3 + (int)(fy * 3), 0) < 500 ? '.' : ':'; c = ci(8, l); }
            } else if (f == 1) {
                ch = hs(gx, gy, (int)(fx * 2) + (int)(fy * 2) * 2) < 300 ? '-' : '_';
                c = ci(6, l + 1);
            } else if (f == 2) {
                ch = hs(gx, gy, (int)(fx * 3) + (int)(fy * 3)) < 500 ? '"' : ',';
                c = ci(9, l);
            } else if (f == 3) {
                ch = ((int)(fx * 4 + fy * 4 + T * 2)) % 3 ? '~' : 0x2248;
                c = ci(10, l);
            } else {
                ch = ((int)(fy * 4)) % 2 ? '=' : '#';
                c = ci(11, l);
            }
            if (LIT[i] && f != 3) c = ci(13, imin(4, l + 3 - LIT[i]));
            put(x, r, ch, c);
        }
    }
    if (rain)
        for (int k = 0; k < ndrops; k++) {
            drops[k][1] = (drops[k][1] + 2) % ROWS;
            drops[k][0] = (drops[k][0] + (rand() % 10 < 3 ? 1 : 0)) % COLS;
            put(drops[k][0], drops[k][1], '/', ci(4, 1));
        }
}

/* ---------- terminal output ---------- */
static int truecolor = 0;
static char FGS[NPAL * 5][24];
static char OB[700000];
static int ol = 0;

static int q6(int v) { return (int)floor(v / 51.0 + .5); }
static void colSeq(char *dst, int bg, int r, int g, int b) {
    if (truecolor) sprintf(dst, "\x1b[%d;2;%d;%d;%dm", bg ? 48 : 38, r, g, b);
    else sprintf(dst, "\x1b[%d;5;%dm", bg ? 48 : 38, 16 + 36 * q6(r) + 6 * q6(g) + q6(b));
}
static void ap(const char *s) {
    size_t n = strlen(s);
    memcpy(OB + ol, s, n);
    ol += (int)n;
}
static void apch(int ch) {
    if (ch == 0x2248) { OB[ol++] = (char)0xe2; OB[ol++] = (char)0x89; OB[ol++] = (char)0x88; }
    else OB[ol++] = (char)ch;
}

static void present(void) {
    double sp = sin(pitch), cp = cos(pitch);
    char buf[64], hud[200];
    ol = 0;
    ap("\x1b[H");
    for (int r = 0; r < ROWS; r++) {
        double up = (ROWS / 2.0 - r - .5) / K;
        if (r < HOR) { /* faint night sky, a touch lighter near the horizon */
            double el = asin(clampd((sp + cp * up) / sqrt(1 + up * up), 0, 1)), g = pow(fmax(0, 1 - el / 1.3), 1.6);
            colSeq(buf, 1, (int)(4 + 12 * g), (int)(3 + 5 * g), (int)(14 + 20 * g));
            ap(buf);
        } else ap("\x1b[40m");
        int cur = -1;
        for (int x = 0; x < COLS; x++) {
            int i = r * COLS + x, c = CLs[i];
            if (c < 0) { OB[ol++] = ' '; continue; }
            if (c != cur) { ap(FGS[c]); cur = c; }
            apch(CHs[i]);
        }
        ap("\x1b[0m\x1b[K\r\n");
    }
    snprintf(hud, sizeof hud, "SHARDS %d/8 (floating gold crystals)%s%s%s", got, flash > 0 ? "  Shard collected!" : "",
             got == 8 ? "  All shards found - the city hums back to life, for you alone." : "", rain ? "  rain" : "");
    ap("\x1b[0m\x1b[97m");
    if ((int)strlen(hud) > COLS) hud[COLS] = 0;
    ap(hud);
    ap("\x1b[K\r\n\x1b[90m");
    ap(COLS >= 80 ? " WASD move  \xe2\x86\x90\xe2\x86\x92 turn  \xe2\x86\x91\xe2\x86\x93 look  Shift run  F auto-walk  R rain  P quit"
                  : " WASD move  arrows turn/look  F auto  R rain  P quit");
    ap("\x1b[0m\x1b[K");
    size_t off = 0;
    while (off < (size_t)ol) {
        ssize_t w = write(STDOUT_FILENO, OB + off, (size_t)ol - off);
        if (w < 0) break;
        off += (size_t)w;
    }
}

/* ---------- input, physics, main loop ---------- */
enum { KW, KS, KA, KD, KQ, KE, KZ, KX, KL, KR, KSH, NK };
static double until[NK];
static int autowalk = 0, running = 1;
static volatile sig_atomic_t gotWinch = 1, gotQuit = 0;
static struct termios orig;
static int rawOn = 0;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void press(int k, double now) { until[k] = now + .24; }

static void restore(void) {
    if (rawOn) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
        rawOn = 0;
        const char *s = "\x1b[0m\x1b[?1049l\x1b[?25h";
        if (write(STDOUT_FILENO, s, strlen(s)) < 0) { /* nothing more to do */ }
    }
}
static void onSignal(int s) {
    if (s == SIGWINCH) gotWinch = 1;
    else gotQuit = 1;
}

static void resize(void) {
    struct winsize ws;
    int cols = 100, rows = 32;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    COLS = (int)clampd(cols - 1, 40, MAXCOLS);
    ROWS = (int)clampd(rows - 2, 12, MAXROWS);
    K = COLS * .3125; /* keeps square proportions: terminal cells are about twice as tall as wide */
    for (int i = 0; i < MAXCOLS * MAXROWS; i++) { CHs[i] = ' '; CLs[i] = -1; }
    ndrops = COLS * ROWS / 40;
    if (ndrops > 400) ndrops = 400;
    for (int k = 0; k < ndrops; k++) { drops[k][0] = rand() % COLS; drops[k][1] = rand() % ROWS; }
    if (write(STDOUT_FILENO, "\x1b[2J", 4) < 0) { /* ignore */ }
}

static int blocked(double x, double y) {
    int gx = (int)x, gy = (int)y;
    return gx < 0 || gy < 0 || gx >= N || gy >= N || BLK[gy * N + gx];
}
static double sgn(double v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

static void step(double dt, double now) {
    T += dt;
    flash -= dt;
    int on[NK];
    for (int k = 0; k < NK; k++) on[k] = now < until[k];
    if (autowalk) on[KW] = 1;
    pitch += ((on[KZ] ? 1 : 0) - (on[KX] ? 1 : 0)) * 1.4 * dt;
    pitch = clampd(pitch, -1.5708, 1.5708);
    HOR = (int)floor(ROWS / 2.0 + K * tan(clampd(pitch, -1.55, 1.55)) + .5);
    double speed = (on[KSH] ? 6 : 3.2) * dt;
    double fw = (on[KW] ? 1 : 0) - (on[KS] ? 1 : 0), sd = (on[KD] ? 1 : 0) - (on[KA] ? 1 : 0);
    double mx = cos(ang) * fw - sin(ang) * sd, my = sin(ang) * fw + cos(ang) * sd;
    double nx = px + mx * speed, ny = py + my * speed, rad = .25;
    if (!blocked(nx + sgn(mx) * rad, py) && !blocked(nx + sgn(mx) * rad, py + rad) && !blocked(nx + sgn(mx) * rad, py - rad)) px = nx;
    if (!blocked(px, ny + sgn(my) * rad) && !blocked(px + rad, ny + sgn(my) * rad) && !blocked(px - rad, ny + sgn(my) * rad)) py = ny;
    ang += ((on[KR] || on[KE] ? 1 : 0) - (on[KL] || on[KQ] ? 1 : 0)) * 2 * dt;
    for (int k = 0; k < 8; k++) {
        int s = shards[k];
        if (H[s] > 0 && hypot((s % N) + .5 - px, (s / N) + .5 - py) < 1.6) {
            H[s] = 0;
            TY[s] = 0;
            got++;
            flash = 2;
        }
    }
}

static void onInput(const unsigned char *s, int n, double now) {
    for (int i = 0; i < n;) {
        if (s[i] == 0x1b && i + 1 < n && (s[i + 1] == '[' || s[i + 1] == 'O')) {
            int j = i + 2;
            while (j < n && !((s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= 'a' && s[j] <= 'z') || s[j] == '~')) j++;
            if (j >= n) break;
            char f = (char)s[j], prev = j > i + 2 ? (char)s[j - 1] : 0;
            if (f == 'A') press(KZ, now);
            else if (f == 'B') press(KX, now);
            else if (f == 'C') press(KR, now);
            else if (f == 'D') press(KL, now);
            else if (f == '~' && prev == '5') press(KZ, now);
            else if (f == '~' && prev == '6') press(KX, now);
            i = j + 1;
            continue;
        }
        int ch = s[i++], lo = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
        if (ch == 3 || lo == 'p') { running = 0; return; }
        static const char keysS[] = "wsadqezx";
        const char *pk = lo ? strchr(keysS, lo) : NULL;
        if (pk) {
            press((int)(pk - keysS), now);
            if (ch != lo) press(KSH, now);
            if (lo == 's') autowalk = 0;
        } else if (lo == 'f') autowalk = !autowalk;
        else if (lo == 'r') rain = !rain;
    }
}

int main(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "ascii-city needs an interactive terminal.\n");
        return 1;
    }
    const char *ct = getenv("COLORTERM");
    truecolor = ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"));
    for (int p = 0; p < NPAL; p++)
        for (int l = 0; l < 5; l++)
            colSeq(FGS[p * 5 + l], 0, (int)(PAL[p][0] * FAC[l]), (int)(PAL[p][1] * FAC[l]), (int)(PAL[p][2] * FAC[l]));
    srand((unsigned)time(NULL));
    generate();
    initLamp();
    initMoon();

    tcgetattr(STDIN_FILENO, &orig);
    struct termios raw = orig;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    rawOn = 1;
    atexit(restore);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onSignal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);
    if (write(STDOUT_FILENO, "\x1b[?1049h\x1b[?25l", 14) < 0) { /* ignore */ }

    double last = now_s();
    while (running && !gotQuit) {
        double frameStart = now_s(), deadline = frameStart + 0.033;
        /* wait for input until it is time for the next frame */
        for (;;) {
            double rem = deadline - now_s();
            if (rem <= 0) break;
            struct pollfd pf = {STDIN_FILENO, POLLIN, 0};
            int pr = poll(&pf, 1, (int)(rem * 1000) + 1);
            if (pr > 0 && (pf.revents & POLLIN)) {
                unsigned char b[256];
                ssize_t n = read(STDIN_FILENO, b, sizeof b);
                if (n > 0) onInput(b, (int)n, now_s());
            }
            if (!running || gotQuit) break;
        }
        if (!running || gotQuit) break;
        if (gotWinch) { gotWinch = 0; resize(); }
        double now = now_s(), dt = now - last;
        if (dt > .1) dt = .1;
        last = now;
        step(dt, now);
        render();
        present();
    }
    return 0;
}
