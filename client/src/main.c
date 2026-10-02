/*
 * HIDway client - remote PC interface.
 *
 * A plain, visible window with an ARM/DISARM switch. While ARMED it reads this
 * PC's own keyboard and mouse through the Windows Raw Input API, shows the
 * current state, and relays that state to the Raspberry Pi over UDP (through
 * Tailscale). It transmits the COMPLETE current state at a steady rate plus on
 * every change; it never sends event streams, so a lost packet self-corrects
 * and a key can never stick.
 *
 * It does not block local input (Raw Input is registered without
 * RIDEV_NOLEGACY): while ARMED, keys still reach the focused app on this PC as
 * well. Local input blocking is a later, separate step.
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

#define HOTKEY_TOGGLE 1
#define TIMER_UI      1
#define TIMER_NET     2
#define UI_REFRESH_MS 50
#define NET_SEND_MS   8   /* ~125 Hz heartbeat while armed (provisional) */
#define NET_RECV_BUDGET 8 /* status datagrams drained per net tick */

#define ID_STATUS 1001
#define ID_BODY   1002
#define ID_BUTTON 1003
#define ID_HINT   1004

enum { BTN_L = 1, BTN_R = 2, BTN_M = 4, BTN_X1 = 8, BTN_X2 = 16 };

static struct {
    hidway_config_t cfg;
    int armed;
    int net_ok;

    /* Current relay state (updated on the UI thread from WM_INPUT). */
    uint8_t modifiers;
    uint8_t keys[HIDWAY_KEY_BITMAP_BYTES];
    uint8_t buttons;

    /* Cumulative mouse counters; advance only while armed, never reset, so
     * re-arming continues smoothly and the receiver applies wrap-safe diffs. */
    int32_t cum_x, cum_y;
    int16_t cum_wheel, cum_pan;
    int wheel_rem_v, wheel_rem_h; /* sub-detent carry from raw wheel units */
    int32_t disp_x, disp_y;       /* cum snapshot at last display refresh */
    int16_t disp_wheel, disp_pan;

    /* Transport. */
    uint32_t session_id;
    uint32_t seq;
    unsigned sent_in_sec, pps;
    uint32_t sec_start_us;
    int rtt_ms;                 /* -1 until a status arrives */
    uint32_t pi_frames_ok;
    uint16_t pi_gaps;
    int status_seen;

    HWND status, body, button, hint;
    HFONT font_body, font_status;
    HBRUSH bg;
} g;

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

/* ------------------------------------------------------------- transport */

static void build_input(hidway_input_pkt_t *p, uint8_t type)
{
    memset(p, 0, sizeof *p);
    p->type = type;
    p->session_id = g.session_id;
    p->seq = ++g.seq;
    p->client_time_us = hidway_now_us();
    if (type == HIDWAY_MSG_STATE) {
        p->mods = g.modifiers;
        memcpy(p->keys, g.keys, sizeof p->keys);
        p->buttons = g.buttons;
        p->x = g.cum_x;
        p->y = g.cum_y;
        p->wheel = g.cum_wheel;
        p->pan = g.cum_pan;
    }
}

static void net_send_state(uint8_t type)
{
    if (!g.net_ok)
        return;
    hidway_input_pkt_t p;
    uint8_t buf[HIDWAY_INPUT_PKT_LEN];
    build_input(&p, type);
    size_t n = hidway_input_encode(buf, &p);
    if (hidway_net_send(buf, n) > 0)
        g.sent_in_sec++;
}

static void net_drain_status(void)
{
    uint8_t buf[64];
    for (int i = 0; i < NET_RECV_BUDGET; i++) {
        int n = hidway_net_recv(buf, sizeof buf);
        if (n <= 0)
            break;
        hidway_status_pkt_t s;
        if (!hidway_status_decode(buf, (size_t)n, &s))
            continue;
        if (s.session_id != g.session_id)
            continue;
        g.status_seen = 1;
        g.pi_frames_ok = s.frames_ok;
        g.pi_gaps = s.seq_gaps;
        uint32_t rtt = hidway_now_us() - s.client_time_us;
        if (rtt < 10000000u) /* ignore absurd values from clock wrap */
            g.rtt_ms = (int)(rtt / 1000u);
    }
}

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
}

/* ----------------------------------------------------------- input state */

static void clear_relay_state(void)
{
    g.modifiers = 0;
    memset(g.keys, 0, sizeof g.keys);
    g.buttons = 0;
}

static void set_armed(HWND hwnd, int on)
{
    if (on == g.armed)
        return;
    g.armed = on;
    clear_relay_state(); /* fail-safe: entering or leaving ARMED releases all */

    if (on) {
        g.sec_start_us = hidway_now_us();
        g.sent_in_sec = 0;
        net_send_state(HIDWAY_MSG_STATE);
        SetTimer(hwnd, TIMER_NET, NET_SEND_MS, NULL);
    } else {
        for (int i = 0; i < 3; i++) /* belt-and-braces release on disarm */
            net_send_state(HIDWAY_MSG_RELEASE);
        KillTimer(hwnd, TIMER_NET);
        g.pps = 0;
    }

    SetWindowTextA(g.status, on ? "  ARMED" : "  DISARMED");
    SetWindowTextA(g.button, on ? "Disarm  (Ctrl+Shift+F12)" : "Arm  (Ctrl+Shift+F12)");
    InvalidateRect(g.status, NULL, TRUE);
}

static void on_raw_keyboard(const RAWKEYBOARD *kb)
{
    if (!g.armed || kb->MakeCode == 0 || kb->MakeCode == 0xFF)
        return;

    bool e0 = (kb->Flags & RI_KEY_E0) != 0;
    bool up = (kb->Flags & RI_KEY_BREAK) != 0;
    uint8_t sc = (uint8_t)kb->MakeCode;

    uint8_t mod = hidway_sc_to_modifier(sc, e0);
    if (mod) {
        if (up)
            g.modifiers &= (uint8_t)~mod;
        else
            g.modifiers |= mod;
        return;
    }

    uint8_t usage = hidway_sc_to_usage(sc, e0);
    if (!usage)
        return;
    if (up)
        hidway_bitmap_clear(g.keys, usage);
    else
        hidway_bitmap_set(g.keys, usage);
}

static void on_raw_mouse(const RAWMOUSE *m)
{
    if (!g.armed)
        return;

    if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) {
        g.cum_x += m->lLastX;
        g.cum_y += m->lLastY;
    }

    USHORT bf = m->usButtonFlags;
    if (bf & RI_MOUSE_LEFT_BUTTON_DOWN)   g.buttons |= BTN_L;
    if (bf & RI_MOUSE_LEFT_BUTTON_UP)     g.buttons &= (uint8_t)~BTN_L;
    if (bf & RI_MOUSE_RIGHT_BUTTON_DOWN)  g.buttons |= BTN_R;
    if (bf & RI_MOUSE_RIGHT_BUTTON_UP)    g.buttons &= (uint8_t)~BTN_R;
    if (bf & RI_MOUSE_MIDDLE_BUTTON_DOWN) g.buttons |= BTN_M;
    if (bf & RI_MOUSE_MIDDLE_BUTTON_UP)   g.buttons &= (uint8_t)~BTN_M;
    if (bf & RI_MOUSE_BUTTON_4_DOWN)      g.buttons |= BTN_X1;
    if (bf & RI_MOUSE_BUTTON_4_UP)        g.buttons &= (uint8_t)~BTN_X1;
    if (bf & RI_MOUSE_BUTTON_5_DOWN)      g.buttons |= BTN_X2;
    if (bf & RI_MOUSE_BUTTON_5_UP)        g.buttons &= (uint8_t)~BTN_X2;

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

static void refresh_body(void)
{
    char s[1024];
    s[0] = 0;

    append(s, sizeof s, "Relay target : %s:%d\r\n", g.cfg.target, g.cfg.port);
    if (!g.net_ok)
        append(s, sizeof s, "Transport    : socket error (check hidway.ini)\r\n");
    else if (!g.armed)
        append(s, sizeof s, "Transport    : idle (not sending)\r\n");
    else if (!g.status_seen)
        append(s, sizeof s, "Transport    : sending %u/s, no reply from relay yet\r\n", g.pps);
    else
        append(s, sizeof s, "Transport    : %u/s  RTT %d ms  |  Pi ok=%u gaps=%u\r\n",
               g.pps, g.rtt_ms, g.pi_frames_ok, g.pi_gaps);
    append(s, sizeof s, "\r\n");

    if (!g.armed) {
        append(s, sizeof s, "Disarmed. Your keyboard and mouse are not being read.\r\n");
        SetWindowTextA(g.body, s);
        return;
    }

    append(s, sizeof s, "Modifiers    : ");
    static const struct { uint8_t bit; const char *name; } mods[] = {
        {0x01, "LCtrl"}, {0x02, "LShift"}, {0x04, "LAlt"}, {0x08, "LGui"},
        {0x10, "RCtrl"}, {0x20, "RShift"}, {0x40, "RAlt"}, {0x80, "RGui"},
    };
    int any = 0;
    for (int i = 0; i < 8; i++)
        if (g.modifiers & mods[i].bit) {
            append(s, sizeof s, "%s%s", any ? " " : "", mods[i].name);
            any = 1;
        }
    append(s, sizeof s, "%s\r\n", any ? "" : "-");

    append(s, sizeof s, "Keys held    : ");
    any = 0;
    for (unsigned u = HIDWAY_KEY_USAGE_MIN; u <= HIDWAY_KEY_USAGE_MAX; u++)
        if (hidway_bitmap_test(g.keys, (uint8_t)u)) {
            char tmp[8];
            append(s, sizeof s, "%s%s", any ? " " : "", hidway_usage_name((uint8_t)u, tmp));
            any = 1;
        }
    append(s, sizeof s, "%s\r\n", any ? "" : "-");

    append(s, sizeof s, "Mouse btns   : ");
    if (g.buttons)
        append(s, sizeof s, "%s%s%s%s%s\r\n",
               (g.buttons & BTN_L) ? "L " : "", (g.buttons & BTN_R) ? "R " : "",
               (g.buttons & BTN_M) ? "M " : "", (g.buttons & BTN_X1) ? "4 " : "",
               (g.buttons & BTN_X2) ? "5" : "");
    else
        append(s, sizeof s, "-\r\n");

    append(s, sizeof s, "Move (50ms)  : dx=%+ld dy=%+ld\r\n",
           (long)(g.cum_x - g.disp_x), (long)(g.cum_y - g.disp_y));
    append(s, sizeof s, "Wheel (50ms) : v=%+d h=%+d\r\n",
           (int)(int16_t)(g.cum_wheel - g.disp_wheel), (int)(int16_t)(g.cum_pan - g.disp_pan));

    g.disp_x = g.cum_x;
    g.disp_y = g.cum_y;
    g.disp_wheel = g.cum_wheel;
    g.disp_pan = g.cum_pan;

    SetWindowTextA(g.body, s);
}

static void create_controls(HWND hwnd)
{
    g.font_status = CreateFontA(34, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g.font_body = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              FIXED_PITCH | FF_MODERN, "Consolas");

    g.status = CreateWindowA("STATIC", "  DISARMED", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                             14, 12, 452, 44, hwnd, (HMENU)(INT_PTR)ID_STATUS, NULL, NULL);
    g.body = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT,
                           16, 70, 452, 236, hwnd, (HMENU)(INT_PTR)ID_BODY, NULL, NULL);
    g.button = CreateWindowA("BUTTON", "Arm  (Ctrl+Shift+F12)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             16, 312, 452, 38, hwnd, (HMENU)(INT_PTR)ID_BUTTON, NULL, NULL);
    g.hint = CreateWindowA("STATIC",
                           "Preview: reads and relays your input; does not block local input.",
                           WS_CHILD | WS_VISIBLE | SS_LEFT,
                           16, 358, 452, 36, hwnd, (HMENU)(INT_PTR)ID_HINT, NULL, NULL);

    SendMessageA(g.status, WM_SETFONT, (WPARAM)g.font_status, TRUE);
    SendMessageA(g.body, WM_SETFONT, (WPARAM)g.font_body, TRUE);
    SendMessageA(g.button, WM_SETFONT, (WPARAM)g.font_status, TRUE);
    SendMessageA(g.hint, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

static int register_raw_input(HWND hwnd)
{
    RAWINPUTDEVICE rid[2] = {
        {0x01, 0x06, RIDEV_INPUTSINK, hwnd}, /* keyboard */
        {0x01, 0x02, RIDEV_INPUTSINK, hwnd}, /* mouse */
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
        RegisterHotKey(hwnd, HOTKEY_TOGGLE, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
        SetTimer(hwnd, TIMER_UI, UI_REFRESH_MS, NULL);
        return 0;

    case WM_INPUT:
        on_raw_input((HRAWINPUT)lp);
        return 0;

    case WM_HOTKEY:
        if (wp == HOTKEY_TOGGLE)
            set_armed(hwnd, !g.armed);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_BUTTON && HIWORD(wp) == BN_CLICKED)
            set_armed(hwnd, !g.armed);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_UI)
            refresh_body();
        else if (wp == TIMER_NET)
            net_tick();
        return 0;

    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g.status) {
            HDC dc = (HDC)wp;
            SetBkColor(dc, RGB(24, 26, 30));
            SetTextColor(dc, g.armed ? RGB(60, 210, 120) : RGB(150, 155, 165));
            return (LRESULT)g.bg;
        }
        SetBkColor((HDC)wp, RGB(24, 26, 30));
        SetTextColor((HDC)wp, (HWND)lp == g.hint ? RGB(140, 145, 155) : RGB(225, 228, 233));
        return (LRESULT)g.bg;

    case WM_CTLCOLORBTN:
        return (LRESULT)g.bg;

    case WM_CLOSE:
        set_armed(hwnd, 0);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_UI);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE);
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

    timeBeginPeriod(1); /* raise timer resolution for the net heartbeat */
    g.bg = CreateSolidBrush(RGB(24, 26, 30));

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g.bg;
    wc.lpszClassName = "HIDwayClient";
    if (!RegisterClassA(&wc))
        return 1;

    RECT r = {0, 0, 484, 408};
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
