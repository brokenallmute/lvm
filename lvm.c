/*
 * Simple Window Manager (LWM)
 * Lightweight X11 window manager with EWMH support
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <sys/select.h>
#include <strings.h>

/*
 * EWMH (Extended Window Manager Hints) atom enumeration
 */
enum {
    NET_SUPPORTED,
    NET_WM_NAME,
    NET_WM_STATE,
    NET_CHECK,
    NET_WM_STATE_FULLSCREEN,
    NET_ACTIVE_WINDOW,
    NET_CLIENT_LIST,
    NET_WM_WINDOW_TYPE,
    NET_WM_WINDOW_TYPE_DOCK,
    NET_WM_WINDOW_TYPE_DIALOG,
    NET_WM_WINDOW_TYPE_NORMAL,
    NET_WM_WINDOW_TYPE_MENU,
    NET_WM_WINDOW_TYPE_TOOLBAR,
    NET_WM_WINDOW_TYPE_SPLASH,
    NET_WM_WINDOW_TYPE_UTILITY,
    NET_WM_WINDOW_TYPE_NOTIFICATION,
    ATOM_LAST
};
Atom wmatoms[ATOM_LAST];

/*
 * Configuration structure for window manager appearance
 */
struct Config {
    char bar_color[16];
    char bg_color[16];
    char border_color[16];
    char button_color[16];
    char text_color[16];
    char line_color[16];
    char font_name[64];
    char mouse_mod[16];
} conf;

/*
 * Keybinding structure
 */
typedef struct {
    unsigned int mod;
    KeySym key;
    char command[128];
} KeyBind;

KeyBind binds[64];
int bind_count = 0;
unsigned int mouse_mod_mask = Mod1Mask;

/*
 * Global constants for window layout
 */
#define TITLE_HEIGHT           26
#define BAR_HEIGHT             26
#define MENU_ITEM_H            30
#define MIN_SIZE               60
#define MAX_CLIENTS            256
#define DEFAULT_WINDOW_WIDTH   800
#define DEFAULT_WINDOW_HEIGHT  500
#define BUTTON_PADDING         8

/*
 * Global X11 display variables
 */
Display *dpy;
Window root;
Window bar_win;
Window check_win;
XFontStruct *font_info;
Window focus_window = 0;

/*
 * Client window state management
 */
typedef struct {
    Window frame;
    Window client;
    int is_fullscreen;
    XWindowAttributes old_attr;
} ClientState;

ClientState clients[MAX_CLIENTS];
int client_count = 0;

/*
 * Drag and resize state tracking
 */
typedef struct {
    int start_root_x, start_root_y;
    int win_x, win_y;
    int win_w, win_h;
    int resize_x_dir;
    int resize_y_dir;
} DragState;

DragState drag_state;
XButtonEvent start_ev;

/*
 * Convert hex color string to X11 pixel color
 * Returns pixel value or 0 on failure
 */
unsigned long get_pixel(const char* color_hex) {
    if (!dpy) {
        return 0;
    }

    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    XColor color;

    if (!XParseColor(dpy, cmap, color_hex, &color)) {
        return 0;
    }
    if (!XAllocColor(dpy, cmap, &color)) {
        return 0;
    }

    return color.pixel;
}

/*
 * Convert modifier string to X11 modifier mask
 */
unsigned int str_to_mod(const char* str) {
    unsigned int mod = 0;

    if (strstr(str, "Mod1")) {
        mod |= Mod1Mask;
    }
    if (strstr(str, "Mod4")) {
        mod |= Mod4Mask;
    }
    if (strstr(str, "Shift")) {
        mod |= ShiftMask;
    }
    if (strstr(str, "Control")) {
        mod |= ControlMask;
    }

    return mod;
}


/*
 * Create default configuration file in home directory
 */
void create_default_config(const char* path) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/.config", getenv("HOME"));
    system(cmd);

    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }

    fprintf(f, "# Window Manager Colors\n");
    fprintf(f, "BAR_COLOR     #4C837E\n");
    fprintf(f, "BG_COLOR      #83A597\n");
    fprintf(f, "BORDER_COLOR  #000000\n");
    fprintf(f, "BUTTON_COLOR  #e8e4cf\n");
    fprintf(f, "TEXT_COLOR    #FFFFFF\n");
    fprintf(f, "LINE_COLOR    #FFFFFF\n");
    fprintf(f, "FONT          fixed\n");
    fprintf(f, "MOUSE_MOD     Mod1\n\n");
    fprintf(f, "# Keybindings configuration\n");
    fprintf(f, "BIND Mod4 Return xterm\n");
    fprintf(f, "BIND Mod4 Tab menu\n");
    fprintf(f, "BIND Mod1 Tab cycle\n");
    fprintf(f, "BIND Mod4 u unhide\n");
    fprintf(f, "BIND Mod4 q quit\n");
    fprintf(f, "BIND Mod4 c close\n");
    fprintf(f, "BIND Mod4 d flameshot gui\n");

    fclose(f);
    printf("[lwm] Created default config at %s\n", path);
}

/*
 * Load window manager configuration from file
 * Sets defaults and reads user config if available
 */
void load_config() {
    strncpy(conf.bar_color, "#4C837E", sizeof(conf.bar_color) - 1);
    strncpy(conf.bg_color, "#83A597", sizeof(conf.bg_color) - 1);
    strncpy(conf.border_color, "#000000", sizeof(conf.border_color) - 1);
    strncpy(conf.button_color, "#e8e4cf", sizeof(conf.button_color) - 1);
    strncpy(conf.text_color, "#FFFFFF", sizeof(conf.text_color) - 1);
    strncpy(conf.line_color, "#FFFFFF", sizeof(conf.line_color) - 1);
    strncpy(conf.font_name, "fixed", sizeof(conf.font_name) - 1);
    strncpy(conf.mouse_mod, "Mod1", sizeof(conf.mouse_mod) - 1);

    char path[256];
    snprintf(path, sizeof(path), "%s/.config/lwm.conf", getenv("HOME"));

    if (access(path, F_OK) != 0) {
        create_default_config(path);
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }

    char line[256];
    char key[64], val[64], cmd[128];

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;

        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }

        if (sscanf(line, "%63s %63s", key, val) == 2) {
            if (strcmp(key, "BAR_COLOR") == 0) {
                strncpy(conf.bar_color, val, sizeof(conf.bar_color) - 1);
            }
            else if (strcmp(key, "BG_COLOR") == 0) {
                strncpy(conf.bg_color, val, sizeof(conf.bg_color) - 1);
            }
            else if (strcmp(key, "BORDER_COLOR") == 0) {
                strncpy(conf.border_color, val, sizeof(conf.border_color) - 1);
            }
            else if (strcmp(key, "BUTTON_COLOR") == 0) {
                strncpy(conf.button_color, val, sizeof(conf.button_color) - 1);
            }
            else if (strcmp(key, "TEXT_COLOR") == 0) {
                strncpy(conf.text_color, val, sizeof(conf.text_color) - 1);
            }
            else if (strcmp(key, "LINE_COLOR") == 0) {
                strncpy(conf.line_color, val, sizeof(conf.line_color) - 1);
            }
            else if (strcmp(key, "FONT") == 0) {
                strncpy(conf.font_name, val, sizeof(conf.font_name) - 1);
            }
            else if (strcmp(key, "MOUSE_MOD") == 0) {
                strncpy(conf.mouse_mod, val, sizeof(conf.mouse_mod) - 1);
            }
        }

        char mod_str[32], key_str[32];

        if (sscanf(line, "BIND %31s %31s %127[^\t\n]", mod_str, key_str, cmd) == 3) {
            if (bind_count < 64) {
                binds[bind_count].mod = str_to_mod(mod_str);
                binds[bind_count].key = XStringToKeysym(key_str);
                strncpy(binds[bind_count].command, cmd, sizeof(binds[bind_count].command) - 1);
                bind_count++;
            }
        }
    }

    fclose(f);

    mouse_mod_mask = str_to_mod(conf.mouse_mod);
    if (mouse_mod_mask == 0) {
        mouse_mod_mask = Mod1Mask;
    }

    if (bind_count == 0) {
        binds[0].mod = Mod4Mask;
        binds[0].key = XK_Return;
        strncpy(binds[0].command, "xterm", sizeof(binds[0].command) - 1);
        binds[1].mod = Mod4Mask;
        binds[1].key = XK_q;
        strncpy(binds[1].command, "quit", sizeof(binds[1].command) - 1);
        bind_count = 2;
    }
}

/*
 * Add a client window to the managed clients list
 */
void add_client(Window client, Window frame) {
    if (client_count < MAX_CLIENTS) {
        clients[client_count].client = client;
        clients[client_count].frame = frame;
        clients[client_count].is_fullscreen = 0;
        client_count++;
    }
}

/*
 * Remove a client window from the managed clients list
 */
void remove_client(Window client) {
    int idx = -1;

    for (int i = 0; i < client_count; i++) {
        if (clients[i].client == client) {
            idx = i;
            break;
        }
    }

    if (idx != -1) {
        for (int i = idx; i < client_count - 1; i++) {
            clients[i] = clients[i + 1];
        }
        client_count--;
    }
}

/*
 * Get the frame window for a client window
 */
Window get_frame(Window client) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].client == client) {
            return clients[i].frame;
        }
    }
    return 0;
}

/*
 * Initialize EWMH (Extended Window Manager Hints) atoms
 * Provides compatibility with EWMH-compliant applications
 */
void init_hints() {
    wmatoms[NET_SUPPORTED] = XInternAtom(dpy, "_NET_SUPPORTED", False);
    wmatoms[NET_WM_NAME] = XInternAtom(dpy, "_NET_WM_NAME", False);
    wmatoms[NET_WM_STATE] = XInternAtom(dpy, "_NET_WM_STATE", False);
    wmatoms[NET_CHECK] = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    wmatoms[NET_WM_STATE_FULLSCREEN] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    wmatoms[NET_ACTIVE_WINDOW] = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    wmatoms[NET_CLIENT_LIST] = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    wmatoms[NET_WM_WINDOW_TYPE] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    wmatoms[NET_WM_WINDOW_TYPE_DOCK] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    wmatoms[NET_WM_WINDOW_TYPE_DIALOG] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    wmatoms[NET_WM_WINDOW_TYPE_NORMAL] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    wmatoms[NET_WM_WINDOW_TYPE_MENU] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
    wmatoms[NET_WM_WINDOW_TYPE_TOOLBAR] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    wmatoms[NET_WM_WINDOW_TYPE_SPLASH] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    wmatoms[NET_WM_WINDOW_TYPE_UTILITY] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    wmatoms[NET_WM_WINDOW_TYPE_NOTIFICATION] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);

    XChangeProperty(dpy, root, wmatoms[NET_SUPPORTED], XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)wmatoms, ATOM_LAST);

    check_win = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, check_win, wmatoms[NET_CHECK], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&check_win, 1);
    XChangeProperty(dpy, check_win, wmatoms[NET_WM_NAME], XInternAtom(dpy, "UTF8_STRING", False),
                    8, PropModeReplace, (unsigned char *)"lwm", 3);
    XChangeProperty(dpy, root, wmatoms[NET_CHECK], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&check_win, 1);
}

/*
 * Update the client list property on the root window
 */
void update_client_list() {
    Window root_ret, parent_ret, *children;
    unsigned int nchildren;

    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        XChangeProperty(dpy, root, wmatoms[NET_CLIENT_LIST], XA_WINDOW, 32, PropModeReplace,
                        (unsigned char *)children, nchildren);
        if (children) {
            XFree(children);
        }
    }
}

/*
 * Set the active window property
 */
void set_active_window(Window w) {
    XChangeProperty(dpy, root, wmatoms[NET_ACTIVE_WINDOW], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&w, 1);
}

/*
 * Execute a shell command as a new process
 * Detaches from display connection and creates a new session
 */
void spawn(const char* command) {
    if (fork() == 0) {
        if (dpy) {
            close(ConnectionNumber(dpy));
        }
        setsid();
        execl("/bin/sh", "sh", "-c", command, NULL);
        exit(0);
    }
}

/*
 * Find client window inside a frame window
 */
Window find_client_in_frame(Window frame) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].frame == frame) {
            return clients[i].client;
        }
    }

    Window root_ret, parent_ret, *children;
    unsigned int nchildren;
    Window client = 0;

    if (XQueryTree(dpy, frame, &root_ret, &parent_ret, &children, &nchildren)) {
        if (nchildren > 0) {
            client = children[0];
        }
        if (children) {
            XFree(children);
        }
    }

    return client;
}

/*
 * Find frame window for a given client window
 */
Window find_frame_of_client(Window client) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].client == client) {
            return clients[i].frame;
        }
    }
    return 0;
}

/*
 * Toggle fullscreen state for a client window
 */
void toggle_fullscreen(Window client) {
    Window frame = get_frame(client);
    if (!frame) {
        return;
    }

    int idx = -1;
    for (int i = 0; i < client_count; i++) {
        if (clients[i].client == client) {
            idx = i;
            break;
        }
    }

    if (idx == -1) {
        return;
    }

    if (!clients[idx].is_fullscreen) {
        XGetWindowAttributes(dpy, frame, &clients[idx].old_attr);
        int sw = DisplayWidth(dpy, DefaultScreen(dpy));
        int sh = DisplayHeight(dpy, DefaultScreen(dpy));
        XMoveResizeWindow(dpy, frame, 0, 0, sw, sh);
        XResizeWindow(dpy, client, sw, sh);
        XRaiseWindow(dpy, frame);
        Atom fs = wmatoms[NET_WM_STATE_FULLSCREEN];
        XChangeProperty(dpy, client, wmatoms[NET_WM_STATE], XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)&fs, 1);
        clients[idx].is_fullscreen = 1;
    } else {
        XMoveResizeWindow(dpy, frame, clients[idx].old_attr.x, clients[idx].old_attr.y,
                          clients[idx].old_attr.width, clients[idx].old_attr.height);
        XResizeWindow(dpy, client, clients[idx].old_attr.width,
                      clients[idx].old_attr.height - TITLE_HEIGHT);
        XDeleteProperty(dpy, client, wmatoms[NET_WM_STATE]);
        clients[idx].is_fullscreen = 0;
    }
}

/*
 * Update the status bar with current time, window name, and RAM usage
 */
void update_bar() {
    if (!dpy || !font_info) {
        return;
    }

    int sw = DisplayWidth(dpy, DefaultScreen(dpy));
    XGCValues gcv;
    gcv.font = font_info->fid;
    GC gc = XCreateGC(dpy, bar_win, GCFont, &gcv);

    XSetForeground(dpy, gc, get_pixel(conf.bar_color));
    XFillRectangle(dpy, bar_win, gc, 0, 0, sw, BAR_HEIGHT);

    char buffer[256];
    char time_str[64];
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    strftime(time_str, sizeof(time_str), "%H:%M | %d/%m", tm_info);

    struct sysinfo info;
    sysinfo(&info);
    unsigned long long total_ram = (unsigned long long)info.totalram * info.mem_unit;
    unsigned long long free_ram = (unsigned long long)info.freeram * info.mem_unit;
    unsigned long long used_ram_mb = (total_ram - free_ram) / 1024 / 1024;

    char *win_name = NULL;
    if (focus_window != 0) {
        XFetchName(dpy, focus_window, &win_name);
    }

    snprintf(buffer, sizeof(buffer), "%s || %s | RAM: %lluMB",
             win_name ? win_name : "Desktop", time_str, used_ram_mb);
    if (win_name) {
        XFree(win_name);
    }

    XSetForeground(dpy, gc, get_pixel(conf.text_color));
    int text_y = (BAR_HEIGHT / 2) + (font_info->ascent / 2) - 1;
    XDrawString(dpy, bar_win, gc, 2, text_y, buffer, strlen(buffer));

    XSetForeground(dpy, gc, get_pixel(conf.line_color));
    XDrawLine(dpy, bar_win, gc, 0, BAR_HEIGHT - 1, sw, BAR_HEIGHT - 1);

    XFreeGC(dpy, gc);
}

/*
 * Draw window decorations (title bar, buttons, borders)
 */
void draw_decorations(Window frame, int width, int height) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].frame == frame && clients[i].is_fullscreen) {
            return;
        }
    }

    if (!dpy) {
        return;
    }

    XGCValues gcv;
    gcv.foreground = get_pixel(conf.border_color);
    GC gc = XCreateGC(dpy, frame, GCForeground, &gcv);

    unsigned long bar_pixel = get_pixel(conf.bar_color);
    unsigned long btn_pixel = get_pixel(conf.button_color);
    unsigned long border_pixel = get_pixel(conf.border_color);
    unsigned long line_pixel = get_pixel(conf.line_color);

    XSetForeground(dpy, gc, bar_pixel);
    XFillRectangle(dpy, frame, gc, 0, 0, width, TITLE_HEIGHT);

    XSetForeground(dpy, gc, border_pixel);
    XDrawRectangle(dpy, frame, gc, 0, 0, width - 1, height + TITLE_HEIGHT - 1);

    XSetForeground(dpy, gc, line_pixel);
    XDrawLine(dpy, frame, gc, 0, TITLE_HEIGHT - 1, width, TITLE_HEIGHT - 1);

    int btn_size = TITLE_HEIGHT;

    XSetForeground(dpy, gc, btn_pixel);
    XFillRectangle(dpy, frame, gc, 0, 0, btn_size, btn_size);
    XSetForeground(dpy, gc, border_pixel);
    XDrawRectangle(dpy, frame, gc, 0, 0, btn_size, btn_size);
    int pad = BUTTON_PADDING;
    XDrawLine(dpy, frame, gc, pad, pad, btn_size - pad, btn_size - pad);
    XDrawLine(dpy, frame, gc, pad + 1, pad, btn_size - pad + 1, btn_size - pad);
    XDrawLine(dpy, frame, gc, pad, btn_size - pad, btn_size - pad, pad);
    XDrawLine(dpy, frame, gc, pad + 1, btn_size - pad, btn_size - pad + 1, pad);

    int x_right = width - btn_size;
    XSetForeground(dpy, gc, btn_pixel);
    XFillRectangle(dpy, frame, gc, x_right, 0, btn_size, btn_size);
    XSetForeground(dpy, gc, border_pixel);
    XDrawRectangle(dpy, frame, gc, x_right, 0, btn_size, btn_size);
    int cx = x_right + (btn_size / 2);
    int cy = (btn_size / 2) + 3;
    XDrawLine(dpy, frame, gc, x_right + 8, 10, cx, cy);
    XDrawLine(dpy, frame, gc, x_right + 9, 10, cx + 1, cy);
    XDrawLine(dpy, frame, gc, x_right + btn_size - 8, 10, cx, cy);
    XDrawLine(dpy, frame, gc, x_right + btn_size - 9, 10, cx - 1, cy);

    XFreeGC(dpy, gc);
}

/*
 * Apply decorations and frame to a new client window
 */
void frame_window(Window client) {
    if (!dpy || !client) {
        return;
    }

    XWindowAttributes attrs;
    if (XGetWindowAttributes(dpy, client, &attrs) == 0) {
        return;
    }
    if (attrs.override_redirect) {
        return;
    }

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    Atom *prop = NULL;
    int should_frame = 1;

    if (XGetWindowProperty(dpy, client, wmatoms[NET_WM_WINDOW_TYPE], 0, 1, False, XA_ATOM,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           (unsigned char**)&prop) == Success) {
        if (prop) {
            Atom type = prop[0];
            if (type == wmatoms[NET_WM_WINDOW_TYPE_DOCK] ||
                type == wmatoms[NET_WM_WINDOW_TYPE_MENU] ||
                type == wmatoms[NET_WM_WINDOW_TYPE_SPLASH] ||
                type == wmatoms[NET_WM_WINDOW_TYPE_NOTIFICATION] ||
                type == wmatoms[NET_WM_WINDOW_TYPE_UTILITY] ||
                type == wmatoms[NET_WM_WINDOW_TYPE_DIALOG]) {
                should_frame = 0;
            }
            XFree(prop);
        }
    }

    if (!should_frame) {
        XMapWindow(dpy, client);
        add_client(client, 0);
        update_client_list();
        return;
    }

    int w = attrs.width;
    int h = attrs.height;
    if (w < MIN_SIZE || h < MIN_SIZE) {
        w = DEFAULT_WINDOW_WIDTH;
        h = DEFAULT_WINDOW_HEIGHT;
        XResizeWindow(dpy, client, w, h);
    }

    int sw = DisplayWidth(dpy, DefaultScreen(dpy));
    int sh = DisplayHeight(dpy, DefaultScreen(dpy));
    int x = (sw - w) / 2;
    int y = (sh - h) / 2;
    if (y < BAR_HEIGHT) {
        y = BAR_HEIGHT;
    }

    Window frame = XCreateSimpleWindow(dpy, root, x, y, w, h + TITLE_HEIGHT, 1,
                                       get_pixel(conf.border_color),
                                       get_pixel(conf.bar_color));

    XSelectInput(dpy, client, StructureNotifyMask | PropertyChangeMask);
    XSelectInput(dpy, frame, SubstructureRedirectMask | SubstructureNotifyMask |
                 ButtonPressMask | ButtonReleaseMask | ExposureMask | EnterWindowMask);
    XReparentWindow(dpy, client, frame, 0, TITLE_HEIGHT);
    XMapWindow(dpy, frame);
    XMapWindow(dpy, client);
    XAddToSaveSet(dpy, client);
    XGrabButton(dpy, Button1, mouse_mod_mask, client, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, mouse_mod_mask, client, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);

    add_client(client, frame);
    update_client_list();
}

typedef struct { Window frame; char *name; } HiddenWindow;
/*
 * Display interactive menu of hidden windows
 */
void show_hidden_menu() {
    Window root_ret, parent_ret, *children;
    unsigned int nchildren;
    HiddenWindow hidden[64];
    int count = 0;

    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            if (children[i] == bar_win) {
                continue;
            }

            XWindowAttributes attr;
            XGetWindowAttributes(dpy, children[i], &attr);

            if (attr.map_state == IsUnmapped) {
                Window client = find_client_in_frame(children[i]);
                if (client) {
                    char *name = NULL;
                    if (XFetchName(dpy, client, &name) > 0) {
                        hidden[count].frame = children[i];
                        hidden[count].name = name;
                        count++;
                        if (count >= 64) {
                            break;
                        }
                    }
                }
            }
        }
        if (children) {
            XFree(children);
        }
    }

    if (count == 0) {
        return;
    }

    int sw = DisplayWidth(dpy, DefaultScreen(dpy));
    int sh = DisplayHeight(dpy, DefaultScreen(dpy));
    int menu_w = 400;
    int menu_h = count * MENU_ITEM_H;
    int menu_x = (sw - menu_w) / 2;
    int menu_y = (sh - menu_h) / 2;

    Window menu = XCreateSimpleWindow(dpy, root, menu_x, menu_y, menu_w, menu_h, 1,
                                      get_pixel(conf.border_color),
                                      get_pixel(conf.bar_color));

    XSelectInput(dpy, menu, ExposureMask | PointerMotionMask | ButtonPressMask | KeyPressMask);
    XSetTransientForHint(dpy, menu, root);
    XMapWindow(dpy, menu);

    XGrabPointer(dpy, menu, True, ButtonPressMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    XGrabKeyboard(dpy, menu, True, GrabModeAsync, GrabModeAsync, CurrentTime);

    GC gc = XCreateGC(dpy, menu, 0, NULL);
    XSetFont(dpy, gc, font_info->fid);

    unsigned long bg_px = get_pixel(conf.bar_color);
    unsigned long hl_px = get_pixel(conf.button_color);
    unsigned long txt_px = get_pixel(conf.text_color);
    unsigned long bdr_px = get_pixel(conf.border_color);

    int selected = -1;
    int done = 0;
    XEvent ev;

    while (!done) {
        XNextEvent(dpy, &ev);

        if (ev.type == Expose) {
            for (int i = 0; i < count; i++) {
                int y = i * MENU_ITEM_H;

                if (i == selected) {
                    XSetForeground(dpy, gc, hl_px);
                    XFillRectangle(dpy, menu, gc, 0, y, menu_w, MENU_ITEM_H);
                    XSetForeground(dpy, gc, bdr_px);
                } else {
                    XSetForeground(dpy, gc, bg_px);
                    XFillRectangle(dpy, menu, gc, 0, y, menu_w, MENU_ITEM_H);
                    XSetForeground(dpy, gc, txt_px);
                }

                int ty = y + (MENU_ITEM_H / 2) + (font_info->ascent / 2) - 1;
                XDrawString(dpy, menu, gc, 10, ty, hidden[i].name, strlen(hidden[i].name));

                XSetForeground(dpy, gc, bdr_px);
                XDrawLine(dpy, menu, gc, 0, y + MENU_ITEM_H - 1, menu_w, y + MENU_ITEM_H - 1);
            }

            XSetForeground(dpy, gc, bdr_px);
            XDrawRectangle(dpy, menu, gc, 0, 0, menu_w - 1, menu_h - 1);

        } else if (ev.type == MotionNotify) {
            int item = ev.xmotion.y / MENU_ITEM_H;
            if (item >= 0 && item < count && item != selected) {
                selected = item;
                XClearArea(dpy, menu, 0, 0, 0, 0, True);
            }

        } else if (ev.type == ButtonPress) {
            if (selected >= 0 && selected < count) {
                XMapWindow(dpy, hidden[selected].frame);
                XRaiseWindow(dpy, hidden[selected].frame);
                Window c = find_client_in_frame(hidden[selected].frame);
                XSetInputFocus(dpy, c ? c : hidden[selected].frame,
                               RevertToPointerRoot, CurrentTime);
                focus_window = c;
                done = 1;
            }

        } else if (ev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_Escape || ks == XK_q) {
                done = 1;
            }
        }
    }

    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    XDestroyWindow(dpy, menu);
    XFreeGC(dpy, gc);

    for (int i = 0; i < count; i++) {
        if (hidden[i].name) {
            XFree(hidden[i].name);
        }
    }

    update_bar();
}

/*
 * Show all hidden windows
 */
void unhide_all() {
    Window root_ret, parent_ret, *children;
    unsigned int nchildren;

    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            if (children[i] != bar_win) {
                XMapWindow(dpy, children[i]);
            }
        }
        if (children) {
            XFree(children);
        }
    }
}

/*
 * Cycle focus to next window in list
 */
void cycle_windows() {
    Window root_ret, parent_ret, *children;
    unsigned int nchildren;

    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        int target_index = -1;

        for (unsigned int i = 0; i < nchildren; i++) {
            if (children[i] != bar_win) {
                target_index = i;
                break;
            }
        }

        if (target_index != -1) {
            Window target = children[target_index];
            XMapWindow(dpy, target);
            XRaiseWindow(dpy, target);
            XRaiseWindow(dpy, bar_win);

            Window client = find_client_in_frame(target);
            XSetInputFocus(dpy, client ? client : target, RevertToPointerRoot, CurrentTime);
            focus_window = client ? client : target;
            update_bar();
        }

        if (children) {
            XFree(children);
        }
    }
}

/*
 * Handle X11 server errors gracefully
 */
int x_error_handler(Display *d, XErrorEvent *e) {
    return 0;
}

/*
 * Main event loop - handles all window manager events
 */
int main() {
    load_config();

    if (!(dpy = XOpenDisplay(0x0))) {
        return 1;
    }

    XSetErrorHandler(x_error_handler);

    root = DefaultRootWindow(dpy);
    init_hints();

    int screen = DefaultScreen(dpy);
    int sw = DisplayWidth(dpy, screen);

    font_info = XLoadQueryFont(dpy, conf.font_name);
    if (!font_info) {
        font_info = XLoadQueryFont(dpy, "fixed");
    }

    bar_win = XCreateSimpleWindow(dpy, root, 0, 0, sw, BAR_HEIGHT, 0, 0,
                                  get_pixel(conf.bar_color));
    XSelectInput(dpy, bar_win, ExposureMask);
    XMapWindow(dpy, bar_win);

    Cursor cursor = XCreateFontCursor(dpy, XC_left_ptr);
    XDefineCursor(dpy, root, cursor);

    /* Background removed so feh can work */
    /* XSetWindowBackground(dpy, root, get_pixel(conf.bg_color)); */
    /* XClearWindow(dpy, root); */

    XSelectInput(dpy, root, SubstructureRedirectMask | KeyPressMask);

    for (int i = 0; i < bind_count; i++) {
        XGrabKey(dpy, XKeysymToKeycode(dpy, binds[i].key), binds[i].mod,
                 root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, XKeysymToKeycode(dpy, binds[i].key),
                 binds[i].mod | Mod2Mask, root, True, GrabModeAsync, GrabModeAsync);
    }

    XGrabButton(dpy, Button3, Mod1Mask, root, True, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, mouse_mod_mask, root, True, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);

    signal(SIGCHLD, SIG_IGN);
    int x11_fd = ConnectionNumber(dpy);
    XEvent ev;

    while (1) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            if (ev.type == MapRequest) {
                frame_window(ev.xmaprequest.window);

            } else if (ev.type == UnmapNotify) {
                Window frame = get_frame(ev.xunmap.window);
                if (frame) {
                    XWindowAttributes attr;
                    XGetWindowAttributes(dpy, frame, &attr);
                    if (attr.map_state == IsViewable) {
                        XDestroyWindow(dpy, frame);
                        remove_client(ev.xunmap.window);
                        update_client_list();
                    }
                }

            } else if (ev.type == DestroyNotify) {
                Window frame = get_frame(ev.xdestroywindow.window);
                if (frame) {
                    XDestroyWindow(dpy, frame);
                    remove_client(ev.xdestroywindow.window);
                    update_client_list();
                } else {
                    for (int i = 0; i < client_count; i++) {
                        if (clients[i].frame == ev.xdestroywindow.window) {
                            remove_client(clients[i].client);
                            break;
                        }
                    }
                }

            } else if (ev.type == ClientMessage) {
                if (ev.xclient.message_type == wmatoms[NET_WM_STATE]) {
                    if ((Atom)ev.xclient.data.l[1] == wmatoms[NET_WM_STATE_FULLSCREEN] ||
                        (Atom)ev.xclient.data.l[2] == wmatoms[NET_WM_STATE_FULLSCREEN]) {
                        toggle_fullscreen(ev.xclient.window);
                    }
                } else if (ev.xclient.message_type == wmatoms[NET_ACTIVE_WINDOW]) {
                    Window frame = get_frame(ev.xclient.window);
                    if (frame) {
                        XRaiseWindow(dpy, frame);
                        XSetInputFocus(dpy, ev.xclient.window, RevertToPointerRoot, CurrentTime);
                        focus_window = ev.xclient.window;
                        set_active_window(ev.xclient.window);
                        update_bar();
                    }
                }

            } else if (ev.type == KeyPress) {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                unsigned int st = ev.xkey.state & (Mod1Mask | Mod4Mask | ShiftMask | ControlMask);

                for (int i = 0; i < bind_count; i++) {
                    if (ks == binds[i].key && st == binds[i].mod) {
                        if (strcasecmp(binds[i].command, "quit") == 0) {
                            XCloseDisplay(dpy);
                            exit(0);
                        } else if (strcasecmp(binds[i].command, "menu") == 0) {
                            show_hidden_menu();
                        } else if (strcasecmp(binds[i].command, "cycle") == 0) {
                            cycle_windows();
                        } else if (strcasecmp(binds[i].command, "unhide") == 0) {
                            unhide_all();
                        } else if (strcasecmp(binds[i].command, "close") == 0) {
                            if (focus_window) {
                                XDestroyWindow(dpy, focus_window);
                            }
                        } else {
                            spawn(binds[i].command);
                        }
                    }
                }

            } else if (ev.type == EnterNotify) {
                if (ev.xcrossing.window != root && ev.xcrossing.window != bar_win) {
                    Window client = find_client_in_frame(ev.xcrossing.window);
                    if (client) {
                        focus_window = client;
                        XSetInputFocus(dpy, focus_window, RevertToPointerRoot, CurrentTime);
                        set_active_window(focus_window);
                        update_bar();
                    }
                }

            } else if (ev.type == Expose && ev.xexpose.count == 0) {
                if (ev.xexpose.window == bar_win) {
                    update_bar();
                } else {
                    Window client = find_client_in_frame(ev.xexpose.window);
                    if (client) {
                        XWindowAttributes fa;
                        XGetWindowAttributes(dpy, ev.xexpose.window, &fa);
                        draw_decorations(ev.xexpose.window, fa.width, fa.height - TITLE_HEIGHT);
                    }
                }

            } else if (ev.type == ButtonPress) {
                Window parent_frame = 0;
                Window root_r, parent_r, *kids;
                unsigned int n_kids;

                if (XQueryTree(dpy, ev.xbutton.window, &root_r, &parent_r, &kids, &n_kids)) {
                    if (parent_r != root && parent_r != 0) {
                        parent_frame = parent_r;
                    } else if (ev.xbutton.window != root) {
                        parent_frame = ev.xbutton.window;
                    }
                    if (kids) {
                        XFree(kids);
                    }
                }

                int is_fs = 0;
                Window client = find_client_in_frame(parent_frame);
                for (int i = 0; i < client_count; i++) {
                    if (clients[i].client == client && clients[i].is_fullscreen) {
                        is_fs = 1;
                    }
                }

                /*
                 * GrabModeSync fix - prevents URxvt freezing
                 */
                if (!is_fs && (ev.xbutton.state & mouse_mod_mask)) {
                    if (ev.xbutton.button == Button1 || ev.xbutton.button == Button3) {
                        XAllowEvents(dpy, AsyncPointer, CurrentTime);

                        if (ev.xbutton.subwindow != None && ev.xbutton.subwindow != bar_win) {
                            XWindowAttributes attr;
                            XGetWindowAttributes(dpy, ev.xbutton.subwindow, &attr);
                            start_ev = ev.xbutton;
                            start_ev.window = ev.xbutton.subwindow;
                            drag_state.start_root_x = ev.xbutton.x_root;
                            drag_state.start_root_y = ev.xbutton.y_root;
                            drag_state.win_x = attr.x;
                            drag_state.win_y = attr.y;
                            drag_state.win_w = attr.width;
                            drag_state.win_h = attr.height;

                            int rel_x = ev.xbutton.x_root - attr.x;
                            int rel_y = ev.xbutton.y_root - attr.y;

                            if (rel_x < attr.width / 3) {
                                drag_state.resize_x_dir = -1;
                            } else if (rel_x > attr.width * 2 / 3) {
                                drag_state.resize_x_dir = 1;
                            } else {
                                drag_state.resize_x_dir = 0;
                            }

                            if (rel_y < attr.height / 3) {
                                drag_state.resize_y_dir = -1;
                            } else if (rel_y > attr.height * 2 / 3) {
                                drag_state.resize_y_dir = 1;
                            } else {
                                drag_state.resize_y_dir = 0;
                            }

                            XGrabPointer(dpy, root, False, ButtonMotionMask | ButtonReleaseMask,
                                        GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                            XRaiseWindow(dpy, start_ev.window);
                        }
                    }

                } else if (!is_fs && ev.xbutton.window != root && ev.xbutton.window != bar_win &&
                          ev.xbutton.y < TITLE_HEIGHT && ev.xbutton.button == Button1) {
                    /*
                     * Drag by title bar
                     */
                    XAllowEvents(dpy, AsyncPointer, CurrentTime);
                    XWindowAttributes fa;
                    XGetWindowAttributes(dpy, ev.xbutton.window, &fa);
                    int btn_w = TITLE_HEIGHT;

                    if (ev.xbutton.x < btn_w) {
                        XDestroyWindow(dpy, ev.xbutton.window);
                    } else if (ev.xbutton.x > (fa.width - btn_w)) {
                        XUnmapWindow(dpy, ev.xbutton.window);
                    } else {
                        XRaiseWindow(dpy, ev.xbutton.window);
                        XWindowAttributes attr;
                        XGetWindowAttributes(dpy, ev.xbutton.window, &attr);
                        drag_state.start_root_x = ev.xbutton.x_root;
                        drag_state.start_root_y = ev.xbutton.y_root;
                        drag_state.win_x = attr.x;
                        drag_state.win_y = attr.y;
                        start_ev = ev.xbutton;
                        XGrabPointer(dpy, root, False, ButtonMotionMask | ButtonReleaseMask,
                                    GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                    }

                } else {
                    /*
                     * Regular click - replay pointer to allow window to handle it
                     */
                    if (parent_frame != 0 && parent_frame != bar_win) {
                        XRaiseWindow(dpy, parent_frame);
                    }
                    XAllowEvents(dpy, ReplayPointer, CurrentTime);
                }

            } else if (ev.type == MotionNotify && start_ev.window) {
                while (XCheckTypedEvent(dpy, MotionNotify, &ev));

                int xdiff = ev.xbutton.x_root - drag_state.start_root_x;
                int ydiff = ev.xbutton.y_root - drag_state.start_root_y;

                if (start_ev.button == Button3) {
                    int new_x = drag_state.win_x;
                    int new_y = drag_state.win_y;
                    int new_w = drag_state.win_w;
                    int new_h = drag_state.win_h;

                    if (drag_state.resize_x_dir == 1) {
                        new_w += xdiff;
                    } else if (drag_state.resize_x_dir == -1) {
                        new_w -= xdiff;
                        new_x += xdiff;
                    }

                    if (drag_state.resize_y_dir == 1) {
                        new_h += ydiff;
                    } else if (drag_state.resize_y_dir == -1) {
                        new_h -= ydiff;
                        new_y += ydiff;
                    }

                    if (new_w < MIN_SIZE) {
                        new_w = MIN_SIZE;
                        if (drag_state.resize_x_dir == -1) {
                            new_x = drag_state.win_x + (drag_state.win_w - MIN_SIZE);
                        }
                    }

                    if (new_h < MIN_SIZE) {
                        new_h = MIN_SIZE;
                        if (drag_state.resize_y_dir == -1) {
                            new_y = drag_state.win_y + (drag_state.win_h - MIN_SIZE);
                        }
                    }

                    XMoveResizeWindow(dpy, start_ev.window, new_x, new_y, new_w, new_h);
                    Window client = find_client_in_frame(start_ev.window);
                    if (client) {
                        XResizeWindow(dpy, client, new_w, new_h - TITLE_HEIGHT);
                    }

                } else if (start_ev.button == Button1) {
                    XMoveWindow(dpy, start_ev.window, drag_state.win_x + xdiff,
                               drag_state.win_y + ydiff);
                }

            } else if (ev.type == ButtonRelease) {
                if (start_ev.window) {
                    XUngrabPointer(dpy, CurrentTime);
                    start_ev.window = 0;
                }
            }
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(x11_fd, &fds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (select(x11_fd + 1, &fds, NULL, NULL, &tv) == 0 || XPending(dpy) == 0) {
            update_bar();
        }
    }

    return 0;
}
