#define _GNU_SOURCE
#include "ui.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

typedef struct { int x, y, w, h; } Rect;

enum { SORT_CPU, SORT_MEM, SORT_PID };

static const Scheme schemes[] = {
    { "default",   7,   7,   6, {   4,   3,   2,   1,   5,   6,   7,   8 },   5,  11,   4,   4,   2,   1 },
    { "ember",   252, 223, 130, { 208, 214, 220, 202, 196, 166, 172, 178 }, 208, 172,  94, 214, 150, 196 },
    { "monokai", 249, 250, 239, {  81,  70, 208, 197, 249, 141, 221, 186 }, 208, 186, 197,  81,  70, 208 },
    { "solarized", 250, 250, 37, { 61,  33,  37,  64, 125, 160, 166, 136 }, 125, 166, 136,  33,  64, 160 },
    { "nord",    253, 110,  60, { 110, 109, 108, 139, 179, 173, 167, 152 }, 139, 179,  60, 110, 108, 167 },
};
#define NSCHEMES (int)(sizeof schemes / sizeof schemes[0])

const Scheme *ui_find_scheme(const char *name)
{
    for (int i = 0; i < NSCHEMES; i++)
        if (!strcmp(schemes[i].name, name)) return &schemes[i];
    return NULL;
}

void ui_list_schemes(void)
{
    for (int i = 0; i < NSCHEMES; i++) printf("%s\n", schemes[i].name);
}

static UIOpts O;
static const Scheme *SC;

/* process table state */
static int sort_by = SORT_CPU;
static int grouped;
static int sel, top;
static int sel_pid = -1;
static char sel_name[64];
static Proc *view;
static int nview, viewcap;
static char filter[64];
static int filtering;
static int show_help;
static char flash[160];
static double flash_until;
static int last_key;
static double last_key_t;
static Rect proc_rect, proc_rows;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void ui_init(const UIOpts *opts)
{
    O = *opts;
    SC = O.scheme;
    if (O.hscale < 1) O.hscale = 1;
}

/* ---------- formatting ---------- */

static void fmt_bytes(double b, char *out, size_t n)
{
    static const char *u[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    int i = 0;
    while (b >= 1024 && i < 5) { b /= 1024; i++; }
    if (i == 0) snprintf(out, n, "%.0f %s", b, u[i]);
    else snprintf(out, n, "%.1f %s", b, u[i]);
}

static void fmt_bytes_short(double b, char *out, size_t n)
{
    static const char u[] = "BKMGTP";
    int i = 0;
    while (b >= 1024 && i < 5) { b /= 1024; i++; }
    if (i == 0 || b >= 100) snprintf(out, n, "%.0f%c", b, u[i]);
    else snprintf(out, n, "%.1f%c", b, u[i]);
}

/* ---------- drawing primitives ---------- */

static Rect inner(Rect r) { return (Rect){ r.x + 1, r.y + 1, r.w - 2, r.h - 2 }; }

static int box(Rect r, const char *title)
{
    if (r.w < 3 || r.h < 3) return 0;
    int c = SC->border_line;
    for (int x = r.x + 1; x < r.x + r.w - 1; x++) {
        fb_put(x, r.y, 0x2500, c, -1, 0);
        fb_put(x, r.y + r.h - 1, 0x2500, c, -1, 0);
    }
    for (int y = r.y + 1; y < r.y + r.h - 1; y++) {
        fb_put(r.x, y, 0x2502, c, -1, 0);
        fb_put(r.x + r.w - 1, y, 0x2502, c, -1, 0);
    }
    fb_put(r.x, r.y, 0x250C, c, -1, 0);
    fb_put(r.x + r.w - 1, r.y, 0x2510, c, -1, 0);
    fb_put(r.x, r.y + r.h - 1, 0x2514, c, -1, 0);
    fb_put(r.x + r.w - 1, r.y + r.h - 1, 0x2518, c, -1, 0);
    if (title) fb_text(r.x + 2, r.y, title, r.w - 4, SC->border_label, -1, 0);
    return 1;
}

/* text clipped to w columns, optionally right-aligned */
static void cell(int x, int y, int w, const char *s, int right, int fg, int bg, int attr)
{
    if (w <= 0) return;
    int len = utf8_len(s);
    if (len > w) len = w;
    fb_text(right ? x + w - len : x, y, s, w, fg, bg, attr);
}

/* ---------- braille canvas ---------- */

static struct { int w, h, cap; uint8_t *m; int16_t *c; } cv;

static void cv_reset(int w, int h)
{
    int n = w * h;
    if (n > cv.cap) {
        cv.cap = n;
        cv.m = realloc(cv.m, (size_t)n);
        cv.c = realloc(cv.c, (size_t)n * sizeof(int16_t));
        if (!cv.m || !cv.c) abort();
    }
    cv.w = w;
    cv.h = h;
    memset(cv.m, 0, (size_t)n);
}

static void cv_dot(int px, int py, int col)
{
    static const uint8_t bits[4][2] = { { 0x01, 0x08 }, { 0x02, 0x10 }, { 0x04, 0x20 }, { 0x40, 0x80 } };
    if (px < 0 || py < 0 || px >= cv.w * 2 || py >= cv.h * 4) return;
    int i = (py / 4) * cv.w + px / 2;
    cv.m[i] |= bits[py % 4][px % 2];
    cv.c[i] = (int16_t)col;
}

static void cv_line(int x0, int y0, int x1, int y1, int col)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        cv_dot(x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void cv_draw(int ox, int oy)
{
    for (int y = 0; y < cv.h; y++)
        for (int x = 0; x < cv.w; x++) {
            int i = y * cv.w + x;
            if (cv.m[i]) fb_put(ox + x, oy + y, 0x2800u + cv.m[i], cv.c[i], -1, 0);
        }
}

static void linegraph(Rect in, const Series *const *ser, const int *cols, int n, double maxv)
{
    if (in.w < 1 || in.h < 1) return;
    cv_reset(in.w, in.h);
    int W = in.w * 2, H = in.h * 4;
    for (int i = 0; i < n; i++) {
        const Series *S = ser[i];
        int px = 0, py = 0;
        for (int k = 0; k < S->len; k++) {
            double v = S->v[S->len - 1 - k];
            if (v < 0) v = 0;
            if (v > maxv) v = maxv;
            int x = W - 1 - k * O.hscale;
            int y = H - 1 - (int)lround(v / maxv * (H - 1));
            if (k == 0) cv_dot(x, y, cols[i]);
            else cv_line(px, py, x, y, cols[i]);
            px = x;
            py = y;
            if (x < 0) break;
        }
    }
    cv_draw(in.x, in.y);
}

static void sparkline(Rect r, const Series *S, int col)
{
    if (r.w < 1 || r.h < 1 || S->len == 0) return;
    int n = S->len < r.w ? S->len : r.w;
    double mx = 0;
    for (int k = 0; k < n; k++)
        if (S->v[S->len - 1 - k] > mx) mx = S->v[S->len - 1 - k];
    if (mx <= 0) return;
    for (int k = 0; k < n; k++) {
        double v = S->v[S->len - 1 - k];
        int eighths = (int)lround(v / mx * r.h * 8);
        if (v > 0 && eighths == 0) eighths = 1;
        int x = r.x + r.w - 1 - k;
        for (int j = 0; j < r.h; j++) {
            int f = eighths - j * 8;
            if (f <= 0) break;
            fb_put(x, r.y + r.h - 1 - j, f >= 8 ? 0x2588u : 0x2580u + (uint32_t)f, col, -1, 0);
        }
    }
}

/* ---------- widgets ---------- */

static void w_cpu(Rect r, Stats *s)
{
    if (!box(r, " CPU Usage ")) return;
    Rect in = inner(r);

    static const Series **ser;
    static int *cols;
    static int cap;
    if (cap < s->ncpu + 1) {
        cap = s->ncpu + 1;
        ser = realloc(ser, sizeof *ser * (size_t)cap);
        cols = realloc(cols, sizeof *cols * (size_t)cap);
        if (!ser || !cols) abort();
    }
    int n = 0;
    if (O.show_percpu)
        for (int i = 0; i < s->ncpu; i++) {
            ser[n] = &s->cpu_h[i];
            cols[n++] = SC->cpu_lines[i % 8];
        }
    int avg_col = O.show_percpu ? SC->fg : SC->cpu_lines[0];
    if (O.show_avg) {
        ser[n] = &s->cpu_avg_h;
        cols[n++] = avg_col;
    }
    linegraph(in, ser, cols, n, 100);

    int row = 0;
    char lbl[48];
    if (O.show_avg && row < in.h) {
        snprintf(lbl, sizeof lbl, "AVRG %3.0f%%", s->cpu_avg);
        fb_text(in.x + 1, in.y + row++, lbl, in.w - 1, avg_col, -1, 0);
    }
    if (O.show_percpu) {
        int digits = s->ncpu > 100 ? 3 : s->ncpu > 10 ? 2 : 1;
        /* lay labels out in columns when there are more cores than rows */
        int colw = 6 + digits + 5;
        int x = in.x + 1;
        for (int i = 0; i < s->ncpu; i++) {
            if (row >= in.h) {
                row = O.show_avg ? 1 : 0;
                x += colw;
                if (x + colw > in.x + in.w) break;
            }
            snprintf(lbl, sizeof lbl, "CPU%0*d %3.0f%%", digits, i, s->cpu[i]);
            fb_text(x, in.y + row++, lbl, in.x + in.w - x, SC->cpu_lines[i % 8], -1, 0);
        }
    }
}

static void w_mem(Rect r, Stats *s)
{
    if (!box(r, " Memory Usage ")) return;
    Rect in = inner(r);
    const Series *ser[2] = { &s->mem_h, &s->swap_h };
    int cols[2] = { SC->main_mem, SC->swap_mem };
    linegraph(in, ser, cols, 2, 100);

    char used[32], total[32], lbl[96];
    fmt_bytes((double)s->mem_used, used, sizeof used);
    fmt_bytes((double)s->mem_total, total, sizeof total);
    snprintf(lbl, sizeof lbl, "Main %3.0f%%  %s/%s",
             s->mem_total ? 100.0 * (double)s->mem_used / (double)s->mem_total : 0, used, total);
    if (in.h > 0) fb_text(in.x + 1, in.y, lbl, in.w - 1, SC->main_mem, -1, 0);
    fmt_bytes((double)s->swap_used, used, sizeof used);
    fmt_bytes((double)s->swap_total, total, sizeof total);
    snprintf(lbl, sizeof lbl, "Swap %3.0f%%  %s/%s",
             s->swap_total ? 100.0 * (double)s->swap_used / (double)s->swap_total : 0, used, total);
    if (in.h > 1) fb_text(in.x + 1, in.y + 1, lbl, in.w - 1, SC->swap_mem, -1, 0);
}

static void w_net(Rect r, Stats *s)
{
    char title[96];
    snprintf(title, sizeof title, " Network Usage%s%s ", s->iface ? ": " : "", s->iface ? s->iface : "");
    if (!box(r, title)) return;
    Rect in = inner(r);
    int top_h = in.h / 2;
    Rect halves[2] = { { in.x, in.y, in.w, top_h }, { in.x, in.y + top_h, in.w, in.h - top_h } };
    const char *names[2] = { "Rx", "Tx" };
    double totals[2] = { (double)s->rx_total, (double)s->tx_total };
    double rates[2] = { s->rx_rate, s->tx_rate };
    const Series *hist[2] = { &s->rx_h, &s->tx_h };

    for (int i = 0; i < 2; i++) {
        Rect h = halves[i];
        if (h.h < 1) continue;
        char a[32], b[32], line[96];
        fmt_bytes(totals[i], a, sizeof a);
        fmt_bytes(rates[i], b, sizeof b);
        int y = h.y;
        if (h.h >= 3) {
            snprintf(line, sizeof line, "Total %s: %s", names[i], a);
            fb_text(h.x + 1, y++, line, h.w - 1, SC->fg, -1, A_BOLD);
        }
        snprintf(line, sizeof line, "%s/s: %s/s", names[i], b);
        fb_text(h.x + 1, y++, line, h.w - 1, SC->fg, -1, 0);
        sparkline((Rect){ h.x, y, h.w, h.y + h.h - y }, hist[i], SC->sparkline);
    }
}

static void w_disk(Rect r, Stats *s)
{
    if (!box(r, " Disk Usage ")) return;
    Rect in = inner(r);
    if (in.h < 1 || in.w < 8) return;

    /* column widths, dropping columns right-to-left as space runs out */
    int wu = 5, wf = 7, wr = 7, ww = 7;
    int show_io = in.w >= 40, show_dev = in.w >= 52;
    int fixed = wu + wf + 2 + (show_io ? wr + ww + 2 : 0);
    int flex = in.w - fixed - 1;
    int wd = 0;
    if (show_dev) {
        wd = 4;
        for (int i = 0; i < s->ndisk; i++) {
            int l = utf8_len(s->disks[i].dev);
            if (l > wd) wd = l;
        }
        if (wd > flex / 2) wd = flex / 2;
        flex -= wd + 1;
    }
    int wm = flex < 1 ? 1 : flex;

    int y = in.y, x;
#define COLS(disk, mount, used, fr, rd, wt, attr) do {                          \
        x = in.x + 1;                                                           \
        if (show_dev) { cell(x, y, wd, disk, 0, SC->fg, -1, attr); x += wd + 1; } \
        cell(x, y, wm, mount, 0, SC->fg, -1, attr); x += wm + 1;                \
        cell(x, y, wu, used, 1, SC->fg, -1, attr); x += wu + 1;                 \
        cell(x, y, wf, fr, 1, SC->fg, -1, attr); x += wf + 1;                   \
        if (show_io) {                                                          \
            cell(x, y, wr, rd, 1, SC->fg, -1, attr); x += wr + 1;               \
            cell(x, y, ww, wt, 1, SC->fg, -1, attr);                            \
        }                                                                       \
    } while (0)

    COLS("Disk", "Mount", "Used", "Free", "R/s", "W/s", A_BOLD);
    y++;
    for (int i = 0; i < s->ndisk && y < in.y + in.h; i++, y++) {
        Disk *d = &s->disks[i];
        char used[16], fr[16], rd[16], wt[16];
        snprintf(used, sizeof used, "%.0f%%", d->used_pct);
        fmt_bytes_short((double)d->free, fr, sizeof fr);
        fmt_bytes_short(d->rps, rd, sizeof rd);
        fmt_bytes_short(d->wps, wt, sizeof wt);
        COLS(d->dev, d->mount, used, fr, rd, wt, 0);
    }
#undef COLS
}

static void w_temp(Rect r, Stats *s)
{
    if (!box(r, " Temperatures ")) return;
    Rect in = inner(r);
    if (in.h < 1) return;
    if (s->ntemp == 0) {
        fb_text(in.x + 1, in.y, "no sensors found", in.w - 1, SC->fg, -1, 0);
        return;
    }
    for (int i = 0; i < s->ntemp && i < in.h; i++) {
        Temp *t = &s->temps[i];
        double v = O.fahrenheit ? t->celsius * 9.0 / 5.0 + 32.0 : t->celsius;
        char val[24];
        snprintf(val, sizeof val, "%.0f°%c", v, O.fahrenheit ? 'F' : 'C');
        int vw = utf8_len(val);
        cell(in.x + 1, in.y + i, in.w - vw - 3, t->label, 0, SC->fg, -1, 0);
        cell(in.x + in.w - vw - 1, in.y + i, vw, val, 1,
             t->celsius >= 80 ? SC->temp_high : SC->temp_low, -1, 0);
    }
}

static int cmp_view(const void *a, const void *b)
{
    const Proc *p = a, *q = b;
    double d = 0;
    if (sort_by == SORT_CPU) d = q->cpu - p->cpu;
    else if (sort_by == SORT_MEM) d = q->mem - p->mem;
    if (d > 0) return 1;
    if (d < 0) return -1;
    return p->pid - q->pid;
}

static int matches(const Proc *p)
{
    if (!filter[0]) return 1;
    if (strcasestr(p->name, filter)) return 1;
    char pid[16];
    snprintf(pid, sizeof pid, "%d", p->pid);
    return strncmp(pid, filter, strlen(filter)) == 0;
}

static void build_view(Stats *s)
{
    if (viewcap < s->nproc) {
        viewcap = s->proccap;
        view = realloc(view, sizeof(Proc) * (size_t)viewcap);
        if (!view) abort();
    }
    nview = 0;
    for (int i = 0; i < s->nproc; i++) {
        const Proc *p = &s->procs[i];
        if (!matches(p)) continue;
        if (grouped) {
            int j;
            for (j = 0; j < nview; j++)
                if (!strcmp(view[j].name, p->name)) break;
            if (j < nview) {
                view[j].count++;
                view[j].cpu += p->cpu;
                view[j].mem += p->mem;
                if (p->pid < view[j].pid) view[j].pid = p->pid;
                continue;
            }
        }
        view[nview] = *p;
        view[nview].count = 1;
        nview++;
    }
    qsort(view, (size_t)nview, sizeof(Proc), cmp_view);

    /* keep the cursor on the same process (or group) across refreshes */
    for (int i = 0; i < nview; i++)
        if (grouped ? !strcmp(view[i].name, sel_name) : view[i].pid == sel_pid) {
            sel = i;
            break;
        }
    if (sel >= nview) sel = nview - 1;
    if (sel < 0) sel = 0;
}

static void remember_sel(void)
{
    if (sel >= 0 && sel < nview) {
        sel_pid = view[sel].pid;
        snprintf(sel_name, sizeof sel_name, "%s", view[sel].name);
    }
}

static void w_procs(Rect r, Stats *s)
{
    proc_rect = r;
    proc_rows = (Rect){ 0, 0, 0, 0 };
    if (!box(r, grouped ? " Processes (grouped) " : " Processes ")) return;
    Rect in = inner(r);
    build_view(s);
    if (in.h < 1 || in.w < 10) return;

    int wp = 7, wc = 6, wm = 6;
    int wn = in.w - wp - wc - wm - 4;
    if (wn < 1) wn = 1;
    const char *arrow = "▼";
    char hp[16], hc[16], hm[16];
    snprintf(hp, sizeof hp, "%s%s", grouped ? "Count" : "PID", sort_by == SORT_PID ? arrow : "");
    snprintf(hc, sizeof hc, "CPU%%%s", sort_by == SORT_CPU ? arrow : "");
    snprintf(hm, sizeof hm, "Mem%%%s", sort_by == SORT_MEM ? arrow : "");

    int x = in.x + 1, y = in.y;
    cell(x, y, wp, hp, 0, SC->fg, -1, A_BOLD);
    cell(x + wp + 1, y, wn, "Command", 0, SC->fg, -1, A_BOLD);
    cell(x + wp + wn + 2, y, wc, hc, 1, SC->fg, -1, A_BOLD);
    cell(x + wp + wn + wc + 3, y, wm, hm, 1, SC->fg, -1, A_BOLD);

    int rows = in.h - 1;
    proc_rows = (Rect){ in.x, in.y + 1, in.w, rows };
    if (rows < 1) return;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    if (top > nview - rows) top = nview - rows;
    if (top < 0) top = 0;

    for (int i = 0; i < rows && top + i < nview; i++) {
        Proc *p = &view[top + i];
        int ry = in.y + 1 + i;
        int bg = -1, fg = SC->fg;
        if (top + i == sel) {
            bg = SC->proc_cursor;
            fb_fill(in.x, ry, in.w, 1, bg);
        }
        char a[16], c[16], m[16];
        snprintf(a, sizeof a, "%d", grouped ? p->count : p->pid);
        snprintf(c, sizeof c, "%.1f", p->cpu);
        snprintf(m, sizeof m, "%.1f", p->mem);
        cell(x, ry, wp, a, 0, fg, bg, 0);
        cell(x + wp + 1, ry, wn, p->name, 0, fg, bg, 0);
        cell(x + wp + wn + 2, ry, wc, c, 1, fg, bg, 0);
        cell(x + wp + wn + wc + 3, ry, wm, m, 1, fg, bg, 0);
    }

    /* footer on the bottom border: filter prompt or a transient message */
    char foot[200] = "";
    if (filtering) snprintf(foot, sizeof foot, " /%s█ ", filter);
    else if (filter[0]) snprintf(foot, sizeof foot, " filter: %s (esc clears) ", filter);
    if (flash[0] && now_sec() < flash_until) snprintf(foot, sizeof foot, " %s ", flash);
    if (foot[0]) fb_text(r.x + 2, r.y + r.h - 1, foot, r.w - 4, SC->border_label, -1, A_BOLD);
}

static void statusbar(int y)
{
    char host[64] = "";
    gethostname(host, sizeof host - 1);
    time_t t = time(NULL);
    char clock[16];
    strftime(clock, sizeof clock, "%H:%M:%S", localtime(&t));
    struct sysinfo si;
    char up[48] = "";
    if (sysinfo(&si) == 0) {
        long u = si.uptime;
        long d = u / 86400, h = (u / 3600) % 24, m = (u / 60) % 60;
        if (d) snprintf(up, sizeof up, "up %ldd %ldh %ldm", d, h, m);
        else snprintf(up, sizeof up, "up %ldh %ldm", h, m);
    }
    fb_text(1, y, host, TW / 3, SC->fg, -1, 0);
    fb_text((TW - (int)strlen(clock)) / 2, y, clock, TW, SC->fg, -1, A_BOLD);
    int ul = (int)strlen(up);
    fb_text(TW - ul - 1, y, up, ul, SC->fg, -1, 0);
}

static const char *help_lines[] = {
    "Quit: q or <C-c>",
    "",
    "Process navigation",
    "  k and <Up>: up",
    "  j and <Down>: down",
    "  <C-u>: half page up",
    "  <C-d>: half page down",
    "  <C-b> and <PgUp>: full page up",
    "  <C-f> and <PgDn>: full page down",
    "  gg and <Home>: jump to top",
    "  G and <End>: jump to bottom",
    "  mouse: click to select, wheel to scroll",
    "",
    "Process actions",
    "  <Tab>: toggle grouping by name",
    "  dd: kill selected process or group",
    "  /: filter processes, <Esc> clears",
    "",
    "Process sorting",
    "  c: CPU   m: Mem   p: PID",
    "",
    "Graphs",
    "  h: scale in   l: scale out",
    "",
    "?: toggle this help",
};
#define NHELP (int)(sizeof help_lines / sizeof help_lines[0])

static void help_overlay(void)
{
    int w = 46, h = NHELP + 2;
    if (w > TW) w = TW;
    if (h > TH) h = TH;
    Rect r = { (TW - w) / 2, (TH - h) / 2, w, h };
    fb_fill(r.x, r.y, r.w, r.h, -1);
    if (!box(r, " Help ")) return;
    for (int i = 0; i < NHELP && i < r.h - 2; i++)
        fb_text(r.x + 2, r.y + 1 + i, help_lines[i], r.w - 4, SC->fg, -1, 0);
}

void ui_render(Stats *s)
{
    fb_clear();
    int H = TH - (O.statusbar ? 1 : 0);
    if (TW < 20 || H < 8) {
        fb_text(0, 0, "tallystat: terminal too small", TW, SC->fg, -1, 0);
        fb_flush();
        return;
    }
    if (O.minimal) {
        int ch = H / 2;
        int mw = TW / 2;
        w_cpu((Rect){ 0, 0, TW, ch }, s);
        w_mem((Rect){ 0, ch, mw, H - ch }, s);
        w_procs((Rect){ mw, ch, TW - mw, H - ch }, s);
    } else {
        int r1 = H / 3, r2 = H / 3, r3 = H - r1 - r2;
        int lw = TW / 3, dh = r2 / 2;
        int nw = TW / 2;
        w_cpu((Rect){ 0, 0, TW, r1 }, s);
        w_disk((Rect){ 0, r1, lw, dh }, s);
        w_temp((Rect){ 0, r1 + dh, lw, r2 - dh }, s);
        w_mem((Rect){ lw, r1, TW - lw, r2 }, s);
        w_net((Rect){ 0, r1 + r2, nw, r3 }, s);
        w_procs((Rect){ nw, r1 + r2, TW - nw, r3 }, s);
    }
    if (O.statusbar) statusbar(TH - 1);
    if (show_help) help_overlay();
    fb_flush();
}

/* ---------- input ---------- */

static void set_flash(const char *msg)
{
    snprintf(flash, sizeof flash, "%s", msg);
    flash_until = now_sec() + 3;
}

static void kill_selected(Stats *s)
{
    if (sel < 0 || sel >= nview) return;
    Proc *p = &view[sel];
    char msg[160];
    if (!grouped) {
        if (kill(p->pid, SIGTERM) == 0)
            snprintf(msg, sizeof msg, "sent SIGTERM to %d (%s)", p->pid, p->name);
        else
            snprintf(msg, sizeof msg, "kill %d: %s", p->pid, strerror(errno));
    } else {
        int ok = 0, fail = 0, err = 0;
        for (int i = 0; i < s->nproc; i++) {
            if (strcmp(s->procs[i].name, p->name) != 0 || !matches(&s->procs[i])) continue;
            if (kill(s->procs[i].pid, SIGTERM) == 0) ok++;
            else { fail++; err = errno; }
        }
        if (fail) snprintf(msg, sizeof msg, "sent SIGTERM to %d %s, %d failed: %s",
                           ok, p->name, fail, strerror(err));
        else snprintf(msg, sizeof msg, "sent SIGTERM to %d %s", ok, p->name);
    }
    set_flash(msg);
}

static void move_sel(int delta)
{
    sel += delta;
    if (sel >= nview) sel = nview - 1;
    if (sel < 0) sel = 0;
    remember_sel();
}

static int page(void) { return proc_rows.h > 0 ? proc_rows.h : 10; }

int ui_handle(const Event *ev, Stats *s)
{
    if (ev->type == EV_RESIZE) return 0;

    if (ev->type == EV_MOUSE) {
        Rect pr = proc_rect;
        int inside = ev->x >= pr.x && ev->x < pr.x + pr.w && ev->y >= pr.y && ev->y < pr.y + pr.h;
        if (!inside || show_help) return 0;
        if (ev->button == M_WHEEL_UP) move_sel(-3);
        else if (ev->button == M_WHEEL_DOWN) move_sel(3);
        else if (ev->y >= proc_rows.y && ev->y < proc_rows.y + proc_rows.h) {
            int idx = top + (ev->y - proc_rows.y);
            if (idx < nview) { sel = idx; remember_sel(); }
        }
        return 0;
    }

    int k = ev->key;
    if (k == K_CTRL_C) return 1;

    if (show_help) {
        if (k == 'q') return 1;
        show_help = 0;
        return 0;
    }

    if (filtering) {
        size_t n = strlen(filter);
        if (k == K_ENTER) filtering = 0;
        else if (k == K_ESC) { filtering = 0; filter[0] = 0; }
        else if (k == K_BACKSPACE) { if (n) filter[n - 1] = 0; }
        else if (k >= 32 && k < 127 && n + 1 < sizeof filter) { filter[n] = (char)k; filter[n + 1] = 0; }
        sel = top = 0;
        build_view(s);
        remember_sel();
        return 0;
    }

    double t = now_sec();
    int repeat = (k == last_key && t - last_key_t < 0.6);
    last_key = k;
    last_key_t = t;

    switch (k) {
    case 'q': return 1;
    case '?': show_help = 1; break;
    case 'j': case K_DOWN: move_sel(1); break;
    case 'k': case K_UP: move_sel(-1); break;
    case K_CTRL_D: move_sel(page() / 2); break;
    case K_CTRL_U: move_sel(-page() / 2); break;
    case K_CTRL_F: case K_PGDN: move_sel(page()); break;
    case K_CTRL_B: case K_PGUP: move_sel(-page()); break;
    case K_HOME: move_sel(-nview); break;
    case 'G': case K_END: move_sel(nview); break;
    case 'g':
        if (repeat) { move_sel(-nview); last_key = 0; }
        break;
    case 'd':
        if (repeat) { kill_selected(s); last_key = 0; }
        break;
    case K_TAB:
        grouped = !grouped;
        sel = top = 0;
        build_view(s);
        remember_sel();
        break;
    case 'c': sort_by = SORT_CPU; break;
    case 'm': sort_by = SORT_MEM; break;
    case 'p': sort_by = SORT_PID; break;
    case 'h': if (O.hscale < 40) O.hscale++; break;
    case 'l': if (O.hscale > 1) O.hscale--; break;
    case '/': filtering = 1; break;
    case K_ESC: filter[0] = 0; break;
    }
    return 0;
}
