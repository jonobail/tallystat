#define _GNU_SOURCE
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "stats.h"
#include "term.h"
#include "ui.h"

#ifndef VERSION
#define VERSION "dev"
#endif

static void usage(FILE *f)
{
    fprintf(f,
        "tallystat %s - a terminal system monitor\n"
        "\n"
        "Usage: tallystat [options]\n"
        "\n"
        "Options:\n"
        "  -c, --color=NAME        color scheme (default, ember, monokai, solarized, nord)\n"
        "  -m, --minimal           only show CPU, memory and process widgets\n"
        "  -r, --rate=SECONDS      refresh interval, e.g. 0.5 or 2 (default 1)\n"
        "  -a, --averagecpu        show the average CPU line\n"
        "  -p, --percpu            show a line per CPU core\n"
        "  -s, --statusbar         show hostname, time and uptime at the bottom\n"
        "  -f, --fahrenheit        show temperatures in Fahrenheit\n"
        "  -S, --graphscale=N      horizontal graph scale, 1-40 (default 7)\n"
        "  -i, --interface=NAME    only measure this network interface\n"
        "  -l, --list-colors       list color schemes and exit\n"
        "  -V, --version           print version and exit\n"
        "  -h, --help              print this help and exit\n"
        "\n"
        "Press ? inside tallystat for key bindings.\n",
        VERSION);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    UIOpts o = { .scheme = ui_find_scheme("default"), .hscale = 7 };
    double rate = 1.0;
    const char *iface = NULL;

    static const struct option longopts[] = {
        { "color", required_argument, 0, 'c' },
        { "minimal", no_argument, 0, 'm' },
        { "rate", required_argument, 0, 'r' },
        { "averagecpu", no_argument, 0, 'a' },
        { "percpu", no_argument, 0, 'p' },
        { "statusbar", no_argument, 0, 's' },
        { "fahrenheit", no_argument, 0, 'f' },
        { "graphscale", required_argument, 0, 'S' },
        { "interface", required_argument, 0, 'i' },
        { "list-colors", no_argument, 0, 'l' },
        { "version", no_argument, 0, 'V' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "c:mr:apsfS:i:lVh", longopts, NULL)) != -1) {
        switch (c) {
        case 'c':
            o.scheme = ui_find_scheme(optarg);
            if (!o.scheme) {
                fprintf(stderr, "tallystat: unknown color scheme '%s' (try --list-colors)\n", optarg);
                return 2;
            }
            break;
        case 'm': o.minimal = 1; break;
        case 'r': {
            char *end;
            rate = strtod(optarg, &end);
            if (*end || rate < 0.1 || rate > 3600) {
                fprintf(stderr, "tallystat: rate must be between 0.1 and 3600 seconds\n");
                return 2;
            }
            break;
        }
        case 'a': o.show_avg = 1; break;
        case 'p': o.show_percpu = 1; break;
        case 's': o.statusbar = 1; break;
        case 'f': o.fahrenheit = 1; break;
        case 'S':
            o.hscale = atoi(optarg);
            if (o.hscale < 1 || o.hscale > 40) {
                fprintf(stderr, "tallystat: graph scale must be between 1 and 40\n");
                return 2;
            }
            break;
        case 'i': iface = optarg; break;
        case 'l': ui_list_schemes(); return 0;
        case 'V': printf("tallystat %s\n", VERSION); return 0;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }

    static Stats st;
    st.iface = iface;
    if (stats_init(&st) < 0) {
        fprintf(stderr, "tallystat: cannot read /proc/stat\n");
        return 1;
    }
    if (!o.show_avg && !o.show_percpu) {
        if (st.ncpu <= 8) o.show_percpu = 1;
        else o.show_avg = 1;
    }
    ui_init(&o);
    if (term_init() < 0) return 1;

    double last = now_sec();
    double next = last + (rate < 0.25 ? rate : 0.25);   /* fill the screen quickly at start */
    int dirty = 1;
    Event evs[64];

    for (;;) {
        double t = now_sec();
        if (t >= next) {
            stats_update(&st, t - last);
            last = t;
            next += rate;
            if (next <= t) next = t + rate;
            dirty = 1;
        }
        if (dirty) {
            ui_render(&st);
            dirty = 0;
        }
        int timeout = (int)((next - now_sec()) * 1000) + 1;
        if (timeout < 0) timeout = 0;
        int n = term_poll(timeout, evs, 64);
        for (int i = 0; i < n; i++) {
            if (ui_handle(&evs[i], &st)) {
                term_restore();
                return 0;
            }
            dirty = 1;
        }
    }
}
