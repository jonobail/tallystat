#ifndef TALLYSTAT_UI_H
#define TALLYSTAT_UI_H

#include "stats.h"
#include "term.h"

typedef struct {
    const char *name;
    int fg, border_label, border_line;
    int cpu_lines[8];
    int main_mem, swap_mem;
    int proc_cursor;
    int sparkline;
    int temp_low, temp_high;
} Scheme;

typedef struct {
    const Scheme *scheme;
    int minimal;
    int show_avg;
    int show_percpu;
    int fahrenheit;
    int statusbar;
    int hscale;
} UIOpts;

const Scheme *ui_find_scheme(const char *name);
void ui_list_schemes(void);

void ui_init(const UIOpts *opts);
void ui_render(Stats *s);
/* Returns 1 when the user asked to quit. */
int  ui_handle(const Event *ev, Stats *s);

#endif
