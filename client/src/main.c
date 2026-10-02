/*
 * HIDway client - remote PC interface (Phase 1a: visible preview).
 *
 * A plain, visible window with an ARM/DISARM switch. While ARMED it reads this
 * PC's own keyboard and mouse through the Windows Raw Input API and shows the
 * current state that WOULD be relayed to the gaming PC: held keys, modifiers,
 * mouse buttons and recent movement.
 *
 * This build only OBSERVES and DISPLAYS. It does not block local input and it
 * does not send anything over the network yet; raw input is registered without
 * RIDEV_NOLEGACY, so every key and click still reaches the focused application
 * normally. The network transport is a later step.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "hidway_kbd.h"
#include "keymap_sc2hid.h"

#define WM_APP_TOGGLE (WM_APP + 1)

#define HOTKEY_TOGGLE 1
#define REFRESH_MS    33

#define ID_STATUS 1001
#define ID_BODY   1002
#define ID_BUTTON 1003
#define ID_HINT   1004

enum { BTN_L = 1, BTN_R = 2, BTN_M = 4, BTN_X1 = 8, BTN_X2 = 16 };

static struct {
    hidway_config_t cfg;
    int armed;

    /* Current relay state (updated on the UI thread from WM_INPUT). */
    uint8_t modifiers;
    uint8_t keys[HIDWAY_KEY_BITMAP_BYTES];
    uint8_t buttons;

    /* Movement accumulated since the last display refresh (activity readout). */
    long dx, dy;
    long wheel, hwheel;

    HWND status, body, button, hint;
    HFONT font_body, font_status;
    HBRUSH bg;
} g;

static void clear_relay_state(void)
{
    g.modifiers = 0;
    memset(g.keys, 0, sizeof g.keys);
    g.buttons = 0;
    g.dx = g.dy = g.wheel = g.hwheel = 0;
}

static void set_armed(int on)
{
    if (on == g.armed)
        return;
    g.armed = on;
    clear_relay_state(); /* fail-safe: entering or leaving ARMED releases all */
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

    if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) { /* relative movement (normal mice) */
        g.dx += m->lLastX;
        g.dy += m->lLastY;
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
    if (bf & RI_MOUSE_WHEEL)              g.wheel += (SHORT)m->usButtonData;
    if (bf & RI_MOUSE_HWHEEL)             g.hwheel += (SHORT)m->usButtonData;
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

static void refresh_body(void)
{
    char s[1024];
    s[0] = 0;

    append(s, sizeof s, "Relay target : %s:%d\r\n", g.cfg.target, g.cfg.port);
    append(s, sizeof s, "Transport    : (preview - not sending yet)\r\n\r\n");

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

    append(s, sizeof s, "Move (33ms)  : dx=%+ld dy=%+ld\r\n", g.dx, g.dy);
    append(s, sizeof s, "Wheel (33ms) : v=%+ld h=%+ld\r\n", g.wheel / WHEEL_DELTA, g.hwheel / WHEEL_DELTA);

    SetWindowTextA(g.body, s);

    /* Movement is an activity readout: reset each tick. Held keys/buttons are
     * level state and persist until released. */
    g.dx = g.dy = g.wheel = g.hwheel = 0;
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
                           16, 70, 450, 220, hwnd, (HMENU)(INT_PTR)ID_BODY, NULL, NULL);
    g.button = CreateWindowA("BUTTON", "Arm  (Ctrl+Shift+F12)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             16, 300, 452, 38, hwnd, (HMENU)(INT_PTR)ID_BUTTON, NULL, NULL);
    g.hint = CreateWindowA("STATIC",
                           "Preview only: reads and shows your input, does not block it or send it.",
                           WS_CHILD | WS_VISIBLE | SS_LEFT,
                           16, 346, 452, 36, hwnd, (HMENU)(INT_PTR)ID_HINT, NULL, NULL);

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
        RegisterHotKey(hwnd, HOTKEY_TOGGLE, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
        SetTimer(hwnd, 1, REFRESH_MS, NULL);
        return 0;

    case WM_INPUT:
        on_raw_input((HRAWINPUT)lp);
        return 0;

    case WM_HOTKEY:
        if (wp == HOTKEY_TOGGLE)
            set_armed(!g.armed);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_BUTTON && HIWORD(wp) == BN_CLICKED)
            set_armed(!g.armed);
        return 0;

    case WM_TIMER:
        refresh_body();
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

    case WM_KILLFOCUS:
        return 0;

    case WM_CLOSE:
        set_armed(0);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE);
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
    g.bg = CreateSolidBrush(RGB(24, 26, 30));

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g.bg;
    wc.lpszClassName = "HIDwayClient";
    if (!RegisterClassA(&wc))
        return 1;

    RECT r = {0, 0, 482, 396};
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
    return 0;
}
