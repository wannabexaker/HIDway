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
 * The look is hand-drawn (owner-draw + a double-buffered WM_PAINT): dark cards,
 * a green accent, a status pill and monospace readouts, with a translucent,
 * rounded, dark-titlebar window.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <timeapi.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "hidway_kbd.h"
#include "keymap_sc2hid.h"
#include "net.h"
#include "protocol.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

#define HK_TOGGLE 1
#define HK_PANIC  2
#define TIMER_UI  1
#define TIMER_NET 2
#define UI_REFRESH_MS 50
#define HISTORY_MAX 60

enum {
    ID_HISTORY = 1001, ID_CLEAR, ID_TARGET, ID_PORT, ID_RATE, ID_AUTO,
    ID_CB_KBD, ID_CB_MOUSE, ID_CB_HIST, ID_SAVE, ID_ARM, ID_PANIC
};

enum { BTN_L = 1, BTN_R = 2, BTN_M = 4, BTN_X1 = 8, BTN_X2 = 16 };

/* ---- palette: dark monochrome "mouse grey" control panel ---------------- */
#define C_BG       RGB(21, 24, 29)    /* window background */
#define C_CARD     RGB(30, 34, 42)    /* card panels */
#define C_FIELD    RGB(39, 44, 53)    /* inputs / controls */
#define C_FIELD2   RGB(48, 54, 64)    /* input hover */
#define C_BORDER   RGB(57, 64, 76)    /* card / control borders */
#define C_ACCENT   RGB(107, 114, 128) /* mouse grey - primary */
#define C_ACCENT2  RGB(132, 140, 152) /* hover */
#define C_ARM      RGB(82, 90, 103)   /* ARM button */
#define C_ARM2     RGB(104, 113, 128) /* ARM hover */
#define C_LIVE     RGB(74, 222, 128)  /* tiny "live" indicator only */
#define C_DANGER   RGB(176, 74, 82)   /* PANIC (muted red) */
#define C_DANGER2  RGB(205, 92, 101)  /* PANIC hover */
#define C_TEXT     RGB(230, 233, 239)
#define C_DIM      RGB(139, 147, 161)

static struct {
    hidway_config_t cfg;
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

    uint32_t session_id;
    uint32_t seq;
    unsigned sent_in_sec, pps;
    uint32_t sec_start_us;
    int rtt_ms;
    uint32_t pi_frames_ok;
    uint16_t pi_gaps;
    uint16_t pi_flags;
    int status_seen;
    uint32_t last_status_us;
    uint32_t armed_at_ms;

    HWND hwnd;
    HWND history, clear, target, port, rate, autoed, cb_kbd, cb_mouse, cb_hist, save, arm, panic;
    HFONT f_big, f_mono, f_ui, f_title;
    HBRUSH b_bg, b_card, b_field;
} g;

/* ---------------------------------------------------------------- helpers */

static void append(char *dst, size_t cap, const char *fmt, ...)
{
    size_t len = strlen(dst);
    if (len >= cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst + len, cap - len, fmt, ap);
    va_end(ap);
}

static void history_add(const char *ev)
{
    if (!g.cfg.history_enabled)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[96];
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d   %s",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ev);
    SendMessageA(g.history, LB_INSERTSTRING, 0, (LPARAM)line);
    int n = (int)SendMessageA(g.history, LB_GETCOUNT, 0, 0);
    while (n-- > HISTORY_MAX)
        SendMessageA(g.history, LB_DELETESTRING, HISTORY_MAX, 0);
}

/* --------------------------------------------------------------- transport */

static void net_send_state(uint8_t type)
{
    if (!g.net_ok)
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
    if (hidway_net_send(buf, n) > 0)
        g.sent_in_sec++;
}

static void net_drain_status(void)
{
    uint8_t buf[64];
    for (int i = 0; i < 8; i++) {
        int n = hidway_net_recv(buf, sizeof buf);
        if (n <= 0)
            break;
        hidway_status_pkt_t s;
        if (!hidway_status_decode(buf, (size_t)n, &s) || s.session_id != g.session_id)
            continue;
        g.status_seen = 1;
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
static void invalidate_readouts(HWND hwnd);

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
        history_add("auto-disarmed (link lost)");
    }
}

/* ----------------------------------------------------------- input state */

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
        g.rtt_ms = -1;
        g.armed_at_ms = timeGetTime();
        net_send_state(HIDWAY_MSG_STATE);
        SetTimer(g.hwnd, TIMER_NET, net_interval_ms(), NULL);
    } else {
        for (int i = 0; i < 3; i++)
            net_send_state(HIDWAY_MSG_RELEASE);
        KillTimer(g.hwnd, TIMER_NET);
        g.pps = 0;
    }
    InvalidateRect(g.arm, NULL, TRUE);
    invalidate_readouts(g.hwnd);
}

static const struct { uint8_t bit; const char *name; } MODS[] = {
    {0x01, "LCtrl"}, {0x02, "LShift"}, {0x04, "LAlt"}, {0x08, "LGui"},
    {0x10, "RCtrl"}, {0x20, "RShift"}, {0x40, "RAlt"}, {0x80, "RGui"},
};

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
                if (MODS[i].bit == mod) {
                    char ev[24];
                    snprintf(ev, sizeof ev, "%s %s", MODS[i].name, up ? "up" : "down");
                    history_add(ev);
                }
        return;
    }
    uint8_t usage = hidway_sc_to_usage(sc, e0);
    if (!usage)
        return;
    bool was = hidway_bitmap_test(g.keys, usage);
    if (up) hidway_bitmap_clear(g.keys, usage); else hidway_bitmap_set(g.keys, usage);
    if (was != !up) {
        char tmp[8], ev[24];
        snprintf(ev, sizeof ev, "%s %s", hidway_usage_name(usage, tmp), up ? "up" : "down");
        history_add(ev);
    }
}

static void mouse_edge(USHORT bf, USHORT d, USHORT u, uint8_t bit, const char *name)
{
    char ev[24];
    if (bf & d) { g.buttons |= bit; snprintf(ev, sizeof ev, "%s down", name); history_add(ev); }
    if (bf & u) { g.buttons &= (uint8_t)~bit; snprintf(ev, sizeof ev, "%s up", name); history_add(ev); }
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
    mouse_edge(bf, RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, BTN_L, "LMB");
    mouse_edge(bf, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, BTN_R, "RMB");
    mouse_edge(bf, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, BTN_M, "MMB");
    mouse_edge(bf, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, BTN_X1, "MB4");
    mouse_edge(bf, RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, BTN_X2, "MB5");
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

/* --------------------------------------------------------------- drawing */

static void fill_round(HDC dc, int l, int t, int r, int b, int rad, COLORREF fill, COLORREF border)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, l, t, r, b, rad * 2, rad * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void draw_text(HDC dc, HFONT f, COLORREF col, int x, int y, const char *s)
{
    SelectObject(dc, f);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, x, y, s, (int)strlen(s));
}

static void draw_text_rect(HDC dc, HFONT f, COLORREF col, RECT *r, UINT fmt, const char *s)
{
    SelectObject(dc, f);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    DrawTextA(dc, s, -1, r, fmt);
}

static void paint(HWND hwnd)
{
    RECT cr;
    GetClientRect(hwnd, &cr);

    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, cr.right, cr.bottom);
    HGDIOBJ obmp = SelectObject(dc, bmp);

    FillRect(dc, &cr, g.b_bg);

    const int X = 16, W = cr.right - 32;

    /* Header card: status pill + health readout. */
    fill_round(dc, X, 14, X + W, 110, 12, C_CARD, C_BORDER);
    COLORREF pill = g.armed ? C_ACCENT : C_FIELD;
    fill_round(dc, X + 16, 30, X + 16 + 172, 66, 15, pill, g.armed ? C_ACCENT2 : C_BORDER);
    /* small status indicator: green when live, grey when idle */
    HBRUSH dot = CreateSolidBrush(g.armed ? C_LIVE : C_DIM);
    HPEN np = CreatePen(PS_SOLID, 1, g.armed ? C_LIVE : C_DIM);
    HGDIOBJ od = SelectObject(dc, dot), opn = SelectObject(dc, np);
    Ellipse(dc, X + 32, 42, X + 44, 54);
    SelectObject(dc, od);
    SelectObject(dc, opn);
    DeleteObject(dot);
    DeleteObject(np);
    draw_text(dc, g.f_big, g.armed ? C_TEXT : C_DIM, X + 54, 36, g.armed ? "ARMED" : "DISARMED");

    char line1[256] = "", line2[256] = "";
    if (!g.net_ok) {
        snprintf(line1, sizeof line1, "socket error");
        snprintf(line2, sizeof line2, "check Target in settings");
    } else if (!g.armed) {
        snprintf(line1, sizeof line1, "idle - not sending");
        snprintf(line2, sizeof line2, "target  %s:%d", g.cfg.target, g.cfg.port);
    } else if (!g.status_seen) {
        unsigned s = (timeGetTime() - g.armed_at_ms) / 1000;
        snprintf(line1, sizeof line1, "sending %u/s", g.pps);
        snprintf(line2, sizeof line2, "waiting for relay...   armed %u:%02u", s / 60, s % 60);
    } else {
        double tot = (double)g.pi_frames_ok + g.pi_gaps;
        double loss = tot > 0 ? 100.0 * g.pi_gaps / tot : 0.0;
        unsigned s = (timeGetTime() - g.armed_at_ms) / 1000;
        snprintf(line1, sizeof line1, "RTT %d ms   loss %.1f%%   %u/s", g.rtt_ms, loss, g.pps);
        snprintf(line2, sizeof line2, "serial %s  ok=%u gaps=%u  armed %u:%02u",
                 (g.pi_flags & 0x0002) ? "open" : "none", g.pi_frames_ok, g.pi_gaps, s / 60, s % 60);
    }
    draw_text(dc, g.f_mono, C_TEXT, X + 206, 36, line1);
    draw_text(dc, g.f_mono, C_DIM, X + 206, 58, line2);

    /* Live card. */
    int ly = 120;
    fill_round(dc, X, ly, X + W, ly + 104, 12, C_CARD, C_BORDER);
    draw_text(dc, g.f_title, C_DIM, X + 16, ly + 10, "LIVE");
    if (!g.armed) {
        draw_text(dc, g.f_mono, C_DIM, X + 16, ly + 44, "Disarmed - keyboard and mouse are not being read.");
    } else {
        char s[256];
        int any = 0;
        s[0] = 0;
        strcpy(s, "mods  ");
        for (int i = 0; i < 8; i++)
            if (g.modifiers & MODS[i].bit) { append(s, sizeof s, "%s%s", any ? " " : "", MODS[i].name); any = 1; }
        if (!any) strcat(s, "-");
        draw_text(dc, g.f_mono, C_TEXT, X + 16, ly + 34, s);

        strcpy(s, "keys  "); any = 0;
        for (unsigned u = HIDWAY_KEY_USAGE_MIN; u <= HIDWAY_KEY_USAGE_MAX; u++)
            if (hidway_bitmap_test(g.keys, (uint8_t)u)) {
                char t[8];
                append(s, sizeof s, "%s%s", any ? " " : "", hidway_usage_name((uint8_t)u, t));
                any = 1;
            }
        if (!any) strcat(s, "-");
        draw_text(dc, g.f_mono, C_TEXT, X + 16, ly + 52, s);

        snprintf(s, sizeof s, "btn   %s%s%s%s%s",
                 (g.buttons & BTN_L) ? "L " : "", (g.buttons & BTN_R) ? "R " : "",
                 (g.buttons & BTN_M) ? "M " : "", (g.buttons & BTN_X1) ? "4 " : "",
                 (g.buttons & BTN_X2) ? "5" : "");
        if (!g.buttons) strcpy(s, "btn   -");
        draw_text(dc, g.f_mono, C_TEXT, X + 16, ly + 70, s);

        snprintf(s, sizeof s, "move  dx=%+ld dy=%+ld   wheel v=%+d h=%+d",
                 (long)(g.cum_x - g.disp_x), (long)(g.cum_y - g.disp_y),
                 (int)(int16_t)(g.cum_wheel - g.disp_wheel), (int)(int16_t)(g.cum_pan - g.disp_pan));
        draw_text(dc, g.f_mono, C_ACCENT2, X + 16, ly + 88, s);
        g.disp_x = g.cum_x; g.disp_y = g.cum_y; g.disp_wheel = g.cum_wheel; g.disp_pan = g.cum_pan;
    }

    /* History card title (listbox is a child positioned inside). */
    int hy = 232;
    fill_round(dc, X, hy, X + W, hy + 180, 12, C_CARD, C_BORDER);
    draw_text(dc, g.f_title, C_DIM, X + 16, hy + 10, "RECENT INPUT   (memory only)");

    /* Settings card. */
    int sy = 424;
    fill_round(dc, X, sy, X + W, sy + 146, 12, C_CARD, C_BORDER);
    draw_text(dc, g.f_title, C_DIM, X + 16, sy + 10, "SETTINGS");
    draw_text(dc, g.f_ui, C_DIM, X + 16, sy + 40, "Target");
    draw_text(dc, g.f_ui, C_DIM, X + 300, sy + 40, "Port");
    draw_text(dc, g.f_ui, C_DIM, X + 16, sy + 74, "Rate Hz");
    draw_text(dc, g.f_ui, C_DIM, X + 150, sy + 74, "Auto-disarm ms");
    char hk[160];
    snprintf(hk, sizeof hk, "toggle  %s        panic  %s", g.cfg.toggle.text, g.cfg.panic.text);
    draw_text(dc, g.f_mono, C_DIM, X + 16, sy + 118, hk);

    BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

/* Owner-draw buttons and checkboxes. Hover is tracked via a per-window prop. */
static void draw_button(LPDRAWITEMSTRUCT d)
{
    int id = (int)d->CtlID;
    RECT r = d->rcItem;
    bool pressed = (d->itemState & ODS_SELECTED) != 0;
    bool hot = GetPropA(d->hwndItem, "hot") != NULL;

    if (id == ID_CB_KBD || id == ID_CB_MOUSE || id == ID_CB_HIST) {
        int checked = (id == ID_CB_KBD) ? g.cfg.relay_keyboard
                    : (id == ID_CB_MOUSE) ? g.cfg.relay_mouse : g.cfg.history_enabled;
        char label[32];
        GetWindowTextA(d->hwndItem, label, sizeof label);
        int box = 18, by = r.top + (r.bottom - r.top - box) / 2;
        fill_round(d->hDC, r.left, by, r.left + box, by + box, 5,
                   checked ? C_ACCENT : C_FIELD, checked ? C_ACCENT2 : C_BORDER);
        if (checked) {
            HPEN p = CreatePen(PS_SOLID, 2, C_TEXT);
            HGDIOBJ op = SelectObject(d->hDC, p);
            MoveToEx(d->hDC, r.left + 4, by + 9, NULL);
            LineTo(d->hDC, r.left + 8, by + 13);
            LineTo(d->hDC, r.left + 14, by + 5);
            SelectObject(d->hDC, op);
            DeleteObject(p);
        }
        draw_text(d->hDC, g.f_ui, hot ? C_TEXT : C_DIM, r.left + box + 8, by + 1, label);
        return;
    }

    COLORREF fill, txt = C_TEXT, border;
    if (id == ID_ARM) {
        fill = g.armed ? C_ACCENT : C_ARM;
        if (hot) fill = g.armed ? C_ACCENT2 : C_ARM2;
        border = hot ? C_ACCENT2 : C_BORDER;
    } else if (id == ID_PANIC) {
        fill = hot ? C_DANGER2 : C_DANGER;
        border = fill;
    } else { /* SAVE, CLEAR: subtle */
        fill = hot ? C_FIELD2 : C_FIELD;
        border = C_BORDER;
    }
    if (pressed) {
        fill = RGB(GetRValue(fill) * 8 / 10, GetGValue(fill) * 8 / 10, GetBValue(fill) * 8 / 10);
    }
    fill_round(d->hDC, r.left, r.top, r.right, r.bottom, 9, fill, border);

    char label[32];
    GetWindowTextA(d->hwndItem, label, sizeof label);
    if (id == ID_ARM)
        strcpy(label, g.armed ? "DISARM" : "ARM");
    HFONT f = (id == ID_ARM || id == ID_PANIC) ? g.f_big : g.f_ui;
    draw_text_rect(d->hDC, f, txt, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, label);
}

static LRESULT CALLBACK btn_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC orig = (WNDPROC)GetPropA(h, "op");
    if (msg == WM_MOUSEMOVE) {
        if (!GetPropA(h, "hot")) {
            SetPropA(h, "hot", (HANDLE)1);
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, NULL, TRUE);
        }
    } else if (msg == WM_MOUSELEAVE) {
        RemovePropA(h, "hot");
        InvalidateRect(h, NULL, TRUE);
    }
    return CallWindowProcA(orig, h, msg, wp, lp);
}

/* --------------------------------------------------------------- settings */

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

    GetWindowTextA(g.target, g.cfg.target, sizeof g.cfg.target);
    g.cfg.port = get_int(g.port, g.cfg.port);
    g.cfg.send_rate_hz = get_int(g.rate, g.cfg.send_rate_hz);
    g.cfg.auto_disarm_ms = get_int(g.autoed, 0);
    if (g.cfg.auto_disarm_ms < 0) g.cfg.auto_disarm_ms = 0;

    hidway_config_save("hidway.ini", &g.cfg);
    if (strcmp(old_target, g.cfg.target) != 0 || old_port != g.cfg.port)
        g.net_ok = (hidway_net_open(g.cfg.target, g.cfg.port) == 0);
    if (g.armed)
        SetTimer(g.hwnd, TIMER_NET, net_interval_ms(), NULL);
    history_add("settings saved");
}

/* --------------------------------------------------------------- window */

static HWND mk(HWND p, const char *cls, const char *txt, DWORD style, int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowA(cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, p, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

static void subclass_btn(HWND b)
{
    WNDPROC op = (WNDPROC)SetWindowLongPtrA(b, GWLP_WNDPROC, (LONG_PTR)btn_proc);
    SetPropA(b, "op", (HANDLE)op);
}

static void create_controls(HWND h)
{
    g.f_big = CreateFontA(22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FF_SWISS, "Segoe UI Semibold");
    g.f_mono = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    g.f_ui = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FF_SWISS, "Segoe UI");
    g.f_title = CreateFontA(13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FF_SWISS, "Segoe UI");

    const int X = 16, W = 492;

    g.history = mk(h, "LISTBOX", "", LBS_NOINTEGRALHEIGHT | LBS_NOSEL | WS_VSCROLL, X + 12, 264, W - 24, 138, ID_HISTORY, g.f_mono);
    g.clear = mk(h, "BUTTON", "Clear", BS_OWNERDRAW, X + W - 96, 234, 84, 22, ID_CLEAR, g.f_ui);

    int sy = 424;
    g.target = mk(h, "EDIT", g.cfg.target, ES_AUTOHSCROLL, X + 74, sy + 38, 214, 24, ID_TARGET, g.f_ui);
    g.port = mk(h, "EDIT", "", ES_NUMBER, X + 336, sy + 38, 64, 24, ID_PORT, g.f_ui);
    g.rate = mk(h, "EDIT", "", ES_NUMBER, X + 80, sy + 72, 56, 24, ID_RATE, g.f_ui);
    g.autoed = mk(h, "EDIT", "", ES_NUMBER, X + 268, sy + 72, 70, 24, ID_AUTO, g.f_ui);

    g.cb_kbd = mk(h, "BUTTON", "Keyboard", BS_OWNERDRAW, X + 16, sy + 98, 120, 22, ID_CB_KBD, g.f_ui);
    g.cb_mouse = mk(h, "BUTTON", "Mouse", BS_OWNERDRAW, X + 146, sy + 98, 110, 22, ID_CB_MOUSE, g.f_ui);
    g.cb_hist = mk(h, "BUTTON", "History", BS_OWNERDRAW, X + 262, sy + 98, 100, 22, ID_CB_HIST, g.f_ui);
    g.save = mk(h, "BUTTON", "Save", BS_OWNERDRAW, X + W - 96, sy + 96, 84, 26, ID_SAVE, g.f_ui);

    g.arm = mk(h, "BUTTON", "ARM", BS_OWNERDRAW, X, 582, 300, 46, ID_ARM, g.f_big);
    g.panic = mk(h, "BUTTON", "PANIC", BS_OWNERDRAW, X + 316, 582, 176, 46, ID_PANIC, g.f_big);

    HWND btns[] = {g.clear, g.save, g.arm, g.panic, g.cb_kbd, g.cb_mouse, g.cb_hist};
    for (size_t i = 0; i < sizeof btns / sizeof btns[0]; i++)
        subclass_btn(btns[i]);

    char tmp[32];
    snprintf(tmp, sizeof tmp, "%d", g.cfg.port); SetWindowTextA(g.port, tmp);
    snprintf(tmp, sizeof tmp, "%d", g.cfg.send_rate_hz); SetWindowTextA(g.rate, tmp);
    snprintf(tmp, sizeof tmp, "%d", g.cfg.auto_disarm_ms); SetWindowTextA(g.autoed, tmp);
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
    /* Dark titlebar and rounded corners only - the window itself is opaque. */
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    int corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
}

/* Repaint only the live readout band (header + LIVE card). It contains no
 * child controls, so repainting it on the UI timer never flickers the
 * list/inputs/buttons below. */
static void invalidate_readouts(HWND hwnd)
{
    RECT rc = {0, 0, 2000, 228};
    InvalidateRect(hwnd, &rc, FALSE);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        create_controls(hwnd);
        apply_window_chrome(hwnd);
        if (register_raw_input(hwnd) != 0)
            MessageBoxA(hwnd, "Failed to register raw input.", "HIDway", MB_ICONERROR);
        g.net_ok = (hidway_net_open(g.cfg.target, g.cfg.port) == 0);
        RegisterHotKey(hwnd, HK_TOGGLE, g.cfg.toggle.mods | MOD_NOREPEAT, g.cfg.toggle.vk);
        if (g.cfg.panic.vk)
            RegisterHotKey(hwnd, HK_PANIC, g.cfg.panic.mods | MOD_NOREPEAT, g.cfg.panic.vk);
        SetTimer(hwnd, TIMER_UI, UI_REFRESH_MS, NULL);
        return 0;

    case WM_ERASEBKGND:
        return 1; /* painted in WM_PAINT, double-buffered */
    case WM_PAINT:
        paint(hwnd);
        return 0;

    case WM_DRAWITEM:
        draw_button((LPDRAWITEMSTRUCT)lp);
        return TRUE;

    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wp, C_FIELD);
        SetTextColor((HDC)wp, C_DIM);
        return (LRESULT)g.b_field;
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp, C_FIELD);
        SetTextColor((HDC)wp, C_TEXT);
        return (LRESULT)g.b_field;

    case WM_INPUT:
        on_raw_input((HRAWINPUT)lp);
        return 0;

    case WM_HOTKEY:
        if (wp == HK_TOGGLE)
            set_armed(!g.armed);
        else if (wp == HK_PANIC) {
            set_armed(0);
            history_add("PANIC");
        }
        return 0;

    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            switch (LOWORD(wp)) {
            case ID_ARM: set_armed(!g.armed); break;
            case ID_PANIC: set_armed(0); history_add("PANIC"); break;
            case ID_CLEAR: SendMessageA(g.history, LB_RESETCONTENT, 0, 0); break;
            case ID_SAVE: apply_settings(); break;
            case ID_CB_KBD: g.cfg.relay_keyboard = !g.cfg.relay_keyboard; InvalidateRect(g.cb_kbd, NULL, TRUE); break;
            case ID_CB_MOUSE: g.cfg.relay_mouse = !g.cfg.relay_mouse; InvalidateRect(g.cb_mouse, NULL, TRUE); break;
            case ID_CB_HIST: g.cfg.history_enabled = !g.cfg.history_enabled; InvalidateRect(g.cb_hist, NULL, TRUE); break;
            }
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_UI)
            invalidate_readouts(hwnd);
        else if (wp == TIMER_NET)
            net_tick();
        return 0;

    case WM_CLOSE:
        set_armed(0);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_UI);
        UnregisterHotKey(hwnd, HK_TOGGLE);
        UnregisterHotKey(hwnd, HK_PANIC);
        hidway_net_close();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev;
    (void)cmd;

    hidway_config_load("hidway.ini", &g.cfg);
    g.rtt_ms = -1;
    srand(GetTickCount() ^ GetCurrentProcessId());
    g.session_id = ((uint32_t)rand() << 17) ^ ((uint32_t)rand() << 3) ^ GetTickCount();

    timeBeginPeriod(1);
    g.b_bg = CreateSolidBrush(C_BG);
    g.b_card = CreateSolidBrush(C_CARD);
    g.b_field = CreateSolidBrush(C_FIELD);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g.b_bg;
    wc.lpszClassName = "HIDwayClient";
    if (!RegisterClassA(&wc))
        return 1;

    RECT r = {0, 0, 524, 642};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowA("HIDwayClient", "HIDway", style, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    timeEndPeriod(1);
    return 0;
}
