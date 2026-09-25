#include "tray.h"
#include "monitors.h"
#include "net.h"
#include "assets.h"
#include "update.h"
#include "autostart.h"

/* GtkStatusIcon (the older approach) speaks only the legacy XEmbed tray
 * protocol, which modern GNOME/Ubuntu no longer implements at all -- it
 * only understands StatusNotifierItem over D-Bus, via the AppIndicator
 * extension. Using GtkStatusIcon there doesn't just look different, it's
 * silently non-functional and throws internal GTK assertions. AppIndicator
 * is what actually shows up in the tray on modern desktops. */
#ifdef SWAPP_USE_AYATANA_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#else
#include <libappindicator/app-indicator.h>
#endif

#include <libnotify/notify.h>

/* For loading the bundled Inter face into this process only. */
#include <fontconfig/fontconfig.h>

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

/* The monitor list window: one row per cached monitor, a label then a
 * button per supported input. The active input's button is insensitive;
 * clicking any other switches the monitor to it. Created once and hidden
 * (not destroyed) on close, so reopening rebuilds the rows and presents it. */
#define SWAPP_LIST_MAX_INPUTS 32
#define SWAPP_LIST_ACTION_W   110

static GtkWidget *g_list_window = NULL;
static GtkWidget *g_list_rows = NULL;

/* A first enumeration can take a minute on a slow DDC link, so the busy
 * caption counts seconds -- an unchanging spinner for that long reads as a
 * hang, which is exactly the complaint this came from. */
static GtkWidget *g_busy_label = NULL;
static guint g_busy_timer = 0;
static gint64 g_busy_started_us = 0;

static void swapp_busy_set_text(void) {
    if (!g_busy_label) {
        return;
    }
    char text[64];
    g_snprintf(text, sizeof(text), "Querying monitors... %ds",
               (int)((g_get_monotonic_time() - g_busy_started_us) / G_USEC_PER_SEC));
    gtk_label_set_text(GTK_LABEL(g_busy_label), text);
}

static gboolean swapp_busy_tick(gpointer user_data) {
    (void)user_data;
    if (!swapp_monitors_busy() || !g_busy_label) {
        g_busy_timer = 0;
        return G_SOURCE_REMOVE;
    }
    swapp_busy_set_text();
    return G_SOURCE_CONTINUE;
}

static void swapp_list_rebuild(void);
static void swapp_show_monitor_list(void);

static gboolean swapp_list_rebuild_idle(gpointer user_data) {
    (void)user_data;
    swapp_list_rebuild();
    return G_SOURCE_REMOVE;
}

/* Both actions start a worker-thread job and return at once. The rebuild
 * that swaps the rows for the spinner destroys the button that was just
 * clicked, so it is deferred out of that button's own handler; the job
 * callback rebuilds again when the results land. */
static void swapp_list_on_refresh(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    swapp_monitors_rescan_async();
    g_idle_add(swapp_list_rebuild_idle, NULL);
}

static void swapp_list_on_switch(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    swapp_monitors_trigger_async(SWAPP_ROLE_SELF);
    g_idle_add(swapp_list_rebuild_idle, NULL);
}

static void swapp_list_on_role(GtkComboBox *combo, gpointer user_data) {
    guint packed = GPOINTER_TO_UINT(user_data);
    size_t index = packed >> 8;
    int code = (int)(packed & 0xFF);
    gint sel = gtk_combo_box_get_active(combo);
    if (sel < 0 || sel > SWAPP_ROLE_WINDOWS) {
        return;
    }

    char label[96];
    swapp_monitors_label(index, label, sizeof(label));
    swapp_roles_set(label, code, (swapp_input_role)sel);
    /* Another input of this monitor may have just lost the role; rebuilding
     * destroys this combo, so defer it out of its own handler. */
    g_idle_add(swapp_list_rebuild_idle, NULL);
}

static void swapp_list_on_button(GtkButton *button, gpointer user_data) {
    (void)button;
    guint packed = GPOINTER_TO_UINT(user_data);
    size_t index = packed >> 8;
    int code = (int)(packed & 0xFF);

    if (!swapp_monitors_set_input(index, code)) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_list_window), GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                                                   "Could not send the input switch command to the monitor.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }
    /* Rebuilding destroys this button, so defer it out of its own handler. */
    g_idle_add(swapp_list_rebuild_idle, NULL);
}

static void swapp_list_rebuild(void) {
    if (!g_list_rows) {
        return;
    }
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_list_rows));
    for (GList *l = children; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    /* A job owns the cache while it runs, so there is nothing safe to
     * render from it -- and nothing the user should be clicking either.
     * The spinner replaces the rows rather than just greying them out,
     * which is what keeps the cache single-threaded. */
    if (swapp_monitors_busy()) {
        GtkWidget *busy = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *spinner = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_box_pack_start(GTK_BOX(busy), spinner, FALSE, FALSE, 0);

        g_busy_label = gtk_label_new(NULL);
        gtk_box_pack_start(GTK_BOX(busy), g_busy_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(g_list_rows), busy, FALSE, FALSE, 0);

        /* A rebuild mid-job (the window being reopened, say) must not
         * restart the count -- only a fresh busy period does. */
        if (g_busy_timer == 0) {
            g_busy_started_us = g_get_monotonic_time();
            g_busy_timer = g_timeout_add(500, swapp_busy_tick, NULL);
        }
        swapp_busy_set_text();

        gtk_widget_show_all(g_list_rows);
        return;
    }
    g_busy_label = NULL;

    size_t n = swapp_monitors_count();
    if (n == 0) {
        gtk_box_pack_start(GTK_BOX(g_list_rows), gtk_label_new("No DDC/CI monitors detected"), FALSE, FALSE, 0);
    }

    for (size_t i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        char label[96];
        swapp_monitors_label(i, label, sizeof(label));
        GtkWidget *name = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
        gtk_label_set_width_chars(GTK_LABEL(name), 22);
        gtk_box_pack_start(GTK_BOX(row), name, FALSE, FALSE, 0);

        int codes[SWAPP_LIST_MAX_INPUTS];
        int n_inputs = swapp_monitors_inputs(i, codes, SWAPP_LIST_MAX_INPUTS);
        int active = swapp_monitors_active_input(i);
        if (n_inputs == 0) {
            gtk_box_pack_start(GTK_BOX(row), gtk_label_new("(inputs unknown)"), FALSE, FALSE, 0);
        }
        for (int j = 0; j < n_inputs; j++) {
            GtkWidget *btn = gtk_button_new_with_label(swapp_monitors_input_name(codes[j]));
            gtk_widget_set_sensitive(btn, codes[j] != active);
            g_signal_connect(btn, "clicked", G_CALLBACK(swapp_list_on_button),
                             GUINT_TO_POINTER(((guint)i << 8) | (guint)(codes[j] & 0xFF)));
            gtk_box_pack_start(GTK_BOX(row), btn, FALSE, FALSE, 0);

            GtkWidget *combo = gtk_combo_box_text_new();
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "None");
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Linux");
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Windows");
            /* Set before connecting, so filling in the saved role doesn't
             * count as the user changing it. */
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), (gint)swapp_roles_get(label, codes[j]));
            g_signal_connect(combo, "changed", G_CALLBACK(swapp_list_on_role),
                             GUINT_TO_POINTER(((guint)i << 8) | (guint)(codes[j] & 0xFF)));
            gtk_widget_set_margin_end(combo, 12);
            gtk_box_pack_start(GTK_BOX(row), combo, FALSE, FALSE, 0);
        }

        gtk_box_pack_start(GTK_BOX(g_list_rows), row, FALSE, FALSE, 0);
    }

    /* Takes the screens over for this machine: switches every monitor to
     * the input assigned this OS. Needs each monitor to have one, and is
     * pointless when they are all already showing it. */
    GtkWidget *switch_btn = gtk_button_new_with_label("Trigger");
    gtk_widget_set_sensitive(switch_btn, swapp_roles_trigger_available(SWAPP_ROLE_SELF));
    gtk_widget_set_size_request(switch_btn, SWAPP_LIST_ACTION_W, -1);
    g_signal_connect(switch_btn, "clicked", G_CALLBACK(swapp_list_on_switch), NULL);

    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    gtk_widget_set_size_request(refresh_btn, SWAPP_LIST_ACTION_W, -1);
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(swapp_list_on_refresh), NULL);

    /* Trigger on the left, Refresh against the right edge, both the same
     * width so they read as a matched pair rather than two odd shapes. */
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(actions, 6);
    gtk_box_pack_start(GTK_BOX(actions), switch_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(actions), refresh_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_list_rows), actions, FALSE, FALSE, 0);

    gtk_widget_show_all(g_list_rows);
}

/* Runs on the UI thread once a job's results are in the cache. */
static void swapp_list_job_done(int trigger_failed) {
    swapp_list_rebuild();
    if (trigger_failed) {
        swapp_show_monitor_list();
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_list_window), GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                                                   "Could not send the input switch command to every monitor.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }
}

static void swapp_show_monitor_list(void) {
    if (!g_list_window) {
        g_list_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(g_list_window), "Swapp - Monitors");
        gtk_window_set_resizable(GTK_WINDOW(g_list_window), FALSE);
        g_signal_connect(g_list_window, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);

        g_list_rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        g_object_set(g_list_rows, "margin", 12, NULL);
        gtk_container_add(GTK_CONTAINER(g_list_window), g_list_rows);
    }

    /* The other machine switches these same monitors without this one
     * hearing about it, so what the cache last saw can be stale. */
    swapp_monitors_refresh_active_inputs();
    swapp_list_rebuild();
    gtk_widget_show_all(g_list_window);
    gtk_window_present(GTK_WINDOW(g_list_window));
}

/* Runs on GTK's own main-thread idle queue, since GTK/GObject calls aren't
 * safe to make from the hotkey's own X11 thread. */
static void swapp_client_switch_all(int role);
static void swapp_show_main_window(void);

#define SWAPP_F16_DOUBLE_TAP_MS 1000

static gboolean swapp_hotkey_fired(gpointer user_data) {
    (void)user_data;
    /* F16 on this machine takes the monitors for this machine: exactly the
     * Linux button, under the same condition. Whichever machine the
     * keyboard is plugged into, F16 brings both screens to it.
     *
     * A double tap, not a single press: the first arms, and only a second
     * within SWAPP_F16_DOUBLE_TAP_MS acts, so a stray press can't yank both
     * screens away. Key repeat never gets here (see swapp_hotkey_thread). */
    static gint64 armed_at = 0;
    gint64 now = g_get_monotonic_time();
    if (armed_at != 0 && now - armed_at <= SWAPP_F16_DOUBLE_TAP_MS * 1000) {
        armed_at = 0;
        swapp_client_switch_all(SWAPP_ROLE_LINUX);
    } else {
        armed_at = now;
    }
    return G_SOURCE_REMOVE;
}

static void swapp_main_open_or_next_screen(void);

/* F15 opens the window: a single press, unlike F16. Showing a window takes
 * nothing away, so there is nothing for a second tap to guard against. */
static gboolean swapp_hotkey_open_fired(gpointer user_data) {
    (void)user_data;
    swapp_main_open_or_next_screen();
    return G_SOURCE_REMOVE;
}

/* Runs on its own X11 connection so it can block in XNextEvent without
 * stalling GTK's main loop. Grabs F16 across every combination of the lock
 * modifiers (NumLock, CapsLock) since XGrabKey requires an exact
 * modifier-state match and either can be toggled on when the key is
 * pressed. Global key grabs are an X11-only mechanism -- under a pure
 * Wayland session (no XWayland) this finds no display and the hotkey just
 * never fires; there is no portable Wayland equivalent. */
#define SWAPP_F16_KEYCODE 194  /* evdev KEY_F16 (186) + the X11 offset of 8 */
#define SWAPP_F15_KEYCODE 193  /* evdev KEY_F15 (185) + the X11 offset of 8 */

static gpointer swapp_hotkey_thread(gpointer user_data) {
    (void)user_data;

    Display *display = XOpenDisplay(NULL);
    if (!display) {
        g_warning("swapp: no X11 display available; F16/F15 global hotkeys disabled");
        return NULL;
    }

    /* Most layouts stop at F12, leaving XK_F16 unmapped and this lookup
     * returning 0 even though the hardware can still send the key. The
     * kernel's KEY_F16 (186) always reaches X as keycode 186 + 8, so grab
     * that scancode directly rather than giving up on the hotkey. */
    KeyCode keycode = XKeysymToKeycode(display, XK_F16);
    if (keycode == 0) {
        keycode = SWAPP_F16_KEYCODE;
    }
    KeyCode open_keycode = XKeysymToKeycode(display, XK_F15);
    if (open_keycode == 0) {
        open_keycode = SWAPP_F15_KEYCODE;
    }

    static const unsigned int lock_masks[] = {0, Mod2Mask, LockMask, Mod2Mask | LockMask};
    int n_screens = ScreenCount(display);
    for (int s = 0; s < n_screens; s++) {
        Window root = RootWindow(display, s);
        for (size_t i = 0; i < G_N_ELEMENTS(lock_masks); i++) {
            XGrabKey(display, keycode, lock_masks[i], root, False, GrabModeAsync, GrabModeAsync);
            if (open_keycode != keycode) {
                XGrabKey(display, open_keycode, lock_masks[i], root, False, GrabModeAsync, GrabModeAsync);
            }
        }
    }
    XSync(display, False);

    /* Lives for the whole process: the app exits via gtk_main_quit from the
     * tray menu, which ends this thread along with everything else. */
    /* Holding the key must not count as a double tap. With detectable
     * auto-repeat X sends a held key as repeated KeyPress with no KeyRelease
     * in between, so only a press after a release is a real press. */
    XkbSetDetectableAutoRepeat(display, True, NULL);
    int down = 0;
    int open_down = 0;
    for (;;) {
        XEvent event;
        XNextEvent(display, &event); /* blocks until a grabbed key is pressed */
        if (event.type != KeyPress && event.type != KeyRelease) {
            continue;
        }
        KeyCode pressed = event.xkey.keycode;
        int *state = pressed == keycode ? &down : (pressed == open_keycode ? &open_down : NULL);
        if (!state) {
            continue;
        }
        if (event.type == KeyPress) {
            if (!*state) {
                *state = 1;
                g_idle_add(pressed == keycode ? swapp_hotkey_fired : swapp_hotkey_open_fired, NULL);
            }
        } else {
            *state = 0;
        }
    }
    return NULL;
}

static void swapp_hotkey_start(void) {
    g_thread_new("swapp-hotkey", swapp_hotkey_thread, NULL);
}

#ifdef SWAPP_USE_AYATANA_APPINDICATOR
/* libayatana-appindicator logs a deprecation warning on every startup, urging
 * a move to libayatana-appindicator-glib. That successor drops GtkMenu in
 * favour of libdbusmenu, so it isn't a drop-in swap, and it isn't packaged on
 * the distros we target yet. Swallow just that one message; anything else from
 * the library still reaches the default handler. */
static void swapp_tray_log_handler(const gchar *domain, GLogLevelFlags level,
                                   const gchar *message, gpointer user_data) {
    if (message && strstr(message, "is deprecated") != NULL) {
        return;
    }
    g_log_default_handler(domain, level, message, user_data);
}
#endif

static void swapp_tray_on_show_monitors(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    swapp_show_monitor_list();
}

/* The main window. The client owns no monitor state of its own, so for now
 * the link status is all it has to show; the monitor rows move in here once
 * the server serves them. */
static GtkWidget *g_main_window = NULL;
static GtkWidget *g_status_label = NULL;
static GtkWidget *g_status_icon = NULL;
static GtkWidget *g_monitors_label = NULL;
static GtkWidget *g_monitors_icon = NULL;
static GtkListStore *g_monitor_store = NULL;

/* The window is one line per thing that has to be true before Swapp can do
 * anything: a connected server, and a monitor list received from it. Each
 * line spins while pending and turns into a checkmark when done, so it says
 * at a glance which half is holding things up. The client owns no monitor
 * state of its own -- the enumeration happens on the server, over its
 * DDC/CI link -- so the second line waits on the wire, not on hardware. */
/* One status row: an icon plus a line of text. The icon is a drawing area
 * rather than a GtkImage because the spinner is a still image the app spins
 * itself -- Material ships no animation, and a rotation drawn with cairo
 * looks better than the four 90-degree steps a pixbuf can manage. */
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

#define SWAPP_ICON_SIZE 20

static GdkPixbuf *g_icons[SWAPP_ICON_COUNT] = {NULL};

/* Per icon, in swapp_icon order: pending blue, done green, failed red,
 * refresh dark gray, inactive-input light gray, Windows blue, Linux near-
 * black. Kept in step with the same table in tray_win.c. */
static const double g_icon_colors[SWAPP_ICON_COUNT][3] = {
    {0x1a / 255.0, 0x73 / 255.0, 0xe8 / 255.0},
    {0x1e / 255.0, 0x8e / 255.0, 0x3e / 255.0},
    {0xd9 / 255.0, 0x30 / 255.0, 0x25 / 255.0},
    {0x44 / 255.0, 0x44 / 255.0, 0x44 / 255.0},
    {0xc8 / 255.0, 0xc8 / 255.0, 0xc8 / 255.0},
    {0x00 / 255.0, 0x78 / 255.0, 0xd4 / 255.0},
    {0x33 / 255.0, 0x33 / 255.0, 0x33 / 255.0},
};
static double g_spinner_angle = 0.0;
static guint g_spinner_source = 0;

/* Loads the bundled Inter face for this process only, then makes it the
 * app's font. Deliberately not installed system-wide: the app should look
 * the same on a machine where the user has never heard of Inter, without
 * touching anything outside its own cache directory. */
/* Some consumers only take files (fontconfig, the AppIndicator icon), so an
 * embedded asset is written to ~/.cache/swapp/<subdir>/<filename> once and
 * reused, rewritten only when the bundled one changed. Returns the path
 * (g_free it), or NULL. */
static char *swapp_asset_to_cache(const char *name, const char *subdir, const char *filename) {
    const swapp_asset *asset = swapp_asset_find(name);
    if (!asset) {
        return NULL;
    }
    char *dir = g_build_filename(g_get_user_cache_dir(), "swapp", subdir, NULL);
    char *path = g_build_filename(dir, filename, NULL);

    gchar *existing = NULL;
    gsize existing_size = 0;
    int current = g_file_get_contents(path, &existing, &existing_size, NULL)
                  && existing_size == asset->size
                  && memcmp(existing, asset->data, asset->size) == 0;
    g_free(existing);

    if (!current && (g_mkdir_with_parents(dir, 0700) != 0
                     || !g_file_set_contents(path, (const gchar *)asset->data,
                                             (gssize)asset->size, NULL))) {
        g_clear_pointer(&path, g_free);
    }
    g_free(dir);
    return path;
}

static int swapp_font_add(const char *name) {
    char *base = g_path_get_basename(name);
    char *path = swapp_asset_to_cache(name, "fonts", base);
    int ok = path && FcConfigAppFontAddFile(NULL, (const FcChar8 *)path);
    g_free(path);
    g_free(base);
    return ok;
}

/* Decodes an embedded PNG, scaled to fit width x height. */
static GdkPixbuf *swapp_pixbuf_from_asset(const char *name, int width, int height) {
    const swapp_asset *asset = swapp_asset_find(name);
    if (!asset) {
        return NULL;
    }
    GInputStream *stream = g_memory_input_stream_new_from_data(asset->data, (gssize)asset->size,
                                                               NULL);
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream_at_scale(stream, width, height, TRUE, NULL,
                                                            NULL);
    g_object_unref(stream);
    return pixbuf;
}

static void swapp_font_load(void) {
    static const char *const faces[] = {"fonts/Inter-Regular.ttf", "fonts/Inter-SemiBold.ttf"};
    int loaded = 0;
    for (guint i = 0; i < G_N_ELEMENTS(faces); i++) {
        if (swapp_font_add(faces[i])) {
            loaded = 1;
        }
    }
    if (!loaded) {
        return; /* fall back to whatever the desktop theme picks */
    }

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, "* { font-family: 'Inter'; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* Desktops scale in two independent ways: GDK's integer scale factor
 * (GDK_SCALE, per monitor) and font DPI (Xft.dpi / text-scaling-factor),
 * which grows text but leaves logical pixels alone. The icon's logical size
 * follows the font DPI so it keeps pace with the text beside it, and the
 * pixbuf is rasterised at that size times the widget's scale factor from
 * the 48px source art, so it is resampled once at full resolution. */
/* Size each of g_icons was last rasterised at, so icons drawn at different
 * sizes (status rows, refresh button) don't keep reloading each other. */
static int g_icon_pixels[SWAPP_ICON_COUNT] = {0};

/* `base` is a size in 96-DPI units. */
static int swapp_icon_logical_size_for(int base) {
    double dpi = gdk_screen_get_resolution(gdk_screen_get_default());
    if (dpi <= 0) {
        dpi = 96.0;
    }
    return (int)(base * dpi / 96.0 + 0.5);
}

static int swapp_icon_logical_size(void) {
    return swapp_icon_logical_size_for(SWAPP_ICON_SIZE);
}

static void swapp_icon_load_at(swapp_icon which, int pixels) {
    if (pixels == g_icon_pixels[which]) {
        return;
    }
    g_icon_pixels[which] = pixels;

    static const char *const files[SWAPP_ICON_COUNT] = {
        "icons/spinner.png", "icons/check.png", "icons/error.png", "icons/refresh.png",
        "icons/check.png", "icons/windows.png", "icons/linux.png"};
    g_clear_object(&g_icons[which]);
    /* A missing icon is not fatal: the text lines still say everything the
     * icons do. */
    g_icons[which] = swapp_pixbuf_from_asset(files[which], pixels, pixels);
}

static void swapp_icons_load(void) {
    for (int i = 0; i < SWAPP_ICON_COUNT; i++) {
        swapp_icon_load_at((swapp_icon)i, swapp_icon_logical_size());
    }
}

static gboolean swapp_icon_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    (void)user_data;
    swapp_icon which = (swapp_icon)GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(widget), "swapp-icon"));
    int base = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "swapp-icon-base"));

    /* Re-rasterised here rather than once at startup: the scale factor is
     * the widget's own monitor's, and can change when the window moves. */
    int logical = swapp_icon_logical_size_for(base ? base : SWAPP_ICON_SIZE);
    int scale = gtk_widget_get_scale_factor(widget);
    if (scale < 1) {
        scale = 1;
    }
    swapp_icon_load_at(which, logical * scale);

    /* Inside a disabled button the glyph borrows the inactive-input gray. */
    swapp_icon tint = gtk_widget_is_sensitive(widget) ? which : SWAPP_ICON_CHECK_MUTED;

    GdkPixbuf *pixbuf = g_icons[which];
    if (!pixbuf) {
        return FALSE;
    }

    double half = logical / 2.0;
    cairo_translate(cr, half, half);
    if (which == SWAPP_ICON_SPINNER) {
        cairo_rotate(cr, g_spinner_angle);
    }
    cairo_translate(cr, -half, -half);

    /* The pixbuf is scale_factor times the logical size, so draw it back
     * down into the widget's own coordinate space. */
    cairo_scale(cr, 1.0 / scale, 1.0 / scale);
    gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);

    /* The icons are single-colour glyphs: use the pixbuf only as a mask and
     * fill it with the state's colour. */
    cairo_pattern_t *mask = cairo_pattern_reference(cairo_get_source(cr));
    const double *color = g_icon_colors[tint];
    cairo_set_source_rgb(cr, color[0], color[1], color[2]);
    cairo_mask(cr, mask);
    cairo_pattern_destroy(mask);
    return TRUE;
}

/* Builds one row into `box` and hands back its icon widget, which the
 * caller keeps so it can switch the row between spinner and result. */
static GtkWidget *swapp_row_new(GtkWidget *box, GtkWidget **label_out) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *icon = gtk_drawing_area_new();
    int icon_size = swapp_icon_logical_size();
    gtk_widget_set_size_request(icon, icon_size, icon_size);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    g_signal_connect(icon, "draw", G_CALLBACK(swapp_icon_draw), NULL);
    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    *label_out = label;
    return icon;
}

static void swapp_main_set_line(GtkWidget *icon, GtkWidget *label, swapp_icon which,
                                const char *text) {
    g_object_set_data(G_OBJECT(icon), "swapp-icon", GINT_TO_POINTER(which));
    gtk_widget_queue_draw(icon);
    gtk_label_set_text(GTK_LABEL(label), text);
}

/* Store columns: the monitor label, then one Pango-markup cell per input
 * slot (the active input gets a green check), then one swapp_owner int per
 * slot for which machine the input belongs to. The store is sized for the
 * most inputs a monitor could report; the view shows only as many "Input N"
 * columns as the monitor with the most inputs actually has. */
#define SWAPP_TABLE_MAX_INPUTS 16
#define SWAPP_TABLE_MONITOR_WIDTH 200
#define SWAPP_TABLE_INPUT_WIDTH 90
#define SWAPP_TABLE_LOGO_SIZE 13       /* optically matches the check glyph */
#define SWAPP_TABLE_LINUX_LOGO_SIZE 16 /* the penguin needs more room than four squares */
#define SWAPP_TABLE_LOGO_PAD 6         /* between the logo and the cell's right edge */

enum {
    SWAPP_TABLE_MONITOR,
    SWAPP_TABLE_FIRST_INPUT,
    SWAPP_TABLE_FIRST_OWNER = SWAPP_TABLE_FIRST_INPUT + SWAPP_TABLE_MAX_INPUTS,
    /* Hidden: the server's own identifiers, sent back verbatim in `switch`. */
    SWAPP_TABLE_INDEX = SWAPP_TABLE_FIRST_OWNER + SWAPP_TABLE_MAX_INPUTS,
    /* The input the monitor is showing -- the green check -- as a code
     * rather than only as colour inside the cells' markup. */
    SWAPP_TABLE_ACTIVE,
    SWAPP_TABLE_FIRST_CODE,
    SWAPP_TABLE_COLUMNS = SWAPP_TABLE_FIRST_CODE + SWAPP_TABLE_MAX_INPUTS
};

/* Which machine an input belongs to, as stored per slot in the table. */
typedef enum {
    SWAPP_OWNER_NONE = 0,
    SWAPP_OWNER_LINUX = 1,
    SWAPP_OWNER_WINDOWS = 2,
} swapp_owner;

static GtkWidget *g_monitor_table = NULL;
static GtkWidget *g_refresh_button = NULL;
static GtkWidget *g_windows_all_button = NULL;
static GtkWidget *g_linux_all_button = NULL;
static GtkWidget *g_table_menu = NULL; /* the open cell context menu, if any */
/* The tray menu's switch-all items. The menu is built once, so they are kept
 * to have their sensitivity follow the link like the buttons'. */
static GtkWidget *g_tray_windows_item = NULL;
static GtkWidget *g_tray_linux_item = NULL;
#define SWAPP_REFRESH_WIDTH 72
#define SWAPP_REFRESH_ICON_SIZE 28

/* Cell selection. GtkTreeView only selects whole rows, so its own selection
 * is switched off and this tracks the one selected input cell of the whole
 * table instead (row -1 = none; slot N = "Input N"; the Monitor column is
 * never selectable). UI-only: it says which input the user picked, not
 * anything about the monitors. */
static int g_selected_row = -1;
static int g_selected_slot = 0;

static int swapp_table_row_of(GtkTreeModel *model, GtkTreeIter *iter) {
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    int row = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    return row;
}

/* Paints a selected cell's background; runs for every input cell drawn. */
static void swapp_table_input_cell_data(GtkTreeViewColumn *column, GtkCellRenderer *renderer,
                                        GtkTreeModel *model, GtkTreeIter *iter,
                                        gpointer user_data) {
    (void)column;
    int slot = GPOINTER_TO_INT(user_data); /* 1-based */
    int row = swapp_table_row_of(model, iter);

    char *markup = NULL;
    gtk_tree_model_get(model, iter, SWAPP_TABLE_FIRST_INPUT + slot - 1, &markup, -1);
    int selected = markup && markup[0] && row == g_selected_row && slot == g_selected_slot;
    g_free(markup);

    GdkRGBA highlight = {0xd6 / 255.0, 0xe8 / 255.0, 0xfc / 255.0, 1.0};
    g_object_set(renderer, "cell-background-rgba", selected ? &highlight : NULL, NULL);
}

/* Switch on the selected cell (right-click selected it before the menu
 * opened). The client never drives DDC/CI: it hands the server the monitor
 * index and input code the server's own list gave, and the new state comes
 * back as a fresh list. */
/* The server's identifiers for the selected cell, which machine it is
 * currently assigned to, and the input its monitor is showing. Returns 0
 * with no selection. */
static int swapp_table_selected(int *monitor_index, int *input_code, int *owner, int *active) {
    if (g_selected_row < 0 || g_selected_slot < 1) {
        return 0;
    }
    GtkTreeIter iter;
    if (!gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(g_monitor_store), &iter, NULL,
                                       g_selected_row)) {
        return 0;
    }
    gtk_tree_model_get(GTK_TREE_MODEL(g_monitor_store), &iter, SWAPP_TABLE_INDEX, monitor_index,
                       SWAPP_TABLE_FIRST_CODE + g_selected_slot - 1, input_code,
                       SWAPP_TABLE_FIRST_OWNER + g_selected_slot - 1, owner,
                       SWAPP_TABLE_ACTIVE, active, -1);
    return 1;
}

/* Whether Switch is worth offering on a cell: the input has to belong to a
 * machine -- an unassigned one is nobody's -- and must not be the one the
 * monitor is already showing, which would switch it to where it already is.
 * Both are the server's rules too; this is only the client's UI reading of
 * the list the server sent. */
static int swapp_table_can_switch(int owner, int input_code, int active) {
    return owner != SWAPP_OWNER_NONE && input_code != active;
}

static void swapp_table_on_switch(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    int monitor_index = -1, input_code = -1, owner = SWAPP_OWNER_NONE, active = -1;
    /* The menu only offers Switch where it would do something (see
     * swapp_table_can_switch), and the server checks again; this is the
     * middle line of the same rule. */
    if (swapp_table_selected(&monitor_index, &input_code, &owner, &active)
        && swapp_table_can_switch(owner, input_code, active)) {
        swapp_net_request_switch(monitor_index, input_code);
    }
}

/* Assign on the selected cell; the role rides in user_data. Like Switch,
 * the server does the work and the result comes back as a fresh list. */
static void swapp_table_on_assign(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    int monitor_index = -1, input_code = -1, owner = 0, active = -1;
    if (swapp_table_selected(&monitor_index, &input_code, &owner, &active)) {
        swapp_net_request_assign(monitor_index, input_code, GPOINTER_TO_INT(user_data));
    }
}

static gboolean swapp_table_menu_destroy(gpointer menu) {
    gtk_widget_destroy(GTK_WIDGET(menu));
    return G_SOURCE_REMOVE;
}

/* Defers the destroy until the item's "activate" has run. */
static void swapp_table_menu_closed(GtkMenuShell *menu, gpointer user_data) {
    (void)user_data;
    g_idle_add(swapp_table_menu_destroy, menu);
}

static gboolean swapp_table_on_click(GtkWidget *widget, GdkEventButton *event,
                                     gpointer user_data) {
    (void)user_data;
    if (event->type != GDK_BUTTON_PRESS || (event->button != 1 && event->button != 3)) {
        return FALSE;
    }
    int right = (event->button == 3);
    GtkTreePath *path = NULL;
    GtkTreeViewColumn *column = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (gint)event->x, (gint)event->y,
                                       &path, &column, NULL, NULL)) {
        return TRUE;
    }
    int row = gtk_tree_path_get_indices(path)[0];
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "swapp-slot"));

    if (slot > 0) {
        GtkTreeIter iter;
        char *markup = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_monitor_store), &iter, path)) {
            gtk_tree_model_get(GTK_TREE_MODEL(g_monitor_store), &iter,
                               SWAPP_TABLE_FIRST_INPUT + slot - 1, &markup, -1);
        }
        if (markup && markup[0]) {
            if (!right && row == g_selected_row && slot == g_selected_slot) {
                /* Left-clicking the selected cell again clears it. */
                g_selected_row = -1;
                g_selected_slot = 0;
            } else {
                g_selected_row = row;
                g_selected_slot = slot;
            }
            gtk_widget_queue_draw(widget);

            if (right) {
                int monitor_index = -1, input_code = -1, owner = 0, active = -1;
                swapp_table_selected(&monitor_index, &input_code, &owner, &active);

                GtkWidget *menu = gtk_menu_new();
                /* Where Switch would do nothing it is left out rather than
                 * shown dead: the menu is four items long, and on most
                 * cells only the assignments are ever available. */
                if (swapp_table_can_switch(owner, input_code, active)) {
                    GtkWidget *item = gtk_menu_item_new_with_label("Switch");
                    g_signal_connect(item, "activate", G_CALLBACK(swapp_table_on_switch), NULL);
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
                }

                static const struct {
                    const char *text;
                    int role;
                    int owner; /* the swapp_owner that means "already this" */
                } assigns[] = {
                    {"Assign to Windows", SWAPP_ROLE_WINDOWS, SWAPP_OWNER_WINDOWS},
                    {"Assign to Linux", SWAPP_ROLE_LINUX, SWAPP_OWNER_LINUX},
                    {"Clear assignment", SWAPP_ROLE_NONE, SWAPP_OWNER_NONE},
                };
                for (guint a = 0; a < G_N_ELEMENTS(assigns); a++) {
                    GtkWidget *assign = gtk_menu_item_new_with_label(assigns[a].text);
                    /* Already-true choices are greyed out, which also greys
                     * Clear on an unassigned input. */
                    gtk_widget_set_sensitive(assign, owner != assigns[a].owner);
                    g_signal_connect(assign, "activate", G_CALLBACK(swapp_table_on_assign),
                                     GINT_TO_POINTER(assigns[a].role));
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), assign);
                }
                gtk_widget_show_all(menu);
                /* Freed once it closes rather than leaking one per click --
                 * but not from "deactivate" itself: GtkMenuShell emits that
                 * *before* the chosen item's "activate", and destroying the
                 * menu there disconnects the item's handler first, so the
                 * click would silently do nothing. */
                g_signal_connect(menu, "deactivate", G_CALLBACK(swapp_table_menu_closed), NULL);
                g_table_menu = menu;
                g_object_add_weak_pointer(G_OBJECT(menu), (gpointer *)&g_table_menu);
                gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
            }
        }
        g_free(markup);
    }
    gtk_tree_path_free(path);
    return TRUE;
}

/* Logical size of an owner's logo: the penguin needs a little more room than
 * the four squares to read at the same weight. */
static int swapp_table_logo_size(swapp_owner owner) {
    return swapp_icon_logical_size_for(owner == SWAPP_OWNER_LINUX ? SWAPP_TABLE_LINUX_LOGO_SIZE
                                                                  : SWAPP_TABLE_LOGO_SIZE);
}

/* An owner's logo for the table, tinted and rasterised at the view's scale
 * factor. Built on first use, since the scale is only known once the view
 * is on screen. */
static cairo_surface_t *swapp_table_logo_surface(GtkWidget *view, swapp_owner owner) {
    static cairo_surface_t *surfaces[3] = {NULL, NULL, NULL};
    static int surface_scales[3] = {0, 0, 0};
    if (owner != SWAPP_OWNER_LINUX && owner != SWAPP_OWNER_WINDOWS) {
        return NULL;
    }
    int scale = gtk_widget_get_scale_factor(view);
    if (scale < 1) {
        scale = 1;
    }
    if (surfaces[owner] && surface_scales[owner] == scale) {
        return surfaces[owner];
    }
    g_clear_pointer(&surfaces[owner], cairo_surface_destroy);
    surface_scales[owner] = scale;

    swapp_icon icon = owner == SWAPP_OWNER_LINUX ? SWAPP_ICON_LINUX : SWAPP_ICON_WINDOWS;
    int pixels = swapp_table_logo_size(owner) * scale;
    GdkPixbuf *source = swapp_pixbuf_from_asset(
        owner == SWAPP_OWNER_LINUX ? "icons/linux.png" : "icons/windows.png", pixels, pixels);
    if (!source) {
        return NULL;
    }
    GdkPixbuf *pixbuf = gdk_pixbuf_get_has_alpha(source) ? g_object_ref(source)
                                                         : gdk_pixbuf_add_alpha(source, FALSE, 0, 0, 0);
    g_object_unref(source);

    /* Same tinting as the status icons: keep the alpha, replace the colour. */
    const double *color = g_icon_colors[icon];
    guchar *px = gdk_pixbuf_get_pixels(pixbuf);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    for (int y = 0; y < gdk_pixbuf_get_height(pixbuf); y++) {
        for (int x = 0; x < gdk_pixbuf_get_width(pixbuf); x++) {
            guchar *p = px + y * stride + x * 4;
            p[0] = (guchar)(color[0] * 255);
            p[1] = (guchar)(color[1] * 255);
            p[2] = (guchar)(color[2] * 255);
        }
    }
    surfaces[owner] =
        gdk_cairo_surface_create_from_pixbuf(pixbuf, scale, gtk_widget_get_window(view));
    g_object_unref(pixbuf);
    return surfaces[owner];
}

/* The input cell renderer: a text renderer that, when its "owner" property
 * names a machine, keeps a strip at the cell's right edge free and paints
 * that machine's logo there itself.
 *
 * One renderer rather than a text renderer plus a pixbuf renderer in the
 * same column: GtkCellAreaBox shares the column's width between the two
 * using sizes aligned across every row, so a long name elsewhere in the
 * column ("DisplayPort-1") pushed the pixbuf past the cell edge and it was
 * clipped away. Drawing the logo from inside the one renderer puts it at
 * the allocated edge no matter what the text asked for. */
typedef struct {
    GtkCellRendererText parent;
    int owner; /* swapp_owner */
} SwappInputRenderer;

typedef struct {
    GtkCellRendererTextClass parent_class;
} SwappInputRendererClass;

G_DEFINE_TYPE(SwappInputRenderer, swapp_input_renderer, GTK_TYPE_CELL_RENDERER_TEXT)

enum { SWAPP_INPUT_RENDERER_PROP_OWNER = 1 };

static void swapp_input_renderer_set_property(GObject *object, guint id, const GValue *value,
                                              GParamSpec *pspec) {
    SwappInputRenderer *self = (SwappInputRenderer *)object;
    if (id == SWAPP_INPUT_RENDERER_PROP_OWNER) {
        self->owner = g_value_get_int(value);
    } else {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
    }
}

static void swapp_input_renderer_get_property(GObject *object, guint id, GValue *value,
                                              GParamSpec *pspec) {
    SwappInputRenderer *self = (SwappInputRenderer *)object;
    if (id == SWAPP_INPUT_RENDERER_PROP_OWNER) {
        g_value_set_int(value, self->owner);
    } else {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
    }
}

static void swapp_input_renderer_render(GtkCellRenderer *cell, cairo_t *cr, GtkWidget *widget,
                                        const GdkRectangle *background_area,
                                        const GdkRectangle *cell_area,
                                        GtkCellRendererState flags) {
    SwappInputRenderer *self = (SwappInputRenderer *)cell;
    GtkCellRendererClass *parent = GTK_CELL_RENDERER_CLASS(swapp_input_renderer_parent_class);

    swapp_owner owner = (swapp_owner)self->owner;
    if (owner != SWAPP_OWNER_LINUX && owner != SWAPP_OWNER_WINDOWS) {
        parent->render(cell, cr, widget, background_area, cell_area, flags);
        return;
    }

    int logo = swapp_table_logo_size(owner);
    int pad = SWAPP_TABLE_LOGO_PAD;

    /* The text gets the cell minus the logo strip, so it ellipsizes before
     * the logo instead of running under it. */
    GdkRectangle text_area = *cell_area;
    text_area.width = MAX(0, cell_area->width - logo - 2 * pad);
    parent->render(cell, cr, widget, background_area, &text_area, flags);

    cairo_surface_t *surface = swapp_table_logo_surface(widget, owner);
    if (surface) {
        double x = cell_area->x + cell_area->width - pad - logo;
        double y = cell_area->y + (cell_area->height - logo) / 2.0;
        cairo_save(cr);
        cairo_set_source_surface(cr, surface, x, y);
        cairo_paint(cr);
        cairo_restore(cr);
    }
}

static void swapp_input_renderer_class_init(SwappInputRendererClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->set_property = swapp_input_renderer_set_property;
    object_class->get_property = swapp_input_renderer_get_property;
    GTK_CELL_RENDERER_CLASS(klass)->render = swapp_input_renderer_render;

    g_object_class_install_property(
        object_class, SWAPP_INPUT_RENDERER_PROP_OWNER,
        g_param_spec_int("owner", "Owner", "The machine this input belongs to (swapp_owner)",
                         SWAPP_OWNER_NONE, SWAPP_OWNER_WINDOWS, SWAPP_OWNER_NONE,
                         G_PARAM_READWRITE));
}

static void swapp_input_renderer_init(SwappInputRenderer *self) {
    self->owner = SWAPP_OWNER_NONE;
}

static void swapp_table_set_input_columns(int count) {
    GtkTreeView *view = GTK_TREE_VIEW(g_monitor_table);
    if ((int)gtk_tree_view_get_n_columns(view) - 1 == count) {
        return;
    }
    while (gtk_tree_view_get_n_columns(view) > 1) {
        gtk_tree_view_remove_column(view, gtk_tree_view_get_column(view, 1));
    }
    for (int c = 0; c < count; c++) {
        char title[32];
        g_snprintf(title, sizeof(title), "Input %d", c + 1);
        GtkCellRenderer *renderer = g_object_new(swapp_input_renderer_get_type(), NULL);
        g_object_set(renderer, "ypad", 4, "xpad", 6, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        /* "owner" draws the logo; see SwappInputRenderer. */
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
            title, renderer, "markup", SWAPP_TABLE_FIRST_INPUT + c, "owner",
            SWAPP_TABLE_FIRST_OWNER + c, NULL);
        /* Equal, fixed-width slots so each "Input N" lines up down the table. */
        gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(column, SWAPP_TABLE_INPUT_WIDTH);
        gtk_tree_view_column_set_expand(column, TRUE);
        g_object_set_data(G_OBJECT(column), "swapp-slot", GINT_TO_POINTER(c + 1));
        gtk_tree_view_column_set_cell_data_func(column, renderer, swapp_table_input_cell_data,
                                                GINT_TO_POINTER(c + 1), NULL);
        gtk_tree_view_append_column(view, column);
    }
}

/* Fills the monitor table from the server's list block:
 *   monitor <index> <active code> <label>
 *   windows <index> <code>
 *   linux <index> <code>
 *   input <index> <code> <name>
 * Skipped when the block is unchanged, so a refresh doesn't reset the
 * selection or scroll position. */
static void swapp_table_set(const char *list) {
    static char *shown = NULL;
    if (shown && strcmp(shown, list) == 0) {
        return;
    }
    g_free(shown);
    shown = g_strdup(list);

    if (list[0] == '\0') {
        /* The list is gone (rescan, lost link): a selection would point at
         * whatever lands in that cell next. */
        g_selected_row = -1;
        g_selected_slot = 0;
    }
    gtk_list_store_clear(g_monitor_store);

    GtkTreeIter iter;
    int have_row = 0;
    int active = -1;
    int windows_code = -1;
    int linux_code = -1;
    int slot = 0;
    int max_inputs = 0;

    gchar **lines = g_strsplit(list, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        const char *line = lines[i];
        int index = 0;
        int code = 0;
        int consumed = 0;
        if (g_str_has_prefix(line, "monitor ")
            && sscanf(line, "monitor %d %d %n", &index, &code, &consumed) >= 2) {
            gtk_list_store_append(g_monitor_store, &iter);
            gtk_list_store_set(g_monitor_store, &iter, SWAPP_TABLE_MONITOR, line + consumed,
                               SWAPP_TABLE_INDEX, index, SWAPP_TABLE_ACTIVE, code, -1);
            have_row = 1;
            active = code;
            windows_code = -1;
            linux_code = -1;
            slot = 0;
        } else if (have_row && sscanf(line, "windows %d %d", &index, &code) == 2) {
            windows_code = code;
        } else if (have_row && sscanf(line, "linux %d %d", &index, &code) == 2) {
            linux_code = code;
        } else if (have_row && slot < SWAPP_TABLE_MAX_INPUTS
                   && sscanf(line, "input %d %d %n", &index, &code, &consumed) >= 2) {
            char *escaped = g_markup_escape_text(line + consumed, -1);
            /* Every input gets a check: green for the active one, light
             * gray for the rest, matching the Windows table. */
            char *markup = g_strdup_printf(
                "<span foreground='%s' weight='bold'>\xE2\x9C\x93</span> %s",
                code == active ? "#1e8e3e" : "#c8c8c8", escaped);
            gtk_list_store_set(g_monitor_store, &iter, SWAPP_TABLE_FIRST_INPUT + slot, markup,
                               SWAPP_TABLE_FIRST_OWNER + slot,
                               code == windows_code ? SWAPP_OWNER_WINDOWS
                               : code == linux_code ? SWAPP_OWNER_LINUX
                                                    : SWAPP_OWNER_NONE,
                               SWAPP_TABLE_FIRST_CODE + slot, code, -1);
            g_free(markup);
            g_free(escaped);
            slot++;
            if (slot > max_inputs) {
                max_inputs = slot;
            }
        }
    }
    g_strfreev(lines);
    swapp_table_set_input_columns(max_inputs);
}

/* The client's view of the server's switch-all condition, from the list the
 * server sent: connected, a finished scan (the list only arrives complete),
 * and every monitor listed with inputs and with both a `windows` and a
 * `linux` assignment. The server checks the same thing again on receipt. */
static int swapp_client_can_switch_all(void) {
    if (swapp_net_get_state() != SWAPP_NET_CONNECTED || !swapp_net_monitors_ready()) {
        return 0;
    }
    char list[8192];
    swapp_net_monitors_text(list, sizeof(list));

    int monitors = 0;
    int complete = 1;
    int has_windows = 0, has_linux = 0, inputs = 0;
    gchar **lines = g_strsplit(list, "\n", -1);
    for (int i = 0;; i++) {
        const char *line = lines[i];
        /* A monitor line, or the end, closes the monitor before it. */
        if ((!line || g_str_has_prefix(line, "monitor ")) && monitors > 0) {
            complete &= has_windows && has_linux && inputs > 0;
        }
        if (!line) {
            break;
        }
        if (g_str_has_prefix(line, "monitor ")) {
            monitors++;
            has_windows = has_linux = inputs = 0;
        } else if (g_str_has_prefix(line, "windows ")) {
            has_windows = 1;
        } else if (g_str_has_prefix(line, "linux ")) {
            has_linux = 1;
        } else if (g_str_has_prefix(line, "input ")) {
            inputs++;
        }
    }
    g_strfreev(lines);
    return monitors > 0 && complete;
}

/* The Windows / Linux buttons and F16: every monitor to role's input. The
 * server does the switching, and ignores monitors already there. */
static void swapp_client_switch_all(int role) {
    if (swapp_client_can_switch_all()) {
        swapp_net_request_switch_all(role);
    }
}

static void swapp_tray_refresh_status(void) {
    int can_switch_all = swapp_client_can_switch_all();
    if (g_tray_windows_item) {
        gtk_widget_set_sensitive(g_tray_windows_item, can_switch_all);
    }
    if (g_tray_linux_item) {
        gtk_widget_set_sensitive(g_tray_linux_item, can_switch_all);
    }

    if (!g_status_label) {
        return;
    }

    char status[128];
    swapp_net_status_text(status, sizeof(status));
    int connected = (swapp_net_get_state() == SWAPP_NET_CONNECTED);
    swapp_main_set_line(g_status_icon, g_status_label,
                        connected ? SWAPP_ICON_CHECK : SWAPP_ICON_SPINNER, status);

    int ready = swapp_net_monitors_ready();
    if (!connected) {
        swapp_main_set_line(g_monitors_icon, g_monitors_label, SWAPP_ICON_SPINNER,
                            "Monitors: waiting for a server");
    } else if (!ready) {
        swapp_main_set_line(g_monitors_icon, g_monitors_label, SWAPP_ICON_SPINNER,
                            "Monitors: waiting for the server's scan");
    } else {
        char list[8192];
        swapp_net_monitors_text(list, sizeof(list));

        /* The list is the server's, verbatim -- the client parses it only
         * far enough to count the monitors it names. */
        int count = 0;
        for (const char *p = list; (p = strstr(p, "monitor ")) != NULL; p += 8) {
            if (p == list || p[-1] == '\n') {
                count++;
            }
        }

        char summary[64];
        g_snprintf(summary, sizeof(summary), "Monitors: %d from the server", count);
        swapp_main_set_line(g_monitors_icon, g_monitors_label, SWAPP_ICON_CHECK, summary);
        swapp_table_set(list);
    }

    if (!connected || !ready) {
        /* Back to the initial state: no list, no selection, and no menu
         * still offering to act on a cell that is gone. */
        swapp_table_set("");
        if (g_table_menu) {
            gtk_menu_shell_deactivate(GTK_MENU_SHELL(g_table_menu));
        }
    }

    /* The scan runs on the server; with no server there is nobody to ask,
     * and while one is running a second request would be ignored. */
    if (g_refresh_button) {
        gtk_widget_set_sensitive(g_refresh_button, connected && ready);
    }
    if (g_windows_all_button) {
        gtk_widget_set_sensitive(g_windows_all_button, can_switch_all);
    }
    if (g_linux_all_button) {
        gtk_widget_set_sensitive(g_linux_all_button, can_switch_all);
    }
}

static void swapp_main_on_refresh(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    swapp_net_request_rescan();
}

/* The Windows / Linux buttons; the role rides in user_data. */
static void swapp_main_on_switch_all(GtkButton *button, gpointer user_data) {
    (void)button;
    swapp_client_switch_all(GPOINTER_TO_INT(user_data));
}

static gboolean swapp_main_spin(gpointer user_data) {
    (void)user_data;
    g_spinner_angle += G_PI / 12.0;
    if (g_spinner_angle >= 2 * G_PI) {
        g_spinner_angle -= 2 * G_PI;
    }
    gtk_widget_queue_draw(g_status_icon);
    gtk_widget_queue_draw(g_monitors_icon);
    return G_SOURCE_CONTINUE;
}

/* The spinner only animates while something is pending; with both lines
 * checked there is nothing left to redraw. Today the monitor line is always
 * pending, so this only ever starts it -- that changes when the server
 * starts serving the list. */
static void swapp_main_update_spinner(void) {
    int pending = (swapp_net_get_state() != SWAPP_NET_CONNECTED)
                  || !swapp_net_monitors_ready();

    if (pending && g_spinner_source == 0) {
        g_spinner_source = g_timeout_add(120, swapp_main_spin, NULL);
    } else if (!pending && g_spinner_source != 0) {
        g_source_remove(g_spinner_source);
        g_spinner_source = 0;
    }
}

/* One big button: a fixed-width GtkButton holding a drawing area with a
 * tinted glyph, which greys out with the button (see swapp_icon_draw). */
static GtkWidget *swapp_big_button_new(swapp_icon icon, const char *tooltip, GCallback clicked,
                                       gpointer user_data) {
    GtkWidget *button = gtk_button_new();
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_widget_set_size_request(button, SWAPP_REFRESH_WIDTH, -1);
    GtkWidget *glyph = gtk_drawing_area_new();
    int glyph_size = swapp_icon_logical_size_for(SWAPP_REFRESH_ICON_SIZE);
    gtk_widget_set_size_request(glyph, glyph_size, glyph_size);
    gtk_widget_set_halign(glyph, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(glyph, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(glyph), "swapp-icon", GINT_TO_POINTER(icon));
    g_object_set_data(G_OBJECT(glyph), "swapp-icon-base", GINT_TO_POINTER(SWAPP_REFRESH_ICON_SIZE));
    g_signal_connect(glyph, "draw", G_CALLBACK(swapp_icon_draw), NULL);
    gtk_container_add(GTK_CONTAINER(button), glyph);
    g_signal_connect(button, "clicked", clicked, user_data);
    return button;
}

static void swapp_show_main_window(void) {
    if (!g_main_window) {
        g_main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(g_main_window), "Swapp");
        gtk_window_set_default_size(GTK_WINDOW(g_main_window), 820, -1);
        /* Closing the window only hides it: the app lives in the tray, and
         * the link has to keep running with no window open. */
        g_signal_connect(g_main_window, "delete-event",
                         G_CALLBACK(gtk_widget_hide_on_delete), NULL);

        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        g_object_set(box, "margin", 12, NULL);
        gtk_container_add(GTK_CONTAINER(g_main_window), box);

        /* The monitor table fills the top; the status rows sit under it. */
        GType types[SWAPP_TABLE_COLUMNS];
        for (int c = 0; c < SWAPP_TABLE_COLUMNS; c++) {
            /* Label and markup are strings; owner, index and codes ints. */
            types[c] = c < SWAPP_TABLE_FIRST_OWNER ? G_TYPE_STRING : G_TYPE_INT;
        }
        g_monitor_store = gtk_list_store_newv(SWAPP_TABLE_COLUMNS, types);
        GtkWidget *table = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_monitor_store));
        g_monitor_table = table;

        /* Only the Monitor column exists up front; the "Input N" columns are
         * added by swapp_table_set once it knows how many inputs there are. */
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ypad", 4, "xpad", 6, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
            "Monitor", renderer, "text", SWAPP_TABLE_MONITOR, NULL);
        gtk_tree_view_column_set_min_width(column, SWAPP_TABLE_MONITOR_WIDTH);
        gtk_tree_view_append_column(GTK_TREE_VIEW(table), column);
        gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(table), GTK_TREE_VIEW_GRID_LINES_BOTH);
        gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(table)),
                                    GTK_SELECTION_NONE);
        g_signal_connect(table, "button-press-event", G_CALLBACK(swapp_table_on_click), NULL);

        /* No scrolled window and no expansion: the tree view takes its
         * natural height, which for the two monitors of this setup is the
         * header plus two rows. */
        GtkWidget *frame = gtk_frame_new(NULL);
        gtk_container_add(GTK_CONTAINER(frame), table);
        gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);

        /* The status rows, with the refresh button to their right spanning
         * both. */
        GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_pack_start(GTK_BOX(bottom), rows, TRUE, TRUE, 0);
        g_status_icon = swapp_row_new(rows, &g_status_label);
        g_monitors_icon = swapp_row_new(rows, &g_monitors_label);

        /* The big buttons, right of the rows: all-to-Windows, all-to-Linux,
         * then refresh at the far right -- same order as the server. */
        g_windows_all_button =
            swapp_big_button_new(SWAPP_ICON_WINDOWS, "Switch all to Windows",
                                 G_CALLBACK(swapp_main_on_switch_all),
                                 GINT_TO_POINTER(SWAPP_ROLE_WINDOWS));
        g_linux_all_button =
            swapp_big_button_new(SWAPP_ICON_LINUX, "Switch all to Linux",
                                 G_CALLBACK(swapp_main_on_switch_all),
                                 GINT_TO_POINTER(SWAPP_ROLE_LINUX));
        g_refresh_button = swapp_big_button_new(SWAPP_ICON_REFRESH, "Rescan monitors",
                                                G_CALLBACK(swapp_main_on_refresh), NULL);
        gtk_box_pack_start(GTK_BOX(bottom), g_windows_all_button, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bottom), g_linux_all_button, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bottom), g_refresh_button, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);
    }

    swapp_main_update_spinner();

    swapp_tray_refresh_status();
    gtk_widget_show_all(g_main_window);
    gtk_window_present(GTK_WINDOW(g_main_window));
}

/* Centres the window on the monitor after the one it is on, wrapping at the
 * end. The monitor the window opened on can be the one currently handed to
 * the other machine, which leaves the window invisible with no way to reach
 * it; pressing the open hotkey again walks it to the next monitor until it
 * lands on one this machine is actually showing. */
/* Asks KWin to move the active window one screen on, which is exactly what
 * Meta+Shift+Right does. Under Wayland a client cannot place its own window
 * -- gtk_window_move is accepted and ignored -- so the only way to move it
 * is to ask the compositor, and KWin's own global shortcut is already that
 * request. Nothing happens on a compositor without kglobalaccel; there is
 * no portable Wayland equivalent to fall back to.
 *
 * The shortcut acts on whatever is focused, so this is only safe to call
 * with our own window active -- see swapp_main_next_screen. */
static void swapp_kwin_window_to_next_screen(void) {
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) {
        return;
    }

    /* Fire and forget: the reply says only that KWin took the shortcut, and
     * waiting for it would block the UI thread the hotkey runs on. */
    g_dbus_connection_call(bus, "org.kde.kglobalaccel", "/component/kwin",
                           "org.kde.kglobalaccel.Component", "invokeShortcut",
                           g_variant_new("(s)", "Window to Next Screen"), NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_object_unref(bus);
}

/* Centres the window on the monitor after the one it is on, wrapping at the
 * end. The monitor the window opened on can be the one currently handed to
 * the other machine, which leaves the window invisible with no way to reach
 * it; pressing the open hotkey again walks it to the next monitor until it
 * lands on one this machine is actually showing. */
static void swapp_main_next_screen(void) {
    GdkWindow *window = g_main_window ? gtk_widget_get_window(g_main_window) : NULL;
    if (!window) {
        return;
    }

    GdkDisplay *display = gdk_window_get_display(window);
    int n_monitors = gdk_display_get_n_monitors(display);
    if (n_monitors < 2) {
        return;
    }

    /* Wayland hands window placement to the compositor, so ask it instead
     * of moving the window ourselves. Checked by display type rather than
     * with GDK_IS_WAYLAND_DISPLAY, which would pull in gdk/gdkwayland.h and
     * the Wayland backend headers for this one test. */
    if (g_strcmp0(G_OBJECT_TYPE_NAME(display), "GdkWaylandDisplay") == 0) {
        /* The compositor moves whatever is focused, which is our window
         * only while it is active. If the hotkey couldn't raise it -- under
         * Wayland an app may not take focus for itself -- moving on would
         * shove some other application's window across instead. */
        if (gtk_window_is_active(GTK_WINDOW(g_main_window))) {
            swapp_kwin_window_to_next_screen();
        }
        return;
    }

    GdkMonitor *current = gdk_display_get_monitor_at_window(display, window);
    int index = 0;
    for (int i = 0; i < n_monitors; i++) {
        if (gdk_display_get_monitor(display, i) == current) {
            index = i;
            break;
        }
    }

    GdkMonitor *next = gdk_display_get_monitor(display, (index + 1) % n_monitors);
    if (!next) {
        return;
    }

    /* The work area, not the full monitor, so the window doesn't end up
     * under a panel. */
    GdkRectangle area;
    gdk_monitor_get_workarea(next, &area);

    int width = 0;
    int height = 0;
    gtk_window_get_size(GTK_WINDOW(g_main_window), &width, &height);
    gtk_window_move(GTK_WINDOW(g_main_window), area.x + (area.width - width) / 2,
                    area.y + (area.height - height) / 2);
}

/* The hotkey's own entry point: opens the window, or, with the window
 * already up, moves it on to the next monitor. */
static void swapp_main_open_or_next_screen(void) {
    gboolean was_open = g_main_window && gtk_widget_get_visible(g_main_window);
    swapp_show_main_window();
    if (was_open) {
        swapp_main_next_screen();
    }
}

static void swapp_tray_on_open(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    swapp_show_main_window();
}

/* The tray's Switch to Windows / Linux; the role rides in user_data. */
static void swapp_tray_on_switch_all(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    swapp_client_switch_all(GPOINTER_TO_INT(user_data));
}

static void swapp_tray_on_quit(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    /* Quit has to work mid-job. The loop isn't blocked -- the job is on a
     * worker thread -- but cancelling stops it at its next checkpoint so it
     * can't go on driving monitors, or fire a modeset, after the app is
     * gone. */
    swapp_monitors_cancel();
    swapp_net_stop();
    gtk_main_quit();
}

/* ---- Check for updates ---- */

/* Only one check or download at a time; touched on the main thread only. */
static gboolean g_update_busy = FALSE;
static GtkWidget *g_update_item = NULL;
static GtkWidget *g_update_window = NULL;
static GtkWidget *g_update_label = NULL;
static GtkWidget *g_update_bar = NULL;
static GtkWidget *g_update_buttons = NULL;
static GtkWidget *g_update_button = NULL;
/* Written by the worker before it hands back to the main thread. */
static char g_update_latest[16];
static char g_update_path[PATH_MAX];

static void swapp_tray_notify(const char *text) {
    NotifyNotification *note = notify_notification_new("Swapp", text, NULL);
    notify_notification_show(note, NULL);
    g_object_unref(note);
}

static void swapp_update_set_busy(gboolean busy) {
    g_update_busy = busy;
    gtk_widget_set_sensitive(g_update_item, !busy);
}

static void swapp_update_close_progress(void) {
    if (g_update_window) {
        gtk_widget_destroy(g_update_window);
        g_update_window = g_update_label = g_update_bar = NULL;
        g_update_buttons = g_update_button = NULL;
    }
}

static void swapp_update_fail(const char *text) {
    swapp_update_close_progress();
    swapp_update_set_busy(FALSE);
    swapp_tray_notify(text);
}

static gboolean swapp_update_on_progress_idle(gpointer data) {
    if (g_update_bar) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_update_bar),
                                      GPOINTER_TO_UINT(data) / 1000.0);
    }
    return G_SOURCE_REMOVE;
}

static void swapp_update_on_progress(unsigned long long done, unsigned long long total, void *ctx) {
    (void)ctx;
    /* Handed over only when the bar would visibly move. */
    static guint last = G_MAXUINT;
    guint permille = total ? (guint)(done * 1000 / total) : 0;
    if (permille != last) {
        last = permille;
        g_idle_add(swapp_update_on_progress_idle, GUINT_TO_POINTER(permille));
    }
}

static gboolean swapp_update_on_downloaded(gpointer data) {
    if (!GPOINTER_TO_INT(data)) {
        swapp_update_fail("Couldn't download the update.");
        return G_SOURCE_REMOVE;
    }
    char *text = g_strdup_printf("Build %s downloaded.", g_update_latest);
    gtk_label_set_text(GTK_LABEL(g_update_label), text);
    g_free(text);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_update_bar), 1.0);
    gtk_widget_show(g_update_buttons);
    gtk_widget_grab_focus(g_update_button);
    return G_SOURCE_REMOVE;
}

/* The new binary installs itself, which starts by asking this instance to
 * quit -- so after launching it the window just waits to be torn down. */
static void swapp_update_on_install(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    gtk_widget_set_sensitive(g_update_buttons, FALSE);
    if (swapp_update_launch(g_update_path)) {
        gtk_label_set_text(GTK_LABEL(g_update_label), "Installing...");
        return;
    }
    swapp_update_discard(g_update_path);
    swapp_update_fail("Couldn't start the update.");
}

static void swapp_update_on_cancel(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    swapp_update_discard(g_update_path);
    swapp_update_close_progress();
    swapp_update_set_busy(FALSE);
}

static gpointer swapp_update_download_thread(gpointer data) {
    (void)data;
    int ok = swapp_update_download(g_update_path, sizeof(g_update_path), swapp_update_on_progress,
                                   NULL);
    g_idle_add(swapp_update_on_downloaded, GINT_TO_POINTER(ok));
    return NULL;
}

/* A small fixed window: a label, a progress bar, and -- once the download is
 * in -- Update and Cancel. Nothing is installed until Update is clicked.
 * Closing it is refused: the download can't be interrupted, and afterwards
 * Cancel is the way out. */
static void swapp_update_show_progress(void) {
    g_update_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_update_window), "Swapp - Updating");
    gtk_window_set_resizable(GTK_WINDOW(g_update_window), FALSE);
    gtk_window_set_position(GTK_WINDOW(g_update_window), GTK_WIN_POS_CENTER);
    gtk_window_set_keep_above(GTK_WINDOW(g_update_window), TRUE);
    g_signal_connect(g_update_window, "delete-event", G_CALLBACK(gtk_true), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    char *text = g_strdup_printf("Downloading build %s...", g_update_latest);
    g_update_label = gtk_label_new(text);
    g_free(text);
    gtk_label_set_xalign(GTK_LABEL(g_update_label), 0.0f);
    g_update_bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(g_update_bar, 340, -1);
    gtk_box_pack_start(GTK_BOX(box), g_update_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), g_update_bar, FALSE, FALSE, 0);

    /* Built now, shown once the download has finished. */
    g_update_buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(g_update_buttons), GTK_BUTTONBOX_END);
    gtk_box_set_spacing(GTK_BOX(g_update_buttons), 8);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel, "clicked", G_CALLBACK(swapp_update_on_cancel), NULL);
    g_update_button = gtk_button_new_with_label("Update");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_update_button),
                                GTK_STYLE_CLASS_SUGGESTED_ACTION);
    g_signal_connect(g_update_button, "clicked", G_CALLBACK(swapp_update_on_install), NULL);
    gtk_container_add(GTK_CONTAINER(g_update_buttons), cancel);
    gtk_container_add(GTK_CONTAINER(g_update_buttons), g_update_button);
    gtk_box_pack_start(GTK_BOX(box), g_update_buttons, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(g_update_window), box);

    gtk_widget_show_all(g_update_window);
    gtk_widget_hide(g_update_buttons);
    gtk_window_present(GTK_WINDOW(g_update_window));
}

static gboolean swapp_update_on_checked(gpointer data) {
    int result = GPOINTER_TO_INT(data); /* 0 failed, 1 latest, 2 newer */
    if (result == 0) {
        swapp_update_fail("Couldn't check for updates.");
    } else if (result == 1) {
        swapp_update_set_busy(FALSE);
        swapp_tray_notify("You're running the latest version (" SWAPP_COMMIT ").");
    } else {
        swapp_update_show_progress();
        g_thread_unref(g_thread_new("swapp-update", swapp_update_download_thread, NULL));
    }
    return G_SOURCE_REMOVE;
}

static gpointer swapp_update_check_thread(gpointer data) {
    (void)data;
    int result = 0;
    if (swapp_update_fetch_latest(g_update_latest, sizeof(g_update_latest))) {
        result = strcmp(g_update_latest, SWAPP_COMMIT) == 0 ? 1 : 2;
    }
    g_idle_add(swapp_update_on_checked, GINT_TO_POINTER(result));
    return NULL;
}

static void swapp_tray_on_check_updates(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    if (g_update_busy) {
        return;
    }
    swapp_update_set_busy(TRUE);
    g_thread_unref(g_thread_new("swapp-update-check", swapp_update_check_thread, NULL));
}

static void swapp_tray_on_autostart(GtkCheckMenuItem *item, gpointer user_data) {
    (void)user_data;
    gboolean enable = gtk_check_menu_item_get_active(item);
    if (swapp_autostart_set(enable)) {
        swapp_tray_notify(enable ? "Swapp will start when you log in."
                                 : "Swapp won't start when you log in anymore.");
        return;
    }
    /* Put the checkmark back without re-entering this handler. */
    g_signal_handlers_block_by_func(item, swapp_tray_on_autostart, NULL);
    gtk_check_menu_item_set_active(item, !enable);
    g_signal_handlers_unblock_by_func(item, swapp_tray_on_autostart, NULL);
    swapp_tray_notify("Couldn't change autostart.");
}

/* One instance per user. The lock is a listening socket in the abstract
 * namespace, which the kernel drops the moment the process dies, so a crash
 * can't leave it stale the way a lock file would. The namespace is shared by
 * every user, hence the uid in the name. A later launch connects and sends
 * one byte: 's' to have the window shown, 'q' to have the app quit. */
static socklen_t swapp_instance_address(struct sockaddr_un *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    /* sun_path[0] stays '\0': that is what makes the name abstract. */
    int length = snprintf(addr->sun_path + 1, sizeof(addr->sun_path) - 1, "swapp-%u",
                          (unsigned)getuid());
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + length);
}

/* Connects to the running instance and sends it `command`. Returns zero if
 * none is running. */
static int swapp_instance_send(char command) {
    struct sockaddr_un addr;
    socklen_t size = swapp_instance_address(&addr);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }
    int sent = connect(fd, (struct sockaddr *)&addr, size) == 0 && write(fd, &command, 1) == 1;
    close(fd);
    return sent;
}

static void swapp_tray_on_quit(GtkMenuItem *item, gpointer user_data);

static gboolean swapp_instance_on_connect(gint fd, GIOCondition condition, gpointer user_data) {
    (void)condition;
    (void)user_data;
    int peer = accept(fd, NULL, NULL);
    if (peer < 0) {
        return G_SOURCE_CONTINUE;
    }
    /* The sender writes right after connecting; the timeout only keeps a
     * peer that never does from stalling the main loop. */
    struct timeval timeout = {1, 0};
    setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char command = 0;
    if (read(peer, &command, 1) != 1) {
        command = 0; /* a bare liveness probe from swapp_tray_stop_running */
    }
    close(peer);

    if (command == 'q') {
        swapp_tray_on_quit(NULL, NULL);
    } else if (command == 's') {
        swapp_show_main_window();
    }
    return G_SOURCE_CONTINUE;
}

/* Returns zero when another instance is already running -- it has been
 * asked to show its window -- and this one should exit. */
static int swapp_instance_claim(void) {
    struct sockaddr_un addr;
    socklen_t size = swapp_instance_address(&addr);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 1; /* can't tell; better two instances than none */
    }
    if (bind(fd, (struct sockaddr *)&addr, size) == 0 && listen(fd, 4) == 0) {
        g_unix_fd_add(fd, G_IO_IN, swapp_instance_on_connect, NULL);
        return 1;
    }
    int in_use = errno == EADDRINUSE;
    close(fd);
    return !(in_use && swapp_instance_send('s'));
}

int swapp_tray_stop_running(void) {
    if (!swapp_instance_send('q')) {
        return 0;
    }
    /* Gone once nothing accepts on the name any more. */
    for (int i = 0; i < 50; i++) {
        g_usleep(100 * 1000);
        struct sockaddr_un addr;
        socklen_t size = swapp_instance_address(&addr);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            break;
        }
        int alive = connect(fd, (struct sockaddr *)&addr, size) == 0;
        close(fd);
        if (!alive) {
            break;
        }
    }
    return 1;
}

void swapp_tray_run(const char *tooltip) {
    int argc = 0;
    gtk_init(&argc, NULL);

    if (!swapp_instance_claim()) {
        return;
    }

    swapp_icons_load();
    swapp_font_load();

    GtkWidget *menu = gtk_menu_new();

    GtkWidget *open_item = gtk_menu_item_new_with_label("Open");
    g_signal_connect(open_item, "activate", G_CALLBACK(swapp_tray_on_open), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), open_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    g_tray_windows_item = gtk_menu_item_new_with_label("Switch to Windows");
    g_signal_connect(g_tray_windows_item, "activate", G_CALLBACK(swapp_tray_on_switch_all),
                     GINT_TO_POINTER(SWAPP_ROLE_WINDOWS));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_tray_windows_item);
    g_tray_linux_item = gtk_menu_item_new_with_label("Switch to Linux");
    g_signal_connect(g_tray_linux_item, "activate", G_CALLBACK(swapp_tray_on_switch_all),
                     GINT_TO_POINTER(SWAPP_ROLE_LINUX));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_tray_linux_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    /* No server yet; the link's state callback enables them. */
    gtk_widget_set_sensitive(g_tray_windows_item, FALSE);
    gtk_widget_set_sensitive(g_tray_linux_item, FALSE);

    /* AppIndicator gives the app no left-click event -- the host always
     * opens the menu -- so the window is reachable from a menu item, which
     * is also the middle-click (secondary activate) target below. */
    /* Startup is intentionally inert for now: no monitor window, no
     * enumeration, no hotplug watch. Kept commented out
     * verbatim so the wiring can be restored as-is later.
    GtkWidget *monitors_item = gtk_menu_item_new_with_label("Monitors");
    g_signal_connect(monitors_item, "activate", G_CALLBACK(swapp_tray_on_show_monitors), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), monitors_item);
    */

    /* Initial state set before the handler is connected, so it doesn't
     * fire for it. */
    GtkWidget *autostart_item = gtk_check_menu_item_new_with_label("Autostart");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(autostart_item), swapp_autostart_enabled());
    g_signal_connect(autostart_item, "toggled", G_CALLBACK(swapp_tray_on_autostart), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), autostart_item);

    g_update_item = gtk_menu_item_new_with_label("Check for updates");
    g_signal_connect(g_update_item, "activate", G_CALLBACK(swapp_tray_on_check_updates), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_update_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit_item, "activate", G_CALLBACK(swapp_tray_on_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);

#ifdef SWAPP_USE_AYATANA_APPINDICATOR
    g_log_set_handler("libayatana-appindicator", G_LOG_LEVEL_WARNING,
                      swapp_tray_log_handler, NULL);
#endif

    /* The white-bezel variant: GNOME's and Ubuntu's top bars are dark
     * whatever the theme. AppIndicator only takes icons from files, found by
     * name in a theme path, so the embedded one is written out first. */
    char *tray_icon = swapp_asset_to_cache("icons/app-dark.png", "icons", "swapp-tray.png");
    char *tray_icon_dir = tray_icon ? g_path_get_dirname(tray_icon) : NULL;
    AppIndicator *indicator = tray_icon_dir
        ? app_indicator_new_with_path("swapp", "swapp-tray", APP_INDICATOR_CATEGORY_APPLICATION_STATUS,
                                      tray_icon_dir)
        : app_indicator_new("swapp", "application-x-executable",
                            APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    g_free(tray_icon_dir);
    g_free(tray_icon);

    /* Window icons (dock, Alt-Tab): same variant, those are dark too. */
    GdkPixbuf *window_icon = swapp_pixbuf_from_asset("icons/app-dark.png", 256, 256);
    if (window_icon) {
        gtk_window_set_default_icon(window_icon);
        g_object_unref(window_icon);
    }
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(indicator, tooltip);
    app_indicator_set_menu(indicator, GTK_MENU(menu));

    /* AppIndicator gives no left-click event, so middle-click is the only
     * pointer shortcut to the window. */
    app_indicator_set_secondary_activate_target(indicator, open_item);
    /* app_indicator_set_secondary_activate_target(indicator, monitors_item); */

    /* libnotify talks to the same org.freedesktop.Notifications D-Bus
     * service notify-send uses; initialised here for the hotkey
     * notification shown later. */
    notify_init("Swapp");

    /* The client does nothing on its own: it holds no monitor state, so
     * until the server answers there is nothing to show but the search. */
    swapp_net_set_state_callback(swapp_tray_refresh_status);
    /* Starts in the tray: the window only opens from the tray menu. */
    swapp_net_start();
    swapp_hotkey_start();

    /* The tray icon and the window go up immediately and the enumeration
     * runs behind them, rather than the app being invisible for however
     * long DDC/CI probing takes. The job is started first so the window
     * opens already showing its spinner instead of a set of live-looking
     * buttons backed by an empty cache. Until it lands F16 does nothing. */
    /*
    swapp_monitors_set_job_callback(swapp_list_job_done);
    swapp_monitors_rescan_async();
    swapp_show_monitor_list();
    swapp_monitors_watch_start();
    */

    gtk_main();

    notify_uninit();
    g_object_unref(indicator);
}
