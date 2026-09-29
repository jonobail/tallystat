#ifndef TALLYSTAT_TERM_H
#define TALLYSTAT_TERM_H

#include <stdint.h>

enum { A_BOLD = 1 };

enum {
    K_CTRL_B = 2, K_CTRL_C = 3, K_CTRL_D = 4, K_CTRL_F = 6,
    K_TAB = 9, K_ENTER = 13, K_CTRL_U = 21, K_ESC = 27, K_BACKSPACE = 127,
    K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL,
};

enum { EV_KEY, EV_MOUSE, EV_RESIZE };
enum { M_LEFT, M_WHEEL_UP, M_WHEEL_DOWN };

typedef struct {
    int type;
    int key;      /* EV_KEY */
    int button;   /* EV_MOUSE */
    int x, y;     /* EV_MOUSE, 0-based */
} Event;

extern int TW, TH;

int  term_init(void);
void term_restore(void);
void term_resize(void);
int  term_poll(int timeout_ms, Event *evs, int max);

int  utf8_decode(const char *s, uint32_t *cp);
int  utf8_len(const char *s);

void fb_clear(void);
void fb_put(int x, int y, uint32_t ch, int fg, int bg, int attr);
int  fb_text(int x, int y, const char *s, int maxw, int fg, int bg, int attr);
void fb_fill(int x, int y, int w, int h, int bg);
void fb_flush(void);

#endif
