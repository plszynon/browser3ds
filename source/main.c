// Browser3DS: thin-client web browser for the Nintendo 3DS.
// Talks to a Supabase Edge Function (browse3ds) that fetches and simplifies pages.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "font.h"

#define TOP_W 400
#define BOT_W 320
#define SCR_H 240

#define CFG_DIR   "sdmc:/3ds/browser3ds"
#define CFG_FILE  CFG_DIR "/config.txt"
#define BM_FILE   CFG_DIR "/bookmarks.txt"

#define MAX_LINES 3000
#define MAX_LINKS 450
#define MAX_HIST  24
#define MAX_DL    (400 * 1024)

#define COLS_TOP 64
#define LINE_H   11
#define BODY_Y   16
#define BODY_ROWS 19

// colors: 0xRRGGBB
#define C_BG     0x202020
#define C_TEXT   0xE6E6E6
#define C_HEAD   0xFFD24A
#define C_LINK   0x6CC8FF
#define C_BAR    0x3A3A5C
#define C_BTN    0x34506E
#define C_BTN2   0x4A6E94
#define C_DIM    0x909090
#define C_ERR    0xFF6B6B

static char g_server[300] = "https://tiricozrtwjkfobvyfjr.supabase.co/functions/v1/browse3ds";
static char g_key[80] = "xk3ds-7Tq9Lm2Vd8Rp4Wa";

typedef struct { char *s; unsigned char style; } Line;   // style 0 text, 1 heading, 2 error, 3 dim
typedef struct { char *url; char *label; int n; } Link;
typedef struct { char target[512]; int search; int page; } Hist;

static Line g_lines[MAX_LINES];
static int g_nlines = 0;
static Link g_links[MAX_LINKS];
static int g_nlinks = 0;
static char g_title[128] = "";
static char g_url[512] = "";
static int g_page = 1, g_total = 1;
static int g_scroll = 0, g_linkScroll = 0;
static Hist g_hist[MAX_HIST];
static int g_histN = 0;
static Hist g_cur;
static int g_dirty = 2;
static char g_status[96] = "";

// ---------------------------------------------------------------- drawing
static u8 *g_fbTop, *g_fbBot;

static inline void put(u8 *fb, int w, int x, int y, u32 c) {
    if (x < 0 || y < 0 || x >= w || y >= SCR_H) return;
    u8 *p = fb + (x * SCR_H + (SCR_H - 1 - y)) * 3;
    p[0] = c & 0xFF; p[1] = (c >> 8) & 0xFF; p[2] = (c >> 16) & 0xFF;
}
static void rect(u8 *fb, int w, int x, int y, int rw, int rh, u32 c) {
    for (int i = 0; i < rw; i++) for (int j = 0; j < rh; j++) put(fb, w, x + i, y + j, c);
}
static void glyph(u8 *fb, int w, int x, int y, char ch, u32 c) {
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *g = font6x10[ch - 32];
    for (int r = 0; r < 10; r++) {
        unsigned char b = g[r];
        if (!b) continue;
        for (int k = 0; k < 6; k++) if (b & (0x80 >> k)) put(fb, w, x + k, y + r, c);
    }
}
static void text(u8 *fb, int w, int x, int y, const char *s, u32 c, int maxc) {
    for (int i = 0; s[i] && (maxc < 0 || i < maxc); i++) glyph(fb, w, x + i * 6, y, s[i], c);
}
static void clear(u8 *fb, int w, u8 v) { memset(fb, v, (size_t)w * SCR_H * 3); }

// ---------------------------------------------------------------- document
static void free_doc(void) {
    for (int i = 0; i < g_nlines; i++) free(g_lines[i].s);
    for (int i = 0; i < g_nlinks; i++) { free(g_links[i].url); free(g_links[i].label); }
    g_nlines = g_nlinks = 0;
    g_scroll = g_linkScroll = 0;
}
static void add_line(const char *s, int len, unsigned char style) {
    if (g_nlines >= MAX_LINES) return;
    char *d = malloc(len + 1);
    if (!d) return;
    memcpy(d, s, len); d[len] = 0;
    g_lines[g_nlines].s = d; g_lines[g_nlines].style = style; g_nlines++;
}
// word-wrap `s` into lines of at most `cols`, first line prefixed with `first`, rest with `rest`
static void wrap(const char *s, unsigned char style, const char *first, const char *rest) {
    char buf[COLS_TOP + 8];
    const char *pre = first;
    size_t pl = strlen(first), rl = strlen(rest);
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        int avail = COLS_TOP - (int)pl;
        int len = (int)strlen(s);
        int cut;
        if (len <= avail) cut = len;
        else {
            cut = avail;
            while (cut > 0 && s[cut] != ' ') cut--;
            if (cut == 0) cut = avail;           // no space: hard break
        }
        memcpy(buf, pre, pl);
        memcpy(buf + pl, s, cut);
        int total = (int)pl + cut;
        while (total > 0 && buf[total - 1] == ' ') total--;
        add_line(buf, total, style);
        s += cut;
        pre = rest; pl = rl;
    }
}
static void add_block(char type, const char *s) {
    if (type == 'H') { add_line("", 0, 0); wrap(s, 1, "", ""); }
    else if (type == 'B') wrap(s, 0, "* ", "  ");
    else { wrap(s, 0, "", ""); add_line("", 0, 0); }
}
static void add_link(int n, const char *url, const char *label) {
    if (g_nlinks >= MAX_LINKS) return;
    Link *l = &g_links[g_nlinks++];
    l->n = n; l->url = strdup(url); l->label = strdup(label && *label ? label : url);
}

static void parse_response(char *buf) {
    free_doc();
    g_title[0] = 0;
    char *save = NULL;
    for (char *ln = strtok_r(buf, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        size_t n = strlen(ln);
        if (n && ln[n - 1] == '\r') ln[--n] = 0;
        if (!n) continue;
        char t = ln[0]; char *v = ln + 1;
        switch (t) {
        case 'T': snprintf(g_title, sizeof g_title, "%s", v); break;
        case 'U': snprintf(g_url, sizeof g_url, "%s", v); break;
        case 'G': g_page = atoi(v); { char *tab = strchr(v, '\t'); g_total = tab ? atoi(tab + 1) : 1; } break;
        case 'H': case 'P': case 'B': add_block(t, v); break;
        case 'E': add_line("", 0, 0); wrap(v, 2, "Error: ", "       "); break;
        case 'L': {
            char *t1 = strchr(v, '\t'); if (!t1) break;
            *t1 = 0; char *url = t1 + 1;
            char *t2 = strchr(url, '\t'); const char *label = "";
            if (t2) { *t2 = 0; label = t2 + 1; }
            add_link(atoi(v), url, label);
        } break;
        }
    }
}

// ---------------------------------------------------------------- http
static void urlenc(const char *s, char *out, size_t cap) {
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') out[o++] = c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

// returns malloc'd NUL-terminated body or NULL; err gets a description
static char *http_get(const char *url, char *err, size_t errcap) {
    char cur[1100];
    snprintf(cur, sizeof cur, "%s", url);
    for (int hop = 0; hop < 4; hop++) {
        httpcContext ctx;
        Result rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, cur, 1);
        if (R_FAILED(rc)) { snprintf(err, errcap, "open failed 0x%08lX", (unsigned long)rc); return NULL; }
        httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
        httpcAddRequestHeaderField(&ctx, "User-Agent", "Browser3DS/1.0");
        rc = httpcBeginRequest(&ctx);
        if (R_FAILED(rc)) {
            snprintf(err, errcap, "request failed 0x%08lX (TLS/Wi-Fi?)", (unsigned long)rc);
            httpcCloseContext(&ctx); return NULL;
        }
        u32 status = 0;
        httpcGetResponseStatusCode(&ctx, &status);
        if (status >= 300 && status < 400) {
            char loc[1100] = "";
            httpcGetResponseHeader(&ctx, "Location", loc, sizeof loc);
            httpcCloseContext(&ctx);
            if (!loc[0]) { snprintf(err, errcap, "redirect without location"); return NULL; }
            snprintf(cur, sizeof cur, "%s", loc);
            continue;
        }
        if (status != 200) {
            snprintf(err, errcap, "HTTP %lu from server", (unsigned long)status);
            httpcCloseContext(&ctx); return NULL;
        }
        size_t cap = 0x4000, size = 0;
        char *buf = malloc(cap);
        if (!buf) { httpcCloseContext(&ctx); snprintf(err, errcap, "out of memory"); return NULL; }
        do {
            u32 rd = 0;
            rc = httpcDownloadData(&ctx, (u8 *)buf + size, (u32)(cap - size - 1), &rd);
            size += rd;
            if (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
                if (cap >= MAX_DL) break;
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) { free(buf); httpcCloseContext(&ctx); snprintf(err, errcap, "out of memory"); return NULL; }
                buf = nb;
            }
        } while (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
        httpcCloseContext(&ctx);
        if (R_FAILED(rc) && size == 0) { free(buf); snprintf(err, errcap, "download failed 0x%08lX", (unsigned long)rc); return NULL; }
        buf[size] = 0;
        return buf;
    }
    snprintf(err, errcap, "too many redirects");
    return NULL;
}

// ---------------------------------------------------------------- pages
static void draw_all(void);

static void show_loading(const char *what) {
    snprintf(g_status, sizeof g_status, "Loading %.60s ...", what);
    draw_all();
    gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
}

static void load_home(void) {
    free_doc();
    snprintf(g_title, sizeof g_title, "Browser3DS - Home");
    g_url[0] = 0; g_page = g_total = 1;
    add_block('H', "Welcome to Browser3DS");
    add_block('P', "Pick a bookmark from the list on the bottom screen, or use URL / Search.");
    add_block('B', "URL: type an address like en.wikipedia.org");
    add_block('B', "Search: type words to search the web");
    add_block('B', "D-pad / Circle Pad: scroll   L / R: page up / down");
    add_block('B', "X / Y: next / previous part of a long page");
    add_block('B', "B: back   START: exit");
    add_block('P', "Pages are simplified by a proxy: text and links only, no images or scripts.");
    static const char *def[][2] = {
        {"https://en.wikipedia.org/wiki/Special:Random", "Wikipedia (random article)"},
        {"https://pl.wikipedia.org/wiki/Wikipedia:Strona_g%C5%82%C3%B3wna", "Wikipedia PL"},
        {"https://news.ycombinator.com/", "Hacker News"},
        {"https://text.npr.org/", "NPR text news"},
        {"https://example.com/", "example.com (test page)"},
    };
    int n = 1;
    for (size_t i = 0; i < sizeof def / sizeof def[0]; i++) add_link(n++, def[i][0], def[i][1]);
    FILE *f = fopen(BM_FILE, "r");
    if (f) {
        char ln[700];
        while (fgets(ln, sizeof ln, f) && g_nlinks < MAX_LINKS) {
            size_t l = strlen(ln);
            while (l && (ln[l - 1] == '\n' || ln[l - 1] == '\r')) ln[--l] = 0;
            char *tab = strchr(ln, '\t');
            if (tab) { *tab = 0; add_link(n++, ln, tab + 1); }
            else if (l) add_link(n++, ln, ln);
        }
        fclose(f);
    }
    g_status[0] = 0;
}

static void fetch_current(void) {
    if (!g_cur.target[0]) { load_home(); g_dirty = 2; return; }
    show_loading(g_cur.target);
    char enc[700], url[1100], err[160] = "";
    urlenc(g_cur.target, enc, sizeof enc);
    char ekey[160];
    urlenc(g_key, ekey, sizeof ekey);
    if (g_cur.search) snprintf(url, sizeof url, "%s?k=%s&q=%s", g_server, ekey, enc);
    else snprintf(url, sizeof url, "%s?k=%s&u=%s&p=%d", g_server, ekey, enc, g_cur.page);
    char *body = http_get(url, err, sizeof err);
    if (!body) {
        free_doc();
        snprintf(g_title, sizeof g_title, "Could not load page");
        add_block('P', "Could not reach the proxy server.");
        add_line(err, (int)strlen(err), 2);
        add_block('P', "Check Wi-Fi, and that server= and key= in sdmc:/3ds/browser3ds/config.txt are right.");
        g_page = g_total = 1;
    } else {
        parse_response(body);
        free(body);
        g_cur.page = g_page;
        if (g_cur.search) snprintf(g_url, sizeof g_url, "search: %s", g_cur.target);
    }
    g_status[0] = 0;
    g_dirty = 2;
}

static void navigate(const char *target, int search, int page, int pushHist) {
    if (pushHist && (g_cur.target[0] || g_nlinks || g_nlines)) {
        if (g_histN == MAX_HIST) { memmove(&g_hist[0], &g_hist[1], sizeof(Hist) * (MAX_HIST - 1)); g_histN--; }
        g_hist[g_histN++] = g_cur;
    }
    snprintf(g_cur.target, sizeof g_cur.target, "%s", target);
    g_cur.search = search; g_cur.page = page;
    fetch_current();
}
static void go_back(void) {
    if (g_histN == 0) return;
    g_cur = g_hist[--g_histN];
    fetch_current();
}

static int ask(const char *hint, const char *initial, char *out, size_t cap) {
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)cap - 1);
    swkbdSetHintText(&kb, hint);
    if (initial && *initial) swkbdSetInitialText(&kb, initial);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    out[0] = 0;
    SwkbdButton b = swkbdInputText(&kb, out, cap);
    g_dirty = 2;
    return b == SWKBD_BUTTON_CONFIRM && out[0];
}

static void add_bookmark(void) {
    if (!g_cur.target[0] || g_cur.search) { snprintf(g_status, sizeof g_status, "Open a page first"); g_dirty = 2; return; }
    mkdir("sdmc:/3ds", 0777); mkdir(CFG_DIR, 0777);
    FILE *f = fopen(BM_FILE, "a");
    if (!f) { snprintf(g_status, sizeof g_status, "Cannot write bookmarks"); g_dirty = 2; return; }
    fprintf(f, "%s\t%s\n", g_url[0] ? g_url : g_cur.target, g_title[0] ? g_title : g_cur.target);
    fclose(f);
    snprintf(g_status, sizeof g_status, "Bookmark saved");
    g_dirty = 2;
}

static void load_config(void) {
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) {   // write a template so the user can edit it
        mkdir("sdmc:/3ds", 0777); mkdir(CFG_DIR, 0777);
        f = fopen(CFG_FILE, "w");
        if (f) { fprintf(f, "server=%s\nkey=%s\n", g_server, g_key); fclose(f); }
        return;
    }
    char ln[420];
    while (fgets(ln, sizeof ln, f)) {
        size_t l = strlen(ln);
        while (l && (ln[l - 1] == '\n' || ln[l - 1] == '\r' || ln[l - 1] == ' ')) ln[--l] = 0;
        if (!strncmp(ln, "server=", 7)) snprintf(g_server, sizeof g_server, "%s", ln + 7);
        else if (!strncmp(ln, "key=", 4)) snprintf(g_key, sizeof g_key, "%s", ln + 4);
    }
    fclose(f);
}

// ---------------------------------------------------------------- UI
typedef struct { int x, y, w, h; const char *label; int id; } Btn;
enum { B_BACK, B_HOME, B_URL, B_SEARCH, B_RELOAD, B_PREV, B_NEXT, B_MARK, B_EXIT };
static const Btn g_btns[] = {
    {2,   2, 61, 20, "Back",   B_BACK},
    {65,  2, 61, 20, "Home",   B_HOME},
    {128, 2, 61, 20, "URL",    B_URL},
    {191, 2, 61, 20, "Search", B_SEARCH},
    {254, 2, 64, 20, "Reload", B_RELOAD},
    {2,  24, 77, 20, "< Part", B_PREV},
    {81, 24, 77, 20, "Part >", B_NEXT},
    {160, 24, 77, 20, "Bookmark", B_MARK},
    {239, 24, 79, 20, "Exit",   B_EXIT},
};
#define LIST_Y 60
#define LIST_ROWS 14
#define LIST_H 12

static void draw_top(void) {
    u8 *fb = g_fbTop;
    clear(fb, TOP_W, 0x20);
    rect(fb, TOP_W, 0, 0, TOP_W, 14, C_BAR);
    text(fb, TOP_W, 3, 2, g_title, 0xFFFFFF, 64);
    for (int r = 0; r < BODY_ROWS; r++) {
        int i = g_scroll + r;
        if (i >= g_nlines) break;
        u32 col = C_TEXT;
        switch (g_lines[i].style) { case 1: col = C_HEAD; break; case 2: col = C_ERR; break; case 3: col = C_DIM; break; }
        int y = BODY_Y + r * LINE_H;
        // highlight [n] link markers
        const char *s = g_lines[i].s;
        int x = 3;
        for (int k = 0; s[k]; k++) {
            u32 c = col;
            glyph(fb, TOP_W, x + k * 6, y, s[k], c);
        }
        // second pass: recolor "[digits]" runs
        for (int k = 0; s[k]; k++) {
            if (s[k] == '[' && s[k + 1] >= '0' && s[k + 1] <= '9') {
                int e = k + 1;
                while (s[e] >= '0' && s[e] <= '9') e++;
                if (s[e] == ']') {
                    rect(fb, TOP_W, x + k * 6, y, (e - k + 1) * 6, 10, 0x202020);
                    for (int q = k; q <= e; q++) glyph(fb, TOP_W, x + q * 6, y, s[q], C_LINK);
                    k = e;
                }
            }
        }
    }
    // scrollbar
    if (g_nlines > BODY_ROWS) {
        int track = BODY_ROWS * LINE_H;
        int bh = track * BODY_ROWS / g_nlines; if (bh < 6) bh = 6;
        int by = BODY_Y + (track - bh) * g_scroll / (g_nlines - BODY_ROWS);
        rect(fb, TOP_W, TOP_W - 4, BODY_Y, 3, track, 0x333333);
        rect(fb, TOP_W, TOP_W - 4, by, 3, bh, 0xAAAAAA);
    }
    rect(fb, TOP_W, 0, 228, TOP_W, 12, C_BAR);
    char st[128];
    if (g_status[0]) snprintf(st, sizeof st, "%s", g_status);
    else snprintf(st, sizeof st, "%.46s  part %d/%d", g_url[0] ? g_url : "home", g_page, g_total);
    text(fb, TOP_W, 3, 229, st, 0xFFFFFF, 64);
}

static void draw_bottom(void) {
    u8 *fb = g_fbBot;
    clear(fb, BOT_W, 0x20);
    for (size_t i = 0; i < sizeof g_btns / sizeof g_btns[0]; i++) {
        const Btn *b = &g_btns[i];
        rect(fb, BOT_W, b->x, b->y, b->w, b->h, C_BTN);
        rect(fb, BOT_W, b->x, b->y, b->w, 1, C_BTN2);
        int tl = (int)strlen(b->label);
        text(fb, BOT_W, b->x + (b->w - tl * 6) / 2, b->y + 5, b->label, 0xFFFFFF, -1);
    }
    char hd[64];
    snprintf(hd, sizeof hd, "Links (%d) - tap to open, drag to scroll", g_nlinks);
    text(fb, BOT_W, 3, 48, hd, C_DIM, 52);
    for (int r = 0; r < LIST_ROWS; r++) {
        int i = g_linkScroll + r;
        if (i >= g_nlinks) break;
        int y = LIST_Y + r * LIST_H;
        if (r & 1) rect(fb, BOT_W, 0, y - 1, BOT_W, LIST_H, 0x282828);
        char ln[80];
        snprintf(ln, sizeof ln, "%d. %s", g_links[i].n, g_links[i].label);
        text(fb, BOT_W, 3, y, ln, C_LINK, 52);
    }
    text(fb, BOT_W, 3, 229, "dpad/L/R scroll  B back  X/Y part", C_DIM, 52);
}

static void draw_all(void) {
    g_fbTop = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    g_fbBot = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    draw_top();
    draw_bottom();
}

static void clamp_scroll(void) {
    int max = g_nlines - BODY_ROWS; if (max < 0) max = 0;
    if (g_scroll > max) g_scroll = max;
    if (g_scroll < 0) g_scroll = 0;
    int lmax = g_nlinks - LIST_ROWS; if (lmax < 0) lmax = 0;
    if (g_linkScroll > lmax) g_linkScroll = lmax;
    if (g_linkScroll < 0) g_linkScroll = 0;
}

static int do_action(int id) {   // returns 1 to quit
    char in[400];
    switch (id) {
    case B_BACK: go_back(); break;
    case B_HOME: navigate("", 0, 1, 1); break;
    case B_URL:
        if (ask("Enter web address (e.g. en.wikipedia.org)", g_cur.search ? "" : g_cur.target, in, sizeof in))
            navigate(in, 0, 1, 1);
        break;
    case B_SEARCH:
        if (ask("Search the web", "", in, sizeof in)) navigate(in, 1, 1, 1);
        break;
    case B_RELOAD: if (g_cur.target[0]) fetch_current(); break;
    case B_PREV: if (g_cur.target[0] && !g_cur.search && g_page > 1) { g_cur.page = g_page - 1; fetch_current(); } break;
    case B_NEXT: if (g_cur.target[0] && !g_cur.search && g_page < g_total) { g_cur.page = g_page + 1; fetch_current(); } break;
    case B_MARK: add_bookmark(); break;
    case B_EXIT: return 1;
    }
    return 0;
}

int main(void) {
    gfxInitDefault();
    httpcInit(0);
    load_config();
    memset(&g_cur, 0, sizeof g_cur);
    load_home();

    int touching = 0, sx = 0, sy = 0, lx = 0, ly = 0, moved = 0, accum = 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp();
        if (down & KEY_START) break;
        if (down & KEY_B) go_back();
        if (down & KEY_X) do_action(B_NEXT);
        if (down & KEY_Y) do_action(B_PREV);
        if (down & KEY_L) { g_scroll -= BODY_ROWS - 1; g_dirty = 2; }
        if (down & KEY_R) { g_scroll += BODY_ROWS - 1; g_dirty = 2; }
        if (down & KEY_A) { g_scroll += BODY_ROWS - 1; g_dirty = 2; }
        static int rep = 0;
        if (held & (KEY_DUP | KEY_DDOWN)) {
            if (down & (KEY_DUP | KEY_DDOWN)) rep = 0;
            if (rep == 0 || rep > 12) { g_scroll += (held & KEY_DDOWN) ? 1 : -1; g_dirty = 2; }
            rep++;
        } else rep = 0;
        circlePosition cp; hidCircleRead(&cp);
        if (cp.dy > 40 || cp.dy < -40) { g_scroll -= cp.dy / 40; g_dirty = 2; }

        touchPosition t;
        if (held & KEY_TOUCH) {
            hidTouchRead(&t);
            if (down & KEY_TOUCH) { touching = 1; sx = lx = t.px; sy = ly = t.py; moved = 0; accum = 0; }
            else if (touching) {
                accum += t.py - ly;
                if (abs(t.px - sx) + abs(t.py - sy) > 8) moved = 1;
                if (moved && sy >= LIST_Y) {
                    while (accum >= LIST_H) { g_linkScroll--; accum -= LIST_H; g_dirty = 2; }
                    while (accum <= -LIST_H) { g_linkScroll++; accum += LIST_H; g_dirty = 2; }
                }
                lx = t.px; ly = t.py;
            }
        }
        if ((up & KEY_TOUCH) && touching) {
            touching = 0;
            if (!moved) {
                int quit = 0, handled = 0;
                for (size_t i = 0; i < sizeof g_btns / sizeof g_btns[0]; i++) {
                    const Btn *b = &g_btns[i];
                    if (lx >= b->x && lx < b->x + b->w && ly >= b->y && ly < b->y + b->h) {
                        quit = do_action(b->id); handled = 1; break;
                    }
                }
                if (quit) break;
                if (!handled && ly >= LIST_Y - 1) {
                    int row = (ly - LIST_Y) / LIST_H, i = g_linkScroll + row;
                    if (row >= 0 && row < LIST_ROWS && i < g_nlinks) navigate(g_links[i].url, 0, 1, 1);
                }
            }
        }
        clamp_scroll();
        if (g_dirty > 0) { draw_all(); g_dirty--; }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    free_doc();
    httpcExit();
    gfxExit();
    return 0;
}
