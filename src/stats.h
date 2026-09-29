#ifndef TALLYSTAT_STATS_H
#define TALLYSTAT_STATS_H

#define HIST 1024
#define MAX_DISKS 32
#define MAX_TEMPS 64

typedef struct {
    double v[HIST];
    int len;
} Series;

typedef struct {
    char dev[64];        /* display name, e.g. nvme0n1p2 */
    char mount[256];
    char statname[64];   /* name in /proc/diskstats */
    unsigned long long used, free;   /* bytes */
    double used_pct;
    unsigned long long rd, wr;       /* cumulative bytes */
    double rps, wps;                 /* bytes/s */
} Disk;

typedef struct {
    char label[64];
    double celsius;
} Temp;

typedef struct {
    int pid;
    char name[64];
    unsigned long long ticks;
    double cpu;          /* percent of one core */
    double mem;          /* percent of total RAM */
    int count;           /* processes in group (grouped view) */
} Proc;

typedef struct {
    int ncpu;
    double cpu_avg;
    double *cpu;
    Series cpu_avg_h;
    Series *cpu_h;

    unsigned long long mem_total, mem_used, swap_total, swap_used;
    Series mem_h, swap_h;

    const char *iface;   /* NULL = all but loopback */
    unsigned long long rx_total, tx_total;
    double rx_rate, tx_rate;
    Series rx_h, tx_h;

    Disk disks[MAX_DISKS];
    int ndisk;

    Temp temps[MAX_TEMPS];
    int ntemp;

    Proc *procs;
    int nproc, proccap;
} Stats;

int  stats_init(Stats *s);
void stats_update(Stats *s, double dt);

#endif
