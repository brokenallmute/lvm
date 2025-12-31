#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xinerama.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <strings.h>

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
    WM_PROTOCOLS,
    WM_DELETE_WINDOW,
    ATOM_LAST
};

Atom wmatoms[ATOM_LAST];

struct Config {
    char bar_color[16];
    char bg_color[16];
    char border_color[16];
    char active_border_color[16];
    char button_color[16];
    char text_color[16];
    char line_color[16];
    char highlight_color[16];
    char font_name[64];
    char mouse_mod[16];
    int border_width;
} conf;

typedef struct {
    unsigned int mod;
    KeySym key;
    char command[128];
} KeyBind;

KeyBind binds[128];
int bind_count = 0;
unsigned int mouse_mod_mask = Mod1Mask;

#define CLEANMASK(mask) (mask & (Mod1Mask | Mod4Mask | ShiftMask | ControlMask))

#define TITLE_HEIGHT           26
#define BAR_HEIGHT             26
#define MENU_ITEM_H            36
#define MIN_SIZE               60
#define MAX_CLIENTS            256
#define MAX_MONITORS           8
#define DEFAULT_WINDOW_WIDTH   800
#define DEFAULT_WINDOW_HEIGHT  500
#define BUTTON_PADDING         8
#define ALT_TAB_WIDTH          500
#define ALT_TAB_ITEM_H         44
#define ALT_TAB_PADDING        6

typedef struct {
    int x, y, w, h;
    Window bar_win;
} Monitor;

Monitor monitors[MAX_MONITORS];
int monitor_count = 0;

Display *dpy;
Window root;
Window check_win;
XFontStruct *font_info;
Window focus_window = 0;
int running = 1;
int active_monitor = 0;

typedef struct {
    Window frame;
    Window client;
    int is_fullscreen;
    int monitor;
    XWindowAttributes old_attr;
} ClientState;

ClientState clients[MAX_CLIENTS];
int client_count = 0;

typedef struct {
    int start_root_x, start_root_y;
    int win_x, win_y;
    int win_w, win_h;
    int resize_x_dir;
    int resize_y_dir;
} DragState;

DragState drag_state;
XButtonEvent start_ev = {0};

typedef struct {
    Window *frames;
    Window *clients;
    char **names;
    int *is_hidden;
    int count;
    int selected;
    Window menu_win;
    GC gc;
    int active;
    int keyboard_grabbed;
} AltTabState;

AltTabState alt_tab = {0};

unsigned long get_pixel(const char *color_hex) {
    if (!dpy || !color_hex) return 0;
    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    XColor color;
    if (!XParseColor(dpy, cmap, color_hex, &color)) return 0;
    if (!XAllocColor(dpy, cmap, &color)) return 0;
    return color.pixel;
}

unsigned int str_to_mod(const char *str) {
    unsigned int mod = 0;
    if (!str) return 0;
    if (strstr(str, "Mod1")) mod |= Mod1Mask;
    if (strstr(str, "Mod4")) mod |= Mod4Mask;
    if (strstr(str, "Shift")) mod |= ShiftMask;
    if (strstr(str, "Control")) mod |= ControlMask;
    return mod;
}

int get_monitor_at(int x, int y) {
    for (int i = 0; i < monitor_count; i++) {
        if (x >= monitors[i].x && x < monitors[i].x + monitors[i].w &&
            y >= monitors[i].y && y < monitors[i].y + monitors[i].h) {
            return i;
        }
    }
    return 0;
}

int get_monitor_for_window(Window win) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, win, &attr)) return 0;
    int cx = attr.x + attr.width / 2;
    int cy = attr.y + attr.height / 2;
    return get_monitor_at(cx, cy);
}

void detect_monitors(void) {
    monitor_count = 0;
    
    if (XineramaIsActive(dpy)) {
        int count;
        XineramaScreenInfo *info = XineramaQueryScreens(dpy, &count);
        if (info) {
            for (int i = 0; i < count && i < MAX_MONITORS; i++) {
                monitors[i].x = info[i].x_org;
                monitors[i].y = info[i].y_org;
                monitors[i].w = info[i].width;
                monitors[i].h = info[i].height;
                monitors[i].bar_win = 0;
                monitor_count++;
            }
            XFree(info);
        }
    }
    
    if (monitor_count == 0) {
        monitors[0].x = 0;
        monitors[0].y = 0;
        monitors[0].w = DisplayWidth(dpy, DefaultScreen(dpy));
        monitors[0].h = DisplayHeight(dpy, DefaultScreen(dpy));
        monitors[0].bar_win = 0;
        monitor_count = 1;
    }
}

void create_bars(void) {
    for (int i = 0; i < monitor_count; i++) {
        monitors[i].bar_win = XCreateSimpleWindow(dpy, root,
            monitors[i].x, monitors[i].y,
            monitors[i].w, BAR_HEIGHT,
            0, 0, get_pixel(conf.bar_color));
        XSelectInput(dpy, monitors[i].bar_win, ExposureMask | ButtonPressMask);
        XMapWindow(dpy, monitors[i].bar_win);
    }
}

void destroy_bars(void) {
    for (int i = 0; i < monitor_count; i++) {
        if (monitors[i].bar_win) {
            XDestroyWindow(dpy, monitors[i].bar_win);
            monitors[i].bar_win = 0;
        }
    }
}

int is_bar_window(Window win) {
    for (int i = 0; i < monitor_count; i++) {
        if (monitors[i].bar_win == win) return 1;
    }
    return 0;
}

void create_default_config(const char *path) {
    char dir_path[256];
    const char *home = getenv("HOME");
    if (!home) return;
    
    snprintf(dir_path, sizeof(dir_path), "%s/.config", home);
    mkdir(dir_path, 0755);

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "BAR_COLOR           #4C837E\n");
    fprintf(f, "BG_COLOR            #83A597\n");
    fprintf(f, "BORDER_COLOR        #555555\n");
    fprintf(f, "ACTIVE_BORDER_COLOR #4C837E\n");
    fprintf(f, "BUTTON_COLOR        #e8e4cf\n");
    fprintf(f, "TEXT_COLOR          #FFFFFF\n");
    fprintf(f, "LINE_COLOR          #FFFFFF\n");
    fprintf(f, "HIGHLIGHT_COLOR     #6CA39E\n");
    fprintf(f, "FONT                fixed\n");
    fprintf(f, "MOUSE_MOD           Mod1\n");
    fprintf(f, "BORDER_WIDTH        1\n");
    fprintf(f, "BIND Mod4 Return xterm\n");
    fprintf(f, "BIND Mod4 d dmenu_run\n");
    fprintf(f, "BIND Mod1 Tab alttab\n");
    fprintf(f, "BIND Mod4 Tab menu\n");
    fprintf(f, "BIND Mod4 q quit\n");
    fprintf(f, "BIND Mod4 c close\n");
    fprintf(f, "BIND Mod4 f fullscreen\n");
    fprintf(f, "BIND Mod4 u unhide\n");
    fprintf(f, "BIND Mod4 Left snap_left\n");
    fprintf(f, "BIND Mod4 Right snap_right\n");
    fprintf(f, "BIND Mod4 Up maximize\n");
    fprintf(f, "BIND Mod4 Down restore\n");

    fclose(f);
}

void load_config(void) {
    strncpy(conf.bar_color, "#4C837E", sizeof(conf.bar_color) - 1);
    strncpy(conf.bg_color, "#83A597", sizeof(conf.bg_color) - 1);
    strncpy(conf.border_color, "#555555", sizeof(conf.border_color) - 1);
    strncpy(conf.active_border_color, "#4C837E", sizeof(conf.active_border_color) - 1);
    strncpy(conf.button_color, "#e8e4cf", sizeof(conf.button_color) - 1);
    strncpy(conf.text_color, "#FFFFFF", sizeof(conf.text_color) - 1);
    strncpy(conf.line_color, "#FFFFFF", sizeof(conf.line_color) - 1);
    strncpy(conf.highlight_color, "#6CA39E", sizeof(conf.highlight_color) - 1);
    strncpy(conf.font_name, "fixed", sizeof(conf.font_name) - 1);
    strncpy(conf.mouse_mod, "Mod1", sizeof(conf.mouse_mod) - 1);
    conf.border_width = 1;

    char path[256];
    const char *home = getenv("HOME");
    if (!home) return;
    
    snprintf(path, sizeof(path), "%s/.config/lwm.conf", home);

    if (access(path, F_OK) != 0) {
        create_default_config(path);
    }

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || line[0] == '\0') continue;

        char key[64], val[64];
        if (sscanf(line, "%63s %63s", key, val) == 2) {
            if (strcmp(key, "BAR_COLOR") == 0)
                strncpy(conf.bar_color, val, sizeof(conf.bar_color) - 1);
            else if (strcmp(key, "BG_COLOR") == 0)
                strncpy(conf.bg_color, val, sizeof(conf.bg_color) - 1);
            else if (strcmp(key, "BORDER_COLOR") == 0)
                strncpy(conf.border_color, val, sizeof(conf.border_color) - 1);
            else if (strcmp(key, "ACTIVE_BORDER_COLOR") == 0)
                strncpy(conf.active_border_color, val, sizeof(conf.active_border_color) - 1);
            else if (strcmp(key, "BUTTON_COLOR") == 0)
                strncpy(conf.button_color, val, sizeof(conf.button_color) - 1);
            else if (strcmp(key, "TEXT_COLOR") == 0)
                strncpy(conf.text_color, val, sizeof(conf.text_color) - 1);
            else if (strcmp(key, "LINE_COLOR") == 0)
                strncpy(conf.line_color, val, sizeof(conf.line_color) - 1);
            else if (strcmp(key, "HIGHLIGHT_COLOR") == 0)
                strncpy(conf.highlight_color, val, sizeof(conf.highlight_color) - 1);
            else if (strcmp(key, "FONT") == 0)
                strncpy(conf.font_name, val, sizeof(conf.font_name) - 1);
            else if (strcmp(key, "MOUSE_MOD") == 0)
                strncpy(conf.mouse_mod, val, sizeof(conf.mouse_mod) - 1);
            else if (strcmp(key, "BORDER_WIDTH") == 0)
                conf.border_width = atoi(val);
        }

        char mod_str[32], key_str[32], cmd[128];
        if (sscanf(line, "BIND %31s %31s %127[^\t\n]", mod_str, key_str, cmd) == 3) {
            if (bind_count < 128) {
                binds[bind_count].mod = str_to_mod(mod_str);
                binds[bind_count].key = XStringToKeysym(key_str);
                if (binds[bind_count].key != NoSymbol) {
                    strncpy(binds[bind_count].command, cmd, sizeof(binds[bind_count].command) - 1);
                    bind_count++;
                }
            }
        }
    }

    fclose(f);

    mouse_mod_mask = str_to_mod(conf.mouse_mod);
    if (mouse_mod_mask == 0) mouse_mod_mask = Mod1Mask;
}

void add_client(Window client, Window frame, int monitor) {
    if (client_count >= MAX_CLIENTS) return;
    
    clients[client_count].client = client;
    clients[client_count].frame = frame;
    clients[client_count].is_fullscreen = 0;
    clients[client_count].monitor = monitor;
    memset(&clients[client_count].old_attr, 0, sizeof(XWindowAttributes));
    client_count++;
}

void remove_client(Window client) {
    int idx = -1;
    for (int i = 0; i < client_count; i++) {
        if (clients[i].client == client) {
            idx = i;
            break;
        }
    }
    if (idx >= 0) {
        for (int i = idx; i < client_count - 1; i++) {
            clients[i] = clients[i + 1];
        }
        client_count--;
    }
}

Window get_frame(Window client) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].client == client) return clients[i].frame;
    }
    return 0;
}

ClientState *get_client_state(Window client) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].client == client) return &clients[i];
    }
    return NULL;
}

ClientState *get_client_state_by_frame(Window frame) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].frame == frame) return &clients[i];
    }
    return NULL;
}

Window find_client_in_frame(Window frame) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].frame == frame) return clients[i].client;
    }
    return 0;
}

void init_hints(void) {
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
    wmatoms[WM_PROTOCOLS] = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wmatoms[WM_DELETE_WINDOW] = XInternAtom(dpy, "WM_DELETE_WINDOW", False);

    XChangeProperty(dpy, root, wmatoms[NET_SUPPORTED], XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)wmatoms, ATOM_LAST);

    check_win = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, check_win, wmatoms[NET_CHECK], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&check_win, 1);
    XChangeProperty(dpy, check_win, wmatoms[NET_WM_NAME], 
                    XInternAtom(dpy, "UTF8_STRING", False),
                    8, PropModeReplace, (unsigned char *)"lwm", 3);
    XChangeProperty(dpy, root, wmatoms[NET_CHECK], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&check_win, 1);
}

void update_client_list(void) {
    if (client_count == 0) {
        XDeleteProperty(dpy, root, wmatoms[NET_CLIENT_LIST]);
        return;
    }
    
    Window *list = malloc(sizeof(Window) * client_count);
    if (!list) return;
    
    for (int i = 0; i < client_count; i++) {
        list[i] = clients[i].client;
    }
    
    XChangeProperty(dpy, root, wmatoms[NET_CLIENT_LIST], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)list, client_count);
    free(list);
}

void set_active_window(Window w) {
    XChangeProperty(dpy, root, wmatoms[NET_ACTIVE_WINDOW], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&w, 1);
}

void spawn(const char *command) {
    if (!command) return;
    
    pid_t pid = fork();
    if (pid == 0) {
        if (dpy) close(ConnectionNumber(dpy));
        setsid();
        execl("/bin/sh", "sh", "-c", command, NULL);
        _exit(127);
    }
}

void close_client(Window client) {
    if (!client) return;

    Atom *protocols = NULL;
    int n = 0;
    int supports_delete = 0;

    if (XGetWMProtocols(dpy, client, &protocols, &n)) {
        for (int i = 0; i < n; i++) {
            if (protocols[i] == wmatoms[WM_DELETE_WINDOW]) {
                supports_delete = 1;
                break;
            }
        }
        if (protocols) XFree(protocols);
    }

    if (supports_delete) {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ClientMessage;
        ev.xclient.window = client;
        ev.xclient.message_type = wmatoms[WM_PROTOCOLS];
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = wmatoms[WM_DELETE_WINDOW];
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, client, False, NoEventMask, &ev);
    } else {
        XKillClient(dpy, client);
    }
}

void toggle_fullscreen(Window client) {
    Window frame = get_frame(client);
    if (!frame) return;

    ClientState *cs = get_client_state(client);
    if (!cs) return;

    int mon = cs->monitor;
    if (mon < 0 || mon >= monitor_count) mon = 0;

    if (!cs->is_fullscreen) {
        XGetWindowAttributes(dpy, frame, &cs->old_attr);
        XMoveResizeWindow(dpy, frame, 
                          monitors[mon].x, monitors[mon].y,
                          monitors[mon].w, monitors[mon].h);
        XResizeWindow(dpy, client, monitors[mon].w, monitors[mon].h);
        XRaiseWindow(dpy, frame);
        cs->is_fullscreen = 1;
    } else {
        XMoveResizeWindow(dpy, frame, 
                          cs->old_attr.x, cs->old_attr.y,
                          cs->old_attr.width, cs->old_attr.height);
        XResizeWindow(dpy, client, 
                      cs->old_attr.width, 
                      cs->old_attr.height - TITLE_HEIGHT);
        cs->is_fullscreen = 0;
    }
}

void snap_window(Window client, int direction) {
    Window frame = get_frame(client);
    if (!frame) return;

    ClientState *cs = get_client_state(client);
    if (!cs || cs->is_fullscreen) return;

    int mon = cs->monitor;
    if (mon < 0 || mon >= monitor_count) mon = 0;

    int mx = monitors[mon].x;
    int my = monitors[mon].y + BAR_HEIGHT;
    int mw = monitors[mon].w;
    int mh = monitors[mon].h - BAR_HEIGHT;

    XGetWindowAttributes(dpy, frame, &cs->old_attr);

    int x, y, w, h;

    switch (direction) {
        case 0:
            x = mx;
            y = my;
            w = mw / 2;
            h = mh;
            break;
        case 1:
            x = mx + mw / 2;
            y = my;
            w = mw / 2;
            h = mh;
            break;
        case 2:
            x = mx;
            y = my;
            w = mw;
            h = mh;
            break;
        case 3:
            x = cs->old_attr.x;
            y = cs->old_attr.y;
            w = cs->old_attr.width;
            h = cs->old_attr.height;
            break;
        default:
            return;
    }

    XMoveResizeWindow(dpy, frame, x, y, w, h);
    XResizeWindow(dpy, client, w, h - TITLE_HEIGHT);
}

void raise_bars(void) {
    for (int i = 0; i < monitor_count; i++) {
        if (monitors[i].bar_win) {
            XRaiseWindow(dpy, monitors[i].bar_win);
        }
    }
}

void update_bar(int mon) {
    if (!dpy || !font_info || mon < 0 || mon >= monitor_count) return;

    Window bar = monitors[mon].bar_win;
    if (!bar) return;

    GC gc = XCreateGC(dpy, bar, 0, NULL);
    XSetFont(dpy, gc, font_info->fid);

    int w = monitors[mon].w;

    XSetForeground(dpy, gc, get_pixel(conf.bar_color));
    XFillRectangle(dpy, bar, gc, 0, 0, w, BAR_HEIGHT);

    char buffer[256];
    char time_str[64];
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    if (tm_info) {
        strftime(time_str, sizeof(time_str), "%H:%M | %d/%m", tm_info);
    } else {
        strcpy(time_str, "--:--");
    }

    struct sysinfo info;
    unsigned long long used = 0;
    if (sysinfo(&info) == 0) {
        unsigned long long total = (unsigned long long)info.totalram * info.mem_unit;
        unsigned long long free_r = (unsigned long long)info.freeram * info.mem_unit;
        used = (total - free_r) / 1024 / 1024;
    }

    char *win_name = NULL;
    if (focus_window) {
        XFetchName(dpy, focus_window, &win_name);
    }

    if (monitor_count > 1) {
        snprintf(buffer, sizeof(buffer), "[%d] %s || %s | RAM: %lluMB",
                 mon + 1,
                 win_name ? win_name : "Desktop", 
                 time_str, used);
    } else {
        snprintf(buffer, sizeof(buffer), "%s || %s | RAM: %lluMB",
                 win_name ? win_name : "Desktop", 
                 time_str, used);
    }

    if (win_name) XFree(win_name);

    XSetForeground(dpy, gc, get_pixel(conf.text_color));
    int text_y = (BAR_HEIGHT / 2) + (font_info->ascent / 2) - 1;
    XDrawString(dpy, bar, gc, 8, text_y, buffer, strlen(buffer));

    XSetForeground(dpy, gc, get_pixel(conf.line_color));
    XDrawLine(dpy, bar, gc, 0, BAR_HEIGHT - 1, w, BAR_HEIGHT - 1);

    if (mon == active_monitor) {
        XSetForeground(dpy, gc, get_pixel(conf.active_border_color));
        XFillRectangle(dpy, bar, gc, 0, 0, 4, BAR_HEIGHT);
    }

    XFreeGC(dpy, gc);
}

void update_all_bars(void) {
    for (int i = 0; i < monitor_count; i++) {
        update_bar(i);
    }
}

void draw_decorations(Window frame, int width, int height) {
    ClientState *cs = get_client_state_by_frame(frame);
    if (cs && cs->is_fullscreen) return;
    if (!dpy) return;

    GC gc = XCreateGC(dpy, frame, 0, NULL);

    int is_focused = (cs && cs->client == focus_window);
    unsigned long bar_px = get_pixel(conf.bar_color);
    unsigned long btn_px = get_pixel(conf.button_color);
    unsigned long bdr_px = is_focused ? 
        get_pixel(conf.active_border_color) : get_pixel(conf.border_color);
    unsigned long line_px = get_pixel(conf.line_color);

    XSetForeground(dpy, gc, bar_px);
    XFillRectangle(dpy, frame, gc, 0, 0, width, TITLE_HEIGHT);

    XSetForeground(dpy, gc, bdr_px);
    XDrawRectangle(dpy, frame, gc, 0, 0, width - 1, height + TITLE_HEIGHT - 1);
    
    XSetForeground(dpy, gc, line_px);
    XDrawLine(dpy, frame, gc, 0, TITLE_HEIGHT - 1, width, TITLE_HEIGHT - 1);

    int btn = TITLE_HEIGHT;
    int p = BUTTON_PADDING;

    XSetForeground(dpy, gc, btn_px);
    XFillRectangle(dpy, frame, gc, 0, 0, btn, btn);
    XSetForeground(dpy, gc, bdr_px);
    XDrawRectangle(dpy, frame, gc, 0, 0, btn, btn);
    XDrawLine(dpy, frame, gc, p, p, btn - p, btn - p);
    XDrawLine(dpy, frame, gc, p, btn - p, btn - p, p);

    int xr = width - btn;
    XSetForeground(dpy, gc, btn_px);
    XFillRectangle(dpy, frame, gc, xr, 0, btn, btn);
    XSetForeground(dpy, gc, bdr_px);
    XDrawRectangle(dpy, frame, gc, xr, 0, btn, btn);
    int cx = xr + btn / 2;
    int cy = btn / 2 + 3;
    XDrawLine(dpy, frame, gc, xr + 8, 10, cx, cy);
    XDrawLine(dpy, frame, gc, xr + btn - 8, 10, cx, cy);

    if (cs && font_info) {
        char *name = NULL;
        XFetchName(dpy, cs->client, &name);
        if (name) {
            XSetForeground(dpy, gc, get_pixel(conf.text_color));
            XSetFont(dpy, gc, font_info->fid);
            int ty = TITLE_HEIGHT / 2 + font_info->ascent / 2 - 1;
            int max_w = width - btn * 2 - 20;
            
            char display[256];
            strncpy(display, name, sizeof(display) - 1);
            display[sizeof(display) - 1] = '\0';
            
            size_t len = strlen(display);
            while (len > 3 && XTextWidth(font_info, display, len) > max_w) {
                display[len - 4] = '.';
                display[len - 3] = '.';
                display[len - 2] = '.';
                display[len - 1] = '\0';
                len--;
            }
            
            XDrawString(dpy, frame, gc, btn + 8, ty, display, strlen(display));
            XFree(name);
        }
    }

    XFreeGC(dpy, gc);
}

void frame_window(Window client) {
    if (!dpy || !client) return;
    if (get_frame(client)) return;

    XWindowAttributes attrs;
    if (XGetWindowAttributes(dpy, client, &attrs) == 0) return;
    if (attrs.override_redirect) return;

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    Atom *prop = NULL;
    int should_frame = 1;

    if (XGetWindowProperty(dpy, client, wmatoms[NET_WM_WINDOW_TYPE], 0, 1, False, XA_ATOM,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           (unsigned char **)&prop) == Success && prop) {
        Atom type = prop[0];
        if (type == wmatoms[NET_WM_WINDOW_TYPE_DOCK] ||
            type == wmatoms[NET_WM_WINDOW_TYPE_MENU] ||
            type == wmatoms[NET_WM_WINDOW_TYPE_SPLASH] ||
            type == wmatoms[NET_WM_WINDOW_TYPE_NOTIFICATION] ||
            type == wmatoms[NET_WM_WINDOW_TYPE_UTILITY]) {
            should_frame = 0;
        }
        XFree(prop);
    }

    if (!should_frame) {
        XMapWindow(dpy, client);
        add_client(client, 0, 0);
        update_client_list();
        return;
    }

    Window pointer_root, pointer_child;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    XQueryPointer(dpy, root, &pointer_root, &pointer_child,
                  &root_x, &root_y, &win_x, &win_y, &mask);
    
    int mon = get_monitor_at(root_x, root_y);

    int w = attrs.width;
    int h = attrs.height;
    if (w < MIN_SIZE || h < MIN_SIZE) {
        w = DEFAULT_WINDOW_WIDTH;
        h = DEFAULT_WINDOW_HEIGHT;
        XResizeWindow(dpy, client, w, h);
    }

    int mx = monitors[mon].x;
    int my = monitors[mon].y + BAR_HEIGHT;
    int mw = monitors[mon].w;
    int mh = monitors[mon].h - BAR_HEIGHT;

    int x = mx + (mw - w) / 2;
    int y = my + (mh - h) / 2;
    if (y < my) y = my;

    Window frame = XCreateSimpleWindow(dpy, root, x, y, w, h + TITLE_HEIGHT, 
                                       conf.border_width,
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
    XGrabButton(dpy, Button1, mouse_mod_mask | Mod2Mask, client, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, mouse_mod_mask | Mod2Mask, client, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);

    add_client(client, frame, mon);
    update_client_list();

    XSetInputFocus(dpy, client, RevertToPointerRoot, CurrentTime);
    focus_window = client;
    active_monitor = mon;
    set_active_window(client);
    update_all_bars();
}

void alt_tab_cleanup(void) {
    if (alt_tab.keyboard_grabbed) {
        XUngrabKeyboard(dpy, CurrentTime);
        alt_tab.keyboard_grabbed = 0;
    }
    if (alt_tab.gc) {
        XFreeGC(dpy, alt_tab.gc);
        alt_tab.gc = NULL;
    }
    if (alt_tab.menu_win) {
        XUnmapWindow(dpy, alt_tab.menu_win);
        XDestroyWindow(dpy, alt_tab.menu_win);
        alt_tab.menu_win = 0;
    }
    if (alt_tab.names) {
        for (int i = 0; i < alt_tab.count; i++) {
            if (alt_tab.names[i]) {
                free(alt_tab.names[i]);
            }
        }
        free(alt_tab.names);
        alt_tab.names = NULL;
    }
    if (alt_tab.frames) {
        free(alt_tab.frames);
        alt_tab.frames = NULL;
    }
    if (alt_tab.clients) {
        free(alt_tab.clients);
        alt_tab.clients = NULL;
    }
    if (alt_tab.is_hidden) {
        free(alt_tab.is_hidden);
        alt_tab.is_hidden = NULL;
    }
    alt_tab.count = 0;
    alt_tab.selected = 0;
    alt_tab.active = 0;
    XSync(dpy, False);
}

void alt_tab_draw(void) {
    if (!alt_tab.active || !alt_tab.menu_win || !alt_tab.gc) return;

    unsigned long bg_px = get_pixel(conf.bar_color);
    unsigned long hl_px = get_pixel(conf.highlight_color);
    unsigned long txt_px = get_pixel(conf.text_color);
    unsigned long bdr_px = get_pixel(conf.border_color);
    unsigned long dim_px = get_pixel("#888888");

    int menu_h = alt_tab.count * ALT_TAB_ITEM_H + ALT_TAB_PADDING * 2;

    XSetForeground(dpy, alt_tab.gc, bg_px);
    XFillRectangle(dpy, alt_tab.menu_win, alt_tab.gc, 0, 0, ALT_TAB_WIDTH, menu_h);

    for (int i = 0; i < alt_tab.count; i++) {
        int y = ALT_TAB_PADDING + i * ALT_TAB_ITEM_H;

        if (i == alt_tab.selected) {
            XSetForeground(dpy, alt_tab.gc, hl_px);
            XFillRectangle(dpy, alt_tab.menu_win, alt_tab.gc, 
                          ALT_TAB_PADDING, y, 
                          ALT_TAB_WIDTH - ALT_TAB_PADDING * 2, ALT_TAB_ITEM_H - 4);
        }

        XSetForeground(dpy, alt_tab.gc, 
                      (alt_tab.is_hidden && alt_tab.is_hidden[i]) ? dim_px : txt_px);
        
        int ty = y + (ALT_TAB_ITEM_H / 2) + (font_info->ascent / 2) - 2;

        char display[128];
        const char *name = (alt_tab.names && alt_tab.names[i]) ? 
                           alt_tab.names[i] : "(unnamed)";
        
        if (alt_tab.is_hidden && alt_tab.is_hidden[i]) {
            snprintf(display, sizeof(display), " %d.  [hidden] %s", i + 1, name);
        } else {
            snprintf(display, sizeof(display), " %d.  %s", i + 1, name);
        }

        int max_w = ALT_TAB_WIDTH - 40;
        size_t len = strlen(display);
        while (len > 3 && XTextWidth(font_info, display, len) > max_w) {
            display[len - 4] = '.';
            display[len - 3] = '.';
            display[len - 2] = '.';
            display[len - 1] = '\0';
            len--;
        }

        XDrawString(dpy, alt_tab.menu_win, alt_tab.gc, 15, ty, display, strlen(display));
    }

    XSetForeground(dpy, alt_tab.gc, bdr_px);
    XDrawRectangle(dpy, alt_tab.menu_win, alt_tab.gc, 0, 0, ALT_TAB_WIDTH - 1, menu_h - 1);
    XFlush(dpy);
}

int alt_tab_build_list(void) {
    int count = 0;
    for (int i = 0; i < client_count; i++) {
        if (clients[i].frame) count++;
    }

    if (count < 1) return 0;

    alt_tab.frames = calloc(count, sizeof(Window));
    alt_tab.clients = calloc(count, sizeof(Window));
    alt_tab.names = calloc(count, sizeof(char *));
    alt_tab.is_hidden = calloc(count, sizeof(int));
    
    if (!alt_tab.frames || !alt_tab.clients || !alt_tab.names || !alt_tab.is_hidden) {
        free(alt_tab.frames);
        free(alt_tab.clients);
        free(alt_tab.names);
        free(alt_tab.is_hidden);
        alt_tab.frames = NULL;
        alt_tab.clients = NULL;
        alt_tab.names = NULL;
        alt_tab.is_hidden = NULL;
        return 0;
    }

    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren;
    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        free(alt_tab.frames);
        free(alt_tab.clients);
        free(alt_tab.names);
        free(alt_tab.is_hidden);
        alt_tab.frames = NULL;
        alt_tab.clients = NULL;
        alt_tab.names = NULL;
        alt_tab.is_hidden = NULL;
        return 0;
    }

    int idx = 0;
    
    for (int i = (int)nchildren - 1; i >= 0 && idx < count; i--) {
        Window frame = children[i];
        if (is_bar_window(frame) || frame == check_win) continue;

        for (int j = 0; j < client_count; j++) {
            if (clients[j].frame == frame) {
                XWindowAttributes attr;
                if (XGetWindowAttributes(dpy, frame, &attr) && attr.map_state == IsViewable) {
                    alt_tab.frames[idx] = frame;
                    alt_tab.clients[idx] = clients[j].client;
                    alt_tab.is_hidden[idx] = 0;
                    
                    char *name = NULL;
                    XFetchName(dpy, clients[j].client, &name);
                    alt_tab.names[idx] = name ? strdup(name) : strdup("(unnamed)");
                    if (name) XFree(name);
                    idx++;
                }
                break;
            }
        }
    }

    for (int i = (int)nchildren - 1; i >= 0 && idx < count; i--) {
        Window frame = children[i];
        if (is_bar_window(frame) || frame == check_win) continue;

        for (int j = 0; j < client_count; j++) {
            if (clients[j].frame == frame) {
                XWindowAttributes attr;
                if (XGetWindowAttributes(dpy, frame, &attr) && attr.map_state == IsUnmapped) {
                    alt_tab.frames[idx] = frame;
                    alt_tab.clients[idx] = clients[j].client;
                    alt_tab.is_hidden[idx] = 1;
                    
                    char *name = NULL;
                    XFetchName(dpy, clients[j].client, &name);
                    alt_tab.names[idx] = name ? strdup(name) : strdup("(unnamed)");
                    if (name) XFree(name);
                    idx++;
                }
                break;
            }
        }
    }

    if (children) XFree(children);
    alt_tab.count = idx;
    alt_tab.selected = (idx > 1) ? 1 : 0;
    return idx;
}

void alt_tab_show(void) {
    if (alt_tab.active) {
        alt_tab.selected = (alt_tab.selected + 1) % alt_tab.count;
        alt_tab_draw();
        return;
    }

    int n = alt_tab_build_list();
    if (n < 1) return;

    if (n == 1) {
        if (alt_tab.is_hidden && alt_tab.is_hidden[0]) {
            XMapWindow(dpy, alt_tab.frames[0]);
        }
        XRaiseWindow(dpy, alt_tab.frames[0]);
        raise_bars();
        XSetInputFocus(dpy, alt_tab.clients[0], RevertToPointerRoot, CurrentTime);
        focus_window = alt_tab.clients[0];
        
        ClientState *cs = get_client_state(focus_window);
        if (cs) active_monitor = cs->monitor;
        
        set_active_window(focus_window);
        update_all_bars();
        
        if (alt_tab.names && alt_tab.names[0]) free(alt_tab.names[0]);
        free(alt_tab.frames);
        free(alt_tab.clients);
        free(alt_tab.names);
        free(alt_tab.is_hidden);
        alt_tab.frames = NULL;
        alt_tab.clients = NULL;
        alt_tab.names = NULL;
        alt_tab.is_hidden = NULL;
        alt_tab.count = 0;
        return;
    }

    int mon = active_monitor;
    if (mon < 0 || mon >= monitor_count) mon = 0;
    
    int menu_h = alt_tab.count * ALT_TAB_ITEM_H + ALT_TAB_PADDING * 2;
    int menu_x = monitors[mon].x + (monitors[mon].w - ALT_TAB_WIDTH) / 2;
    int menu_y = monitors[mon].y + (monitors[mon].h - menu_h) / 2;

    alt_tab.menu_win = XCreateSimpleWindow(dpy, root, menu_x, menu_y,
                                           ALT_TAB_WIDTH, menu_h, 2,
                                           get_pixel(conf.border_color),
                                           get_pixel(conf.bar_color));

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.save_under = True;
    XChangeWindowAttributes(dpy, alt_tab.menu_win, CWOverrideRedirect | CWSaveUnder, &swa);
    XSelectInput(dpy, alt_tab.menu_win, ExposureMask | KeyPressMask | KeyReleaseMask);
    XMapRaised(dpy, alt_tab.menu_win);
    XSync(dpy, False);

    alt_tab.gc = XCreateGC(dpy, alt_tab.menu_win, 0, NULL);
    XSetFont(dpy, alt_tab.gc, font_info->fid);

    int grabbed = 0;
    for (int retry = 0; retry < 50; retry++) {
        if (XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) {
            alt_tab.keyboard_grabbed = 1;
            grabbed = 1;
            break;
        }
        usleep(10000);
    }

    if (!grabbed) {
        XDestroyWindow(dpy, alt_tab.menu_win);
        XFreeGC(dpy, alt_tab.gc);
        alt_tab.menu_win = 0;
        alt_tab.gc = NULL;
        for (int i = 0; i < alt_tab.count; i++) {
            if (alt_tab.names && alt_tab.names[i]) free(alt_tab.names[i]);
        }
        free(alt_tab.frames);
        free(alt_tab.clients);
        free(alt_tab.names);
        free(alt_tab.is_hidden);
        alt_tab.frames = NULL;
        alt_tab.clients = NULL;
        alt_tab.names = NULL;
        alt_tab.is_hidden = NULL;
        alt_tab.count = 0;
        return;
    }

    alt_tab.active = 1;
    alt_tab_draw();
}

void alt_tab_confirm(void) {
    if (!alt_tab.active) return;

    Window frame = 0, client = 0;
    int was_hidden = 0;
    
    if (alt_tab.selected >= 0 && alt_tab.selected < alt_tab.count) {
        frame = alt_tab.frames[alt_tab.selected];
        client = alt_tab.clients[alt_tab.selected];
        was_hidden = alt_tab.is_hidden ? alt_tab.is_hidden[alt_tab.selected] : 0;
    }

    alt_tab_cleanup();
    
    if (frame && client) {
        if (was_hidden) XMapWindow(dpy, frame);
        XRaiseWindow(dpy, frame);
        raise_bars();
        XSetInputFocus(dpy, client, RevertToPointerRoot, CurrentTime);
        focus_window = client;
        
        ClientState *cs = get_client_state(client);
        if (cs) active_monitor = cs->monitor;
        
        set_active_window(client);
    }
    
    update_all_bars();
}

void alt_tab_cancel(void) {
    if (!alt_tab.active) return;
    alt_tab_cleanup();
}

void alt_tab_prev(void) {
    if (!alt_tab.active || alt_tab.count < 1) return;
    alt_tab.selected = (alt_tab.selected - 1 + alt_tab.count) % alt_tab.count;
    alt_tab_draw();
}

void alt_tab_next(void) {
    if (!alt_tab.active || alt_tab.count < 1) return;
    alt_tab.selected = (alt_tab.selected + 1) % alt_tab.count;
    alt_tab_draw();
}

int is_alt_pressed(void) {
    char keys[32];
    XQueryKeymap(dpy, keys);
    
    KeyCode alt_l = XKeysymToKeycode(dpy, XK_Alt_L);
    KeyCode alt_r = XKeysymToKeycode(dpy, XK_Alt_R);
    KeyCode meta_l = XKeysymToKeycode(dpy, XK_Meta_L);
    KeyCode meta_r = XKeysymToKeycode(dpy, XK_Meta_R);
    
    if (alt_l && (keys[alt_l / 8] & (1 << (alt_l % 8)))) return 1;
    if (alt_r && (keys[alt_r / 8] & (1 << (alt_r % 8)))) return 1;
    if (meta_l && (keys[meta_l / 8] & (1 << (meta_l % 8)))) return 1;
    if (meta_r && (keys[meta_r / 8] & (1 << (meta_r % 8)))) return 1;
    
    return 0;
}

void show_hidden_menu(void) {
    typedef struct { Window frame; char *name; } HiddenWin;
    HiddenWin hidden[64];
    int count = 0;

    for (int i = 0; i < client_count && count < 64; i++) {
        if (clients[i].frame) {
            XWindowAttributes attr;
            if (XGetWindowAttributes(dpy, clients[i].frame, &attr) && 
                attr.map_state == IsUnmapped) {
                char *name = NULL;
                XFetchName(dpy, clients[i].client, &name);
                hidden[count].frame = clients[i].frame;
                hidden[count].name = name ? strdup(name) : strdup("(unnamed)");
                if (name) XFree(name);
                count++;
            }
        }
    }

    if (count == 0) return;

    int mon = active_monitor;
    if (mon < 0 || mon >= monitor_count) mon = 0;
    
    int menu_w = 400;
    int menu_h = count * MENU_ITEM_H;
    int menu_x = monitors[mon].x + (monitors[mon].w - menu_w) / 2;
    int menu_y = monitors[mon].y + (monitors[mon].h - menu_h) / 2;

    Window menu = XCreateSimpleWindow(dpy, root, menu_x, menu_y, menu_w, menu_h, 2,
                                      get_pixel(conf.border_color), 
                                      get_pixel(conf.bar_color));

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    XChangeWindowAttributes(dpy, menu, CWOverrideRedirect, &swa);

    XSelectInput(dpy, menu, ExposureMask | PointerMotionMask | 
                 ButtonPressMask | KeyPressMask);
    XMapRaised(dpy, menu);
    XGrabPointer(dpy, menu, True, ButtonPressMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    XGrabKeyboard(dpy, menu, True, GrabModeAsync, GrabModeAsync, CurrentTime);

    GC gc = XCreateGC(dpy, menu, 0, NULL);
    XSetFont(dpy, gc, font_info->fid);

    int selected = 0;
    int done = 0;
    XEvent ev;

    while (!done) {
        XNextEvent(dpy, &ev);

        if (ev.type == Expose) {
            for (int i = 0; i < count; i++) {
                int y = i * MENU_ITEM_H;
                XSetForeground(dpy, gc, (i == selected) ? 
                              get_pixel(conf.highlight_color) : 
                              get_pixel(conf.bar_color));
                XFillRectangle(dpy, menu, gc, 0, y, menu_w, MENU_ITEM_H);
                XSetForeground(dpy, gc, get_pixel(conf.text_color));
                int ty = y + MENU_ITEM_H / 2 + font_info->ascent / 2 - 1;
                XDrawString(dpy, menu, gc, 10, ty, 
                           hidden[i].name, strlen(hidden[i].name));
                XSetForeground(dpy, gc, get_pixel(conf.border_color));
                XDrawLine(dpy, menu, gc, 0, y + MENU_ITEM_H - 1, 
                         menu_w, y + MENU_ITEM_H - 1);
            }
        } 
        else if (ev.type == MotionNotify) {
            int item = ev.xmotion.y / MENU_ITEM_H;
            if (item >= 0 && item < count && item != selected) {
                selected = item;
                XClearArea(dpy, menu, 0, 0, 0, 0, True);
            }
        } 
        else if (ev.type == ButtonPress || 
                 (ev.type == KeyPress && XLookupKeysym(&ev.xkey, 0) == XK_Return)) {
            XMapWindow(dpy, hidden[selected].frame);
            XRaiseWindow(dpy, hidden[selected].frame);
            raise_bars();
            Window c = find_client_in_frame(hidden[selected].frame);
            if (c) {
                XSetInputFocus(dpy, c, RevertToPointerRoot, CurrentTime);
                focus_window = c;
                ClientState *cs = get_client_state(c);
                if (cs) active_monitor = cs->monitor;
                set_active_window(c);
            }
            done = 1;
        } 
        else if (ev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_Escape) {
                done = 1;
            } else if (ks == XK_Up || ks == XK_k) {
                selected = (selected - 1 + count) % count;
                XClearArea(dpy, menu, 0, 0, 0, 0, True);
            } else if (ks == XK_Down || ks == XK_j) {
                selected = (selected + 1) % count;
                XClearArea(dpy, menu, 0, 0, 0, 0, True);
            }
        }
    }

    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    XDestroyWindow(dpy, menu);
    XFreeGC(dpy, gc);

    for (int i = 0; i < count; i++) {
        if (hidden[i].name) free(hidden[i].name);
    }

    update_all_bars();
}

void unhide_all(void) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].frame) {
            XMapWindow(dpy, clients[i].frame);
        }
    }
    raise_bars();
    update_all_bars();
}

int x_error_handler(Display *d, XErrorEvent *e) {
    (void)d;
    (void)e;
    return 0;
}

void execute_keybind(KeySym key, unsigned int state) {
    unsigned int clean_state = CLEANMASK(state);
    
    for (int i = 0; i < bind_count; i++) {
        if (binds[i].key == key && binds[i].mod == clean_state) {
            const char *cmd = binds[i].command;
            
            if (strcasecmp(cmd, "quit") == 0) {
                running = 0;
            } else if (strcasecmp(cmd, "alttab") == 0) {
                alt_tab_show();
            } else if (strcasecmp(cmd, "menu") == 0) {
                show_hidden_menu();
            } else if (strcasecmp(cmd, "unhide") == 0) {
                unhide_all();
            } else if (strcasecmp(cmd, "close") == 0) {
                if (focus_window) close_client(focus_window);
            } else if (strcasecmp(cmd, "fullscreen") == 0) {
                if (focus_window) toggle_fullscreen(focus_window);
            } else if (strcasecmp(cmd, "snap_left") == 0) {
                if (focus_window) snap_window(focus_window, 0);
            } else if (strcasecmp(cmd, "snap_right") == 0) {
                if (focus_window) snap_window(focus_window, 1);
            } else if (strcasecmp(cmd, "maximize") == 0) {
                if (focus_window) snap_window(focus_window, 2);
            } else if (strcasecmp(cmd, "restore") == 0) {
                if (focus_window) snap_window(focus_window, 3);
            } else {
                spawn(cmd);
            }
            return;
        }
    }
}

void grab_keys(void) {
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    
    for (int i = 0; i < bind_count; i++) {
        KeyCode kc = XKeysymToKeycode(dpy, binds[i].key);
        if (!kc) continue;
        
        unsigned int modifiers[] = {
            binds[i].mod,
            binds[i].mod | Mod2Mask,
            binds[i].mod | LockMask,
            binds[i].mod | Mod2Mask | LockMask,
        };
        
        for (int m = 0; m < 4; m++) {
            XGrabKey(dpy, kc, modifiers[m], root, True, 
                     GrabModeAsync, GrabModeAsync);
        }
    }
    
    XSync(dpy, False);
}

void cleanup(void) {
    alt_tab_cleanup();
    destroy_bars();
    
    if (check_win) {
        XDestroyWindow(dpy, check_win);
    }
    
    if (font_info) {
        XFreeFont(dpy, font_info);
        font_info = NULL;
    }
}

int main(void) {
    load_config();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }
    
    XSetErrorHandler(x_error_handler);
    XSync(dpy, False);

    root = DefaultRootWindow(dpy);
    
    detect_monitors();
    init_hints();

    font_info = XLoadQueryFont(dpy, conf.font_name);
    if (!font_info) font_info = XLoadQueryFont(dpy, "fixed");
    if (!font_info) {
        fprintf(stderr, "Cannot load font\n");
        XCloseDisplay(dpy);
        return 1;
    }

    create_bars();

    Cursor cursor = XCreateFontCursor(dpy, XC_left_ptr);
    XDefineCursor(dpy, root, cursor);

    XSetWindowBackground(dpy, root, get_pixel(conf.bg_color));
    XClearWindow(dpy, root);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | 
                 KeyPressMask | KeyReleaseMask);

    grab_keys();

    unsigned int mods[] = { 
        mouse_mod_mask, 
        mouse_mod_mask | Mod2Mask,
        mouse_mod_mask | LockMask, 
        mouse_mod_mask | Mod2Mask | LockMask 
    };
    for (int m = 0; m < 4; m++) {
        XGrabButton(dpy, Button1, mods[m], root, True, ButtonPressMask,
                    GrabModeAsync, GrabModeAsync, None, None);
        XGrabButton(dpy, Button3, mods[m], root, True, ButtonPressMask,
                    GrabModeAsync, GrabModeAsync, None, None);
    }

    signal(SIGCHLD, SIG_IGN);

    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren;
    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            if (!is_bar_window(children[i])) {
                XWindowAttributes attr;
                if (XGetWindowAttributes(dpy, children[i], &attr) &&
                    attr.map_state == IsViewable && !attr.override_redirect) {
                    frame_window(children[i]);
                }
            }
        }
        if (children) XFree(children);
    }

    int x11_fd = ConnectionNumber(dpy);
    XEvent ev;

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            if (alt_tab.active) {
                if (ev.type == Expose && ev.xexpose.window == alt_tab.menu_win) {
                    alt_tab_draw();
                    continue;
                }
                if (ev.type == KeyPress) {
                    KeySym ks = XLookupKeysym(&ev.xkey, 0);
                    if (ks == XK_Tab) {
                        if (ev.xkey.state & ShiftMask) alt_tab_prev();
                        else alt_tab_next();
                    } else if (ks == XK_Escape) {
                        alt_tab_cancel();
                    } else if (ks == XK_Return) {
                        alt_tab_confirm();
                    } else if (ks == XK_Up || ks == XK_k) {
                        alt_tab_prev();
                    } else if (ks == XK_Down || ks == XK_j) {
                        alt_tab_next();
                    }
                    continue;
                }
                if (ev.type == KeyRelease) {
                    KeySym ks = XLookupKeysym(&ev.xkey, 0);
                    if (ks == XK_Alt_L || ks == XK_Alt_R ||
                        ks == XK_Meta_L || ks == XK_Meta_R) {
                        if (!is_alt_pressed()) alt_tab_confirm();
                    }
                    continue;
                }
                continue;
            }

            switch (ev.type) {
                case MapRequest:
                    frame_window(ev.xmaprequest.window);
                    break;
                    
                case UnmapNotify:
                    if (ev.xunmap.event == root) break;
                    {
                        Window frame = get_frame(ev.xunmap.window);
                        if (frame) {
                            XWindowAttributes attr;
                            if (XGetWindowAttributes(dpy, ev.xunmap.window, &attr) == 0) {
                                XDestroyWindow(dpy, frame);
                                remove_client(ev.xunmap.window);
                                update_client_list();
                                if (focus_window == ev.xunmap.window) {
                                    focus_window = 0;
                                    update_all_bars();
                                }
                            }
                        }
                    }
                    break;
                    
                case DestroyNotify:
                    {
                        Window frame = get_frame(ev.xdestroywindow.window);
                        if (frame) {
                            XDestroyWindow(dpy, frame);
                            remove_client(ev.xdestroywindow.window);
                            update_client_list();
                            if (focus_window == ev.xdestroywindow.window) {
                                focus_window = 0;
                                update_all_bars();
                            }
                        } else {
                            for (int i = 0; i < client_count; i++) {
                                if (clients[i].frame == ev.xdestroywindow.window) {
                                    if (focus_window == clients[i].client) {
                                        focus_window = 0;
                                    }
                                    remove_client(clients[i].client);
                                    update_client_list();
                                    update_all_bars();
                                    break;
                                }
                            }
                        }
                    }
                    break;
                    
                case ConfigureRequest:
                    {
                        XWindowChanges wc;
                        wc.x = ev.xconfigurerequest.x;
                        wc.y = ev.xconfigurerequest.y;
                        wc.width = ev.xconfigurerequest.width;
                        wc.height = ev.xconfigurerequest.height;
                        wc.border_width = ev.xconfigurerequest.border_width;
                        wc.sibling = ev.xconfigurerequest.above;
                        wc.stack_mode = ev.xconfigurerequest.detail;
                        XConfigureWindow(dpy, ev.xconfigurerequest.window,
                                        ev.xconfigurerequest.value_mask, &wc);
                    }
                    break;
                    
                case ClientMessage:
                    if (ev.xclient.message_type == wmatoms[NET_WM_STATE]) {
                        if ((Atom)ev.xclient.data.l[1] == wmatoms[NET_WM_STATE_FULLSCREEN] ||
                            (Atom)ev.xclient.data.l[2] == wmatoms[NET_WM_STATE_FULLSCREEN]) {
                            toggle_fullscreen(ev.xclient.window);
                        }
                    } else if (ev.xclient.message_type == wmatoms[NET_ACTIVE_WINDOW]) {
                        Window frame = get_frame(ev.xclient.window);
                        if (frame) {
                            XMapWindow(dpy, frame);
                            XRaiseWindow(dpy, frame);
                            raise_bars();
                            XSetInputFocus(dpy, ev.xclient.window, 
                                          RevertToPointerRoot, CurrentTime);
                            focus_window = ev.xclient.window;
                            ClientState *cs = get_client_state(focus_window);
                            if (cs) active_monitor = cs->monitor;
                            set_active_window(focus_window);
                            update_all_bars();
                        }
                    }
                    break;
                    
                case KeyPress:
                    {
                        KeySym ks = XLookupKeysym(&ev.xkey, 0);
                        execute_keybind(ks, ev.xkey.state);
                    }
                    break;
                    
                case EnterNotify:
                    if (!is_bar_window(ev.xcrossing.window) && 
                        ev.xcrossing.window != root) {
                        Window client = find_client_in_frame(ev.xcrossing.window);
                        if (client) {
                            focus_window = client;
                            XSetInputFocus(dpy, focus_window, 
                                          RevertToPointerRoot, CurrentTime);
                            ClientState *cs = get_client_state(focus_window);
                            if (cs) active_monitor = cs->monitor;
                            set_active_window(focus_window);
                            update_all_bars();
                        }
                    }
                    break;
                    
                case Expose:
                    if (ev.xexpose.count == 0) {
                        int bar_idx = -1;
                        for (int i = 0; i < monitor_count; i++) {
                            if (monitors[i].bar_win == ev.xexpose.window) {
                                bar_idx = i;
                                break;
                            }
                        }
                        if (bar_idx >= 0) {
                            update_bar(bar_idx);
                        } else {
                            Window client = find_client_in_frame(ev.xexpose.window);
                            if (client) {
                                XWindowAttributes fa;
                                if (XGetWindowAttributes(dpy, ev.xexpose.window, &fa)) {
                                    draw_decorations(ev.xexpose.window, fa.width, 
                                                    fa.height - TITLE_HEIGHT);
                                }
                            }
                        }
                    }
                    break;
                    
                case ButtonPress:
                    {
                        Window parent_frame = 0;
                        Window root_r, parent_r, *kids = NULL;
                        unsigned int n_kids;

                        if (XQueryTree(dpy, ev.xbutton.window, &root_r, &parent_r, 
                                      &kids, &n_kids)) {
                            if (parent_r != root && parent_r != 0) {
                                parent_frame = parent_r;
                            } else if (ev.xbutton.window != root) {
                                parent_frame = ev.xbutton.window;
                            }
                            if (kids) XFree(kids);
                        }

                        int is_fs = 0;
                        Window client = find_client_in_frame(parent_frame);
                        ClientState *cs = get_client_state(client);
                        if (cs && cs->is_fullscreen) is_fs = 1;

                        if (!is_fs && (ev.xbutton.state & mouse_mod_mask) && 
                            ev.xbutton.button == Button1) {
                            XAllowEvents(dpy, AsyncPointer, CurrentTime);
                            Window target = parent_frame ? parent_frame : ev.xbutton.subwindow;
                            if (target && !is_bar_window(target) && target != root) {
                                XWindowAttributes attr;
                                if (XGetWindowAttributes(dpy, target, &attr)) {
                                    start_ev = ev.xbutton;
                                    start_ev.window = target;
                                    start_ev.button = Button1;
                                    drag_state.start_root_x = ev.xbutton.x_root;
                                    drag_state.start_root_y = ev.xbutton.y_root;
                                    drag_state.win_x = attr.x;
                                    drag_state.win_y = attr.y;
                                    XGrabPointer(dpy, root, False, 
                                                ButtonMotionMask | ButtonReleaseMask,
                                                GrabModeAsync, GrabModeAsync, 
                                                None, None, CurrentTime);
                                    XRaiseWindow(dpy, target);
                                    raise_bars();
                                }
                            }
                        } else if (!is_fs && (ev.xbutton.state & mouse_mod_mask) && 
                                   ev.xbutton.button == Button3) {
                            XAllowEvents(dpy, AsyncPointer, CurrentTime);
                            Window target = parent_frame ? parent_frame : ev.xbutton.subwindow;
                            if (target && !is_bar_window(target) && target != root) {
                                XWindowAttributes attr;
                                if (XGetWindowAttributes(dpy, target, &attr)) {
                                    start_ev = ev.xbutton;
                                    start_ev.window = target;
                                    start_ev.button = Button3;
                                    drag_state.start_root_x = ev.xbutton.x_root;
                                    drag_state.start_root_y = ev.xbutton.y_root;
                                    drag_state.win_x = attr.x;
                                    drag_state.win_y = attr.y;
                                    drag_state.win_w = attr.width;
                                    drag_state.win_h = attr.height;
                                    drag_state.resize_x_dir = 
                                        (ev.xbutton.x_root > attr.x + attr.width / 2) ? 1 : -1;
                                    drag_state.resize_y_dir = 
                                        (ev.xbutton.y_root > attr.y + attr.height / 2) ? 1 : -1;
                                    XGrabPointer(dpy, root, False, 
                                                ButtonMotionMask | ButtonReleaseMask,
                                                GrabModeAsync, GrabModeAsync, 
                                                None, None, CurrentTime);
                                    XRaiseWindow(dpy, target);
                                    raise_bars();
                                }
                            }
                        } else if (!is_fs && !is_bar_window(ev.xbutton.window) && 
                                   ev.xbutton.window != root &&
                                   ev.xbutton.y < TITLE_HEIGHT && 
                                   ev.xbutton.button == Button1) {
                            XAllowEvents(dpy, AsyncPointer, CurrentTime);
                            XWindowAttributes fa;
                            if (XGetWindowAttributes(dpy, ev.xbutton.window, &fa)) {
                                int btn = TITLE_HEIGHT;
                                if (ev.xbutton.x < btn) {
                                    Window cl = find_client_in_frame(ev.xbutton.window);
                                    if (cl) close_client(cl);
                                } else if (ev.xbutton.x > fa.width - btn) {
                                    XUnmapWindow(dpy, ev.xbutton.window);
                                } else {
                                    XRaiseWindow(dpy, ev.xbutton.window);
                                    raise_bars();
                                    XWindowAttributes attr;
                                    if (XGetWindowAttributes(dpy, ev.xbutton.window, &attr)) {
                                        drag_state.start_root_x = ev.xbutton.x_root;
                                        drag_state.start_root_y = ev.xbutton.y_root;
                                        drag_state.win_x = attr.x;
                                        drag_state.win_y = attr.y;
                                        start_ev = ev.xbutton;
                                        XGrabPointer(dpy, root, False, 
                                                    ButtonMotionMask | ButtonReleaseMask,
                                                    GrabModeAsync, GrabModeAsync, 
                                                    None, None, CurrentTime);
                                    }
                                }
                            }
                        } else {
                            if (parent_frame && !is_bar_window(parent_frame)) {
                                XRaiseWindow(dpy, parent_frame);
                                raise_bars();
                            }
                            XAllowEvents(dpy, ReplayPointer, CurrentTime);
                        }
                    }
                    break;
                    
                case MotionNotify:
                    if (start_ev.window) {
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
                            } else {
                                new_w -= xdiff;
                                new_x += xdiff;
                            }
                            if (drag_state.resize_y_dir == 1) {
                                new_h += ydiff;
                            } else {
                                new_h -= ydiff;
                                new_y += ydiff;
                            }

                            if (new_w < MIN_SIZE) {
                                new_w = MIN_SIZE;
                                if (drag_state.resize_x_dir == -1) {
                                    new_x = drag_state.win_x + drag_state.win_w - MIN_SIZE;
                                }
                            }
                            if (new_h < MIN_SIZE + TITLE_HEIGHT) {
                                new_h = MIN_SIZE + TITLE_HEIGHT;
                                if (drag_state.resize_y_dir == -1) {
                                    new_y = drag_state.win_y + drag_state.win_h - 
                                            MIN_SIZE - TITLE_HEIGHT;
                                }
                            }

                            XMoveResizeWindow(dpy, start_ev.window, new_x, new_y, new_w, new_h);
                            Window c = find_client_in_frame(start_ev.window);
                            if (c) XResizeWindow(dpy, c, new_w, new_h - TITLE_HEIGHT);
                            
                            ClientState *cs = get_client_state(c);
                            if (cs) {
                                cs->monitor = get_monitor_at(new_x + new_w / 2, 
                                                             new_y + new_h / 2);
                            }
                        } else if (start_ev.button == Button1) {
                            int new_x = drag_state.win_x + xdiff;
                            int new_y = drag_state.win_y + ydiff;
                            if (new_y < 0) new_y = 0;
                            XMoveWindow(dpy, start_ev.window, new_x, new_y);
                            
                            Window c = find_client_in_frame(start_ev.window);
                            ClientState *cs = get_client_state(c);
                            if (cs) {
                                XWindowAttributes attr;
                                if (XGetWindowAttributes(dpy, start_ev.window, &attr)) {
                                    cs->monitor = get_monitor_at(
                                        attr.x + attr.width / 2,
                                        attr.y + attr.height / 2);
                                    active_monitor = cs->monitor;
                                }
                            }
                        }
                    }
                    break;
                    
                case ButtonRelease:
                    if (start_ev.window) {
                        XUngrabPointer(dpy, CurrentTime);
                        start_ev.window = 0;
                        update_all_bars();
                    }
                    break;
            }
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(x11_fd, &fds);
        struct timeval tv = {1, 0};

        if (select(x11_fd + 1, &fds, NULL, NULL, &tv) == 0) {
            update_all_bars();
        }
    }

    cleanup();
    XCloseDisplay(dpy);
    return 0;
}
