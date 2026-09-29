#define _GNU_SOURCE
#include "stats.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

static void series_push(Series *s, double v)
{
    if (s->len == HIST) {
        memmove(s->v, s->v + 1, (HIST - 1) * sizeof(double));
        s->len--;
    }
    s->v[s->len++] = v;
}

static int read_file(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return (int)r;
}

/* ---------- CPU ---------- */

typedef struct { unsigned long long total, idle; } CpuTimes;

static CpuTimes *cpu_prev;

/* Fills out[0] with the aggregate and out[1+i] with cpu i. Returns entries seen. */
static int read_cpu(CpuTimes *out, int max)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    char line[512];
    int seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        char name[16];
        unsigned long long v[8] = { 0 };
        int k = sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu", name,
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        if (k < 5) continue;
        int idx = name[3] ? 1 + atoi(name + 3) : 0;
        if (idx >= seen) seen = idx + 1;
        if (!out || idx >= max) continue;
        unsigned long long total = 0;
        for (int i = 0; i < 8; i++) total += v[i];   /* guest time is already in user */
        out[idx].total = total;
        out[idx].idle = v[3] + v[4];                 /* idle + iowait */
    }
    fclose(f);
    return seen;
}

static double cpu_pct(CpuTimes now, CpuTimes prev)
{
    if (now.total <= prev.total) return 0;
    double dt = (double)(now.total - prev.total);
    double di = now.idle >= prev.idle ? (double)(now.idle - prev.idle) : 0;
    double p = (1.0 - di / dt) * 100.0;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

static void update_cpu(Stats *s)
{
    int n = s->ncpu + 1;
    CpuTimes now[n];
    memset(now, 0, sizeof now);
    read_cpu(now, n);
    s->cpu_avg = cpu_pct(now[0], cpu_prev[0]);
    series_push(&s->cpu_avg_h, s->cpu_avg);
    for (int i = 0; i < s->ncpu; i++) {
        s->cpu[i] = cpu_pct(now[i + 1], cpu_prev[i + 1]);
        series_push(&s->cpu_h[i], s->cpu[i]);
    }
    memcpy(cpu_prev, now, sizeof now);
}

/* ---------- memory ---------- */

static void update_mem(Stats *s)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char key[64];
    unsigned long long val, total = 0, avail = 0, stotal = 0, sfree = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%63[^:]: %llu", key, &val) != 2) continue;
        if (!strcmp(key, "MemTotal")) total = val;
        else if (!strcmp(key, "MemAvailable")) avail = val;
        else if (!strcmp(key, "SwapTotal")) stotal = val;
        else if (!strcmp(key, "SwapFree")) sfree = val;
    }
    fclose(f);
    s->mem_total = total * 1024;
    s->mem_used = (total - (avail < total ? avail : total)) * 1024;
    s->swap_total = stotal * 1024;
    s->swap_used = (stotal - (sfree < stotal ? sfree : stotal)) * 1024;
    series_push(&s->mem_h, s->mem_total ? 100.0 * (double)s->mem_used / (double)s->mem_total : 0);
    series_push(&s->swap_h, s->swap_total ? 100.0 * (double)s->swap_used / (double)s->swap_total : 0);
}

/* ---------- network ---------- */

static int net_primed;

static void update_net(Stats *s, double dt)
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[512];
    unsigned long long rx = 0, tx = 0;
    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *name = line;
        while (*name == ' ') name++;
        if (s->iface ? strcmp(name, s->iface) != 0 : strcmp(name, "lo") == 0) continue;
        unsigned long long r, t;
        if (sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &r, &t) == 2) {
            rx += r;
            tx += t;
        }
    }
    fclose(f);
    if (net_primed && dt > 0) {
        s->rx_rate = rx >= s->rx_total ? (double)(rx - s->rx_total) / dt : 0;
        s->tx_rate = tx >= s->tx_total ? (double)(tx - s->tx_total) / dt : 0;
        series_push(&s->rx_h, s->rx_rate);
        series_push(&s->tx_h, s->tx_rate);
    }
    net_primed = 1;
    s->rx_total = rx;
    s->tx_total = tx;
}

/* ---------- disks ---------- */

static void unescape_mount(char *s)
{
    char *o = s;
    while (*s) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' && s[2] <= '7' &&
            s[3] >= '0' && s[3] <= '7') {
            *o++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
            s += 4;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
}

typedef struct { char name[64]; unsigned long long rd, wr; } DiskIO;

static void update_disks(Stats *s, double dt)
{
    static DiskIO io[256];
    int nio = 0;
    FILE *f = fopen("/proc/diskstats", "r");
    if (f) {
        char line[512];
        while (nio < 256 && fgets(line, sizeof line, f)) {
            unsigned long long rs, ws;
            if (sscanf(line, "%*u %*u %63s %*u %*u %llu %*u %*u %*u %llu",
                       io[nio].name, &rs, &ws) == 3) {
                io[nio].rd = rs * 512;
                io[nio].wr = ws * 512;
                nio++;
            }
        }
        fclose(f);
    }

    f = fopen("/proc/mounts", "r");
    if (!f) return;
    Disk next[MAX_DISKS];
    int n = 0;
    char line[1024];
    while (n < MAX_DISKS && fgets(line, sizeof line, f)) {
        char dev[256], mnt[512], fs[64];
        if (sscanf(line, "%255s %511s %63s", dev, mnt, fs) != 3) continue;
        if (strncmp(dev, "/dev/", 5) != 0 || !strncmp(dev, "/dev/loop", 9)) continue;
        if (!strcmp(fs, "squashfs") || !strcmp(fs, "iso9660")) continue;
        unescape_mount(mnt);

        char real[PATH_MAX];
        const char *rp = realpath(dev, real) ? real : dev;
        const char *statname = strrchr(rp, '/') ? strrchr(rp, '/') + 1 : rp;

        int dup = 0;
        for (int i = 0; i < n; i++)
            if (!strcmp(next[i].statname, statname)) { dup = 1; break; }
        if (dup) continue;

        struct statvfs sv;
        if (statvfs(mnt, &sv) != 0 || sv.f_blocks == 0) continue;

        Disk *d = &next[n];
        memset(d, 0, sizeof *d);
        const char *base = strrchr(dev, '/') + 1;
        snprintf(d->dev, sizeof d->dev, "%s", base);
        snprintf(d->mount, sizeof d->mount, "%s", mnt);
        snprintf(d->statname, sizeof d->statname, "%s", statname);
        unsigned long long fr = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        d->used = (unsigned long long)(sv.f_blocks - sv.f_bfree) * fr;
        d->free = (unsigned long long)sv.f_bavail * fr;
        d->used_pct = (d->used + d->free) ? 100.0 * (double)d->used / (double)(d->used + d->free) : 0;

        for (int i = 0; i < nio; i++) {
            if (strcmp(io[i].name, statname) != 0) continue;
            d->rd = io[i].rd;
            d->wr = io[i].wr;
            for (int j = 0; j < s->ndisk; j++) {
                Disk *p = &s->disks[j];
                if (strcmp(p->statname, statname) != 0 || dt <= 0) continue;
                d->rps = d->rd >= p->rd ? (double)(d->rd - p->rd) / dt : 0;
                d->wps = d->wr >= p->wr ? (double)(d->wr - p->wr) / dt : 0;
            }
            break;
        }
        n++;
    }
    fclose(f);
    memcpy(s->disks, next, sizeof(Disk) * (size_t)n);
    s->ndisk = n;
}

/* ---------- temperatures ---------- */

static void update_temps(Stats *s)
{
    s->ntemp = 0;
    for (int h = 0; h < 64 && s->ntemp < MAX_TEMPS; h++) {
        char path[128], name[64], buf[64];
        snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/name", h);
        if (read_file(path, name, sizeof name) < 0) {
            /* hwmon indices are dense, but tolerate a few gaps */
            if (h > 8) break;
            continue;
        }
        name[strcspn(name, "\n")] = 0;
        for (int t = 1; t <= 32 && s->ntemp < MAX_TEMPS; t++) {
            snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/temp%d_input", h, t);
            if (read_file(path, buf, sizeof buf) <= 0) continue;
            long milli = strtol(buf, NULL, 10);
            Temp *tp = &s->temps[s->ntemp];

            char label[64] = "";
            snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/temp%d_label", h, t);
            if (read_file(path, label, sizeof label) > 0) {
                label[strcspn(label, "\n")] = 0;
                /* "Package id 0" -> "packageid0" */
                char *o = label;
                for (char *c = label; *c; c++)
                    if (!isspace((unsigned char)*c)) *o++ = (char)tolower((unsigned char)*c);
                *o = 0;
            }
            if (label[0]) snprintf(tp->label, sizeof tp->label, "%s_%s", name, label);
            else if (t > 1) snprintf(tp->label, sizeof tp->label, "%s_%d", name, t);
            else snprintf(tp->label, sizeof tp->label, "%s", name);
            tp->celsius = (double)milli / 1000.0;
            s->ntemp++;
        }
    }
}

/* ---------- processes ---------- */

typedef struct { int pid; unsigned long long ticks; } PrevProc;

static PrevProc *pprev;
static int npprev, pprevcap;
static long clk_tck = 100, page_size = 4096;

static int cmp_prev(const void *a, const void *b)
{
    return ((const PrevProc *)a)->pid - ((const PrevProc *)b)->pid;
}

static void update_procs(Stats *s, double dt)
{
    DIR *d = opendir("/proc");
    if (!d) return;
    s->nproc = 0;
    struct dirent *e;
    char path[64], buf[1024];
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        int pid = atoi(e->d_name);
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        if (read_file(path, buf, sizeof buf) <= 0) continue;
        char *l = strchr(buf, '('), *r = strrchr(buf, ')');
        if (!l || !r || r < l) continue;

        char state;
        unsigned long long ut, st;
        long long rss;
        if (sscanf(r + 2,
                   "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu "
                   "%*d %*d %*d %*d %*d %*d %*u %*u %lld",
                   &state, &ut, &st, &rss) != 4)
            continue;

        if (s->nproc == s->proccap) {
            s->proccap = s->proccap ? s->proccap * 2 : 512;
            s->procs = realloc(s->procs, sizeof(Proc) * (size_t)s->proccap);
            if (!s->procs) abort();
        }
        Proc *p = &s->procs[s->nproc++];
        p->pid = pid;
        size_t nl = (size_t)(r - l - 1);
        if (nl >= sizeof p->name) nl = sizeof p->name - 1;
        memcpy(p->name, l + 1, nl);
        p->name[nl] = 0;
        p->ticks = ut + st;
        p->count = 1;
        p->mem = s->mem_total ? 100.0 * (double)(rss * page_size) / (double)s->mem_total : 0;
        if (p->mem < 0) p->mem = 0;

        p->cpu = 0;
        PrevProc key = { pid, 0 };
        PrevProc *pp = npprev ? bsearch(&key, pprev, (size_t)npprev, sizeof key, cmp_prev) : NULL;
        if (pp && dt > 0 && p->ticks >= pp->ticks)
            p->cpu = 100.0 * (double)(p->ticks - pp->ticks) / (dt * (double)clk_tck);
    }
    closedir(d);

    if (s->nproc > pprevcap) {
        pprevcap = s->proccap;
        pprev = realloc(pprev, sizeof(PrevProc) * (size_t)pprevcap);
        if (!pprev) abort();
    }
    for (int i = 0; i < s->nproc; i++)
        pprev[i] = (PrevProc){ s->procs[i].pid, s->procs[i].ticks };
    npprev = s->nproc;
    qsort(pprev, (size_t)npprev, sizeof(PrevProc), cmp_prev);
}

/* ---------- public ---------- */

int stats_init(Stats *s)
{
    int seen = read_cpu(NULL, 0);
    if (seen < 1) return -1;
    s->ncpu = seen > 1 ? seen - 1 : 1;
    s->cpu = calloc((size_t)s->ncpu, sizeof(double));
    s->cpu_h = calloc((size_t)s->ncpu, sizeof(Series));
    cpu_prev = calloc((size_t)s->ncpu + 1, sizeof(CpuTimes));
    if (!s->cpu || !s->cpu_h || !cpu_prev) return -1;
    read_cpu(cpu_prev, s->ncpu + 1);

    long t = sysconf(_SC_CLK_TCK);
    if (t > 0) clk_tck = t;
    long p = sysconf(_SC_PAGESIZE);
    if (p > 0) page_size = p;

    /* prime counters so the first real sample has a baseline */
    update_mem(s);
    s->mem_h.len = s->swap_h.len = 0;
    update_net(s, 0);
    update_disks(s, 0);
    update_temps(s);
    update_procs(s, 0);
    return 0;
}

void stats_update(Stats *s, double dt)
{
    update_cpu(s);
    update_mem(s);
    update_net(s, dt);
    update_disks(s, dt);
    update_temps(s);
    update_procs(s, dt);
}
