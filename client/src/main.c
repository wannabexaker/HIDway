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
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

#define HK_TOGGLE 1
#define HK_PANIC  2
#define TIMER_UI  1
#define TIMER_NET 2
#define UI_REFRESH_MS 50

#define HISTORY_MAX 60

enum {
    ID_STATUS = 1001, ID_HEALTH, ID_BODY, ID_HISTLABEL, ID_HISTORY, ID_CLEAR,
    ID_SETLABEL, ID_LTARGET, ID_TARGET, ID_LPORT, ID_PORT, ID_LRATE, ID_RATE,
    ID_LAUTO, ID_AUTO, ID_CB_KBD, ID_CB_MOUSE, ID_CB_HIST, ID_HOTKEYS,
    ID_SAVE, ID_ARM, ID_PANIC
};

enum { BTN_L = 1, BTN_R = 2, BTN_M = 4, BTN_X1 = 8, BTN_X2 = 16 };

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

    HWND status, health, body, histlabel, history, clear;
    HWND setlabel, target, port, rate, autoed, cb_kbd, cb_mouse, cb_hist, hotkeys, save, arm, panic;
    HFONT font_big, font_mono, font_ui;
    HBRUSH bg;
} g;

static const COLORREF C_BG = RGB(24, 26, 30);
static const COLORREF C_TXT = RGB(225, 228, 233);
static const COLORREF C_DIM = RGB(140, 145, 155);

/* --------------------------------------------------------------- helpers */

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
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d  %s",
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

static void set_armed(HWND hwnd, int on);

static void net_tick(HWND hwnd)
{
    net_send_state(HIDWAY_MSG_STATE);
    net_drain_status();

    uint32_t now = hidway_now_us();
    if (now - g.sec_start_us >= 1000000u) {
        g.pps = g.sent_in_sec;
        g.sent_in_sec = 0;
        g.sec_start_us = now;
    }

    /* Auto-disarm if the link was up and then went silent. */
    if (g.armed && g.status_seen && g.cfg.auto_disarm_ms > 0 &&
        (now - g.last_status_us) > (uint32_t)g.cfg.auto_disarm_ms * 1000u) {
        set_armed(hwnd, 0);
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

static void set_armed(HWND hwnd, int on)
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
        SetTimer(hwnd, TIMER_NET, net_interval_ms(), NULL);
    } else {
        for (int i = 0; i < 3; i++)
            net_send_state(HIDWAY_MSG_RELEASE);
        KillTimer(hwnd, TIMER_NET);
        g.pps = 0;
    }

    SetWindowTextA(g.status, on ? "  ARMED" : "  DISARMED");
    SetWindowTextA(g.arm, on ? "DISARM" : "ARM");
    InvalidateRect(g.status, NULL, TRUE);
}

static void on_raw_keyboard(const RAWKEYBOARD *kb)
{
    if (!g.armed || !g.cfg.relay_keyboard || kb->MakeCode == 0 || kb->MakeCode == 0xFF)
        return;

    bool e0 = (kb->Flags & RI_KEY_E0) != 0;
    bool up = (kb->Flags & RI_KEY_BREAK) != 0;
    uint8_t sc = (uint8_t)kb->MakeCode;

    static const struct { uint8_t bit; const char *name; } mods[] = {
        {0x01, "LCtrl"}, {0x02, "LShift"}, {0x04, "LAlt"}, {0x08, "LGui"},
        {0x10, "RCtrl"}, {0x20, "RShift"}, {0x40, "RAlt"}, {0x80, "RGui"},
    };

    uint8_t mod = hidway_sc_to_modifier(sc, e0);
    if (mod) {
        bool was = (g.modifiers & mod) != 0;
        if (up)
            g.modifiers &= (uint8_t)~mod;
        else
            g.modifiers |= mod;
        if (was != !up) {
            for (int i = 0; i < 8; i++)
                if (mods[i].bit == mod) {
                    char ev[24];
                    snprintf(ev, sizeof ev, "%s %s", mods[i].name, up ? "up" : "down");
                    history_add(ev);
                }
        }
        return;
    }

    uint8_t usage = hidway_sc_to_usage(sc, e0);
    if (!usage)
        return;
    bool was = hidway_bitmap_test(g.keys, usage);
    if (up)
        hidway_bitmap_clear(g.keys, usage);
    else
        hidway_bitmap_set(g.keys, usage);
    if (was != !up) {
        char tmp[8], ev[24];
        snprintf(ev, sizeof ev, "%s %s", hidway_usage_name(usage, tmp), up ? "up" : "down");
        history_add(ev);
    }
}

static void mouse_button_edge(USHORT bf, USHORT downf, USHORT upf, uint8_t bit, const char *name)
{
    if (bf & downf) {
        g.buttons |= bit;
        char ev[24];
        snprintf(ev, sizeof ev, "%s down", name);
        history_add(ev);
    }
    if (bf & upf) {
        g.buttons &= (uint8_t)~bit;
        char ev[24];
        snprintf(ev, sizeof ev, "%s up", name);
        history_add(ev);
    }
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
    mouse_button_edge(bf, RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, BTN_L, "LMB");
    mouse_button_edge(bf, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, BTN_R, "RMB");
    mouse_button_edge(bf, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, BTN_M, "MMB");
    mouse_button_edge(bf, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, BTN_X1, "MB4");
    mouse_button_edge(bf, RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, BTN_X2, "MB5");

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

/* --------------------------------------------------------------- display */

static void refresh_health(void)
{
    char s[512];
    s[0] = 0;

    if (!g.net_ok) {
        append(s, sizeof s, "socket error - check target in Settings");
        SetWindowTextA(g.health, s);
        return;
    }
    if (!g.armed) {
        append(s, sizeof s, "idle - not sending\r\ntarget %s:%d", g.cfg.target, g.cfg.port);
        SetWindowTextA(g.health, s);
        return;
    }

    if (!g.status_seen) {
        append(s, sizeof s, "sending %u/s - waiting for relay reply...", g.pps);
    } else {
        double total = (double)g.pi_frames_ok + (double)g.pi_gaps;
        double loss = total > 0 ? 100.0 * g.pi_gaps / total : 0.0;
        append(s, sizeof s, "RTT %d ms   loss %.1f%%   %u pkt/s\r\n", g.rtt_ms, loss, g.pps);
        append(s, sizeof s, "relay serial: %s   Pi ok=%u gaps=%u",
               (g.pi_flags & 0x0002) ? "open" : "none", g.pi_frames_ok, g.pi_gaps);
    }
    unsigned secs = (timeGetTime() - g.armed_at_ms) / 1000;
    append(s, sizeof s, "   armed %u:%02u", secs / 60, secs % 60);
    SetWindowTextA(g.health, s);
}

static void refresh_body(void)
{
    refresh_health();

    char s[768];
    s[0] = 0;
    if (!g.armed) {
        append(s, sizeof s, "Disarmed. Keyboard and mouse are not being read.");
        SetWindowTextA(g.body, s);
        return;
    }

    append(s, sizeof s, "Modifiers : ");
    static const struct { uint8_t bit; const char *name; } mods[] = {
        {0x01, "LCtrl"}, {0x02, "LShift"}, {0x04, "LAlt"}, {0x08, "LGui"},
        {0x10, "RCtrl"}, {0x20, "RShift"}, {0x40, "RAlt"}, {0x80, "RGui"},
    };
    int any = 0;
    for (int i = 0; i < 8; i++)
        if (g.modifiers & mods[i].bit) { append(s, sizeof s, "%s%s", any ? " " : "", mods[i].name); any = 1; }
    append(s, sizeof s, "%s\r\n", any ? "" : "-");

    append(s, sizeof s, "Keys      : ");
    any = 0;
    for (unsigned u = HIDWAY_KEY_USAGE_MIN; u <= HIDWAY_KEY_USAGE_MAX; u++)
        if (hidway_bitmap_test(g.keys, (uint8_t)u)) {
            char tmp[8];
            append(s, sizeof s, "%s%s", any ? " " : "", hidway_usage_name((uint8_t)u, tmp));
            any = 1;
        }
    append(s, sizeof s, "%s\r\n", any ? "" : "-");

    append(s, sizeof s, "Buttons   : ");
    if (g.buttons)
        append(s, sizeof s, "%s%s%s%s%s\r\n",
               (g.buttons & BTN_L) ? "L " : "", (g.buttons & BTN_R) ? "R " : "",
               (g.buttons & BTN_M) ? "M " : "", (g.buttons & BTN_X1) ? "4 " : "",
               (g.buttons & BTN_X2) ? "5" : "");
    else
        append(s, sizeof s, "-\r\n");

    append(s, sizeof s, "Move 50ms : dx=%+ld dy=%+ld   wheel v=%+d h=%+d",
           (long)(g.cum_x - g.disp_x), (long)(g.cum_y - g.disp_y),
           (int)(int16_t)(g.cum_wheel - g.disp_wheel), (int)(int16_t)(g.cum_pan - g.disp_pan));
    g.disp_x = g.cum_x; g.disp_y = g.cum_y; g.disp_wheel = g.cum_wheel; g.disp_pan = g.cum_pan;

    SetWindowTextA(g.body, s);
}

/* --------------------------------------------------------------- settings */

static int get_int(HWND edit, int fallback)
{
    char b[32];
    GetWindowTextA(edit, b, sizeof b);
    int v = atoi(b);
    return v ? v : fallback;
}

static void apply_settings(HWND hwnd)
{
    char old_target[128];
    int old_port = g.cfg.port;
    strcpy(old_target, g.cfg.target);

    GetWindowTextA(g.target, g.cfg.target, sizeof g.cfg.target);
    g.cfg.port = get_int(g.port, g.cfg.port);
    g.cfg.send_rate_hz = get_int(g.rate, g.cfg.send_rate_hz);
    g.cfg.auto_disarm_ms = get_int(g.autoed, 0);
    if (g.cfg.auto_disarm_ms < 0) g.cfg.auto_disarm_ms = 0;
    g.cfg.relay_keyboard = (int)SendMessageA(g.cb_kbd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g.cfg.relay_mouse = (int)SendMessageA(g.cb_mouse, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g.cfg.history_enabled = (int)SendMessageA(g.cb_hist, BM_GETCHECK, 0, 0) == BST_CHECKED;

    hidway_config_save("hidway.ini", &g.cfg);

    if (strcmp(old_target, g.cfg.target) != 0 || old_port != g.cfg.port)
        g.net_ok = (hidway_net_open(g.cfg.target, g.cfg.port) == 0);
    if (g.armed)
        SetTimer(hwnd, TIMER_NET, net_interval_ms(), NULL);

    history_add("settings saved");
}

/* --------------------------------------------------------------- window */

static HWND mk(HWND p, const char *cls, const char *txt, DWORD style, int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowA(cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, p, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

static void create_controls(HWND h)
{
    g.font_big = CreateFontA(32, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FF_SWISS, "Segoe UI");
    g.font_mono = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    g.font_ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g.status = mk(h, "STATIC", "  DISARMED", SS_LEFT | SS_CENTERIMAGE, 14, 8, 300, 40, ID_STATUS, g.font_big);
    g.health = mk(h, "STATIC", "", SS_LEFT, 16, 52, 492, 54, ID_HEALTH, g.font_mono);
    g.body = mk(h, "STATIC", "", SS_LEFT, 16, 112, 492, 96, ID_BODY, g.font_mono);

    g.histlabel = mk(h, "STATIC", "Recent input (memory only):", SS_LEFT, 16, 214, 320, 18, ID_HISTLABEL, g.font_ui);
    g.clear = mk(h, "BUTTON", "Clear", BS_PUSHBUTTON, 412, 210, 96, 24, ID_CLEAR, g.font_ui);
    g.history = mk(h, "LISTBOX", "", LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER, 16, 236, 492, 150, ID_HISTORY, g.font_mono);

    g.setlabel = mk(h, "STATIC", "Settings", SS_LEFT, 16, 396, 200, 18, ID_SETLABEL, g.font_ui);
    mk(h, "STATIC", "Target", SS_LEFT, 16, 422, 50, 20, ID_LTARGET, g.font_ui);
    g.target = mk(h, "EDIT", g.cfg.target, ES_AUTOHSCROLL | WS_BORDER, 70, 420, 236, 24, ID_TARGET, g.font_ui);
    mk(h, "STATIC", "Port", SS_LEFT, 318, 422, 32, 20, ID_LPORT, g.font_ui);
    g.port = mk(h, "EDIT", "", ES_NUMBER | WS_BORDER, 352, 420, 60, 24, ID_PORT, g.font_ui);

    mk(h, "STATIC", "Rate Hz", SS_LEFT, 16, 454, 54, 20, ID_LRATE, g.font_ui);
    g.rate = mk(h, "EDIT", "", ES_NUMBER | WS_BORDER, 74, 452, 56, 24, ID_RATE, g.font_ui);
    mk(h, "STATIC", "Auto-disarm ms (0=off)", SS_LEFT, 150, 454, 160, 20, ID_LAUTO, g.font_ui);
    g.autoed = mk(h, "EDIT", "", ES_NUMBER | WS_BORDER, 318, 452, 70, 24, ID_AUTO, g.font_ui);

    g.cb_kbd = mk(h, "BUTTON", "Relay keyboard", BS_AUTOCHECKBOX, 16, 486, 140, 20, ID_CB_KBD, g.font_ui);
    g.cb_mouse = mk(h, "BUTTON", "Relay mouse", BS_AUTOCHECKBOX, 164, 486, 120, 20, ID_CB_MOUSE, g.font_ui);
    g.cb_hist = mk(h, "BUTTON", "History", BS_AUTOCHECKBOX, 292, 486, 90, 20, ID_CB_HIST, g.font_ui);
    g.save = mk(h, "BUTTON", "Save", BS_PUSHBUTTON, 412, 482, 96, 26, ID_SAVE, g.font_ui);

    g.hotkeys = mk(h, "STATIC", "", SS_LEFT, 16, 514, 492, 18, ID_HOTKEYS, g.font_ui);

    g.arm = mk(h, "BUTTON", "ARM", BS_PUSHBUTTON, 16, 540, 300, 44, ID_ARM, g.font_big);
    g.panic = mk(h, "BUTTON", "PANIC", BS_PUSHBUTTON, 330, 540, 178, 44, ID_PANIC, g.font_big);

    /* Initialize settings controls from config. */
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%d", g.cfg.port); SetWindowTextA(g.port, tmp);
    snprintf(tmp, sizeof tmp, "%d", g.cfg.send_rate_hz); SetWindowTextA(g.rate, tmp);
    snprintf(tmp, sizeof tmp, "%d", g.cfg.auto_disarm_ms); SetWindowTextA(g.autoed, tmp);
    SendMessageA(g.cb_kbd, BM_SETCHECK, g.cfg.relay_keyboard ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(g.cb_mouse, BM_SETCHECK, g.cfg.relay_mouse ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(g.cb_hist, BM_SETCHECK, g.cfg.history_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

    char hk[128];
    snprintf(hk, sizeof hk, "Toggle: %s      Panic: %s", g.cfg.toggle.text, g.cfg.panic.text);
    SetWindowTextA(g.hotkeys, hk);
}

static int register_raw_input(HWND hwnd)
{
    RAWINPUTDEVICE rid[2] = {
        {0x01, 0x06, RIDEV_INPUTSINK, hwnd},
        {0x01, 0x02, RIDEV_INPUTSINK, hwnd},
    };
    return RegisterRawInputDevices(rid, 2, sizeof rid[0]) ? 0 : -1;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        create_controls(hwnd);
        if (register_raw_input(hwnd) != 0)
            MessageBoxA(hwnd, "Failed to register raw input.", "HIDway", MB_ICONERROR);
        g.net_ok = (hidway_net_open(g.cfg.target, g.cfg.port) == 0);
        RegisterHotKey(hwnd, HK_TOGGLE, g.cfg.toggle.mods | MOD_NOREPEAT, g.cfg.toggle.vk);
        if (g.cfg.panic.vk)
            RegisterHotKey(hwnd, HK_PANIC, g.cfg.panic.mods | MOD_NOREPEAT, g.cfg.panic.vk);
        SetTimer(hwnd, TIMER_UI, UI_REFRESH_MS, NULL);
        return 0;

    case WM_INPUT:
        on_raw_input((HRAWINPUT)lp);
        return 0;

    case WM_HOTKEY:
        if (wp == HK_TOGGLE)
            set_armed(hwnd, !g.armed);
        else if (wp == HK_PANIC) {
            set_armed(hwnd, 0);
            history_add("PANIC");
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ARM: if (HIWORD(wp) == BN_CLICKED) set_armed(hwnd, !g.armed); break;
        case ID_PANIC: if (HIWORD(wp) == BN_CLICKED) { set_armed(hwnd, 0); history_add("PANIC"); } break;
        case ID_CLEAR: if (HIWORD(wp) == BN_CLICKED) SendMessageA(g.history, LB_RESETCONTENT, 0, 0); break;
        case ID_SAVE: if (HIWORD(wp) == BN_CLICKED) apply_settings(hwnd); break;
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_UI)
            refresh_body();
        else if (wp == TIMER_NET)
            net_tick(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, C_BG);
        if ((HWND)lp == g.status)
            SetTextColor(dc, g.armed ? RGB(60, 210, 120) : C_DIM);
        else if ((HWND)lp == g.histlabel || (HWND)lp == g.setlabel || (HWND)lp == g.hotkeys)
            SetTextColor(dc, C_DIM);
        else
            SetTextColor(dc, C_TXT);
        return (LRESULT)g.bg;
    }
    case WM_CTLCOLORBTN:
        return (LRESULT)g.bg;

    case WM_CLOSE:
        set_armed(hwnd, 0);
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
    g.bg = CreateSolidBrush(C_BG);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g.bg;
    wc.lpszClassName = "HIDwayClient";
    if (!RegisterClassA(&wc))
        return 1;

    RECT r = {0, 0, 524, 598};
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
