#define _GNU_SOURCE
#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    uint32_t ch;
    int16_t fg, bg;
    uint8_t attr;
} Cell;

int TW = 80, TH = 24;

static Cell *back, *front;
static int need_clear = 1;
static struct termios orig_tio;
static int raw_on;
static int winch_pipe[2] = { -1, -1 };

static char *obuf;
static size_t olen, ocap;

static void out(const char *s, size_t n)
{
    if (olen + n > ocap) {
        ocap = (olen + n) * 2 + 4096;
        obuf = realloc(obuf, ocap);
        if (!obuf) abort();
    }
    memcpy(obuf + olen, s, n);
    olen += n;
}

static void outs(const char *s) { out(s, strlen(s)); }

static void write_all(const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static void oflush(void)
{
    write_all(obuf, olen);
    olen = 0;
}

static const char RESTORE_SEQ[] =
    "\x1b[?1000l\x1b[?1006l\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";

void term_restore(void)
{
    if (!raw_on) return;
    raw_on = 0;
    write_all(RESTORE_SEQ, sizeof RESTORE_SEQ - 1);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio);
}

static void on_fatal(int sig)
{
    term_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_winch(int sig)
{
    (void)sig;
    int e = errno;
    char c = 'r';
    if (write(winch_pipe[1], &c, 1) < 0) { /* pipe full: a resize is already pending */ }
    errno = e;
}

int term_init(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "tallystat: stdin and stdout must be a terminal\n");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &orig_tio) < 0) return -1;

    struct termios t = orig_tio;
    t.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~(unsigned)OPOST;
    t.c_cflag |= CS8;
    t.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) < 0) return -1;
    raw_on = 1;
    atexit(term_restore);

    if (pipe2(winch_pipe, O_NONBLOCK | O_CLOEXEC) < 0) return -1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = on_fatal;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    /* alt screen, hide cursor, no autowrap, SGR mouse reporting */
    outs("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1006h");
    oflush();
    term_resize();
    return 0;
}

void term_resize(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        TW = ws.ws_col;
        TH = ws.ws_row;
    }
    size_t n = (size_t)TW * (size_t)TH;
    free(back);
    free(front);
    back = calloc(n, sizeof(Cell));
    front = calloc(n, sizeof(Cell));
    if (!back || !front) abort();
    need_clear = 1;
    fb_clear();
}

/* ---------- UTF-8 ---------- */

int utf8_decode(const char *str, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)str;
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    int n;
    uint32_t c;
    if ((s[0] & 0xE0) == 0xC0) { n = 2; c = s[0] & 0x1F; }
    else if ((s[0] & 0xF0) == 0xE0) { n = 3; c = s[0] & 0x0F; }
    else if ((s[0] & 0xF8) == 0xF0) { n = 4; c = s[0] & 0x07; }
    else { *cp = '?'; return 1; }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = '?'; return i; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *cp = c;
    return n;
}

int utf8_len(const char *s)
{
    int n = 0;
    uint32_t cp;
    while (*s) {
        s += utf8_decode(s, &cp);
        n++;
    }
    return n;
}

static int utf8_encode(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) {
        o[0] = (char)(0xC0 | (c >> 6));
        o[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12));
        o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (c >> 18));
    o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* ---------- framebuffer ---------- */

void fb_clear(void)
{
    size_t n = (size_t)TW * (size_t)TH;
    for (size_t i = 0; i < n; i++)
        back[i] = (Cell){ ' ', -1, -1, 0 };
}

void fb_put(int x, int y, uint32_t ch, int fg, int bg, int attr)
{
    if (x < 0 || y < 0 || x >= TW || y >= TH) return;
    /* everything is laid out one column per code point: squash
       control characters and anything that might render double-width */
    if (ch < 0x20 || ch == 0x7F || (ch >= 0x1100 && !(ch >= 0x2500 && ch <= 0x28FF)))
        ch = '?';
    back[y * TW + x] = (Cell){ ch, (int16_t)fg, (int16_t)bg, (uint8_t)attr };
}

int fb_text(int x, int y, const char *s, int maxw, int fg, int bg, int attr)
{
    int n = 0;
    uint32_t cp;
    while (*s && n < maxw) {
        s += utf8_decode(s, &cp);
        fb_put(x + n, y, cp, fg, bg, attr);
        n++;
    }
    return n;
}

void fb_fill(int x, int y, int w, int h, int bg)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            fb_put(i, j, ' ', -1, bg, 0);
}

void fb_flush(void)
{
    char tmp[64];
    int cfg = -2, cbg = -2, cattr = -1;
    int cx = -1, cy = -1;

    if (need_clear) {
        outs("\x1b[0m\x1b[2J");
        need_clear = 0;
        size_t n = (size_t)TW * (size_t)TH;
        for (size_t i = 0; i < n; i++)
            front[i] = (Cell){ 0, -3, -3, 0xFF };
    }

    for (int y = 0; y < TH; y++) {
        for (int x = 0; x < TW; x++) {
            Cell *b = &back[y * TW + x], *f = &front[y * TW + x];
            if (b->ch == f->ch && b->fg == f->fg && b->bg == f->bg && b->attr == f->attr)
                continue;
            if (cx != x || cy != y) {
                snprintf(tmp, sizeof tmp, "\x1b[%d;%dH", y + 1, x + 1);
                outs(tmp);
            }
            if (b->fg != cfg || b->bg != cbg || b->attr != cattr) {
                outs("\x1b[0");
                if (b->attr & A_BOLD) outs(";1");
                if (b->fg >= 0) { snprintf(tmp, sizeof tmp, ";38;5;%d", b->fg); outs(tmp); }
                if (b->bg >= 0) { snprintf(tmp, sizeof tmp, ";48;5;%d", b->bg); outs(tmp); }
                outs("m");
                cfg = b->fg;
                cbg = b->bg;
                cattr = b->attr;
            }
            int n = utf8_encode(b->ch, tmp);
            out(tmp, (size_t)n);
            cx = x + 1;
            cy = y;
            *f = *b;
        }
    }
    oflush();
}

/* ---------- input ---------- */

static int parse_csi_num(const unsigned char *b, int n, int *i, int *val)
{
    int v = 0, got = 0;
    while (*i < n && b[*i] >= '0' && b[*i] <= '9') {
        v = v * 10 + (b[*i] - '0');
        (*i)++;
        got = 1;
    }
    *val = v;
    return got;
}

/* Parse one event from b[*i..n). Returns 1 if an event was produced. */
static int parse_one(const unsigned char *b, int n, int *i, Event *ev)
{
    unsigned char c = b[(*i)++];
    ev->type = EV_KEY;
    if (c != 0x1b) {
        ev->key = (c == 0x08) ? K_BACKSPACE : (c == '\n' ? K_ENTER : c);
        return 1;
    }
    if (*i >= n) { ev->key = K_ESC; return 1; }

    unsigned char k = b[*i];
    if (k == 'O' && *i + 1 < n) {          /* SS3: application cursor keys */
        unsigned char f = b[*i + 1];
        *i += 2;
        switch (f) {
        case 'A': ev->key = K_UP; return 1;
        case 'B': ev->key = K_DOWN; return 1;
        case 'C': ev->key = K_RIGHT; return 1;
        case 'D': ev->key = K_LEFT; return 1;
        case 'H': ev->key = K_HOME; return 1;
        case 'F': ev->key = K_END; return 1;
        }
        return 0;
    }
    if (k != '[') {                         /* Alt+key: treat as the key */
        (*i)++;
        ev->key = k;
        return 1;
    }
    (*i)++;

    if (*i < n && b[*i] == '<') {           /* SGR mouse: ESC [ < b ; x ; y (M|m) */
        (*i)++;
        int btn, x, y;
        parse_csi_num(b, n, i, &btn);
        if (*i < n && b[*i] == ';') (*i)++;
        parse_csi_num(b, n, i, &x);
        if (*i < n && b[*i] == ';') (*i)++;
        parse_csi_num(b, n, i, &y);
        if (*i >= n) return 0;
        unsigned char fin = b[(*i)++];
        ev->type = EV_MOUSE;
        ev->x = x - 1;
        ev->y = y - 1;
        if (btn == 64) ev->button = M_WHEEL_UP;
        else if (btn == 65) ev->button = M_WHEEL_DOWN;
        else if (btn == 0 && fin == 'M') ev->button = M_LEFT;
        else return 0;
        return 1;
    }

    int num = 0;
    parse_csi_num(b, n, i, &num);
    while (*i < n && (b[*i] == ';' || (b[*i] >= '0' && b[*i] <= '9'))) (*i)++;  /* modifiers */
    if (*i >= n) return 0;
    unsigned char fin = b[(*i)++];
    switch (fin) {
    case 'A': ev->key = K_UP; return 1;
    case 'B': ev->key = K_DOWN; return 1;
    case 'C': ev->key = K_RIGHT; return 1;
    case 'D': ev->key = K_LEFT; return 1;
    case 'H': ev->key = K_HOME; return 1;
    case 'F': ev->key = K_END; return 1;
    case 'Z': ev->key = K_TAB; return 1;
    case '~':
        switch (num) {
        case 1: case 7: ev->key = K_HOME; return 1;
        case 4: case 8: ev->key = K_END; return 1;
        case 3: ev->key = K_DEL; return 1;
        case 5: ev->key = K_PGUP; return 1;
        case 6: ev->key = K_PGDN; return 1;
        }
    }
    return 0;
}

int term_poll(int timeout_ms, Event *evs, int max)
{
    struct pollfd p[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = winch_pipe[0], .events = POLLIN },
    };
    int r = poll(p, 2, timeout_ms);
    if (r <= 0) return 0;

    int count = 0;
    if (p[1].revents & POLLIN) {
        char junk[64];
        while (read(winch_pipe[0], junk, sizeof junk) > 0) {}
        term_resize();
        evs[count++] = (Event){ .type = EV_RESIZE };
    }
    if (p[0].revents & POLLIN) {
        unsigned char buf[512];
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        int i = 0;
        while (n > 0 && i < n && count < max) {
            Event ev = { 0 };
            if (parse_one(buf, (int)n, &i, &ev))
                evs[count++] = ev;
        }
    }
    if (p[0].revents & (POLLHUP | POLLERR)) {
        evs[0] = (Event){ .type = EV_KEY, .key = 'q' };
        return 1;
    }
    return count;
}
