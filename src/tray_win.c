#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include "tray.h"
#include "monitors.h"
#include "net.h"
#include "assets.h"
#include "gdiplus_min.h"

#include <initguid.h>
#include <windows.h>
#include <dbt.h>
#include <shellapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <stdio.h>
#include <string.h>

#define SWAPP_TRAY_MSG        (WM_APP + 1)
#define SWAPP_TRAY_UID        1
#define SWAPP_ID_QUIT         1001
#define SWAPP_HOTKEY_F16      1
#define SWAPP_HOTKEY_F15      2
#define SWAPP_F16_DOUBLE_TAP_MS 1000
#define SWAPP_RESCAN_TIMER    2
#define SWAPP_RESCAN_SETTLE_MS 3000
#define SWAPP_NET_STATE_MSG   (WM_APP + 5)
#define SWAPP_NET_RESCAN_MSG  (WM_APP + 6)
#define SWAPP_NET_SWITCH_MSG  (WM_APP + 7)
#define SWAPP_NET_ASSIGN_MSG  (WM_APP + 8)
#define SWAPP_NET_SWITCH_ALL_MSG (WM_APP + 9)
#define SWAPP_ID_OPEN         1002
#define SWAPP_ID_WINDOWS_ALL  1003
#define SWAPP_ID_LINUX_ALL    1004

/* The main window. The monitor rows move in here once the client can drive
 * them; for now it shows the link status, which is the only thing the
 * server has to report. */
#define SWAPP_MAIN_CLASS      "SwappMainWindow"
#define SWAPP_MAIN_STYLE      (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)
#define SWAPP_MAIN_MARGIN     12
#define SWAPP_MAIN_STATUS_ID  4100
#define SWAPP_MAIN_REFRESH_ID 4101
#define SWAPP_MAIN_WINDOWS_ALL_ID 4106
#define SWAPP_MAIN_LINUX_ALL_ID   4107
#define SWAPP_TABLE_SWITCH_ID 4102
#define SWAPP_SWITCH_RESCAN_DELAY_MS 2500 /* for the monitor to finish changing inputs */
#define SWAPP_TABLE_ASSIGN_WINDOWS_ID 4103
#define SWAPP_TABLE_ASSIGN_LINUX_ID   4104
#define SWAPP_TABLE_ASSIGN_NONE_ID    4105
#define SWAPP_REFRESH_WIDTH   72 /* the button spans both status rows */
#define SWAPP_MAIN_SPIN_TIMER 4
#define SWAPP_MAIN_SPIN_MS    60
#define SWAPP_ICON_CLASS      "SwappIconWindow"

/* Not pulled in by the SDK headers this project targets without also
 * dragging in the full display-driver header set, so it's defined here
 * from its well-known value instead. */
DEFINE_GUID(SWAPP_GUID_DEVINTERFACE_MONITOR, 0xe6f07b5f, 0xee97, 0x4a90,
            0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7);

/* Tray icon data, kept at file scope so the F16 hotkey handler in
 * swapp_wnd_proc can show a balloon notification off the same icon that
 * swapp_tray_run registered -- only one tray icon ever exists per process. */
static NOTIFYICONDATAA g_nid = {0};
static HDEVNOTIFY g_dev_notify = NULL;

/* The monitor list window -- a normal top-level window, separate from the
 * message-only one that owns the tray icon. One row per cached monitor: a
 * label, then a button per supported input. The active input's button is
 * disabled; clicking any other switches the monitor to it. Only one window
 * exists at a time; reopening it rebuilds the rows and raises it. */
#define SWAPP_LIST_CLASS      "SwappMonitorListWindow"
#define SWAPP_LIST_STYLE      (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)
#define SWAPP_LIST_MAX_INPUTS 32
#define SWAPP_LIST_BTN_BASE   100 /* control id = base + monitor * max_inputs + input */
#define SWAPP_LIST_MARGIN     12
#define SWAPP_LIST_LABEL_W    150
#define SWAPP_LIST_BTN_W      110
#define SWAPP_LIST_BTN_H      28
#define SWAPP_LIST_GAP        6
#define SWAPP_LIST_REBUILD_MSG (WM_APP + 2)
#define SWAPP_LIST_COMBO_BASE 5000 /* same layout as SWAPP_LIST_BTN_BASE */
#define SWAPP_LIST_COMBO_W    90
#define SWAPP_LIST_SWITCH_ID  4000
#define SWAPP_LIST_REFRESH_ID 4001
#define SWAPP_LIST_REFRESH_MSG (WM_APP + 3)
#define SWAPP_LIST_TRIGGER_MSG (WM_APP + 4)
#define SWAPP_LIST_SPIN_TIMER  3
#define SWAPP_LIST_SPIN_MS     120
#define SWAPP_LIST_SPIN_ID     4002
#define SWAPP_LIST_ACTION_W    110

/* Indexed by swapp_input_role. */
static const char *const g_role_names[] = {"None", "Linux", "Windows"};

/* The busy indicator shown while a job is querying the monitors. A static
 * text cycled from a timer rather than a PBS_MARQUEE progress bar: the
 * marquee style needs comctl32 v6 and an application manifest to animate,
 * neither of which this build carries, whereas this works unconditionally. */
static const char *const g_spin_frames[] = {"|", "/", "-", "\\"};
static int g_spin_frame = 0;
static ULONGLONG g_busy_started_ms = 0;

/* A first enumeration can take a minute on a slow DDC link, so the caption
 * counts seconds -- an unchanging indicator for that long reads as a hang. */
static void swapp_list_spinner_text(char *buf, size_t buf_size) {
    unsigned long elapsed = (unsigned long)((GetTickCount64() - g_busy_started_ms) / 1000);
    snprintf(buf, buf_size, "%s  Querying monitors... %lus", g_spin_frames[g_spin_frame & 3], elapsed);
}

static HWND g_list_hwnd = NULL;

static void swapp_show_monitor_list(void);

static BOOL CALLBACK swapp_list_destroy_child(HWND child, LPARAM lp) {
    (void)lp;
    DestroyWindow(child);
    return TRUE;
}

static HWND swapp_list_add_control(const char *cls, const char *text, DWORD style, int x, int y, int w, int h,
                                   int id) {
    HWND ctl = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_list_hwnd,
                               (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(ctl, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return ctl;
}

static void swapp_list_rebuild(void) {
    if (!g_list_hwnd) {
        return;
    }
    EnumChildWindows(g_list_hwnd, swapp_list_destroy_child, 0);

    size_t n = swapp_monitors_count();
    int y = SWAPP_LIST_MARGIN;
    int max_buttons = 0;

    /* A job owns the cache while it runs, so there is nothing safe to
     * render from it -- and nothing the user should be clicking either.
     * The spinner replaces the rows rather than just disabling them, which
     * is what keeps the cache single-threaded. */
    if (swapp_monitors_busy()) {
        char text[64];
        /* A rebuild mid-job (the window being reopened, say) must not
         * restart the count -- only a fresh busy period does. */
        if (!g_busy_started_ms) {
            g_busy_started_ms = GetTickCount64();
        }
        swapp_list_spinner_text(text, sizeof(text));
        swapp_list_add_control("STATIC", text, SS_CENTERIMAGE, SWAPP_LIST_MARGIN, y, 300, SWAPP_LIST_BTN_H,
                               SWAPP_LIST_SPIN_ID);
        SetTimer(g_list_hwnd, SWAPP_LIST_SPIN_TIMER, SWAPP_LIST_SPIN_MS, NULL);

        RECT busy_rc = {0, 0, 360, y + SWAPP_LIST_BTN_H + SWAPP_LIST_MARGIN};
        AdjustWindowRect(&busy_rc, SWAPP_LIST_STYLE, FALSE);
        SetWindowPos(g_list_hwnd, NULL, 0, 0, busy_rc.right - busy_rc.left, busy_rc.bottom - busy_rc.top,
                     SWP_NOMOVE | SWP_NOZORDER);
        return;
    }
    KillTimer(g_list_hwnd, SWAPP_LIST_SPIN_TIMER);
    g_busy_started_ms = 0;

    if (n == 0) {
        swapp_list_add_control("STATIC", "No DDC/CI monitors detected", 0, SWAPP_LIST_MARGIN, y, 300,
                               SWAPP_LIST_BTN_H, 0);
        y += SWAPP_LIST_BTN_H + SWAPP_LIST_GAP;
    }

    for (size_t i = 0; i < n; i++) {
        char label[96];
        swapp_monitors_label(i, label, sizeof(label));
        swapp_list_add_control("STATIC", label, SS_CENTERIMAGE, SWAPP_LIST_MARGIN, y, SWAPP_LIST_LABEL_W,
                               SWAPP_LIST_BTN_H, 0);

        int codes[SWAPP_LIST_MAX_INPUTS];
        int n_inputs = swapp_monitors_inputs(i, codes, SWAPP_LIST_MAX_INPUTS);
        int active = swapp_monitors_active_input(i);
        int x = SWAPP_LIST_MARGIN + SWAPP_LIST_LABEL_W;

        if (n_inputs == 0) {
            swapp_list_add_control("STATIC", "(inputs unknown)", SS_CENTERIMAGE, x, y, 200, SWAPP_LIST_BTN_H, 0);
        }
        for (int j = 0; j < n_inputs; j++) {
            HWND btn = swapp_list_add_control("BUTTON", swapp_monitors_input_name(codes[j]), BS_PUSHBUTTON, x, y,
                                              SWAPP_LIST_BTN_W, SWAPP_LIST_BTN_H,
                                              SWAPP_LIST_BTN_BASE + (int)i * SWAPP_LIST_MAX_INPUTS + j);
            if (codes[j] == active) {
                EnableWindow(btn, FALSE);
            }
            x += SWAPP_LIST_BTN_W + SWAPP_LIST_GAP;

            /* The height passed for a drop-down list covers the opened list;
             * the closed control sizes itself to the font. */
            HWND combo = swapp_list_add_control("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, x, y + 3,
                                                SWAPP_LIST_COMBO_W, 120,
                                                SWAPP_LIST_COMBO_BASE + (int)i * SWAPP_LIST_MAX_INPUTS + j);
            for (int r = 0; r < 3; r++) {
                SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)g_role_names[r]);
            }
            SendMessageA(combo, CB_SETCURSEL, (WPARAM)swapp_roles_get(label, codes[j]), 0);
            x += SWAPP_LIST_COMBO_W + SWAPP_LIST_GAP * 3;
        }
        if (n_inputs > max_buttons) {
            max_buttons = n_inputs;
        }
        y += SWAPP_LIST_BTN_H + SWAPP_LIST_GAP;
    }

    /* Settled before the action row is placed, because Refresh is pinned
     * to the right edge and so needs to know how wide the window will be. */
    int width = SWAPP_LIST_MARGIN * 2 + SWAPP_LIST_LABEL_W + max_buttons * (SWAPP_LIST_BTN_W + SWAPP_LIST_COMBO_W + SWAPP_LIST_GAP * 4);
    if (width < 360) {
        width = 360;
    }

    /* Takes the screens over for this machine: switches every monitor to
     * the input assigned this OS. Needs each monitor to have one, and is
     * pointless when they are all already showing it. Trigger on the left,
     * Refresh against the right edge, both the same width so they read as a
     * matched pair. */
    y += SWAPP_LIST_GAP;
    HWND switch_btn = swapp_list_add_control("BUTTON", "Trigger", BS_PUSHBUTTON, SWAPP_LIST_MARGIN, y,
                                             SWAPP_LIST_ACTION_W, SWAPP_LIST_BTN_H, SWAPP_LIST_SWITCH_ID);
    EnableWindow(switch_btn, swapp_roles_trigger_available(SWAPP_ROLE_SELF) ? TRUE : FALSE);
    swapp_list_add_control("BUTTON", "Refresh", BS_PUSHBUTTON, width - SWAPP_LIST_MARGIN - SWAPP_LIST_ACTION_W,
                           y, SWAPP_LIST_ACTION_W, SWAPP_LIST_BTN_H, SWAPP_LIST_REFRESH_ID);
    y += SWAPP_LIST_BTN_H + SWAPP_LIST_GAP;

    RECT rc = {0, 0, width, y - SWAPP_LIST_GAP + SWAPP_LIST_MARGIN};
    AdjustWindowRect(&rc, SWAPP_LIST_STYLE, FALSE);
    SetWindowPos(g_list_hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
}

static void swapp_list_on_role(int id, HWND combo) {
    int rel = id - SWAPP_LIST_COMBO_BASE;
    size_t index = (size_t)(rel / SWAPP_LIST_MAX_INPUTS);
    int input = rel % SWAPP_LIST_MAX_INPUTS;

    int codes[SWAPP_LIST_MAX_INPUTS];
    int n_inputs = swapp_monitors_inputs(index, codes, SWAPP_LIST_MAX_INPUTS);
    LRESULT sel = SendMessageA(combo, CB_GETCURSEL, 0, 0);
    if (input >= n_inputs || sel < 0 || sel > SWAPP_ROLE_WINDOWS) {
        return;
    }
    char label[96];
    swapp_monitors_label(index, label, sizeof(label));
    swapp_roles_set(label, codes[input], (swapp_input_role)sel);
    /* Another input of this monitor may have just lost the role. */
    PostMessageA(g_list_hwnd, SWAPP_LIST_REBUILD_MSG, 0, 0);
}

static void swapp_list_on_button(int id) {
    int rel = id - SWAPP_LIST_BTN_BASE;
    if (rel < 0 || id >= SWAPP_LIST_COMBO_BASE) {
        return;
    }
    size_t index = (size_t)(rel / SWAPP_LIST_MAX_INPUTS);
    int input = rel % SWAPP_LIST_MAX_INPUTS;

    int codes[SWAPP_LIST_MAX_INPUTS];
    int n_inputs = swapp_monitors_inputs(index, codes, SWAPP_LIST_MAX_INPUTS);
    if (input >= n_inputs) {
        return;
    }
    if (!swapp_monitors_set_input(index, codes[input])) {
        MessageBoxA(g_list_hwnd, "The monitor did not accept the input switch command.", "Swapp",
                    MB_OK | MB_ICONWARNING);
    }
    /* Rebuilding destroys the clicked button, so defer it out of the
     * button's own notification. */
    PostMessageA(g_list_hwnd, SWAPP_LIST_REBUILD_MSG, 0, 0);
}

static LRESULT CALLBACK swapp_list_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) >= SWAPP_LIST_COMBO_BASE) {
                if (HIWORD(wp) == CBN_SELCHANGE) {
                    swapp_list_on_role(LOWORD(wp), (HWND)lp);
                }
            } else if (LOWORD(wp) == SWAPP_LIST_REFRESH_ID) {
                /* Swapping the rows for the spinner destroys this button,
                 * so run it outside the button's own notification. */
                if (HIWORD(wp) == BN_CLICKED) {
                    PostMessageA(hwnd, SWAPP_LIST_REFRESH_MSG, 0, 0);
                }
            } else if (LOWORD(wp) == SWAPP_LIST_SWITCH_ID) {
                if (HIWORD(wp) == BN_CLICKED) {
                    PostMessageA(hwnd, SWAPP_LIST_TRIGGER_MSG, 0, 0);
                }
            } else if (HIWORD(wp) == BN_CLICKED) {
                swapp_list_on_button(LOWORD(wp));
            }
            return 0;
        case SWAPP_LIST_REBUILD_MSG:
            swapp_list_rebuild();
            return 0;
        /* Both start a worker-thread job and return at once; the rebuild
         * puts the spinner up, and the job callback rebuilds again when the
         * results land. */
        case SWAPP_LIST_REFRESH_MSG:
            swapp_monitors_rescan_async();
            swapp_list_rebuild();
            return 0;
        case SWAPP_LIST_TRIGGER_MSG:
            swapp_monitors_trigger_async(SWAPP_ROLE_SELF);
            swapp_list_rebuild();
            return 0;
        case WM_TIMER:
            if (wp == SWAPP_LIST_SPIN_TIMER) {
                char text[64];
                g_spin_frame++;
                swapp_list_spinner_text(text, sizeof(text));
                SetDlgItemTextA(hwnd, SWAPP_LIST_SPIN_ID, text);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, SWAPP_LIST_SPIN_TIMER);
            g_list_hwnd = NULL;
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

/* Runs on the UI thread once a job's results are in the cache. */
static void swapp_list_job_done(int trigger_failed) {
    swapp_list_rebuild();
    if (trigger_failed) {
        swapp_show_monitor_list();
        MessageBoxA(g_list_hwnd, "Not every monitor accepted the input switch command.", "Swapp",
                    MB_OK | MB_ICONWARNING);
    }
}

static void swapp_show_monitor_list(void) {
    if (!g_list_hwnd) {
        g_list_hwnd = CreateWindowExA(0, SWAPP_LIST_CLASS, "Swapp - Monitors", SWAPP_LIST_STYLE, CW_USEDEFAULT,
                                      CW_USEDEFAULT, 400, 200, NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (!g_list_hwnd) {
            return;
        }
    }

    /* The other machine switches these same monitors without this one
     * hearing about it, so what the cache last saw can be stale. */
    swapp_monitors_refresh_active_inputs();
    swapp_list_rebuild();
    ShowWindow(g_list_hwnd, IsIconic(g_list_hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_list_hwnd);
}

/* The window the socket thread pokes when the link state changes, so the
 * status text is rebuilt on the UI thread rather than from the socket. */
static HWND g_tray_hwnd = NULL;
static HWND g_main_hwnd = NULL;
static HWND g_status_hwnd = NULL;
static HWND g_refresh_hwnd = NULL;
static HWND g_windows_all_hwnd = NULL; /* switch every monitor to Windows */
static HWND g_linux_all_hwnd = NULL;   /* ... and to Linux */
static int swapp_main_can_switch_all(swapp_input_role role);

/* GDI+, resolved at startup. All of it is optional: if the DLL or an icon
 * is missing the rows simply draw no icon, and the text still says
 * everything they would have. */
static struct {
    HMODULE module;
    ULONG_PTR token;
    GdipCreateBitmapFromFile_t create_bitmap;
    GdipCreateFromHDC_t create_graphics;
    GdipDeleteGraphics_t delete_graphics;
    GdipTranslateWorldTransform_t translate;
    GdipRotateWorldTransform_t rotate;
    GdipDrawImageRectI_t draw_image;
    GdipSetInterpolationMode_t interpolation;
    GdipCreateImageAttributes_t create_attributes;
    GdipSetImageAttributesColorMatrix_t set_color_matrix;
    GdipGetImageDimension_t image_width;
    GdipGetImageDimension_t image_height;
    GdipDrawImageRectRectI_t draw_image_tinted;
} g_gdip = {0};

typedef enum {
    SWAPP_ICON_SPINNER = 0,
    SWAPP_ICON_CHECK = 1,
    SWAPP_ICON_ERROR = 2,
    SWAPP_ICON_REFRESH = 3,
    SWAPP_ICON_CHECK_MUTED = 4, /* check.png again, tinted light gray */
    SWAPP_ICON_WINDOWS = 5,
    SWAPP_ICON_LINUX = 6,
    SWAPP_ICON_COUNT
} swapp_icon;

/* Layout metrics are in 96-DPI units and scaled through swapp_dpi_scale()
 * at use, because the monitors this runs on are at 200%. The icons are
 * 48px source art, so they still have pixels to spare at 2x. */
#define SWAPP_ICON_SIZE 20
#define SWAPP_MAIN_WIDTH 820 /* client area */
#define SWAPP_TABLE_ROWS 2
#define SWAPP_TABLE_MONITOR_WIDTH 200
#define SWAPP_TABLE_MIN_INPUT_WIDTH 90
#define SWAPP_TABLE_ROW_HEIGHT 28
#define SWAPP_TABLE_CELL_PAD_X 12

static int g_dpi = 96;

static int swapp_dpi_scale(int value) {
    return MulDiv(value, g_dpi, 96);
}

static GpBitmap *g_icons[SWAPP_ICON_COUNT] = {NULL};
static GpImageAttributes *g_icon_tints[SWAPP_ICON_COUNT] = {NULL};

/* Per icon, in swapp_icon order: pending blue, done green, failed red,
 * refresh dark gray, inactive-input light gray, Windows blue, Linux near-
 * black. Kept in step with the same table in tray_linux.c. */
static const float g_icon_colors[SWAPP_ICON_COUNT][3] = {
    {0x1a / 255.0f, 0x73 / 255.0f, 0xe8 / 255.0f},
    {0x1e / 255.0f, 0x8e / 255.0f, 0x3e / 255.0f},
    {0xd9 / 255.0f, 0x30 / 255.0f, 0x25 / 255.0f},
    {0x44 / 255.0f, 0x44 / 255.0f, 0x44 / 255.0f},
    {0xc8 / 255.0f, 0xc8 / 255.0f, 0xc8 / 255.0f},
    {0x00 / 255.0f, 0x78 / 255.0f, 0xd4 / 255.0f},
    {0x33 / 255.0f, 0x33 / 255.0f, 0x33 / 255.0f},
};
static float g_spinner_angle = 0.0f;
static HFONT g_font = NULL;
static HFONT g_font_bold = NULL;
static int g_font_added = 0;

static void *swapp_gdip_proc(const char *name) {
    return (void *)GetProcAddress(g_gdip.module, name);
}

static void swapp_icons_load(void) {
    g_gdip.module = LoadLibraryA("gdiplus.dll");
    if (!g_gdip.module) {
        return;
    }

    GdiplusStartup_t startup = (GdiplusStartup_t)swapp_gdip_proc("GdiplusStartup");
    g_gdip.create_bitmap = (GdipCreateBitmapFromFile_t)swapp_gdip_proc("GdipCreateBitmapFromFile");
    g_gdip.create_graphics = (GdipCreateFromHDC_t)swapp_gdip_proc("GdipCreateFromHDC");
    g_gdip.delete_graphics = (GdipDeleteGraphics_t)swapp_gdip_proc("GdipDeleteGraphics");
    g_gdip.translate = (GdipTranslateWorldTransform_t)swapp_gdip_proc("GdipTranslateWorldTransform");
    g_gdip.rotate = (GdipRotateWorldTransform_t)swapp_gdip_proc("GdipRotateWorldTransform");
    g_gdip.draw_image = (GdipDrawImageRectI_t)swapp_gdip_proc("GdipDrawImageRectI");
    g_gdip.interpolation = (GdipSetInterpolationMode_t)swapp_gdip_proc("GdipSetInterpolationMode");
    g_gdip.create_attributes = (GdipCreateImageAttributes_t)swapp_gdip_proc("GdipCreateImageAttributes");
    g_gdip.set_color_matrix =
        (GdipSetImageAttributesColorMatrix_t)swapp_gdip_proc("GdipSetImageAttributesColorMatrix");
    g_gdip.image_width = (GdipGetImageDimension_t)swapp_gdip_proc("GdipGetImageWidth");
    g_gdip.image_height = (GdipGetImageDimension_t)swapp_gdip_proc("GdipGetImageHeight");
    g_gdip.draw_image_tinted = (GdipDrawImageRectRectI_t)swapp_gdip_proc("GdipDrawImageRectRectI");

    if (!startup || !g_gdip.create_bitmap || !g_gdip.create_graphics || !g_gdip.draw_image) {
        g_gdip.module = NULL;
        return;
    }

    GdiplusStartupInput input = {1, NULL, FALSE, FALSE};
    if (startup(&g_gdip.token, &input, NULL) != 0) {
        g_gdip.module = NULL;
        return;
    }

    static const char *const files[SWAPP_ICON_COUNT] = {
        "icons\\spinner.png", "icons\\check.png", "icons\\error.png", "icons\\refresh.png",
        "icons\\check.png", "icons\\windows.png", "icons\\linux.png"};
    for (int i = 0; i < SWAPP_ICON_COUNT; i++) {
        char path[MAX_PATH];
        if (!swapp_asset_path(files[i], path, sizeof(path))) {
            continue;
        }
        WCHAR wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, path, -1, wide, ARRAYSIZE(wide));
        g_gdip.create_bitmap(wide, &g_icons[i]);

        if (g_gdip.create_attributes && g_gdip.set_color_matrix
            && g_gdip.create_attributes(&g_icon_tints[i]) == 0) {
            GpColorMatrix matrix = {0};
            matrix.m[3][3] = 1.0f; /* alpha passes through */
            matrix.m[4][0] = g_icon_colors[i][0];
            matrix.m[4][1] = g_icon_colors[i][1];
            matrix.m[4][2] = g_icon_colors[i][2];
            matrix.m[4][4] = 1.0f;
            g_gdip.set_color_matrix(g_icon_tints[i], 0, TRUE, &matrix, NULL, 0);
        }
    }
}

/* Registers the bundled Inter face for this process only -- nothing is
 * installed for the machine, so the app looks the same everywhere without
 * touching anything outside its own directory. */
static void swapp_font_load(void) {
    char path[MAX_PATH];
    if (!swapp_asset_path("fonts\\Inter-Regular.ttf", path, sizeof(path))) {
        return;
    }
    if (AddFontResourceExA(path, FR_PRIVATE, NULL) == 0) {
        return; /* fall back to the stock GUI font */
    }
    g_font_added = 1;

    g_font = CreateFontA(-swapp_dpi_scale(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, "Inter");

    /* The table header, bold to match GTK's column titles. */
    if (swapp_asset_path("fonts\\Inter-SemiBold.ttf", path, sizeof(path))) {
        AddFontResourceExA(path, FR_PRIVATE, NULL);
    }
    g_font_bold = CreateFontA(-swapp_dpi_scale(15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Inter");
}

/* One status row: an owner-drawn icon plus a text control. The spinner is a
 * still image the app rotates itself -- Material ships no animation, and a
 * rotation is smoother than swapping frames. */
#define SWAPP_ROW_HEIGHT 26

typedef struct {
    HWND icon;
    HWND label;
    swapp_icon which;
} swapp_row;

static swapp_row g_client_row = {NULL, NULL, SWAPP_ICON_SPINNER};
static swapp_row g_monitors_row = {NULL, NULL, SWAPP_ICON_SPINNER};

/* Draws icon `which` into `hdc` at (x, y), `size` pixels square, in the
 * colour of icon `tint`. The spinner is drawn at its current rotation. */
static void swapp_icon_draw_tinted(HDC hdc, swapp_icon which, swapp_icon tint, int x, int y,
                                   int size) {
    GpBitmap *bitmap = g_icons[which];
    if (!g_gdip.module || !bitmap) {
        return;
    }
    GpGraphics *graphics = NULL;
    if (g_gdip.create_graphics(hdc, &graphics) != 0) {
        return;
    }
    if (g_gdip.interpolation) {
        g_gdip.interpolation(graphics, SWAPP_GDIP_INTERPOLATION_HIGH_QUALITY);
    }
    if (g_gdip.translate) {
        g_gdip.translate(graphics, (float)x, (float)y, SWAPP_GDIP_MATRIX_ORDER_PREPEND);
    }
    if (which == SWAPP_ICON_SPINNER && g_gdip.rotate && g_gdip.translate) {
        /* Rotate about the icon's own centre, not the origin. */
        float half = size / 2.0f;
        g_gdip.translate(graphics, half, half, SWAPP_GDIP_MATRIX_ORDER_PREPEND);
        g_gdip.rotate(graphics, g_spinner_angle, SWAPP_GDIP_MATRIX_ORDER_PREPEND);
        g_gdip.translate(graphics, -half, -half, SWAPP_GDIP_MATRIX_ORDER_PREPEND);
    }
    /* Drawn at the scaled size from the 48px source, so the icon is
     * resampled once rather than blown up from a 20px bitmap. */
    UINT src_w = 0;
    UINT src_h = 0;
    if (g_icon_tints[tint] && g_gdip.draw_image_tinted && g_gdip.image_width
        && g_gdip.image_height && g_gdip.image_width(bitmap, &src_w) == 0
        && g_gdip.image_height(bitmap, &src_h) == 0) {
        g_gdip.draw_image_tinted(graphics, bitmap, 0, 0, size, size, 0, 0, (int)src_w,
                                 (int)src_h, SWAPP_GDIP_UNIT_PIXEL, g_icon_tints[tint], NULL,
                                 NULL);
    } else {
        g_gdip.draw_image(graphics, bitmap, 0, 0, size, size);
    }
    g_gdip.delete_graphics(graphics);
}

static void swapp_icon_draw(HDC hdc, swapp_icon which, int x, int y, int size) {
    swapp_icon_draw_tinted(hdc, which, which, x, y, size);
}

/* Cell selection. The list view only knows whole-row selection, so its own
 * is suppressed and this tracks the one selected input cell of the whole
 * table instead (row -1 = none; the Monitor column is never selectable).
 * UI-only: it says which input the user picked, not anything about the
 * monitors. */
static int g_selected_row = -1;
static int g_selected_col = 0;

/* Custom-draws one "Input N" cell: a gray check before the input's name, or
 * a green one when it is marked '*' as active, over a highlight when the
 * cell is selected. */
static void swapp_table_draw_input(NMLVCUSTOMDRAW *cd) {
    HWND list = cd->nmcd.hdr.hwndFrom;
    int row = (int)cd->nmcd.dwItemSpec;
    HDC hdc = cd->nmcd.hdc;

    RECT cell;
    ListView_GetSubItemRect(list, row, cd->iSubItem, LVIR_BOUNDS, &cell);

    char text[512];
    ListView_GetItemText(list, row, cd->iSubItem, text, sizeof(text));

    if (text[0] && row == g_selected_row && cd->iSubItem == g_selected_col) {
        RECT fill = {cell.left + 1, cell.top + 1, cell.right - 1, cell.bottom - 1};
        HBRUSH back = CreateSolidBrush(RGB(0xd6, 0xe8, 0xfc));
        FillRect(hdc, &fill, back);
        DeleteObject(back);
        HBRUSH edge = CreateSolidBrush(RGB(0x1a, 0x73, 0xe8));
        FrameRect(hdc, &fill, edge);
        DeleteObject(edge);
    }

    HFONT old_font = (HFONT)SelectObject(hdc, (HGDIOBJ)SendMessageA(list, WM_GETFONT, 0, 0));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));

    int icon = swapp_dpi_scale(16);
    int gap = swapp_dpi_scale(4);
    int x = cell.left + swapp_dpi_scale(SWAPP_TABLE_CELL_PAD_X);
    int mid = (cell.top + cell.bottom) / 2;

    char *name = text;
    if (cd->iSubItem == 0) {
        /* The Monitor column: plain text, drawn here only so its inset
         * matches the input cells. */
        RECT r = {x, cell.top, cell.right - swapp_dpi_scale(4), cell.bottom};
        DrawTextA(hdc, name, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(hdc, old_font);
        return;
    }
    if (*name == '\0') {
        SelectObject(hdc, old_font);
        return; /* a slot past this monitor's last input */
    }
    /* Two flag characters lead the name: see swapp_main_refresh_status. */
    int is_active = (name[0] == '*');
    char owner = (name[0] && name[1]) ? name[1] : '-';
    name += (name[0] && name[1]) ? 2 : strlen(name);

    swapp_icon_draw(hdc, is_active ? SWAPP_ICON_CHECK : SWAPP_ICON_CHECK_MUTED, x,
                    mid - icon / 2, icon);
    x += icon + gap;

    /* The owning machine's logo is pinned to the cell's right edge, with
     * the same inset as the left; the name ellipsizes before it rather than
     * under it. */
    int has_logo = (owner == 'W' || owner == 'L');
    int pad = swapp_dpi_scale(SWAPP_TABLE_CELL_PAD_X);
    int logo = swapp_dpi_scale(owner == 'L' ? 16 : 13); /* optically matched to the check */
    int text_right = cell.right - (has_logo ? pad + logo + gap : swapp_dpi_scale(4));
    RECT r = {x, cell.top, text_right, cell.bottom};
    DrawTextA(hdc, name, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (has_logo) {
        swapp_icon_draw(hdc, owner == 'W' ? SWAPP_ICON_WINDOWS : SWAPP_ICON_LINUX,
                        cell.right - pad - logo, mid - logo / 2, logo);
    }
    SelectObject(hdc, old_font);
}

static void swapp_row_paint(HWND hwnd, swapp_icon which) {
    PAINTSTRUCT ps;
    HDC screen = BeginPaint(hwnd, &ps);

    /* Composed off-screen and blitted once: clearing and drawing straight to
     * the window shows the blank frame between the two on every tick. */
    RECT rect;
    GetClientRect(hwnd, &rect);
    HDC hdc = CreateCompatibleDC(screen);
    HBITMAP buffer = CreateCompatibleBitmap(screen, rect.right, rect.bottom);
    HGDIOBJ old_buffer = SelectObject(hdc, buffer);
    FillRect(hdc, &rect, (HBRUSH)(COLOR_BTNFACE + 1));

    swapp_icon_draw(hdc, which, 0, 0, swapp_dpi_scale(SWAPP_ICON_SIZE));

    BitBlt(screen, 0, 0, rect.right, rect.bottom, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, old_buffer);
    DeleteObject(buffer);
    DeleteDC(hdc);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK swapp_icon_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        swapp_row *row = (swapp_row *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        swapp_row_paint(hwnd, row ? row->which : SWAPP_ICON_SPINNER);
        return 0;
    }
    if (msg == WM_ERASEBKGND) {
        return 1; /* painted in WM_PAINT; erasing first only flickers */
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void swapp_row_create(swapp_row *row, HWND parent, int top, int width) {
    HINSTANCE instance = GetModuleHandleA(NULL);

    int margin = swapp_dpi_scale(SWAPP_MAIN_MARGIN);
    int size = swapp_dpi_scale(SWAPP_ICON_SIZE);

    row->icon = CreateWindowExA(0, SWAPP_ICON_CLASS, "", WS_CHILD | WS_VISIBLE,
                                margin, top, size, size, parent, NULL, instance, NULL);
    SetWindowLongPtrA(row->icon, GWLP_USERDATA, (LONG_PTR)row);

    int label_left = margin + size + swapp_dpi_scale(8);
    row->label = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 label_left, top + swapp_dpi_scale(2),
                                 width - label_left - margin, size,
                                 parent, NULL, instance, NULL);
    SendMessageA(row->label, WM_SETFONT,
                 (WPARAM)(g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

/* The status refresh runs on every spinner tick; setting identical text
 * still makes the control erase and redraw, which blinks. */
static void swapp_set_text_if_changed(HWND hwnd, const char *text) {
    char current[4096];
    GetWindowTextA(hwnd, current, sizeof(current));
    if (strcmp(current, text) != 0) {
        SetWindowTextA(hwnd, text);
    }
}

static void swapp_row_set(swapp_row *row, swapp_icon which, const char *text) {
    row->which = which;
    InvalidateRect(row->icon, NULL, FALSE);
    swapp_set_text_if_changed(row->label, text);
}

/* Replaces the table's rows with `snapshot`: one line per monitor, cells
 * separated by tabs. A no-op when nothing changed. */
static void swapp_table_set(const char *snapshot) {
    static char shown[8192] = "";
    if (!g_status_hwnd || strcmp(shown, snapshot) == 0) {
        return;
    }
    strncpy_s(shown, sizeof(shown), snapshot, _TRUNCATE);

    SendMessageA(g_status_hwnd, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_status_hwnd);

    /* One "Input N" column per input slot, as many as the monitor with the
     * most inputs reports; the rest of a shorter monitor's row stays blank. */
    int max_inputs = 0;
    int cells = 0;
    for (const char *p = snapshot;; p++) {
        if (*p == '\t') {
            cells++;
        } else if (*p == '\n' || *p == '\0') {
            if (cells > max_inputs) {
                max_inputs = cells;
            }
            cells = 0;
            if (*p == '\0') {
                break;
            }
        }
    }
    HWND header = ListView_GetHeader(g_status_hwnd);
    int have_inputs = Header_GetItemCount(header) - 1;
    if (snapshot[0] == '\0') {
        /* The list is gone (rescan, lost link): a selection would point at
         * whatever lands in that cell next. */
        g_selected_row = -1;
        g_selected_col = 0;
    }
    if (have_inputs != max_inputs) {
        while (Header_GetItemCount(header) > 1) {
            ListView_DeleteColumn(g_status_hwnd, 1);
        }
        RECT client;
        GetClientRect(g_status_hwnd, &client);
        int monitor_width = ListView_GetColumnWidth(g_status_hwnd, 0);
        int input_width = max_inputs
                              ? (client.right - monitor_width) / max_inputs
                              : 0;
        int min_width = swapp_dpi_scale(SWAPP_TABLE_MIN_INPUT_WIDTH);
        if (input_width < min_width) {
            input_width = min_width;
        }
        for (int c = 1; c <= max_inputs; c++) {
            char title[32];
            _snprintf_s(title, sizeof(title), _TRUNCATE, "Input %d", c);
            LVCOLUMNA column = {0};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = title;
            column.cx = input_width;
            ListView_InsertColumn(g_status_hwnd, c, &column);
        }
    }

    char copy[8192];
    strncpy_s(copy, sizeof(copy), snapshot, _TRUNCATE);
    char *line_ctx = NULL;
    int row = 0;
    for (char *line = strtok_s(copy, "\n", &line_ctx); line;
         line = strtok_s(NULL, "\n", &line_ctx), row++) {
        char *cell_ctx = NULL;
        int col = 0;
        for (char *cell = strtok_s(line, "\t", &cell_ctx); cell;
             cell = strtok_s(NULL, "\t", &cell_ctx), col++) {
            if (col == 0) {
                LVITEMA item = {0};
                item.mask = LVIF_TEXT;
                item.iItem = row;
                item.pszText = cell;
                ListView_InsertItem(g_status_hwnd, &item);
            } else {
                ListView_SetItemText(g_status_hwnd, row, col, cell);
            }
        }
    }

    SendMessageA(g_status_hwnd, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_status_hwnd, NULL, TRUE);
}

static void swapp_main_refresh_status(void) {
    if (!g_client_row.label) {
        return;
    }

    char status[128];
    swapp_net_status_text(status, sizeof(status));

    swapp_net_state state = swapp_net_get_state();
    int connected = (state == SWAPP_NET_CONNECTED);
    swapp_row_set(&g_client_row,
                  connected ? SWAPP_ICON_CHECK
                            : (state == SWAPP_NET_FAILED ? SWAPP_ICON_ERROR : SWAPP_ICON_SPINNER),
                  status);

    /* The scan is the server's own business and runs with or without a
     * client, so the monitor row never reports on the link. */
    char monitors[160];
    size_t count = swapp_monitors_count();
    int scanned = 0;
    int incomplete = 0;
    if (swapp_monitors_busy() || count == 0) {
        _snprintf_s(monitors, sizeof(monitors), _TRUNCATE, "Monitors: scanning over DDC/CI...");
    } else {
        scanned = 1;
        /* A monitor that answered but never gave its inputs (and had none
         * from an earlier scan to fall back on) is a partial result, not a
         * success: say so, rather than a green check over an empty row. */
        size_t without_inputs = 0;
        for (size_t i = 0; i < count; i++) {
            int codes[SWAPP_LIST_MAX_INPUTS];
            without_inputs += (swapp_monitors_inputs(i, codes, SWAPP_LIST_MAX_INPUTS) == 0);
        }
        if (without_inputs) {
            _snprintf_s(monitors, sizeof(monitors), _TRUNCATE,
                        "Monitors: %zu found, %zu without inputs -- rescan", count, without_inputs);
        } else {
            _snprintf_s(monitors, sizeof(monitors), _TRUNCATE, "Monitors: %zu found", count);
        }
        incomplete = (without_inputs > 0);
    }
    swapp_row_set(&g_monitors_row,
                  !scanned ? SWAPP_ICON_SPINNER : incomplete ? SWAPP_ICON_ERROR : SWAPP_ICON_CHECK,
                  monitors);

    /* A rescan only needs no scan already running: the result is the
     * server's own, whether or not there is a client to forward it to. */
    int can_rescan = !swapp_monitors_busy();
    if (g_refresh_hwnd && !IsWindowEnabled(g_refresh_hwnd) != !can_rescan) {
        EnableWindow(g_refresh_hwnd, can_rescan);
    }
    for (int b = 0; b < 2; b++) {
        HWND button = b ? g_linux_all_hwnd : g_windows_all_hwnd;
        int can_switch_all =
            swapp_main_can_switch_all(b ? SWAPP_ROLE_LINUX : SWAPP_ROLE_WINDOWS);
        if (button && !IsWindowEnabled(button) != !can_switch_all) {
            EnableWindow(button, can_switch_all);
        }
    }

    /* Above the rows, what the scan actually found: one table row per
     * monitor. Built into a flat snapshot first and only pushed into the
     * list view when it differs, since this runs on every spinner tick and
     * repopulating the control each time would flicker. */
    char snapshot[8192] = "";
    int used = 0;
    for (size_t i = 0; scanned && i < count && (size_t)used < sizeof(snapshot); i++) {
        char label[128];
        swapp_monitors_label(i, label, sizeof(label));

        int codes[SWAPP_LIST_MAX_INPUTS];
        int n_codes = swapp_monitors_inputs(i, codes, SWAPP_LIST_MAX_INPUTS);
        int active = swapp_monitors_active_input(i);
        int windows_code = swapp_roles_code_for(label, SWAPP_ROLE_WINDOWS);
        int linux_code = swapp_roles_code_for(label, SWAPP_ROLE_LINUX);

        /* One tab-separated cell per input, each name behind two flag
         * characters the cell's custom draw turns into icons
         * (swapp_table_draw_input): '*' or '-' for active or not, then 'W',
         * 'L' or '-' for the machine the input belongs to. */
        used += _snprintf_s(snapshot + used, sizeof(snapshot) - used, _TRUNCATE, "%s", label);
        for (int c = 0; c < n_codes && (size_t)used < sizeof(snapshot); c++) {
            used += _snprintf_s(snapshot + used, sizeof(snapshot) - used, _TRUNCATE, "\t%c%c%s",
                                codes[c] == active ? '*' : '-',
                                codes[c] == windows_code ? 'W' : codes[c] == linux_code ? 'L' : '-',
                                swapp_monitors_input_name(codes[c]));
        }
        used += _snprintf_s(snapshot + used, sizeof(snapshot) - used, _TRUNCATE, "\n");
    }
    swapp_table_set(snapshot);
}

/* The spinner only animates while something is pending; once both rows are
 * resolved there is nothing left to redraw. */
static void swapp_main_update_spinner(void) {
    if (!g_main_hwnd) {
        return;
    }
    int pending = (swapp_net_get_state() == SWAPP_NET_WAITING) || swapp_monitors_busy()
                  || swapp_monitors_count() == 0;
    if (pending) {
        SetTimer(g_main_hwnd, SWAPP_MAIN_SPIN_TIMER, SWAPP_MAIN_SPIN_MS, NULL);
    } else {
        KillTimer(g_main_hwnd, SWAPP_MAIN_SPIN_TIMER);
    }
}

/* Runs on the UI thread: monitors_win.c posts job completions to its own
 * sink window rather than calling back from the worker. */
static void swapp_main_job_done(int trigger_failed) {
    (void)trigger_failed;
    /* Results go out the moment they land rather than waiting to be asked
     * for; with no client this is a no-op and the list is the server's own. */
    swapp_net_send_monitors();
    swapp_main_refresh_status();
    swapp_main_update_spinner();
}

/* The server's rescan, from its own button or the client's `rescan`: drop
 * the list on both sides and scan again. */
static void swapp_main_rescan(void);

/* The server's Switch, from its own table menu or the client's `switch`:
 * one monitor to one input over DDC/CI. The request is checked against the
 * cache first, since a client's indices can be from a list that has since
 * been replaced. */
/* When the Windows / Linux buttons may act: no scan running, every monitor
 * listed with its inputs, and every monitor with both a Windows and a Linux
 * input assigned, so neither button could strand a monitor on nothing. Also
 * re-checked when the client's `switchall` arrives, whatever its UI thought.
 *
 * Switching to Linux additionally needs a connected client: handing the
 * screens to a machine that is not there would leave nothing on them, and
 * nobody to switch them back. Switching to Windows is safe either way --
 * this machine keeps the screens, and it is exactly the way back. */
static int swapp_main_can_switch_all(swapp_input_role role) {
    if (swapp_monitors_busy()) {
        return 0;
    }
    if (role == SWAPP_ROLE_LINUX && swapp_net_get_state() != SWAPP_NET_CONNECTED) {
        return 0;
    }
    size_t count = swapp_monitors_count();
    for (size_t i = 0; i < count; i++) {
        int codes[SWAPP_LIST_MAX_INPUTS];
        if (swapp_monitors_inputs(i, codes, SWAPP_LIST_MAX_INPUTS) == 0) {
            return 0;
        }
    }
    return swapp_roles_complete(); /* also 0 with no monitors at all */
}

/* When one cell's Switch may act. The input has to belong to a machine:
 * an unassigned input is nobody's, and the rest of the app reasons in the
 * two roles, so parking a monitor outside them is not offered. Linux's
 * input additionally needs the client, for the reason switch-all has
 * (above) -- one screen handed to a machine that is not there is the same
 * hazard as both. Checked for the server's own menu and again for the
 * client's `switch`, whatever its UI thought.
 *
 * The third case -- the input the monitor is already showing -- is not
 * here: the menu leaves Switch out on that cell from the cache the green
 * check is drawn from, but the switch itself judges it on a fresh read
 * (see swapp_main_switch), since a stale cache must not veto a real
 * switch. */
static int swapp_main_can_switch_to(swapp_input_role role) {
    if (role == SWAPP_ROLE_LINUX) {
        return swapp_net_get_state() == SWAPP_NET_CONNECTED;
    }
    return role == SWAPP_ROLE_WINDOWS;
}

/* Called by the post-switch watch, on the monitors worker thread, when the
 * client should do its modeset; the send is locked. */
static void swapp_main_acquire(void) {
    swapp_net_send_acquire();
}

/* What follows any switch, once the VCP 0x60 writes have gone out. */
static void swapp_main_after_switch(int to_linux) {
    /* A switch changes what every monitor reports, so it is always followed
     * by a full rescan. The UI side of that happens now -- the job counts
     * as busy immediately, which empties the table, puts the spinner up and
     * tells the client `scanning` -- but the DDC/CI querying waits for the
     * monitor to finish changing inputs. */
    swapp_monitors_set_job_callback(swapp_main_job_done);
    if (to_linux) {
        /* Switched to the client's input: it has to force a modeset to start
         * driving the monitor again (see swapp_monitors_acquire_async). The
         * watch sends `acquire` as soon as every monitor now on its Linux
         * input has finished switching, then watches each until it shows a
         * signal and asks again if it doesn't: the modeset doesn't always
         * take, and the monitor then just sleeps. */
        int watch[16]; /* SWAPP_WATCH_MAX; any more go unwatched */
        size_t n_watch = 0;
        for (size_t i = 0; i < swapp_monitors_count() && i < 16; i++) {
            char label[128];
            swapp_monitors_label(i, label, sizeof(label));
            int code = swapp_roles_code_for(label, SWAPP_ROLE_LINUX);
            watch[n_watch++] = code >= 0 && swapp_monitors_active_input(i) == code ? code : -1;
        }
        swapp_monitors_rescan_watched_async(watch, n_watch, swapp_main_acquire);
    } else {
        swapp_monitors_rescan_delayed_async(SWAPP_SWITCH_RESCAN_DELAY_MS);
    }
    swapp_net_send_monitors();
    swapp_main_refresh_status();
    swapp_main_update_spinner();
}

static void swapp_main_switch(int monitor_index, int input_code) {
    if (swapp_monitors_busy() || monitor_index < 0
        || (size_t)monitor_index >= swapp_monitors_count()) {
        return;
    }
    int codes[SWAPP_LIST_MAX_INPUTS];
    int n_codes = swapp_monitors_inputs((size_t)monitor_index, codes, SWAPP_LIST_MAX_INPUTS);
    int offered = 0;
    for (int c = 0; c < n_codes; c++) {
        offered |= (codes[c] == input_code);
    }
    if (!offered) {
        return;
    }

    char label[128];
    swapp_monitors_label((size_t)monitor_index, label, sizeof(label));
    swapp_input_role role = swapp_roles_get(label, input_code);
    if (!swapp_main_can_switch_to(role)) {
        return;
    }

    /* Already showing that input: nothing to do -- no write, no acquire,
     * no rescan. Judged on a fresh read, since the monitor's OSD or a lost
     * write can leave the cache behind what it is really showing. */
    swapp_monitors_refresh_active_inputs();
    if (swapp_monitors_active_input((size_t)monitor_index) == input_code) {
        swapp_net_send_monitors();
        swapp_main_refresh_status();
        return;
    }

    swapp_monitors_set_input((size_t)monitor_index, input_code);
    swapp_main_after_switch(role == SWAPP_ROLE_LINUX);
}

/* The Windows / Linux buttons, and the client's `switchall`: every monitor
 * with an input assigned to role is switched to it, as if Switch had been
 * picked on each of those cells. Monitors with nothing assigned to role are
 * left alone. */
static void swapp_main_switch_all(swapp_input_role role) {
    if ((role != SWAPP_ROLE_WINDOWS && role != SWAPP_ROLE_LINUX)
        || !swapp_main_can_switch_all(role)) {
        return;
    }
    /* Only monitors not already on role's input are written, judged on a
     * fresh read (see swapp_main_switch); everything already there is a
     * no-op, and so is the whole thing when nothing needed switching. */
    swapp_monitors_refresh_active_inputs();
    int switched = 0;
    size_t count = swapp_monitors_count();
    for (size_t i = 0; i < count; i++) {
        char label[128];
        swapp_monitors_label(i, label, sizeof(label));
        int code = swapp_roles_code_for(label, role);
        if (code >= 0 && swapp_monitors_active_input(i) != code) {
            swapp_monitors_set_input(i, code);
            switched = 1;
        }
    }
    if (!switched) {
        swapp_net_send_monitors();
        swapp_main_refresh_status();
    } else {
        swapp_main_after_switch(role == SWAPP_ROLE_LINUX);
    }
}


/* Called on the socket thread; DDC/CI traffic belongs on the UI thread,
 * alongside the cache it reads. Both arguments fit a message's params. */
static void swapp_net_switch_requested(int monitor_index, int input_code) {
    if (g_tray_hwnd) {
        PostMessageA(g_tray_hwnd, SWAPP_NET_SWITCH_MSG, (WPARAM)monitor_index, (LPARAM)input_code);
    }
}

/* The server's Assign, from its own table menu or the client's `assign`:
 * records which machine an input is cabled to. The only source of that
 * knowledge -- see monitors.h for why it isn't detected. Persisted by the
 * roles store; SWAPP_ROLE_NONE clears whatever the input held. */
static void swapp_main_assign(int monitor_index, int input_code, swapp_input_role role) {
    if (swapp_monitors_busy() || monitor_index < 0
        || (size_t)monitor_index >= swapp_monitors_count()) {
        return;
    }
    int codes[SWAPP_LIST_MAX_INPUTS];
    int n_codes = swapp_monitors_inputs((size_t)monitor_index, codes, SWAPP_LIST_MAX_INPUTS);
    int offered = 0;
    for (int c = 0; c < n_codes; c++) {
        offered |= (codes[c] == input_code);
    }
    if (!offered) {
        return;
    }
    char label[128];
    swapp_monitors_label((size_t)monitor_index, label, sizeof(label));
    swapp_roles_set(label, input_code, role);
    swapp_net_send_monitors();
    swapp_main_refresh_status();
}

/* Called on the socket thread. */
static void swapp_net_switch_all_requested(int role) {
    if (g_tray_hwnd) {
        PostMessageA(g_tray_hwnd, SWAPP_NET_SWITCH_ALL_MSG, (WPARAM)role, 0);
    }
}

/* Called on the socket thread. The monitor index and role share wParam
 * (both are small), the input code takes lParam. */
static void swapp_net_assign_requested(int monitor_index, int input_code, int role) {
    if (g_tray_hwnd && monitor_index >= 0 && monitor_index < 0x10000) {
        PostMessageA(g_tray_hwnd, SWAPP_NET_ASSIGN_MSG, (WPARAM)((monitor_index << 8) | role),
                     (LPARAM)input_code);
    }
}

static void swapp_main_rescan(void) {
    if (swapp_monitors_busy()) {
        return;
    }
    swapp_monitors_set_job_callback(swapp_main_job_done);
    swapp_monitors_rescan_async();
    swapp_net_send_monitors(); /* busy now, so this tells the client `scanning` */
    swapp_main_refresh_status();
    swapp_main_update_spinner();
}

/* Called on the socket thread; the scan has to start from the UI thread. */
static void swapp_net_rescan_requested(void) {
    if (g_tray_hwnd) {
        PostMessageA(g_tray_hwnd, SWAPP_NET_RESCAN_MSG, 0, 0);
    }
}

/* The big buttons (refresh, all-to-Windows, all-to-Linux): a flat rounded
 * rectangle with one glyph centred, drawn by hand so the icon can be the
 * same tinted art as the rest of the window. */
static void swapp_refresh_draw(DRAWITEMSTRUCT *item) {
    swapp_icon glyph = item->CtlID == SWAPP_MAIN_WINDOWS_ALL_ID ? SWAPP_ICON_WINDOWS
                       : item->CtlID == SWAPP_MAIN_LINUX_ALL_ID ? SWAPP_ICON_LINUX
                                                                : SWAPP_ICON_REFRESH;
    HDC hdc = item->hDC;
    RECT rect = item->rcItem;
    int disabled = (item->itemState & ODS_DISABLED) != 0;
    int pressed = (item->itemState & ODS_SELECTED) != 0;

    FillRect(hdc, &rect, (HBRUSH)(COLOR_BTNFACE + 1));
    COLORREF face = disabled ? RGB(0xf3, 0xf3, 0xf3) : pressed ? RGB(0xd8, 0xd8, 0xd8)
                                                                : RGB(0xfb, 0xfb, 0xfb);
    HBRUSH brush = CreateSolidBrush(face);
    HPEN pen = CreatePen(PS_SOLID, 1, disabled ? RGB(0xdd, 0xdd, 0xdd) : RGB(0xb8, 0xb8, 0xb8));
    HGDIOBJ old_brush = SelectObject(hdc, brush);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    int radius = swapp_dpi_scale(8);
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);

    int size = swapp_dpi_scale(28);
    int x = (rect.left + rect.right - size) / 2;
    int y = (rect.top + rect.bottom - size) / 2;
    /* Disabled borrows the light gray of the inactive-input check. */
    swapp_icon_draw_tinted(hdc, glyph, disabled ? SWAPP_ICON_CHECK_MUTED : glyph, x, y, size);
}

static void swapp_net_state_changed(void) {
    if (g_tray_hwnd) {
        PostMessageA(g_tray_hwnd, SWAPP_NET_STATE_MSG, 0, 0);
    }
}

/* A click on the table at client point pt: selects the input cell under it
 * (left-clicking the selected cell clears it), and for a right click opens
 * the cell menu, owned by owner. */
static void swapp_table_cell_clicked(HWND owner, POINT pt, int right) {
    LVHITTESTINFO hit = {0};
    hit.pt = pt;
    if (ListView_SubItemHitTest(g_status_hwnd, &hit) < 0 || hit.iSubItem < 1) {
        return;
    }
    char text[8];
    ListView_GetItemText(g_status_hwnd, hit.iItem, hit.iSubItem, text, sizeof(text));
    if (!text[0]) {
        return;
    }
    if (!right && hit.iItem == g_selected_row && hit.iSubItem == g_selected_col) {
        /* Left-clicking the selected cell again clears it. */
        g_selected_row = -1;
        g_selected_col = 0;
    } else {
        g_selected_row = hit.iItem;
        g_selected_col = hit.iSubItem;
    }
    /* Repainted before the menu opens, so the cell it acts on
     * is already highlighted while the menu is up. */
    RedrawWindow(g_status_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);

    if (right) {
        HMENU menu = CreatePopupMenu();
        /* Table rows are monitors in cache order, and the
         * "Input N" columns their inputs in the same order
         * swapp_monitors_inputs() gives them. */
        int codes[SWAPP_LIST_MAX_INPUTS];
        int n_codes = swapp_monitors_inputs((size_t)hit.iItem, codes,
                                            SWAPP_LIST_MAX_INPUTS);
        if (hit.iSubItem - 1 >= n_codes) {
            DestroyMenu(menu);
            return;
        }
        int code = codes[hit.iSubItem - 1];
        char label[128];
        swapp_monitors_label((size_t)hit.iItem, label, sizeof(label));
        swapp_input_role current = swapp_roles_get(label, code);

        /* Where Switch would do nothing it is left out rather than shown
         * dead: the menu is four items long, and on most cells only the
         * assignments are ever available. */
        if (swapp_main_can_switch_to(current)
            && swapp_monitors_active_input((size_t)hit.iItem) != code) {
            AppendMenuA(menu, MF_STRING, SWAPP_TABLE_SWITCH_ID, "Switch");
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        }
        AppendMenuA(menu, MF_STRING | (current == SWAPP_ROLE_WINDOWS ? MF_CHECKED : 0),
                    SWAPP_TABLE_ASSIGN_WINDOWS_ID, "Assign to Windows");
        AppendMenuA(menu, MF_STRING | (current == SWAPP_ROLE_LINUX ? MF_CHECKED : 0),
                    SWAPP_TABLE_ASSIGN_LINUX_ID, "Assign to Linux");
        AppendMenuA(menu, MF_STRING | (current == SWAPP_ROLE_NONE ? MF_GRAYED : 0),
                    SWAPP_TABLE_ASSIGN_NONE_ID, "Clear assignment");
        POINT screen = pt;
        ClientToScreen(g_status_hwnd, &screen);
        int chosen = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screen.x,
                                    screen.y, 0, owner, NULL);
        DestroyMenu(menu);
        if (chosen == SWAPP_TABLE_SWITCH_ID) {
            swapp_main_switch(hit.iItem, code);
        } else if (chosen == SWAPP_TABLE_ASSIGN_WINDOWS_ID) {
            swapp_main_assign(hit.iItem, code, SWAPP_ROLE_WINDOWS);
        } else if (chosen == SWAPP_TABLE_ASSIGN_LINUX_ID) {
            swapp_main_assign(hit.iItem, code, SWAPP_ROLE_LINUX);
        } else if (chosen == SWAPP_TABLE_ASSIGN_NONE_ID) {
            swapp_main_assign(hit.iItem, code, SWAPP_ROLE_NONE);
        }
    }
}

/* The table's mouse buttons, taken before the list view sees them. Its own
 * handling focuses the control and runs a drag-detect loop before it gets
 * around to NM_CLICK, and on the first click after the window opens that
 * swallowed the notification entirely -- the click appeared to do nothing.
 * Nothing of the list view's own click behaviour is wanted (its row
 * selection is vetoed anyway), so the buttons are handled here outright. */
static LRESULT CALLBACK swapp_table_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                                  UINT_PTR id, DWORD_PTR data) {
    (void)id;
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDOWN) {
        SetFocus(hwnd);
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
        swapp_table_cell_clicked((HWND)data, pt, msg == WM_RBUTTONDOWN);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK swapp_main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == SWAPP_MAIN_SPIN_TIMER) {
                g_spinner_angle += 15.0f;
                if (g_spinner_angle >= 360.0f) {
                    g_spinner_angle -= 360.0f;
                }
                swapp_main_refresh_status();
            }
            return 0;

        case WM_NOTIFY: {
            NMHDR *hdr = (NMHDR *)lp;
            if (hdr->hwndFrom != g_status_hwnd) {
                break;
            }
            if (hdr->code == LVN_ITEMCHANGING) {
                /* Veto the list view's own whole-row selection; cells are
                 * selected by swapp_table_subclass_proc instead. */
                NMLISTVIEW *change = (NMLISTVIEW *)lp;
                return (change->uChanged & LVIF_STATE)
                       && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED);
            }
            if (hdr->code != NM_CUSTOMDRAW) {
                break;
            }
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            switch (cd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT:
                    return CDRF_NOTIFYSUBITEMDRAW;
                case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                    swapp_table_draw_input(cd);
                    return CDRF_SKIPDEFAULT;
                default:
                    return CDRF_DODEFAULT;
            }
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lp;
            if (item->CtlID == SWAPP_MAIN_REFRESH_ID || item->CtlID == SWAPP_MAIN_WINDOWS_ALL_ID
                || item->CtlID == SWAPP_MAIN_LINUX_ALL_ID) {
                swapp_refresh_draw(item);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == SWAPP_MAIN_REFRESH_ID && HIWORD(wp) == BN_CLICKED) {
                swapp_main_rescan();
                return 0;
            }
            if (LOWORD(wp) == SWAPP_MAIN_WINDOWS_ALL_ID && HIWORD(wp) == BN_CLICKED) {
                swapp_main_switch_all(SWAPP_ROLE_WINDOWS);
                return 0;
            }
            if (LOWORD(wp) == SWAPP_MAIN_LINUX_ALL_ID && HIWORD(wp) == BN_CLICKED) {
                swapp_main_switch_all(SWAPP_ROLE_LINUX);
                return 0;
            }
            break;

        case WM_CLOSE:
            /* Hide, never destroy: the app lives in the tray and the link
             * has to keep running with no window open. */
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void swapp_show_main_window(void) {
    if (!g_main_hwnd) {
        g_main_hwnd = CreateWindowExA(0, SWAPP_MAIN_CLASS, "Swapp", SWAPP_MAIN_STYLE,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                                      NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (!g_main_hwnd) {
            return;
        }

        /* The window's own DPI, not the primary monitor's: on a mixed-DPI
         * desktop the two differ, and every metric below is derived from
         * this one value. */
        HMODULE user32 = GetModuleHandleA("user32.dll");
        UINT (WINAPI *get_dpi_for_window)(HWND) =
            (UINT (WINAPI *)(HWND))GetProcAddress(user32, "GetDpiForWindow");
        if (get_dpi_for_window) {
            UINT dpi = get_dpi_for_window(g_main_hwnd);
            if (dpi > 0) {
                g_dpi = (int)dpi;
            }
        }
        /* The font is sized in DPI units too, so it can only be built once
         * the DPI is known. */
        swapp_font_load();

        int margin = swapp_dpi_scale(SWAPP_MAIN_MARGIN);
        int row_height = swapp_dpi_scale(SWAPP_ROW_HEIGHT);
        int client_width = swapp_dpi_scale(SWAPP_MAIN_WIDTH);
        int inner_width = client_width - 2 * margin;

        INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&icc);
        g_status_hwnd = CreateWindowExA(0, WC_LISTVIEWA, "",
                                        WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT
                                            | LVS_SINGLESEL | LVS_NOSORTHEADER,
                                        margin, margin, inner_width, 100,
                                        g_main_hwnd, (HMENU)(INT_PTR)SWAPP_MAIN_STATUS_ID,
                                        GetModuleHandleA(NULL), NULL);
        /* Explorer's theme gives the modern header and hover rows instead of
         * the classic grey look. */
        SetWindowTheme(g_status_hwnd, L"Explorer", NULL);
        SetWindowSubclass(g_status_hwnd, swapp_table_subclass_proc, 1, (DWORD_PTR)g_main_hwnd);
        ListView_SetExtendedListViewStyle(g_status_hwnd,
                                          LVS_EX_DOUBLEBUFFER);
        SendMessageA(g_status_hwnd, WM_SETFONT,
                     (WPARAM)(g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        if (g_font_bold) {
            SendMessageA(ListView_GetHeader(g_status_hwnd), WM_SETFONT, (WPARAM)g_font_bold, TRUE);
        }

        /* A list view has no row-height setting; its rows are as tall as
         * the font or the small image list, whichever is taller. An empty
         * 1px-wide image list of the wanted height is the standard way to
         * get vertical padding. */
        HIMAGELIST spacer = ImageList_Create(1, swapp_dpi_scale(SWAPP_TABLE_ROW_HEIGHT),
                                             ILC_COLOR32, 0, 0);
        ListView_SetImageList(g_status_hwnd, spacer, LVSIL_SMALL);

        /* Only the Monitor column exists up front; the "Input N" columns are
         * added by swapp_table_set once it knows how many inputs there are. */
        LVCOLUMNA column = {0};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = "Monitor";
        column.cx = swapp_dpi_scale(SWAPP_TABLE_MONITOR_WIDTH);
        ListView_InsertColumn(g_status_hwnd, 0, &column);

        /* Sized to the header plus exactly SWAPP_TABLE_ROWS rows: the setup
         * has two monitors, so any extra height is just empty space. */
        DWORD view = ListView_ApproximateViewRect(g_status_hwnd, -1, -1, SWAPP_TABLE_ROWS);
        int table_height = HIWORD(view) + swapp_dpi_scale(4);
        SetWindowPos(g_status_hwnd, NULL, 0, 0, inner_width, table_height,
                     SWP_NOMOVE | SWP_NOZORDER);

        int rows_top = margin + table_height + margin;
        /* The status rows give up their right end to the big buttons, each
         * as tall as both rows together: all-to-Windows, all-to-Linux, then
         * refresh at the far right. */
        int button_width = swapp_dpi_scale(SWAPP_REFRESH_WIDTH);
        int button_gap = swapp_dpi_scale(8);
        int buttons_left = client_width - margin - 3 * button_width - 2 * button_gap;
        int rows_width = buttons_left - margin;
        swapp_row_create(&g_client_row, g_main_hwnd, rows_top, rows_width);
        swapp_row_create(&g_monitors_row, g_main_hwnd, rows_top + row_height, rows_width);

        static const struct {
            const char *text;
            int id;
        } buttons[] = {
            {"Switch all to Windows", SWAPP_MAIN_WINDOWS_ALL_ID},
            {"Switch all to Linux", SWAPP_MAIN_LINUX_ALL_ID},
            {"Rescan monitors", SWAPP_MAIN_REFRESH_ID},
        };
        for (int b = 0; b < 3; b++) {
            HWND button = CreateWindowExA(0, "BUTTON", buttons[b].text,
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                          buttons_left + b * (button_width + button_gap), rows_top,
                                          button_width, 2 * row_height, g_main_hwnd,
                                          (HMENU)(INT_PTR)buttons[b].id,
                                          GetModuleHandleA(NULL), NULL);
            if (buttons[b].id == SWAPP_MAIN_WINDOWS_ALL_ID) {
                g_windows_all_hwnd = button;
            } else if (buttons[b].id == SWAPP_MAIN_LINUX_ALL_ID) {
                g_linux_all_hwnd = button;
            } else {
                g_refresh_hwnd = button;
            }
        }

        /* The window is sized from its content rather than the other way
         * round. */
        RECT frame = {0, 0, client_width, rows_top + 2 * row_height + margin};
        AdjustWindowRectEx(&frame, SWAPP_MAIN_STYLE, FALSE, 0);
        SetWindowPos(g_main_hwnd, NULL, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOMOVE | SWP_NOZORDER);
    }

    swapp_main_refresh_status();
    swapp_main_update_spinner();
    ShowWindow(g_main_hwnd, IsIconic(g_main_hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_main_hwnd);
}

#define SWAPP_MAX_SCREENS 16

typedef struct {
    HMONITOR handles[SWAPP_MAX_SCREENS];
    int count;
} swapp_screen_list;

static BOOL CALLBACK swapp_screen_collect(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM param) {
    (void)dc;
    (void)rect;
    swapp_screen_list *list = (swapp_screen_list *)param;
    if (list->count < SWAPP_MAX_SCREENS) {
        list->handles[list->count++] = monitor;
    }
    return TRUE;
}

/* Centres the window on the screen after the one it is on, wrapping at the
 * end. The screen the window opened on can be the one currently handed to
 * the other machine, which leaves the window invisible with no way to reach
 * it; pressing the open hotkey again walks it to the next screen until it
 * lands on one this machine is actually showing. */
static void swapp_main_next_screen(void) {
    if (!g_main_hwnd) {
        return;
    }

    swapp_screen_list list = {0};
    EnumDisplayMonitors(NULL, NULL, swapp_screen_collect, (LPARAM)&list);
    if (list.count < 2) {
        return;
    }

    HMONITOR current = MonitorFromWindow(g_main_hwnd, MONITOR_DEFAULTTONEAREST);
    int index = 0;
    for (int i = 0; i < list.count; i++) {
        if (list.handles[i] == current) {
            index = i;
            break;
        }
    }

    MONITORINFO info = {0};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(list.handles[(index + 1) % list.count], &info)) {
        return;
    }

    RECT frame;
    if (!GetWindowRect(g_main_hwnd, &frame)) {
        return;
    }
    int width = frame.right - frame.left;
    int height = frame.bottom - frame.top;
    /* The work area, not the full screen, so the window doesn't end up
     * under the taskbar. */
    int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
    int y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2;
    SetWindowPos(g_main_hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static void swapp_tray_show_menu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, SWAPP_ID_OPEN, "Open");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    /* The main window's switch-all buttons, under the same guard. */
    AppendMenuA(menu, MF_STRING | (swapp_main_can_switch_all(SWAPP_ROLE_WINDOWS) ? 0 : MF_GRAYED),
                SWAPP_ID_WINDOWS_ALL, "Switch to Windows");
    AppendMenuA(menu, MF_STRING | (swapp_main_can_switch_all(SWAPP_ROLE_LINUX) ? 0 : MF_GRAYED), SWAPP_ID_LINUX_ALL,
                "Switch to Linux");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, SWAPP_ID_QUIT, "Quit");

    POINT pt;
    GetCursorPos(&pt);

    /* Required so the menu closes correctly when the user clicks elsewhere. */
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageA(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

static LRESULT CALLBACK swapp_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case SWAPP_TRAY_MSG:
            if (lp == WM_RBUTTONUP) {
                swapp_tray_show_menu(hwnd);
            } else if (lp == WM_LBUTTONUP) {
                swapp_show_main_window();
            }
            return 0;
        case WM_HOTKEY:
            /* Ignored outright while a job is querying the monitors --
             * same rule as the buttons, which aren't on screen just then. */
            /* F16 on this machine takes the monitors for this machine:
             * exactly the Windows button, guarded by the same condition
             * (checked inside). Whichever machine the keyboard is plugged
             * into, F16 brings both screens to it. */
            if (wp == SWAPP_HOTKEY_F16) {
                /* A double tap, not a single press: the first arms, and
                 * only a second within SWAPP_F16_DOUBLE_TAP_MS acts, so a
                 * stray press can't yank both screens away. Registered
                 * with MOD_NOREPEAT, so holding the key doesn't count. */
                static ULONGLONG armed_at = 0;
                ULONGLONG now = GetTickCount64();
                if (armed_at != 0 && now - armed_at <= SWAPP_F16_DOUBLE_TAP_MS) {
                    armed_at = 0;
                    swapp_main_switch_all(SWAPP_ROLE_WINDOWS);
                } else {
                    armed_at = now;
                }
            }
            /* F15 opens the window: a single press, unlike F16. Showing a
             * window takes nothing away, so there is nothing for a second
             * tap to guard against. Pressed again with the window already
             * up, it moves the window on to the next screen instead. */
            if (wp == SWAPP_HOTKEY_F15) {
                BOOL was_open = g_main_hwnd && IsWindowVisible(g_main_hwnd) && !IsIconic(g_main_hwnd);
                swapp_show_main_window();
                if (was_open) {
                    swapp_main_next_screen();
                }
            }
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
                /* Restarts the timer if it's already running, so a burst of
                 * events from one physical plug/unplug collapses into a
                 * single rescan a few seconds after the last of them. */
                SetTimer(hwnd, SWAPP_RESCAN_TIMER, SWAPP_RESCAN_SETTLE_MS, NULL);
            }
            return TRUE;
        case WM_TIMER:
            if (wp == SWAPP_RESCAN_TIMER) {
                KillTimer(hwnd, SWAPP_RESCAN_TIMER);
                /* Dropped if a job is already running -- that job ends in a
                 * fresh enumeration of its own anyway. */
                swapp_monitors_rescan_async();
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == SWAPP_ID_OPEN) {
                swapp_show_main_window();
            }
            if (LOWORD(wp) == SWAPP_ID_WINDOWS_ALL) {
                swapp_main_switch_all(SWAPP_ROLE_WINDOWS);
            }
            if (LOWORD(wp) == SWAPP_ID_LINUX_ALL) {
                swapp_main_switch_all(SWAPP_ROLE_LINUX);
            }
            if (LOWORD(wp) == SWAPP_ID_QUIT) {
                /* Quit has to work mid-job. The message loop isn't blocked
                 * -- the job is on a worker thread -- but cancelling stops
                 * it at its next checkpoint so it can't go on driving
                 * monitors after the app is gone. */
                swapp_monitors_cancel();
                PostQuitMessage(0);
            }
            return 0;
        case SWAPP_NET_RESCAN_MSG:
            swapp_main_rescan();
            return 0;
        case SWAPP_NET_SWITCH_MSG:
            swapp_main_switch((int)wp, (int)lp);
            return 0;
        case SWAPP_NET_SWITCH_ALL_MSG:
            swapp_main_switch_all((swapp_input_role)wp);
            return 0;
        case SWAPP_NET_ASSIGN_MSG:
            swapp_main_assign((int)(wp >> 8), (int)lp, (swapp_input_role)(wp & 0xFF));
            return 0;
        case SWAPP_NET_STATE_MSG:
            /* The link no longer starts or stops the scan: the monitors are
             * the server's, it keeps them cached whether or not anyone is
             * connected, and it scans them on its own schedule. A client
             * arriving is served whatever is there at the time (net_win.c
             * sends the list, or `scanning`, as it takes the connection),
             * and a client leaving takes nothing with it. All that changes
             * here is the link row -- and the Linux button, which needs the
             * client that just went away. */
            swapp_main_refresh_status();
            swapp_main_update_spinner();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

void swapp_tray_run(const char *tooltip) {
    /* Per-monitor DPI awareness, set in code rather than through a manifest
     * so the plain executable this project builds gets it too. Without it
     * Windows bitmap-stretches the window on the 200% monitors this runs
     * on, and the icons come out blurred. Resolved dynamically: the call
     * only exists on Windows 10 1703 and newer. */
    HMODULE user32 = GetModuleHandleA("user32.dll");
    BOOL (WINAPI *set_dpi_context)(HANDLE) =
        (BOOL (WINAPI *)(HANDLE))GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (set_dpi_context) {
        /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        set_dpi_context((HANDLE)-4);
    }

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = swapp_wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "SwappTrayWindow";
    RegisterClassA(&wc);

    WNDCLASSA list_wc = {0};
    list_wc.lpfnWndProc = swapp_list_wnd_proc;
    list_wc.hInstance = wc.hInstance;
    list_wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    list_wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    list_wc.lpszClassName = SWAPP_LIST_CLASS;
    RegisterClassA(&list_wc);

    WNDCLASSA main_wc = {0};
    main_wc.lpfnWndProc = swapp_main_wnd_proc;
    main_wc.hInstance = wc.hInstance;
    main_wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    main_wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    main_wc.lpszClassName = SWAPP_MAIN_CLASS;
    RegisterClassA(&main_wc);

    WNDCLASSA icon_wc = {0};
    icon_wc.lpfnWndProc = swapp_icon_wnd_proc;
    icon_wc.hInstance = wc.hInstance;
    icon_wc.lpszClassName = SWAPP_ICON_CLASS;
    RegisterClassA(&icon_wc);

    swapp_icons_load();
    /* Startup is intentionally inert for now: no monitor window, no
     * enumeration, no device-change watch. Kept commented
     * out verbatim so the wiring can be restored as-is later.
    swapp_monitors_set_job_callback(swapp_list_job_done);
    */

    HWND hwnd = CreateWindowA(wc.lpszClassName, "Swapp", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, wc.hInstance, NULL);

    /* The tray icon and the window go up immediately and the enumeration
     * runs behind them, rather than the app being invisible for however
     * long DDC/CI probing takes. The job is started first so the window
     * opens already showing its spinner instead of a set of live-looking
     * buttons backed by an empty cache. Until it lands F16 does nothing. */
    /*
    swapp_monitors_rescan_async();
    swapp_show_monitor_list();

    DEV_BROADCAST_DEVICEINTERFACE_A filter = {0};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = SWAPP_GUID_DEVINTERFACE_MONITOR;
    g_dev_notify = RegisterDeviceNotificationA(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    */

    RegisterHotKey(hwnd, SWAPP_HOTKEY_F16, MOD_NOREPEAT, VK_F16);
    RegisterHotKey(hwnd, SWAPP_HOTKEY_F15, MOD_NOREPEAT, VK_F15);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = SWAPP_TRAY_UID;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = SWAPP_TRAY_MSG;
    g_nid.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    strncpy(g_nid.szTip, tooltip, sizeof(g_nid.szTip) - 1);

    Shell_NotifyIconA(NIM_ADD, &g_nid);

    /* Start listening, then let the tooltip track whatever the link is
     * doing. */
    g_tray_hwnd = hwnd;
    swapp_net_set_state_callback(swapp_net_state_changed);
    swapp_net_set_rescan_callback(swapp_net_rescan_requested);
    swapp_net_set_switch_callback(swapp_net_switch_requested);
    swapp_net_set_assign_callback(swapp_net_assign_requested);
    swapp_net_set_switch_all_callback(swapp_net_switch_all_requested);
    /* Starts in the tray: the window only opens from the tray icon. */
    swapp_net_start();

    /* The server owns the monitors, so it scans them at startup rather than
     * when a client turns up: probing capabilities over DDC/CI takes
     * seconds, and paying that once at launch means the first client (and
     * the first F16) finds a list already there. */
    swapp_monitors_set_job_callback(swapp_main_job_done);
    swapp_monitors_rescan_async();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    swapp_net_stop();

    if (g_dev_notify) {
        UnregisterDeviceNotification(g_dev_notify);
    }
    UnregisterHotKey(hwnd, SWAPP_HOTKEY_F15);
    UnregisterHotKey(hwnd, SWAPP_HOTKEY_F16);
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
    if (g_list_hwnd) {
        DestroyWindow(g_list_hwnd);
    }
    DestroyWindow(hwnd);
    if (g_main_hwnd) {
        DestroyWindow(g_main_hwnd);
    }
    UnregisterClassA(SWAPP_ICON_CLASS, wc.hInstance);
    UnregisterClassA(SWAPP_MAIN_CLASS, wc.hInstance);
    UnregisterClassA(SWAPP_LIST_CLASS, wc.hInstance);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
}
