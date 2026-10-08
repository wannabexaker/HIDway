/*
 * HIDway client - remote PC interface.
 *
 * A visible window with an ARM/DISARM switch. While ARMED it reads this PC's
 * own keyboard and mouse through Raw Input and relays the COMPLETE current
 * state to the Raspberry Pi over UDP (through Tailscale) at a steady rate plus
 * on every change. It never sends event streams, so a lost packet self-
 * corrects and a key can never stick.
 *
 * It does not block local input (Raw Input without RIDEV_NOLEGACY): while
 * ARMED, keys still reach the focused app on this PC too. Local input blocking
 * is a separate, later step.
 *
 * The on-screen "recent input" list is in memory only, never written to disk,
 * and cleared on exit.
 *
 * Presentation: per-monitor DPI aware, one layout table shared by painting and
 * child placement, anti-aliased shapes through GDI+, ClearType text through
 * GDI, every owner-drawn control paints its whole rectangle (no default-grey
 * corners) and the parent uses WS_CLIPCHILDREN with a double-buffered paint.
 *
 * Dev aid: "--preview" / "--preview-armed" render the UI with sample data and
 * no network or input capture; "--shot=<file.bmp>" saves a screenshot of that
 * preview and exits ("--hot=arm,save,..." shows hover states).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <timeapi.h>
#include <uxtheme.h>

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "hidway_crypto.h"
#include "hidway_kbd.h"
#include "keymap_sc2hid.h"
#include "net.h"
#include "protocol.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")

/* ---- DWM attributes (Windows 11) ---------------------------------------- */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define HW_DWMWA_CORNER   33
#define HW_DWMWA_BORDER   34
#define HW_DWMWA_CAPTION  35
#define HW_DWMWA_TEXT     36
#define HW_DWMWCP_ROUND   2

/* ---- minimal GDI+ flat API (C has no gdiplus headers) ------------------- */
typedef DWORD HW_ARGB;
typedef struct { float X, Y; } HW_PointF;
typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} HW_GdiplusStartupInput;
int WINAPI GdiplusStartup(ULONG_PTR *token, const HW_GdiplusStartupInput *in, void *out);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int WINAPI GdipCreateFromHDC(HDC hdc, void **graphics);
int WINAPI GdipDeleteGraphics(void *graphics);
int WINAPI GdipSetSmoothingMode(void *graphics, int mode);
int WINAPI GdipSetPixelOffsetMode(void *graphics, int mode);
int WINAPI GdipCreatePath(int fill_mode, void **path);
int WINAPI GdipDeletePath(void *path);
int WINAPI GdipAddPathArc(void *path, float x, float y, float w, float h, float start, float sweep);
int WINAPI GdipClosePathFigure(void *path);
int WINAPI GdipCreateSolidFill(HW_ARGB color, void **brush);
int WINAPI GdipDeleteBrush(void *brush);
int WINAPI GdipFillPath(void *graphics, void *brush, void *path);
int WINAPI GdipFillEllipse(void *graphics, void *brush, float x, float y, float w, float h);
int WINAPI GdipCreatePen1(HW_ARGB color, float width, int unit, void **pen);
int WINAPI GdipDeletePen(void *pen);
int WINAPI GdipSetPenStartCap(void *pen, int cap);
int WINAPI GdipSetPenEndCap(void *pen, int cap);
int WINAPI GdipSetPenLineJoin(void *pen, int join);
int WINAPI GdipDrawLines(void *graphics, void *pen, const HW_PointF *points, int count);

/* ---- constants ----------------------------------------------------------- */
#define HK_TOGGLE 1
#define HK_PANIC  2
#define TIMER_UI    1
#define TIMER_NET   2
#define TIMER_PROBE 3
#define TIMER_SAVED 4
#define TIMER_SHOT  5
#define UI_REFRESH_MS 50
#define PROBE_MS      400
#define LINK_RECENT_US 900000u
#define HISTORY_MAX 80
#define WM_TRAYICON (WM_APP + 1)
#define WIN_STYLE (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN)

enum {
    ID_HISTORY = 1001, ID_CLEAR, ID_TARGET, ID_PORT, ID_RATE, ID_AUTO,
    ID_CB_KBD, ID_CB_MOUSE, ID_CB_HIST, ID_SAVE, ID_ARM, ID_PANIC,
    IDM_ARM, IDM_PANIC, IDM_SHOW, IDM_QUIT
};

enum { BTN_L = 1, BTN_R = 2, BTN_M = 4, BTN_X1 = 8, BTN_X2 = 16 };

/* ---- palette: neutral dark "mouse grey" ----------------------------------- */
#define C_BG        RGB(30, 31, 34)
#define C_CARD      RGB(37, 38, 42)
#define C_CARD_BD   RGB(50, 52, 57)
#define C_TILE      RGB(44, 46, 51)
#define C_FIELD     RGB(43, 45, 50)
#define C_FIELD_BD  RGB(62, 65, 71)
#define C_FOCUS_BD  RGB(140, 145, 154)
#define C_CHIP      RGB(54, 56, 62)
#define C_CHIP_BD   RGB(72, 75, 82)
#define C_TEXT      RGB(236, 237, 239)
#define C_TEXT2     RGB(168, 171, 178)
#define C_TEXT3     RGB(118, 121, 129)
#define C_WHITE     RGB(255, 255, 255)
#define C_BTN       RGB(95, 99, 107)   /* mouse grey */
#define C_BTN_H     RGB(111, 115, 123)
#define C_BTN_P     RGB(83, 86, 93)
#define C_SEC       RGB(48, 50, 55)
#define C_SEC_H     RGB(60, 62, 68)
#define C_SEC_BD    RGB(68, 71, 77)
#define C_DNG       RGB(64, 31, 34)
#define C_DNG_H     RGB(82, 37, 41)
#define C_DNG_P     RGB(54, 27, 30)
#define C_DNG_BD    RGB(140, 58, 64)
#define C_DNG_BD_H  RGB(182, 76, 84)
#define C_DNG_TX    RGB(255, 154, 146)
#define C_LIVE      RGB(63, 185, 80)
#define C_LIVE_F    RGB(30, 46, 35)
#define C_LIVE_BD   RGB(48, 102, 62)
#define C_LIVE_TX   RGB(130, 230, 140)
#define C_WARN      RGB(248, 96, 88)
#define C_AMBER     RGB(214, 160, 56)

typedef struct {
    int W, H;
    RECT mark, pill;
    int title_x, title_y, sub_y;
    RECT stats, tiles[4];
    RECT live;
    RECT recent, clear, list;
    RECT settings, save, frame[4];
    int label_y;
    RECT cb[3];
    int hk_cy;
    RECT arm, panic;
    int band_bottom;
} layout_t;

static struct {
    hidway_config_t cfg;
    int preview; /* 0 normal, 1 preview disarmed, 2 preview armed */
    char shot_path[MAX_PATH];
    char hot_list[96];

    int armed;
    int net_ok;

    uint8_t modifiers;
    uint8_t keys[HIDWAY_KEY_BITMAP_BYTES];
    uint8_t buttons;

    int32_t cum_x, cum_y;
    int16_t cum_wheel, cum_pan;
    int wheel_rem_v, wheel_rem_h;
    int32_t disp_x, disp_y;
    int16_t disp_wheel, disp_pan;
    int32_t show_dx, show_dy;
    int show_dw, show_dp;

    uint32_t session_id;
    uint32_t seq;
    unsigned sent_in_sec, pps;
    uint32_t sec_start_us;
    int rtt_ms;
    uint32_t pi_frames_ok;
    uint16_t pi_gaps;
    uint16_t pi_flags;
    int status_seen;
    int ever_status;
    uint32_t last_status_us;
    uint32_t armed_at_ms;
    int prev_link_up;
    int toggle_hk_ok;
    int probes_unanswered;
    int focus_id;
    int saved_flash;

    int crypto_on;                /* a valid key is configured: seal everything */
    int key_error;                /* a key is configured but invalid: send nothing */
    uint8_t key[HIDWAY_KEY_LEN];

    UINT dpi;
    layout_t L;
    HINSTANCE inst;
    HWND hwnd;
    HWND history, clear, edit[4], cb[3], save, arm, panic;
    HFONT f_title, f_sub, f_section, f_label, f_body, f_pill, f_tile, f_mono, f_mono_s,
          f_btn, f_btn_big, f_chip;
    HICON mark_icon;
    HBRUSH b_bg, b_field;
    ULONG_PTR gdip;

    NOTIFYICONDATAA nid;
    int tray_shown;
    HICON ic_grey, ic_green, ic_red;
    int cur_icon;
} g;

static int S(int v)
{
    return MulDiv(v, (int)g.dpi, 96);
}

/* ======================================================================== */
/*  Text and shape primitives                                               */
/* ======================================================================== */

static void to_wide(const char *s, WCHAR *w, int cap)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap))
        w[0] = 0;
}

static void text_at(HDC dc, HFONT f, COLORREF c, int x, int y, const char *s)
{
    WCHAR w[256];
    to_wide(s, w, 256);
    SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, x, y, w, lstrlenW(w));
}

static int text_w(HDC dc, HFONT f, const char *s)
{
    WCHAR w[256];
    SIZE sz = {0};
    to_wide(s, w, 256);
    SelectObject(dc, f);
    GetTextExtentPoint32W(dc, w, lstrlenW(w), &sz);
    return sz.cx;
}

static int font_h(HDC dc, HFONT f)
{
    TEXTMETRICW tm;
    SelectObject(dc, f);
    GetTextMetricsW(dc, &tm);
    return tm.tmHeight;
}

/* Draw text vertically centred on the line y = cy. */
static void text_vc(HDC dc, HFONT f, COLORREF c, int x, int cy, const char *s)
{
    text_at(dc, f, c, x, cy - font_h(dc, f) / 2, s);
}

static void text_in(HDC dc, HFONT f, COLORREF c, RECT r, UINT fmt, const char *s)
{
    WCHAR w[256];
    to_wide(s, w, 256);
    SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, w, -1, &r, fmt | DT_NOPREFIX);
}

/* Uppercase label with letter-spacing (section titles, tile labels). */
static void tracked(HDC dc, HFONT f, COLORREF c, int x, int y, const char *s)
{
    SetTextCharacterExtra(dc, S(1));
    text_at(dc, f, c, x, y, s);
    SetTextCharacterExtra(dc, 0);
}

static int tracked_w(HDC dc, HFONT f, const char *s)
{
    SetTextCharacterExtra(dc, S(1));
    int w = text_w(dc, f, s);
    SetTextCharacterExtra(dc, 0);
    return w;
}

static HW_ARGB argb(COLORREF c)
{
    return 0xFF000000u | ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | GetBValue(c);
}

static void *gp_begin(HDC dc)
{
    void *gr = NULL;
    GdipCreateFromHDC(dc, &gr);
    GdipSetSmoothingMode(gr, 4);   /* AntiAlias */
    GdipSetPixelOffsetMode(gr, 4); /* Half */
    return gr;
}

static void *path_round(float x, float y, float w, float h, float r)
{
    void *p = NULL;
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    GdipCreatePath(0, &p);
    if (d < 1.0f) {
        GdipAddPathArc(p, x, y, 0.01f, 0.01f, 180, 90);
        GdipAddPathArc(p, x + w, y, 0.01f, 0.01f, 270, 90);
        GdipAddPathArc(p, x + w, y + h, 0.01f, 0.01f, 0, 90);
        GdipAddPathArc(p, x, y + h, 0.01f, 0.01f, 90, 90);
    } else {
        GdipAddPathArc(p, x, y, d, d, 180, 90);
        GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
        GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
        GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    }
    GdipClosePathFigure(p);
    return p;
}

static void fill_path(void *gr, void *path, COLORREF c)
{
    void *b = NULL;
    GdipCreateSolidFill(argb(c), &b);
    GdipFillPath(gr, b, path);
    GdipDeleteBrush(b);
}

/* Anti-aliased rounded rectangle; the 1px border (if different from the
 * fill) is drawn inside the rectangle so edges stay crisp. */
static void round_rect(HDC dc, RECT r, int rad, COLORREF fill, COLORREF border)
{
    void *gr = gp_begin(dc);
    float x = (float)r.left, y = (float)r.top;
    float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    void *outer = path_round(x, y, w, h, (float)rad);
    fill_path(gr, outer, border);
    GdipDeletePath(outer);
    if (border != fill) {
        float t = (float)(S(1) > 0 ? S(1) : 1);
        void *inner = path_round(x + t, y + t, w - 2 * t, h - 2 * t, (float)rad - t);
        fill_path(gr, inner, fill);
        GdipDeletePath(inner);
    }
    GdipDeleteGraphics(gr);
}

static void dot(HDC dc, int cx, int cy, int r, COLORREF c)
{
    void *gr = gp_begin(dc);
    void *b = NULL;
    GdipCreateSolidFill(argb(c), &b);
    GdipFillEllipse(gr, b, (float)(cx - r), (float)(cy - r), (float)(2 * r), (float)(2 * r));
    GdipDeleteBrush(b);
    GdipDeleteGraphics(gr);
}

static void check_mark(HDC dc, RECT box, COLORREF c)
{
    void *gr = gp_begin(dc);
    void *pen = NULL;
    float w = (float)(box.right - box.left), h = (float)(box.bottom - box.top);
    float x = (float)box.left, y = (float)box.top;
    GdipCreatePen1(argb(c), (float)g.dpi / 96.0f * 1.9f, 2 /* UnitPixel */, &pen);
    GdipSetPenStartCap(pen, 2); /* round */
    GdipSetPenEndCap(pen, 2);
    GdipSetPenLineJoin(pen, 2);
    HW_PointF pts[3] = {{x + w * 0.26f, y + h * 0.53f}, {x + w * 0.44f, y + h * 0.71f}, {x + w * 0.76f, y + h * 0.33f}};
    GdipDrawLines(gr, pen, pts, 3);
    GdipDeletePen(pen);
    GdipDeleteGraphics(gr);
}

/* Keycap-style chip centred on cy; returns its width. */
static int chip(HDC dc, int x, int cy, int h, const char *label, COLORREF fill, COLORREF bd, COLORREF tx)
{
    int pad = S(8);
    int w = text_w(dc, g.f_chip, label) + 2 * pad;
    if (w < h)
        w = h;
    RECT r = {x, cy - h / 2, x + w, cy - h / 2 + h};
    round_rect(dc, r, S(5), fill, bd);
    text_in(dc, g.f_chip, tx, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, label);
    return w;
}

static void fill_solid(HDC dc, RECT r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

/* ======================================================================== */
/*  Naming helpers                                                          */
/* ======================================================================== */

static const struct { uint8_t bit; const char *name; } MODS[] = {
    {0x01, "Ctrl"}, {0x02, "Shift"}, {0x04, "Alt"}, {0x08, "Win"},
    {0x10, "RCtrl"}, {0x20, "RShift"}, {0x40, "AltGr"}, {0x80, "RWin"},
};

/* "ctrl+shift+f12" -> tokens {"Ctrl","Shift","F12"} */
static int hotkey_tokens(const char *text, char tok[6][12])
{
    char buf[64];
    int n = 0;
    strncpy(buf, text, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char *save = NULL;
    for (char *t = strtok_s(buf, "+", &save); t && n < 6; t = strtok_s(NULL, "+", &save)) {
        char *o = tok[n++];
        if (!strcmp(t, "ctrl")) strcpy(o, "Ctrl");
        else if (!strcmp(t, "alt")) strcpy(o, "Alt");
        else if (!strcmp(t, "shift")) strcpy(o, "Shift");
        else if (!strcmp(t, "win")) strcpy(o, "Win");
        else if (!strcmp(t, "pgup")) strcpy(o, "PgUp");
        else if (!strcmp(t, "pgdn")) strcpy(o, "PgDn");
        else {
            snprintf(o, 12, "%s", t);
            o[0] = (char)toupper((unsigned char)o[0]);
        }
    }
    return n;
}

static void hotkey_pretty(const char *text, char *out, size_t cap)
{
    char tok[6][12];
    int n = hotkey_tokens(text, tok);
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        strncat(out, tok[i], cap - strlen(out) - 1);
        if (i + 1 < n)
            strncat(out, "+", cap - strlen(out) - 1);
    }
}

/* ======================================================================== */
/*  History (memory only)                                                   */
/* ======================================================================== */

/* kind: 'd' key/button down, 'u' up, 'i' info, 'w' warning */
static void history_add(char kind, const char *label)
{
    if (!g.cfg.history_enabled || !g.history)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[128];
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d" "\x1f" "%s" "\x1f" "%c",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, label, kind);
    SendMessageA(g.history, LB_INSERTSTRING, 0, (LPARAM)line);
    int n = (int)SendMessageA(g.history, LB_GETCOUNT, 0, 0);
    while (n-- > HISTORY_MAX)
        SendMessageA(g.history, LB_DELETESTRING, HISTORY_MAX, 0);
}

/* ======================================================================== */
/*  Transport                                                               */
/* ======================================================================== */

static void net_send_state(uint8_t type)
{
    if (!g.net_ok || g.key_error)
        return;
    hidway_input_pkt_t p;
    memset(&p, 0, sizeof p);
    p.type = type;
    p.session_id = g.session_id;
    p.seq = ++g.seq;
    p.client_time_us = hidway_now_us();
    if (type == HIDWAY_MSG_STATE) {
        if (g.cfg.relay_keyboard) {
            p.mods = g.modifiers;
            memcpy(p.keys, g.keys, sizeof p.keys);
        }
        if (g.cfg.relay_mouse) {
            p.buttons = g.buttons;
            p.x = g.cum_x;
            p.y = g.cum_y;
            p.wheel = g.cum_wheel;
            p.pan = g.cum_pan;
        }
    }
    uint8_t buf[HIDWAY_INPUT_PKT_LEN];
    size_t n = hidway_input_encode(buf, &p);
    if (!g.crypto_on) {
        if (hidway_net_send(buf, n) > 0)
            g.sent_in_sec++;
        return;
    }
    /* End-to-end: only ciphertext leaves this PC. */
    uint8_t nonce[HIDWAY_NONCE_LEN], env[HIDWAY_SEAL_MAX];
    if (hidway_random(nonce, sizeof nonce) != 0)
        return; /* no randomness: send nothing rather than weaken the nonce */
    size_t el = hidway_seal(env, sizeof env, g.key, nonce, hidway_wall_us(), buf, n);
    hidway_wipe(buf, sizeof buf);
    if (el && hidway_net_send(env, el) > 0)
        g.sent_in_sec++;
}

static void net_drain_status(void)
{
    uint8_t buf[HIDWAY_SEAL_MAX + 16];
    for (int i = 0; i < 8; i++) {
        int n = hidway_net_recv(buf, sizeof buf);
        if (n <= 0)
            break;
        const uint8_t *pkt = buf;
        size_t len = (size_t)n;
        uint8_t inner[HIDWAY_SEAL_MAX_INNER];
        if (g.crypto_on) {
            /* accept only authentic replies sealed with our key */
            len = hidway_open(inner, sizeof inner, g.key, buf, (size_t)n, NULL);
            if (!len)
                continue;
            pkt = inner;
        } else if (hidway_is_sealed(buf, (size_t)n)) {
            continue; /* relay has a key but we do not */
        }
        hidway_status_pkt_t s;
        if (!hidway_status_decode(pkt, len, &s) || s.session_id != g.session_id)
            continue;
        g.status_seen = 1;
        g.ever_status = 1;
        g.probes_unanswered = 0;
        g.last_status_us = hidway_now_us();
        g.pi_frames_ok = s.frames_ok;
        g.pi_gaps = s.seq_gaps;
        g.pi_flags = s.flags;
        uint32_t rtt = hidway_now_us() - s.client_time_us;
        if (rtt < 10000000u)
            g.rtt_ms = (int)(rtt / 1000u);
    }
}

static void set_armed(int on);
static void invalidate_readouts(void);

static void net_tick(void)
{
    net_send_state(HIDWAY_MSG_STATE);
    net_drain_status();

    uint32_t now = hidway_now_us();
    if (now - g.sec_start_us >= 1000000u) {
        g.pps = g.sent_in_sec;
        g.sent_in_sec = 0;
        g.sec_start_us = now;
    }
    if (g.armed && g.status_seen && g.cfg.auto_disarm_ms > 0 &&
        (now - g.last_status_us) > (uint32_t)g.cfg.auto_disarm_ms * 1000u) {
        set_armed(0);
        history_add('w', "Link lost - auto-disarmed");
    }
}

static int link_recent(void)
{
    return g.ever_status && (hidway_now_us() - g.last_status_us) < LINK_RECENT_US;
}

static void net_probe(void)
{
    if (g.net_ok) {
        net_send_state(HIDWAY_MSG_PROBE);
        g.probes_unanswered++;
    }
    net_drain_status();
}

/* ======================================================================== */
/*  Tray                                                                    */
/* ======================================================================== */

static HICON make_dot_icon(COLORREF c)
{
    const int S2 = 32;
    HDC sdc = GetDC(NULL);
    HDC dc = CreateCompatibleDC(sdc);
    HBITMAP color = CreateCompatibleBitmap(sdc, S2, S2);
    HBITMAP mask = CreateBitmap(S2, S2, 1, 1, NULL);
    RECT full = {0, 0, S2, S2};

    HGDIOBJ ob = SelectObject(dc, color);
    FillRect(dc, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));
    HBRUSH fill = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ of = SelectObject(dc, fill), op = SelectObject(dc, pen);
    Ellipse(dc, 4, 4, S2 - 4, S2 - 4);
    SelectObject(dc, of);
    SelectObject(dc, op);
    DeleteObject(fill);
    DeleteObject(pen);

    SelectObject(dc, mask);
    FillRect(dc, &full, (HBRUSH)GetStockObject(WHITE_BRUSH));
    HGDIOBJ obr = SelectObject(dc, GetStockObject(BLACK_BRUSH));
    Ellipse(dc, 4, 4, S2 - 4, S2 - 4);
    SelectObject(dc, obr);

    SelectObject(dc, ob);
    DeleteDC(dc);
    ReleaseDC(NULL, sdc);

    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

static void tray_update(void)
{
    if (!g.tray_shown)
        return;
    int want = g.armed ? (link_recent() ? 1 : 2) : 0;

    char tip[128];
    if (g.armed)
        snprintf(tip, sizeof tip, "HIDway - ARMED (RTT %d ms)", g.rtt_ms);
    else if (link_recent())
        snprintf(tip, sizeof tip, "HIDway - disarmed, relay reachable (%d ms)", g.rtt_ms);
    else
        snprintf(tip, sizeof tip, "HIDway - disarmed");

    g.nid.uFlags = NIF_TIP;
    strncpy(g.nid.szTip, tip, sizeof g.nid.szTip - 1);
    g.nid.szTip[sizeof g.nid.szTip - 1] = 0;
    if (want != g.cur_icon) {
        g.cur_icon = want;
        g.nid.hIcon = want == 1 ? g.ic_green : want == 2 ? g.ic_red : g.ic_grey;
        g.nid.uFlags |= NIF_ICON;
    }
    Shell_NotifyIconA(NIM_MODIFY, &g.nid);
}

static void tray_balloon(const char *title, const char *text)
{
    if (!g.tray_shown)
        return;
    NOTIFYICONDATAA b = g.nid;
    b.uFlags = NIF_INFO;
    b.dwInfoFlags = NIIF_WARNING;
    strncpy(b.szInfoTitle, title, sizeof b.szInfoTitle - 1);
    strncpy(b.szInfo, text, sizeof b.szInfo - 1);
    Shell_NotifyIconA(NIM_MODIFY, &b);
}

static void tray_init(HWND hwnd)
{
    g.ic_grey = make_dot_icon(RGB(128, 132, 140));
    g.ic_green = make_dot_icon(C_LIVE);
    g.ic_red = make_dot_icon(C_WARN);

    memset(&g.nid, 0, sizeof g.nid);
    g.nid.cbSize = sizeof g.nid;
    g.nid.hWnd = hwnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.nid.uCallbackMessage = WM_TRAYICON;
    g.nid.hIcon = g.ic_grey;
    g.cur_icon = 0;
    strcpy(g.nid.szTip, "HIDway - disarmed");
    g.tray_shown = Shell_NotifyIconA(NIM_ADD, &g.nid);
}

static void tray_destroy(void)
{
    if (g.tray_shown)
        Shell_NotifyIconA(NIM_DELETE, &g.nid);
    if (g.ic_grey) DestroyIcon(g.ic_grey);
    if (g.ic_green) DestroyIcon(g.ic_green);
    if (g.ic_red) DestroyIcon(g.ic_red);
}

static void show_main_window(int show)
{
    if (show) {
        ShowWindow(g.hwnd, SW_SHOW);
        ShowWindow(g.hwnd, SW_RESTORE);
        SetForegroundWindow(g.hwnd);
    } else {
        ShowWindow(g.hwnd, SW_HIDE);
    }
}

static void monitor(void)
{
    if (!g.armed)
        net_probe();
    else
        net_drain_status();

    int up = g.armed && link_recent();
    if (g.prev_link_up && !up && g.armed) {
        FLASHWINFO fw = {sizeof fw, g.hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
        FlashWindowEx(&fw);
        tray_balloon("HIDway", "Link lost - input released on the target.");
        history_add('w', "Link lost");
    }
    g.prev_link_up = up;
    tray_update();
    invalidate_readouts();
}

/* ======================================================================== */
/*  Input capture                                                           */
/* ======================================================================== */

static void clear_relay_state(void)
{
    g.modifiers = 0;
    memset(g.keys, 0, sizeof g.keys);
    g.buttons = 0;
}

static int net_interval_ms(void)
{
    int ms = 1000 / (g.cfg.send_rate_hz > 0 ? g.cfg.send_rate_hz : 250);
    return ms < 1 ? 1 : ms;
}

static void set_armed(int on)
{
    if (on == g.armed)
        return;
    g.armed = on;
    clear_relay_state();

    if (on) {
        g.sec_start_us = hidway_now_us();
        g.sent_in_sec = 0;
        g.status_seen = 0;
        g.armed_at_ms = timeGetTime();
        net_send_state(HIDWAY_MSG_STATE);
        SetTimer(g.hwnd, TIMER_NET, net_interval_ms(), NULL);
        history_add('i', "Armed");
    } else {
        for (int i = 0; i < 3; i++)
            net_send_state(HIDWAY_MSG_RELEASE);
        KillTimer(g.hwnd, TIMER_NET);
        g.pps = 0;
        history_add('i', "Disarmed - all released");
    }
    InvalidateRect(g.arm, NULL, FALSE);
    invalidate_readouts();
    tray_update();
}

static void on_raw_keyboard(const RAWKEYBOARD *kb)
{
    if (!g.armed || !g.cfg.relay_keyboard || kb->MakeCode == 0 || kb->MakeCode == 0xFF)
        return;
    bool e0 = (kb->Flags & RI_KEY_E0) != 0;
    bool up = (kb->Flags & RI_KEY_BREAK) != 0;
    uint8_t sc = (uint8_t)kb->MakeCode;

    uint8_t mod = hidway_sc_to_modifier(sc, e0);
    if (mod) {
        bool was = (g.modifiers & mod) != 0;
        if (up) g.modifiers &= (uint8_t)~mod; else g.modifiers |= mod;
        if (was != !up)
            for (int i = 0; i < 8; i++)
                if (MODS[i].bit == mod)
                    history_add(up ? 'u' : 'd', MODS[i].name);
        return;
    }
    uint8_t usage = hidway_sc_to_usage(sc, e0);
    if (!usage)
        return;
    bool was = hidway_bitmap_test(g.keys, usage);
    if (up) hidway_bitmap_clear(g.keys, usage); else hidway_bitmap_set(g.keys, usage);
    if (was != !up) {
        char tmp[8];
        history_add(up ? 'u' : 'd', hidway_usage_name(usage, tmp));
    }
}

static void mouse_edge(USHORT bf, USHORT d, USHORT u, uint8_t bit, const char *name)
{
    if (bf & d) { g.buttons |= bit; history_add('d', name); }
    if (bf & u) { g.buttons &= (uint8_t)~bit; history_add('u', name); }
}

static void on_raw_mouse(const RAWMOUSE *m)
{
    if (!g.armed || !g.cfg.relay_mouse)
        return;
    if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) {
        g.cum_x += m->lLastX;
        g.cum_y += m->lLastY;
    }
    USHORT bf = m->usButtonFlags;
    mouse_edge(bf, RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, BTN_L, "Left click");
    mouse_edge(bf, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, BTN_R, "Right click");
    mouse_edge(bf, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, BTN_M, "Middle click");
    mouse_edge(bf, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, BTN_X1, "Mouse back");
    mouse_edge(bf, RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, BTN_X2, "Mouse forward");
    if (bf & RI_MOUSE_WHEEL) {
        g.wheel_rem_v += (SHORT)m->usButtonData;
        g.cum_wheel = (int16_t)(g.cum_wheel + g.wheel_rem_v / WHEEL_DELTA);
        g.wheel_rem_v %= WHEEL_DELTA;
    }
    if (bf & RI_MOUSE_HWHEEL) {
        g.wheel_rem_h += (SHORT)m->usButtonData;
        g.cum_pan = (int16_t)(g.cum_pan + g.wheel_rem_h / WHEEL_DELTA);
        g.wheel_rem_h %= WHEEL_DELTA;
    }
}

static void on_raw_input(HRAWINPUT h)
{
    UINT size = 0;
    if (GetRawInputData(h, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0)
        return;
    BYTE stack[256];
    BYTE *buf = size <= sizeof stack ? stack : (BYTE *)malloc(size);
    if (!buf)
        return;
    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
        RAWINPUT *ri = (RAWINPUT *)buf;
        if (ri->header.dwType == RIM_TYPEKEYBOARD)
            on_raw_keyboard(&ri->data.keyboard);
        else if (ri->header.dwType == RIM_TYPEMOUSE)
            on_raw_mouse(&ri->data.mouse);
    }
    if (buf != stack)
        free(buf);
}

/* ======================================================================== */
/*  Layout                                                                  */
/* ======================================================================== */

static RECT R(int l, int t, int r, int b)
{
    RECT x = {S(l), S(t), S(r), S(b)};
    return x;
}

static void compute_layout(void)
{
    layout_t *L = &g.L;
    const int W = 520, P = 18, XL = P + 14, XR = W - P - 14;

    L->W = S(W);
    L->mark = R(P, 18, P + 30, 48);
    L->title_x = S(P + 42);
    L->title_y = S(15);
    L->sub_y = S(38);
    L->pill = R(W - P - 122, 20, W - P, 46);

    /* CONNECTION: title row + four stat tiles */
    L->stats = R(P, 64, W - P, 164);
    int tile_w = (XR - XL - 3 * 8) / 4;
    for (int i = 0; i < 4; i++) {
        int x = XL + i * (tile_w + 8);
        L->tiles[i] = R(x, 100, x + tile_w, 150);
    }

    /* LIVE INPUT */
    L->live = R(P, 176, W - P, 272);

    /* RECENT INPUT */
    L->recent = R(P, 284, W - P, 424);
    L->clear = R(XR - 64, 290, XR, 314);
    L->list = R(XL - 2, 322, XR + 2, 412);

    /* SETTINGS */
    L->settings = R(P, 436, W - P, 594);
    L->save = R(XR - 64, 442, XR, 466);
    L->label_y = S(474);
    const int fw[4] = {226, 72, 66, 72};
    int x = XL - 2;
    for (int i = 0; i < 4; i++) {
        L->frame[i] = R(x, 492, x + fw[i], 522);
        x += fw[i] + 8;
    }
    L->cb[0] = R(XL - 2, 534, XL + 108, 556);
    L->cb[1] = R(XL + 116, 534, XL + 210, 556);
    L->cb[2] = R(XL + 218, 534, XL + 312, 556);
    L->hk_cy = S(574);

    /* action bar */
    L->arm = R(P, 608, W - P - 8 - 150, 654);
    L->panic = R(W - P - 150, 608, W - P, 654);

    L->H = S(672);
    L->band_bottom = S(274);
}

static void place(HWND h, RECT r)
{
    SetWindowPos(h, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static void apply_layout(void)
{
    layout_t *L = &g.L;
    RECT lr = L->list;
    InflateRect(&lr, -S(4), -S(4));
    place(g.history, lr);
    SendMessageA(g.history, LB_SETITEMHEIGHT, 0, S(21));
    place(g.clear, L->clear);
    place(g.save, L->save);
    place(g.arm, L->arm);
    place(g.panic, L->panic);
    for (int i = 0; i < 3; i++)
        place(g.cb[i], L->cb[i]);

    HDC dc = GetDC(g.hwnd);
    int eh = font_h(dc, g.f_body) + S(2);
    ReleaseDC(g.hwnd, dc);
    for (int i = 0; i < 4; i++) {
        RECT f = L->frame[i];
        RECT e = {f.left + S(9), f.top + ((f.bottom - f.top) - eh) / 2, f.right - S(9), 0};
        e.bottom = e.top + eh;
        place(g.edit[i], e);
        SendMessageA(g.edit[i], WM_SETFONT, (WPARAM)g.f_body, TRUE);
        SendMessageA(g.edit[i], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    }
}

static void invalidate_readouts(void)
{
    if (!g.hwnd)
        return;
    RECT rc = {0, 0, g.L.W, g.L.band_bottom};
    InvalidateRect(g.hwnd, &rc, FALSE);
}

static void invalidate_fields(void)
{
    RECT rc = {g.L.frame[0].left - S(2), g.L.frame[0].top - S(2),
               g.L.frame[3].right + S(2), g.L.frame[3].bottom + S(2)};
    InvalidateRect(g.hwnd, &rc, FALSE);
}

/* ======================================================================== */
/*  Fonts                                                                   */
/* ======================================================================== */

static HFONT mkfont(int tenths_px, int weight, const char *face)
{
    return CreateFontA(-MulDiv(tenths_px, (int)g.dpi, 960), 0, 0, 0, weight, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, face);
}

static void destroy_fonts(void)
{
    HFONT *fs[] = {&g.f_title, &g.f_sub, &g.f_section, &g.f_label, &g.f_body, &g.f_pill,
                   &g.f_tile, &g.f_mono, &g.f_mono_s, &g.f_btn, &g.f_btn_big, &g.f_chip};
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++)
        if (*fs[i]) {
            DeleteObject(*fs[i]);
            *fs[i] = NULL;
        }
}

static void create_fonts(void)
{
    const char *disp = "Segoe UI Variable Display", *txt = "Segoe UI Variable Text", *mono = "Cascadia Mono";
    destroy_fonts();
    g.f_title = mkfont(195, FW_SEMIBOLD, disp);
    g.f_sub = mkfont(120, FW_NORMAL, txt);
    g.f_section = mkfont(108, FW_SEMIBOLD, txt);
    g.f_label = mkfont(120, FW_NORMAL, txt);
    g.f_body = mkfont(130, FW_NORMAL, txt);
    g.f_pill = mkfont(118, FW_SEMIBOLD, txt);
    g.f_tile = mkfont(170, FW_SEMIBOLD, mono);
    g.f_mono = mkfont(120, FW_NORMAL, mono);
    g.f_mono_s = mkfont(115, FW_NORMAL, mono);
    g.f_btn = mkfont(125, FW_SEMIBOLD, txt);
    g.f_btn_big = mkfont(145, FW_SEMIBOLD, disp);
    g.f_chip = mkfont(115, FW_SEMIBOLD, txt);
}

static void load_mark_icon(void)
{
    if (g.mark_icon)
        DestroyIcon(g.mark_icon);
    int sz = g.L.mark.right - g.L.mark.left;
    g.mark_icon = (HICON)LoadImageA(g.inst, MAKEINTRESOURCEA(1), IMAGE_ICON, sz, sz, 0);
}

/* ======================================================================== */
/*  Painting                                                                */
/* ======================================================================== */

static void card(HDC dc, RECT r, const char *title)
{
    round_rect(dc, r, S(10), C_CARD, C_CARD_BD);
    tracked(dc, g.f_section, C_TEXT3, r.left + S(14), r.top + S(12), title);
}

static int title_cy(RECT card_rect)
{
    return card_rect.top + S(12) + S(7);
}

static void paint_header(HDC dc)
{
    layout_t *L = &g.L;
    if (g.mark_icon)
        DrawIconEx(dc, L->mark.left, L->mark.top, g.mark_icon, L->mark.right - L->mark.left,
                   L->mark.bottom - L->mark.top, 0, NULL, DI_NORMAL);
    text_at(dc, g.f_title, C_TEXT, L->title_x, L->title_y, "HIDway");
    text_at(dc, g.f_sub, C_TEXT3, L->title_x, L->sub_y, "Remote keyboard & mouse over real USB HID");

    RECT p = L->pill;
    COLORREF fill = g.armed ? C_LIVE_F : C_FIELD;
    COLORREF bd = g.armed ? C_LIVE_BD : C_FIELD_BD;
    COLORREF dc_ = g.armed ? C_LIVE : C_TEXT3;
    COLORREF tx = g.armed ? C_LIVE_TX : C_TEXT2;
    const char *label = g.armed ? "ARMED" : "DISARMED";
    round_rect(dc, p, (p.bottom - p.top) / 2, fill, bd);

    SetTextCharacterExtra(dc, S(1));
    int tw = text_w(dc, g.f_pill, label);
    SetTextCharacterExtra(dc, 0);
    int dd = S(8), gap = S(8);
    int gx = p.left + ((p.right - p.left) - (dd + gap + tw)) / 2;
    int cy = (p.top + p.bottom) / 2;
    dot(dc, gx + dd / 2, cy, dd / 2, dc_);
    SetTextCharacterExtra(dc, S(1));
    text_vc(dc, g.f_pill, tx, gx + dd + gap, cy, label);
    SetTextCharacterExtra(dc, 0);
}

static void paint_stats(HDC dc)
{
    layout_t *L = &g.L;
    card(dc, L->stats, "CONNECTION");

    /* end-to-end encryption badge next to the title */
    int bx = L->stats.left + S(14) + tracked_w(dc, g.f_section, "CONNECTION") + S(10);
    if (g.key_error)
        chip(dc, bx, title_cy(L->stats), S(18), "KEY ERROR", C_DNG, C_DNG_BD, C_DNG_TX);
    else if (g.crypto_on)
        chip(dc, bx, title_cy(L->stats), S(18), "E2E", C_LIVE_F, C_LIVE_BD, C_LIVE_TX);
    else
        chip(dc, bx, title_cy(L->stats), S(18), "E2E OFF", C_FIELD, C_FIELD_BD, C_TEXT3);

    /* status on the right of the title row */
    COLORREF sc;
    const char *st;
    if (g.key_error) { sc = C_WARN; st = "invalid key in hidway.ini"; }
    else if (!g.net_ok && !g.preview) { sc = C_WARN; st = "socket error"; }
    else if (!g.armed) {
        if (link_recent()) { sc = C_LIVE; st = "relay reachable"; }
        else if (g.ever_status || g.probes_unanswered > 5) { sc = C_WARN; st = "no reply from relay"; }
        else { sc = C_TEXT3; st = "probing relay"; }
    } else if (!g.status_seen) { sc = C_AMBER; st = "waiting for relay"; }
    else if (link_recent()) { sc = C_LIVE; st = (g.pi_flags & 0x0002) ? "linked · serial open" : "linked · no serial"; }
    else { sc = C_WARN; st = "link lost"; }

    char addr[160];
    snprintf(addr, sizeof addr, "%s:%d", g.cfg.target, g.cfg.port);
    int cy = title_cy(L->stats);
    int right = L->stats.right - S(14);
    int aw = text_w(dc, g.f_mono_s, addr);
    text_vc(dc, g.f_mono_s, C_TEXT3, right - aw, cy, addr);
    int sepw = text_w(dc, g.f_label, "  ·  ");
    text_vc(dc, g.f_label, C_TEXT3, right - aw - sepw, cy, "  ·  ");
    int sw = text_w(dc, g.f_label, st);
    int sx = right - aw - sepw - sw;
    text_vc(dc, g.f_label, sc == C_TEXT3 ? C_TEXT2 : sc, sx, cy, st);
    dot(dc, sx - S(9), cy, S(3), sc);

    /* tiles */
    char v[4][32];
    int live[4] = {0, 0, 0, 0};
    unsigned secs = g.armed ? (timeGetTime() - g.armed_at_ms) / 1000 : 0;
    if ((g.armed && g.status_seen) || (!g.armed && link_recent())) {
        snprintf(v[0], 32, "%d ms", g.rtt_ms);
        live[0] = 1;
    } else strcpy(v[0], "—");
    if (g.armed && g.status_seen) {
        double tot = (double)g.pi_frames_ok + g.pi_gaps;
        snprintf(v[1], 32, "%.1f %%", tot > 0 ? 100.0 * g.pi_gaps / tot : 0.0);
        live[1] = 1;
    } else strcpy(v[1], "—");
    if (g.armed) { snprintf(v[2], 32, "%u/s", g.pps); live[2] = 1; } else strcpy(v[2], "—");
    if (g.armed) { snprintf(v[3], 32, "%02u:%02u", secs / 60, secs % 60); live[3] = 1; } else strcpy(v[3], "—");

    static const char *names[4] = {"RTT", "LOSS", "RATE", "ARMED FOR"};
    for (int i = 0; i < 4; i++) {
        RECT t = L->tiles[i];
        round_rect(dc, t, S(8), C_TILE, C_TILE);
        tracked(dc, g.f_section, C_TEXT3, t.left + S(11), t.top + S(8), names[i]);
        text_at(dc, g.f_tile, live[i] ? C_TEXT : C_TEXT3, t.left + S(11), t.top + S(23), v[i]);
    }
}

static void paint_live(HDC dc)
{
    layout_t *L = &g.L;
    RECT c = L->live;
    card(dc, c, "LIVE INPUT");

    const char *mode = g.cfg.relay_keyboard && g.cfg.relay_mouse ? "keyboard + mouse"
                     : g.cfg.relay_keyboard ? "keyboard only"
                     : g.cfg.relay_mouse ? "mouse only" : "nothing relayed";
    int mw = text_w(dc, g.f_label, mode);
    text_vc(dc, g.f_label, C_TEXT3, c.right - S(14) - mw, title_cy(c), mode);

    if (!g.armed) {
        char hk[48], hint[96];
        hotkey_pretty(g.cfg.toggle.text, hk, sizeof hk);
        snprintf(hint, sizeof hint, "Press ARM or %s to start relaying", hk);
        RECT a = {c.left, c.top + S(36), c.right, c.top + S(58)};
        RECT b = {c.left, c.top + S(58), c.right, c.top + S(80)};
        text_in(dc, g.f_body, C_TEXT2, a, DT_CENTER | DT_VCENTER | DT_SINGLELINE, "Input capture is off");
        text_in(dc, g.f_label, C_TEXT3, b, DT_CENTER | DT_VCENTER | DT_SINGLELINE, hint);
        return;
    }

    int lx = c.left + S(14), cx0 = c.left + S(70), maxx = c.right - S(14);
    int ch = S(22), gap = S(6);

    /* keys row */
    int cy = c.top + S(50);
    text_vc(dc, g.f_label, C_TEXT3, lx, cy, "Keys");
    int x = cx0, shown = 0, total = 0;
    const char *names[24];
    char tmp[24][8];
    int n = 0;
    for (int i = 0; i < 8 && n < 24; i++)
        if (g.modifiers & MODS[i].bit)
            names[n++] = MODS[i].name;
    for (unsigned u = HIDWAY_KEY_USAGE_MIN; u <= HIDWAY_KEY_USAGE_MAX && n < 24; u++)
        if (hidway_bitmap_test(g.keys, (uint8_t)u)) {
            names[n] = hidway_usage_name((uint8_t)u, tmp[n]);
            n++;
        }
    total = n;
    for (int i = 0; i < n; i++) {
        int w = text_w(dc, g.f_chip, names[i]) + 2 * S(8);
        if (w < ch) w = ch;
        if (x + w > maxx - S(40) && i < n - 1)
            break;
        x += chip(dc, x, cy, ch, names[i], C_CHIP, C_CHIP_BD, C_TEXT) + gap;
        shown++;
    }
    if (shown < total) {
        char more[8];
        snprintf(more, sizeof more, "+%d", total - shown);
        chip(dc, x, cy, ch, more, C_FIELD, C_FIELD_BD, C_TEXT2);
    }
    if (total == 0)
        text_vc(dc, g.f_label, C_TEXT3, cx0, cy, "none held");

    /* mouse row */
    cy = c.top + S(78);
    text_vc(dc, g.f_label, C_TEXT3, lx, cy, "Mouse");
    static const struct { uint8_t bit; const char *name; } BTNS[] = {
        {BTN_L, "Left"}, {BTN_R, "Right"}, {BTN_M, "Middle"}, {BTN_X1, "Back"}, {BTN_X2, "Fwd"}};
    x = cx0;
    for (int i = 0; i < 5; i++)
        if (g.buttons & BTNS[i].bit)
            x += chip(dc, x, cy, ch, BTNS[i].name, C_CHIP, C_CHIP_BD, C_TEXT) + gap;
    char mv[64];
    snprintf(mv, sizeof mv, "dx %+ld   dy %+ld   wheel %+d", (long)g.show_dx, (long)g.show_dy, g.show_dw);
    text_vc(dc, g.f_mono, C_TEXT2, x == cx0 ? cx0 : x + S(4), cy, mv);
}

static void paint_recent(HDC dc)
{
    layout_t *L = &g.L;
    card(dc, L->recent, "RECENT INPUT");
    int tw = tracked_w(dc, g.f_section, "RECENT INPUT");
    text_vc(dc, g.f_label, C_TEXT3, L->recent.left + S(14) + tw + S(10), title_cy(L->recent), "memory only");
    round_rect(dc, L->list, S(7), C_FIELD, C_FIELD_BD);
}

static void paint_settings(HDC dc)
{
    layout_t *L = &g.L;
    card(dc, L->settings, "SETTINGS");

    static const char *labels[4] = {"Relay address", "Port", "Rate (Hz)", "Disarm (ms)"};
    for (int i = 0; i < 4; i++) {
        text_at(dc, g.f_label, C_TEXT2, L->frame[i].left + S(2), L->label_y, labels[i]);
        int focused = g.focus_id == ID_TARGET + i;
        round_rect(dc, L->frame[i], S(7), C_FIELD, focused ? C_FOCUS_BD : C_FIELD_BD);
    }

    /* hotkeys as keycaps */
    int x = L->frame[0].left + S(2), cy = L->hk_cy, ch = S(20), gap = S(4);
    if (!g.toggle_hk_ok) {
        char hk[48], msg[128];
        hotkey_pretty(g.cfg.toggle.text, hk, sizeof hk);
        snprintf(msg, sizeof msg, "Toggle hotkey %s is in use — set toggle_hotkey in hidway.ini", hk);
        text_vc(dc, g.f_label, C_WARN, x, cy, msg);
        return;
    }
    const char *groups[2] = {g.cfg.toggle.text, g.cfg.panic.text};
    const char *gl[2] = {"Toggle", "Panic"};
    for (int k = 0; k < 2; k++) {
        text_vc(dc, g.f_label, C_TEXT3, x, cy, gl[k]);
        x += text_w(dc, g.f_label, gl[k]) + S(8);
        char tok[6][12];
        int n = hotkey_tokens(groups[k], tok);
        for (int i = 0; i < n; i++)
            x += chip(dc, x, cy, ch, tok[i], C_CHIP, C_CHIP_BD, C_TEXT2) + gap;
        x += S(18);
    }
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, cr.right, cr.bottom);
    HGDIOBJ obmp = SelectObject(dc, bmp);

    fill_solid(dc, cr, C_BG);
    RECT tmp;
    RECT band = {0, 0, cr.right, g.L.band_bottom};
    if (IntersectRect(&tmp, &ps.rcPaint, &band)) {
        paint_header(dc);
        paint_stats(dc);
        paint_live(dc);
    }
    if (IntersectRect(&tmp, &ps.rcPaint, &g.L.recent))
        paint_recent(dc);
    if (IntersectRect(&tmp, &ps.rcPaint, &g.L.settings))
        paint_settings(dc);

    BitBlt(wdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
           ps.rcPaint.bottom - ps.rcPaint.top, dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    SelectObject(dc, obmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

/* ---- owner-drawn controls ---------------------------------------------- */

static int is_checkbox(int id)
{
    return id == ID_CB_KBD || id == ID_CB_MOUSE || id == ID_CB_HIST;
}

static int cb_checked(int id)
{
    return id == ID_CB_KBD ? g.cfg.relay_keyboard : id == ID_CB_MOUSE ? g.cfg.relay_mouse : g.cfg.history_enabled;
}

static void draw_button(const DRAWITEMSTRUCT *d)
{
    RECT r = d->rcItem;
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bm = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ ob = SelectObject(dc, bm);
    RECT lr = {0, 0, w, h};
    int id = (int)d->CtlID;
    bool hot = GetPropA(d->hwndItem, "hot") != NULL;
    bool down = (d->itemState & ODS_SELECTED) != 0;
    bool focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);

    /* paint what is behind the control first: no default-grey corners */
    fill_solid(dc, lr, (id == ID_ARM || id == ID_PANIC) ? C_BG : C_CARD);

    char label[32];
    GetWindowTextA(d->hwndItem, label, sizeof label);

    if (is_checkbox(id)) {
        int on = cb_checked(id);
        int box = S(16);
        RECT b = {S(1), (h - box) / 2, S(1) + box, (h - box) / 2 + box};
        if (on) {
            round_rect(dc, b, S(4), hot ? C_WHITE : C_TEXT, hot ? C_WHITE : C_TEXT);
            check_mark(dc, b, C_BG);
        } else {
            round_rect(dc, b, S(4), hot ? C_SEC_H : C_FIELD, hot ? C_FOCUS_BD : C_FIELD_BD);
        }
        text_vc(dc, g.f_body, hot ? C_WHITE : C_TEXT, b.right + S(8), h / 2, label);
        if (focus) {
            RECT fr = b;
            InflateRect(&fr, S(2), S(2));
            round_rect(dc, fr, S(5), C_FOCUS_BD, C_FOCUS_BD);
            InflateRect(&fr, -S(2), -S(2));
            if (on) {
                round_rect(dc, b, S(4), C_TEXT, C_TEXT);
                check_mark(dc, b, C_BG);
            } else {
                round_rect(dc, b, S(4), C_FIELD, C_FIELD_BD);
            }
        }
    } else {
        COLORREF fill, bd, tx;
        HFONT f = g.f_btn;
        int rad = S(7);
        if (id == ID_ARM) {
            fill = down ? C_BTN_P : hot ? C_BTN_H : C_BTN;
            bd = fill;
            tx = C_WHITE;
            f = g.f_btn_big;
            rad = S(10);
            strcpy(label, g.armed ? "DISARM" : "ARM");
        } else if (id == ID_PANIC) {
            fill = down ? C_DNG_P : hot ? C_DNG_H : C_DNG;
            bd = hot ? C_DNG_BD_H : C_DNG_BD;
            tx = hot ? RGB(255, 186, 178) : C_DNG_TX;
            f = g.f_btn_big;
            rad = S(10);
        } else {
            fill = down ? C_SEC : hot ? C_SEC_H : C_SEC;
            bd = hot ? C_FOCUS_BD : C_SEC_BD;
            tx = hot ? C_WHITE : C_TEXT;
            if (id == ID_SAVE && g.saved_flash)
                strcpy(label, "Saved ✓");
        }
        round_rect(dc, lr, rad, fill, bd);
        if (focus) {
            RECT fr = lr;
            InflateRect(&fr, -S(3), -S(3));
            round_rect(dc, fr, rad - S(2), C_FOCUS_BD, C_FOCUS_BD);
            InflateRect(&fr, -S(1), -S(1));
            round_rect(dc, fr, rad - S(3), fill, fill);
        }
        text_in(dc, f, tx, lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE, label);
    }

    BitBlt(d->hDC, r.left, r.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
}

static void draw_history_item(const DRAWITEMSTRUCT *d)
{
    if (d->itemID == (UINT)-1)
        return;
    RECT r = d->rcItem;
    fill_solid(d->hDC, r, C_FIELD);

    char buf[160];
    if (SendMessageA(d->hwndItem, LB_GETTEXT, d->itemID, (LPARAM)buf) == LB_ERR)
        return;
    char *t = buf, *lab = strchr(buf, '\x1f'), *kind = NULL;
    if (!lab)
        return;
    *lab++ = 0;
    kind = strchr(lab, '\x1f');
    if (kind)
        *kind++ = 0;
    char k = kind ? kind[0] : 'i';

    int cy = (r.top + r.bottom) / 2;
    text_vc(d->hDC, g.f_mono_s, C_TEXT3, r.left + S(10), cy, t);
    int ax = r.left + S(112), lx = r.left + S(132);
    if (k == 'd') {
        text_vc(d->hDC, g.f_body, C_TEXT, ax, cy, "↓");
        text_vc(d->hDC, g.f_body, C_TEXT, lx, cy, lab);
    } else if (k == 'u') {
        text_vc(d->hDC, g.f_body, C_TEXT3, ax, cy, "↑");
        text_vc(d->hDC, g.f_body, C_TEXT2, lx, cy, lab);
    } else if (k == 'w') {
        dot(d->hDC, ax + S(4), cy, S(3), C_WARN);
        text_vc(d->hDC, g.f_body, C_WARN, lx, cy, lab);
    } else {
        dot(d->hDC, ax + S(4), cy, S(3), C_TEXT3);
        text_vc(d->hDC, g.f_body, C_TEXT2, lx, cy, lab);
    }
}

static LRESULT CALLBACK btn_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC orig = (WNDPROC)GetPropA(h, "op");
    if (!orig)
        return DefWindowProcA(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; /* owner-draw paints everything; no grey flash */
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    case WM_MOUSEMOVE:
        if (!GetPropA(h, "hot")) {
            SetPropA(h, "hot", (HANDLE)1);
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, NULL, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropA(h, "hot");
        InvalidateRect(h, NULL, FALSE);
        break;
    case WM_NCDESTROY:
        RemovePropA(h, "hot");
        RemovePropA(h, "op");
        break;
    }
    return CallWindowProcA(orig, h, msg, wp, lp);
}

static LRESULT CALLBACK list_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC orig = (WNDPROC)GetPropA(h, "op");
    if (!orig)
        return DefWindowProcA(h, msg, wp, lp);
    /* Ask the original proc directly: SendMessage(LB_GETCOUNT) here would
     * re-enter this function forever. */
    int empty = (msg == WM_PAINT || msg == WM_ERASEBKGND) &&
                CallWindowProcA(orig, h, LB_GETCOUNT, 0, 0) == 0;
    if (msg == WM_ERASEBKGND && empty)
        return 1;
    if (msg == WM_PAINT && empty) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        fill_solid(dc, r, C_FIELD);
        text_in(dc, g.f_label, C_TEXT3, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE,
                g.cfg.history_enabled ? "No input yet — arm to start relaying" : "History is turned off");
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_NCDESTROY)
        RemovePropA(h, "op");
    return CallWindowProcA(orig, h, msg, wp, lp);
}

static void subclass(HWND h, WNDPROC proc)
{
    WNDPROC op = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)proc);
    SetPropA(h, "op", (HANDLE)op);
}

/* ======================================================================== */
/*  Settings                                                                */
/* ======================================================================== */

static int get_int(HWND edit, int fallback)
{
    char b[32];
    GetWindowTextA(edit, b, sizeof b);
    int v = atoi(b);
    return v ? v : fallback;
}

static void apply_settings(void)
{
    char old_target[128];
    int old_port = g.cfg.port;
    strcpy(old_target, g.cfg.target);

    GetWindowTextA(g.edit[0], g.cfg.target, sizeof g.cfg.target);
    g.cfg.port = get_int(g.edit[1], g.cfg.port);
    g.cfg.send_rate_hz = get_int(g.edit[2], g.cfg.send_rate_hz);
    g.cfg.auto_disarm_ms = get_int(g.edit[3], 0);
    if (g.cfg.auto_disarm_ms < 0)
        g.cfg.auto_disarm_ms = 0;

    if (!g.preview)
        hidway_config_save("hidway.ini", &g.cfg);
    if (!g.preview && (strcmp(old_target, g.cfg.target) != 0 || old_port != g.cfg.port)) {
        g.net_ok = (hidway_net_open(g.cfg.target, g.cfg.port) == 0);
        g.ever_status = 0;
        g.probes_unanswered = 0;
    }
    if (g.armed)
        SetTimer(g.hwnd, TIMER_NET, net_interval_ms(), NULL);
    history_add('i', "Settings saved");
    invalidate_readouts();
}

/* ======================================================================== */
/*  Window                                                                  */
/* ======================================================================== */

static HWND mk(HWND p, const char *cls, const char *txt, DWORD style, int id)
{
    return CreateWindowExA(0, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, p,
                           (HMENU)(INT_PTR)id, g.inst, NULL);
}

static void create_controls(HWND h)
{
    g.history = mk(h, "LISTBOX", "",
                   LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | LBS_NOSEL | WS_VSCROLL,
                   ID_HISTORY);
    SetWindowTheme(g.history, L"DarkMode_Explorer", NULL); /* dark scrollbar */
    subclass(g.history, list_proc);

    g.clear = mk(h, "BUTTON", "Clear", BS_OWNERDRAW | WS_TABSTOP, ID_CLEAR);
    g.save = mk(h, "BUTTON", "Save", BS_OWNERDRAW | WS_TABSTOP, ID_SAVE);
    g.arm = mk(h, "BUTTON", "ARM", BS_OWNERDRAW | WS_TABSTOP, ID_ARM);
    g.panic = mk(h, "BUTTON", "PANIC", BS_OWNERDRAW | WS_TABSTOP, ID_PANIC);
    g.cb[0] = mk(h, "BUTTON", "Keyboard", BS_OWNERDRAW | WS_TABSTOP, ID_CB_KBD);
    g.cb[1] = mk(h, "BUTTON", "Mouse", BS_OWNERDRAW | WS_TABSTOP, ID_CB_MOUSE);
    g.cb[2] = mk(h, "BUTTON", "History", BS_OWNERDRAW | WS_TABSTOP, ID_CB_HIST);

    char tmp[32];
    g.edit[0] = mk(h, "EDIT", g.cfg.target, ES_AUTOHSCROLL | WS_TABSTOP, ID_TARGET);
    snprintf(tmp, sizeof tmp, "%d", g.cfg.port);
    g.edit[1] = mk(h, "EDIT", tmp, ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, ID_PORT);
    snprintf(tmp, sizeof tmp, "%d", g.cfg.send_rate_hz);
    g.edit[2] = mk(h, "EDIT", tmp, ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, ID_RATE);
    snprintf(tmp, sizeof tmp, "%d", g.cfg.auto_disarm_ms);
    g.edit[3] = mk(h, "EDIT", tmp, ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, ID_AUTO);

    HWND btns[] = {g.clear, g.save, g.arm, g.panic, g.cb[0], g.cb[1], g.cb[2]};
    for (size_t i = 0; i < sizeof btns / sizeof btns[0]; i++)
        subclass(btns[i], btn_proc);

    apply_layout();
}

static int register_raw_input(HWND hwnd)
{
    RAWINPUTDEVICE rid[2] = {
        {0x01, 0x06, RIDEV_INPUTSINK, hwnd},
        {0x01, 0x02, RIDEV_INPUTSINK, hwnd},
    };
    return RegisterRawInputDevices(rid, 2, sizeof rid[0]) ? 0 : -1;
}

static void apply_window_chrome(HWND hwnd)
{
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    int corner = HW_DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, HW_DWMWA_CORNER, &corner, sizeof corner);
    COLORREF cap = C_BG, txt = C_TEXT2, bd = C_CARD_BD;
    DwmSetWindowAttribute(hwnd, HW_DWMWA_CAPTION, &cap, sizeof cap); /* seamless titlebar */
    DwmSetWindowAttribute(hwnd, HW_DWMWA_TEXT, &txt, sizeof txt);
    DwmSetWindowAttribute(hwnd, HW_DWMWA_BORDER, &bd, sizeof bd);
}

static void enable_dark_menus(void)
{
    HMODULE ux = LoadLibraryExA("uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux)
        return;
    typedef int(WINAPI * set_mode_t)(int);
    typedef void(WINAPI * flush_t)(void);
    set_mode_t set_mode = (set_mode_t)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    flush_t flush = (flush_t)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set_mode)
        set_mode(2); /* force dark context menus */
    if (flush)
        flush();
}

static void resize_to_layout(HWND hwnd, const RECT *pos)
{
    RECT wr = {0, 0, g.L.W, g.L.H};
    AdjustWindowRectExForDpi(&wr, WIN_STYLE, FALSE, 0, g.dpi);
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | (pos ? 0 : SWP_NOMOVE);
    SetWindowPos(hwnd, NULL, pos ? pos->left : 0, pos ? pos->top : 0,
                 wr.right - wr.left, wr.bottom - wr.top, flags);
}

/* ---- preview / screenshot (dev aid) ------------------------------------ */

static void preview_fill(void)
{
    g.toggle_hk_ok = 1;
    g.crypto_on = 1;
    g.key_error = 0;
    g.last_status_us = hidway_now_us();
    if (g.preview == 1) {
        g.ever_status = 1;
        g.rtt_ms = 91;
    } else {
        g.armed = 1;
        g.status_seen = g.ever_status = 1;
        g.rtt_ms = 92;
        g.pps = 250;
        g.pi_frames_ok = 48213;
        g.pi_gaps = 2;
        g.pi_flags = 0x0002;
        g.armed_at_ms = timeGetTime() - 83000;
        g.modifiers = 0x02;
        hidway_bitmap_set(g.keys, 0x1A); /* W */
        hidway_bitmap_set(g.keys, 0x2C); /* Space */
        g.buttons = BTN_L;
        g.show_dx = 14;
        g.show_dy = -3;
        static const char *rows[][3] = {
            {"21:04:11.902", "Armed", "i"},
            {"21:04:12.481", "W", "d"},
            {"21:04:12.483", "Shift", "d"},
            {"21:04:12.611", "Left click", "d"},
            {"21:04:12.702", "Left click", "u"},
            {"21:04:13.020", "Space", "d"},
            {"21:04:13.388", "E", "d"},
            {"21:04:13.452", "E", "u"},
        };
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
            char line[96];
            snprintf(line, sizeof line, "%s" "\x1f" "%s" "\x1f" "%s", rows[i][0], rows[i][1], rows[i][2]);
            SendMessageA(g.history, LB_INSERTSTRING, 0, (LPARAM)line);
        }
    }
    struct { const char *name; HWND h; } hot[] = {
        {"arm", g.arm}, {"panic", g.panic}, {"save", g.save}, {"clear", g.clear},
        {"kbd", g.cb[0]}, {"mouse", g.cb[1]}, {"hist", g.cb[2]},
    };
    for (size_t i = 0; i < sizeof hot / sizeof hot[0]; i++)
        if (strstr(g.hot_list, hot[i].name))
            SetPropA(hot[i].h, "hot", (HANDLE)1);
    if (strstr(g.hot_list, "focus"))
        g.focus_id = ID_TARGET;
}

static void save_screenshot(HWND hwnd)
{
    RECT wr, fr;
    GetWindowRect(hwnd, &wr);
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof fr)))
        fr = wr;
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    HDC sdc = GetDC(NULL);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP bm = CreateCompatibleBitmap(sdc, w, h);
    HGDIOBJ ob = SelectObject(mdc, bm);
    PrintWindow(hwnd, mdc, 2 /* PW_RENDERFULLCONTENT */);
    SelectObject(mdc, ob);

    int cx = fr.left - wr.left, cy = fr.top - wr.top;
    int cw = fr.right - fr.left, chh = fr.bottom - fr.top;
    BITMAPINFOHEADER bi = {sizeof bi, w, -h, 1, 32, BI_RGB};
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
    if (px && GetDIBits(sdc, bm, 0, (UINT)h, px, (BITMAPINFO *)&bi, DIB_RGB_COLORS)) {
        FILE *f = fopen(g.shot_path, "wb");
        if (f) {
            BITMAPINFOHEADER oi = {sizeof oi, cw, -chh, 1, 32, BI_RGB};
            BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof fh + sizeof oi + (size_t)cw * chh * 4), 0, 0,
                                   (DWORD)(sizeof fh + sizeof oi)};
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&oi, sizeof oi, 1, f);
            for (int y = 0; y < chh; y++)
                fwrite(px + ((size_t)(y + cy) * w + cx) * 4, 4, (size_t)cw, f);
            fclose(f);
        }
    }
    free(px);
    DeleteObject(bm);
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        g.dpi = GetDpiForWindow(hwnd);
        create_fonts();
        compute_layout();
        load_mark_icon();
        create_controls(hwnd);
        apply_window_chrome(hwnd);
        resize_to_layout(hwnd, NULL);
        if (!g.preview) {
            if (register_raw_input(hwnd) != 0)
                MessageBoxA(hwnd, "Failed to register raw input.", "HIDway", MB_ICONERROR);
            g.net_ok = (hidway_net_open(g.cfg.target, g.cfg.port) == 0);
            g.toggle_hk_ok = RegisterHotKey(hwnd, HK_TOGGLE, g.cfg.toggle.mods | MOD_NOREPEAT, g.cfg.toggle.vk) ? 1 : 0;
            if (g.cfg.panic.vk)
                RegisterHotKey(hwnd, HK_PANIC, g.cfg.panic.mods | MOD_NOREPEAT, g.cfg.panic.vk);
            tray_init(hwnd);
            SetTimer(hwnd, TIMER_PROBE, PROBE_MS, NULL);
        } else {
            preview_fill();
            if (g.shot_path[0])
                SetTimer(hwnd, TIMER_SHOT, 900, NULL);
        }
        SetTimer(hwnd, TIMER_UI, UI_REFRESH_MS, NULL);
        return 0;

    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        create_fonts();
        compute_layout();
        load_mark_icon();
        resize_to_layout(hwnd, (const RECT *)lp);
        apply_layout();
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;

    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
        if (mi->CtlType == ODT_LISTBOX)
            mi->itemHeight = (UINT)S(21);
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
        if (d->CtlType == ODT_LISTBOX)
            draw_history_item(d);
        else
            draw_button(d);
        return TRUE;
    }

    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wp, C_FIELD);
        SetTextColor((HDC)wp, C_TEXT2);
        return (LRESULT)g.b_field;
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp, C_FIELD);
        SetTextColor((HDC)wp, C_TEXT);
        return (LRESULT)g.b_field;

    case WM_LBUTTONDOWN: {
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
        for (int i = 0; i < 4; i++)
            if (PtInRect(&g.L.frame[i], pt)) {
                SetFocus(g.edit[i]);
                SendMessageA(g.edit[i], EM_SETSEL, 0, -1);
            }
        return 0;
    }

    case WM_INPUT:
        on_raw_input((HRAWINPUT)lp);
        return 0;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK) {
            show_main_window(1);
        } else if (lp == WM_RBUTTONUP) {
            HMENU m = CreatePopupMenu();
            AppendMenuA(m, MF_STRING, IDM_ARM, g.armed ? "Disarm" : "Arm");
            AppendMenuA(m, MF_STRING, IDM_PANIC, "Panic - release all");
            AppendMenuA(m, MF_SEPARATOR, 0, NULL);
            AppendMenuA(m, MF_STRING, IDM_SHOW, IsWindowVisible(hwnd) ? "Hide window" : "Show window");
            AppendMenuA(m, MF_SEPARATOR, 0, NULL);
            AppendMenuA(m, MF_STRING, IDM_QUIT, "Quit HIDway");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);
        }
        return 0;

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED && g.tray_shown)
            show_main_window(0); /* minimize to tray */
        return 0;

    case WM_HOTKEY:
        if (wp == HK_TOGGLE)
            set_armed(!g.armed);
        else if (wp == HK_PANIC) {
            set_armed(0);
            history_add('w', "PANIC - all released");
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_ARM: set_armed(!g.armed); return 0;
        case IDM_PANIC: set_armed(0); history_add('w', "PANIC - all released"); return 0;
        case IDM_SHOW: show_main_window(!IsWindowVisible(hwnd)); return 0;
        case IDM_QUIT: DestroyWindow(hwnd); return 0;
        }
        if (LOWORD(wp) >= ID_TARGET && LOWORD(wp) <= ID_AUTO) {
            if (HIWORD(wp) == EN_SETFOCUS) {
                g.focus_id = LOWORD(wp);
                invalidate_fields();
            } else if (HIWORD(wp) == EN_KILLFOCUS) {
                g.focus_id = 0;
                invalidate_fields();
            }
            return 0;
        }
        if (HIWORD(wp) == BN_CLICKED) {
            switch (LOWORD(wp)) {
            case ID_ARM: set_armed(!g.armed); break;
            case ID_PANIC: set_armed(0); history_add('w', "PANIC - all released"); break;
            case ID_CLEAR: SendMessageA(g.history, LB_RESETCONTENT, 0, 0); break;
            case ID_SAVE:
                apply_settings();
                g.saved_flash = 1;
                InvalidateRect(g.save, NULL, FALSE);
                SetTimer(hwnd, TIMER_SAVED, 1400, NULL);
                break;
            case ID_CB_KBD: g.cfg.relay_keyboard = !g.cfg.relay_keyboard; break;
            case ID_CB_MOUSE: g.cfg.relay_mouse = !g.cfg.relay_mouse; break;
            case ID_CB_HIST:
                g.cfg.history_enabled = !g.cfg.history_enabled;
                InvalidateRect(g.history, NULL, FALSE);
                break;
            }
            if (LOWORD(wp) >= ID_CB_KBD && LOWORD(wp) <= ID_CB_HIST) {
                InvalidateRect((HWND)lp, NULL, FALSE);
                invalidate_readouts();
            }
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_UI) {
            if (!g.preview) {
                g.show_dx = g.cum_x - g.disp_x;
                g.show_dy = g.cum_y - g.disp_y;
                g.show_dw = (int16_t)(g.cum_wheel - g.disp_wheel);
                g.show_dp = (int16_t)(g.cum_pan - g.disp_pan);
                g.disp_x = g.cum_x;
                g.disp_y = g.cum_y;
                g.disp_wheel = g.cum_wheel;
                g.disp_pan = g.cum_pan;
            } else {
                g.last_status_us = hidway_now_us(); /* preview: link always fresh */
            }
            invalidate_readouts();
        } else if (wp == TIMER_NET) {
            net_tick();
        } else if (wp == TIMER_PROBE) {
            monitor();
        } else if (wp == TIMER_SAVED) {
            KillTimer(hwnd, TIMER_SAVED);
            g.saved_flash = 0;
            InvalidateRect(g.save, NULL, FALSE);
        } else if (wp == TIMER_SHOT) {
            KillTimer(hwnd, TIMER_SHOT);
            RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            save_screenshot(hwnd);
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_CLOSE:
        set_armed(0);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY: {
        if (!g.preview) {
            WINDOWPLACEMENT wpl = {sizeof wpl};
            if (GetWindowPlacement(hwnd, &wpl)) {
                g.cfg.window_x = wpl.rcNormalPosition.left;
                g.cfg.window_y = wpl.rcNormalPosition.top;
                hidway_config_save("hidway.ini", &g.cfg);
            }
        }
        tray_destroy();
        KillTimer(hwnd, TIMER_UI);
        KillTimer(hwnd, TIMER_PROBE);
        UnregisterHotKey(hwnd, HK_TOGGLE);
        UnregisterHotKey(hwnd, HK_PANIC);
        hidway_net_close();
        destroy_fonts();
        if (g.mark_icon)
            DestroyIcon(g.mark_icon);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void parse_args(const char *cmd)
{
    if (strstr(cmd, "--preview-armed"))
        g.preview = 2;
    else if (strstr(cmd, "--preview"))
        g.preview = 1;
    const char *s = strstr(cmd, "--shot=");
    if (s) {
        s += 7;
        size_t n = strcspn(s, " ");
        if (n >= sizeof g.shot_path)
            n = sizeof g.shot_path - 1;
        memcpy(g.shot_path, s, n);
        g.shot_path[n] = 0;
    }
    const char *hot = strstr(cmd, "--hot=");
    if (hot) {
        hot += 6;
        size_t n = strcspn(hot, " ");
        if (n >= sizeof g.hot_list)
            n = sizeof g.hot_list - 1;
        memcpy(g.hot_list, hot, n);
        g.hot_list[n] = 0;
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g.inst = inst;
    parse_args(cmd ? cmd : "");

    hidway_config_load("hidway.ini", &g.cfg);
    if (g.preview) /* previews never show a real address */
        strcpy(g.cfg.target, "100.100.100.100");
    if (g.cfg.key_hex[0]) {
        /* A key was asked for: encrypt, or (if it is malformed) send nothing.
         * Never fall back to plaintext. */
        g.crypto_on = hidway_key_parse(g.cfg.key_hex, g.key);
        g.key_error = !g.crypto_on;
    }
    g.rtt_ms = -1;
    if (hidway_random((uint8_t *)&g.session_id, sizeof g.session_id) != 0) {
        srand(GetTickCount() ^ GetCurrentProcessId());
        g.session_id = ((uint32_t)rand() << 17) ^ ((uint32_t)rand() << 3) ^ GetTickCount();
    }

    HW_GdiplusStartupInput gsi = {1, NULL, FALSE, FALSE};
    GdiplusStartup(&g.gdip, &gsi, NULL);
    enable_dark_menus();
    timeBeginPeriod(1);
    g.b_bg = CreateSolidBrush(C_BG);
    g.b_field = CreateSolidBrush(C_FIELD);

    HICON appicon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g.b_bg;
    wc.hIcon = appicon;
    wc.lpszClassName = "HIDwayClient";
    if (!RegisterClassA(&wc))
        return 1;

    g.dpi = GetDpiForSystem();
    compute_layout();
    RECT wr = {0, 0, g.L.W, g.L.H};
    AdjustWindowRectExForDpi(&wr, WIN_STYLE, FALSE, 0, g.dpi);
    int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT;
    if (g.preview && g.shot_path[0]) {
        wx = wy = -20000; /* screenshot runs render off-screen */
    } else if (!g.preview && g.cfg.window_x != INT_MIN && g.cfg.window_y != INT_MIN) {
        wx = g.cfg.window_x;
        wy = g.cfg.window_y;
    }
    HWND hwnd = CreateWindowExA(0, "HIDwayClient", "HIDway", WIN_STYLE, wx, wy,
                                wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;
    SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)appicon);
    SendMessageA(hwnd, WM_SETICON, ICON_SMALL,
                 (LPARAM)LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                    GetSystemMetricsForDpi(SM_CXSMICON, g.dpi),
                                    GetSystemMetricsForDpi(SM_CYSMICON, g.dpi), 0));

    ShowWindow(hwnd, g.shot_path[0] ? SW_SHOWNOACTIVATE : show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
    }
    timeEndPeriod(1);
    GdiplusShutdown(g.gdip);
    hidway_wipe(g.key, sizeof g.key);
    hidway_wipe(g.cfg.key_hex, sizeof g.cfg.key_hex);
    return 0;
}
