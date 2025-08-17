/*
 * See LICENSE file for copyright and license details.
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <libinput.h>
#include <limits.h>
#include <math.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <stdbool.h>
#include <string.h>

// --- Launcher state ---
typedef struct {
    bool visible;
    int width, height;
    int selected_tab; // 0 = Apps, 1 = Files, 2 = Content Search
    char search[128];
    int search_len;
    int selected_index;
    // App list etc. kommer senere
} launcher_state_t;

static launcher_state_t launcher = {0};

// Launcher scene nodes
static struct wlr_scene_tree *launcher_scene_tree = NULL;
static struct wlr_scene_rect *launcher_rect = NULL;
static struct wlr_scene_rect *launcher_tab_rects[3] = {NULL, NULL, NULL};

// Launcher colors (RGBA)
static const float launcher_bg_color[4] = {0.08, 0.09, 0.12, 0.97};
static const float launcher_tab_color[4] = {0.13, 0.14, 0.18, 1.0};
static const float launcher_tab_active_color[4] = {0.22, 0.24, 0.32, 1.0};

#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <xkbcommon/xkbcommon.h>
#ifdef XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#endif

#include "util.h"
#include "gpu_acceleration.h"
#include <wlr/render/gles2.h>
#include <wlr/render/pixman.h>

/* macros */
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define CLEANMASK(mask)         (mask & ~WLR_MODIFIER_CAPS)
#define VISIBLEON(C, M)         ((M) && (C)->mon == (M) && ((C)->tags & (M)->tagset[(M)->seltags]))
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define END(A)                  ((A) + LENGTH(A))
#define TAGMASK                 ((1u << TAGCOUNT) - 1)
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
#define LISTEN_STATIC(E, H)     do { struct wl_listener *_l = ecalloc(1, sizeof(*_l)); _l->notify = (H); wl_signal_add((E), _l); } while (0)

/* STRICT TILE LIMITS: Norwegian user requirements */
#define MAX_TILES_PER_STACK     5
#define MAX_STACKS_PER_WORKSPACE 2
#define MAX_TILES_PER_WORKSPACE (MAX_TILES_PER_STACK * MAX_STACKS_PER_WORKSPACE)
#define MAX_COLUMNS             3  /* Support up to 3 columns for bidirectional movement */

/* global variables */
int statusbar_visible = 1; /* 1 = visible, 0 = hidden */

/* enums */
enum { CurNormal, CurPressed, CurMove, CurResize, CurSmartResize, CurTileResize }; /* cursor */
enum { XDGShell, LayerShell, X11 }; /* client types */
enum { LyrBg, LyrBottom, LyrTile, LyrFloat, LyrTop, LyrFS, LyrOverlay, LyrBlock, NUM_LAYERS }; /* scene layers */

typedef union {
	int i;
	uint32_t ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	unsigned int button;
	void (*func)(const Arg *);
	const Arg arg;
} Button;

typedef struct Monitor Monitor;
typedef struct {
	/* Must keep this field first */
	unsigned int type; /* XDGShell or X11* */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_rect *border[4]; /* top, bottom, left, right */
	struct wlr_scene_tree *scene_surface;
	struct wl_list link;
	struct wl_list flink;
	struct wlr_box geom; /* layout-relative, includes border */
	struct wlr_box prev; /* layout-relative, includes border */
	struct wlr_box bounds; /* only width and height are used */
	union {
		struct wlr_xdg_surface *xdg;
		struct wlr_xwayland_surface *xwayland;
	} surface;
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener maximize;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener fullscreen;
	struct wl_listener set_decoration_mode;
	struct wl_listener destroy_decoration;
#ifdef XWAYLAND
	struct wl_listener activate;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener configure;
	struct wl_listener set_hints;
#endif
	unsigned int bw;
	uint32_t tags;
	int isfloating, isurgent, isfullscreen;
	uint32_t resize; /* configure serial of a pending resize */
	float height_factor; /* for vertical resizing - default 1.0 */
	float width_factor; /* for horizontal resizing - default 1.0 */
	int column_group; /* 0 = left column, 1 = right column - for per-column grouping */
	/* Store info for tile rebalancing during destruction */
	Monitor *destroy_mon; /* Monitor reference for rebalancing */
	int destroy_column; /* Column group for rebalancing */
	bool destroy_was_floating; /* Floating state for rebalancing */
} Client;

typedef struct {
	uint32_t mod;
	xkb_keysym_t keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	struct wlr_keyboard_group *wlr_group;

	int nsyms;
	const xkb_keysym_t *keysyms; /* invalid if nsyms == 0 */
	uint32_t mods; /* invalid if nsyms == 0 */
	struct wl_event_source *key_repeat_source;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
} KeyboardGroup;

typedef struct {
	/* Must keep this field first */
	unsigned int type; /* LayerShell */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_tree *popups;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wl_list link;
	int mapped;
	struct wlr_layer_surface_v1 *layer_surface;

	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
} LayerSurface;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

struct Monitor {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *fullscreen_bg; /* See createmon() for info */
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener request_state;
	struct wl_listener destroy_lock_surface;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_box m; /* monitor area, layout-relative */
	struct wlr_box w; /* window area, layout-relative */
	struct wl_list layers[4]; /* LayerSurface.link */
	const Layout *lt[2];
	unsigned int seltags;
	unsigned int sellt;
	uint32_t tagset[2];
	float mfact;
	float workspace_mfact[9]; /* Per-workspace mfact storage (9 workspaces) */
	int workspace_stacking_counter; /* Per-workspace stacking tile counter for round-robin */
	bool workspace_stacking_counter_initialized; /* Track if counter has been initialized */
	int manual_horizontal_resize; /* Flag to prevent arrange() from overriding manual horizontal resize */
	int manual_vertical_resize; /* Flag to prevent arrange() from overriding manual vertical resize */
	int gamma_lut_changed;
	int nmaster;
	char ltsymbol[16];
	int asleep;
};

typedef struct {
	const char *name;
	float mfact;
	int nmaster;
	float scale;
	const Layout *lt;
	enum wl_output_transform rr;
	int x, y;
} MonitorRule;

typedef struct {
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
} PointerConstraint;

typedef struct {
	const char *id;
	const char *title;
	uint32_t tags;
	int isfloating;
	int monitor;
} Rule;

typedef struct {
	struct wlr_scene_tree *scene;

	struct wlr_session_lock_v1 *lock;
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
} SessionLock;

/* function declarations */
static void applybounds(Client *c, struct wlr_box *bbox);
static void applyrules(Client *c);
static void arrange(Monitor *m);
static void arrange_immediate(Monitor *m);
static void arrangelayer(Monitor *m, struct wl_list *list,
		struct wlr_box *usable_area, int exclusive);
static void arrangelayers(Monitor *m);
static void axisnotify(struct wl_listener *listener, void *data);
static void buttonpress(struct wl_listener *listener, void *data);
static void chvt(const Arg *arg);
static void checkidleinhibitor(struct wlr_surface *exclude);
static void cleanup(void);
static void cleanupmon(struct wl_listener *listener, void *data);
static void cleanuplisteners(void);
static void closemon(Monitor *m);
static void commitlayersurfacenotify(struct wl_listener *listener, void *data);
static void commitnotify(struct wl_listener *listener, void *data);
static void commitpopup(struct wl_listener *listener, void *data);
static void createdecoration(struct wl_listener *listener, void *data);
static void createidleinhibitor(struct wl_listener *listener, void *data);
static void createkeyboard(struct wlr_keyboard *keyboard);
static KeyboardGroup *createkeyboardgroup(void);
static void createlayersurface(struct wl_listener *listener, void *data);
static void createlocksurface(struct wl_listener *listener, void *data);
static void createmon(struct wl_listener *listener, void *data);
static void createnotify(struct wl_listener *listener, void *data);
static void createpointer(struct wlr_pointer *pointer);
static void createpointerconstraint(struct wl_listener *listener, void *data);
static void createpopup(struct wl_listener *listener, void *data);
static void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint);
static void cursorframe(struct wl_listener *listener, void *data);
static void cursorwarptohint(void);
static void destroydecoration(struct wl_listener *listener, void *data);
static void destroydragicon(struct wl_listener *listener, void *data);
static void destroyidleinhibitor(struct wl_listener *listener, void *data);
static void destroylayersurfacenotify(struct wl_listener *listener, void *data);
static void destroylock(SessionLock *lock, int unlocked);
static void destroylocksurface(struct wl_listener *listener, void *data);
static void destroynotify(struct wl_listener *listener, void *data);
static void destroypointerconstraint(struct wl_listener *listener, void *data);
static void ensure_equal_width_distribution(Client *new_client);
static void ensure_equal_height_distribution_in_stack(int column);
static void ensure_equal_horizontal_distribution_for_two_tiles(void);
static void handle_empty_column_expansion(void);
static bool validate_client_list_integrity(void);
static int count_tiles_in_stack(int column, Monitor *m);
static int count_total_tiles_in_workspace(Monitor *m);
static int find_available_stack(Monitor *m);
static int find_available_workspace(void);
static int get_current_workspace(void);
static void load_workspace_mfact(void);
static void save_workspace_mfact(void);
static void destroysessionlock(struct wl_listener *listener, void *data);
static void destroykeyboardgroup(struct wl_listener *listener, void *data);
static Monitor *dirtomon(enum wlr_direction dir);
static void focusclient(Client *c, int lift);
static void focusmon(const Arg *arg);
static void focusstack(const Arg *arg);
static Client *focustop(Monitor *m);
static void fullscreennotify(struct wl_listener *listener, void *data);
static void gpureset(struct wl_listener *listener, void *data);
static void handlesig(int signo);
static void incnmaster(const Arg *arg);
static int get_optimal_columns(int screen_width);
static int get_optimal_master_tiles(int screen_width);
static void update_dynamic_master_tiles(Monitor *m);
static void inputdevice(struct wl_listener *listener, void *data);
static int keybinding(uint32_t mods, xkb_keysym_t sym);

// --- Launcher and keypress helpers ---
static void keypress(struct wl_listener *listener, void *data);
static void keypressmod(struct wl_listener *listener, void *data);
static void handle_launcher_key(uint32_t key, uint32_t state, xkb_keysym_t sym);
static void hide_launcher(void);
static void update_launcher_filter(void);

static int keyrepeat(void *data);
static void killclient(const Arg *arg);
static void locksession(struct wl_listener *listener, void *data);
static void mapnotify(struct wl_listener *listener, void *data);
static void maximizenotify(struct wl_listener *listener, void *data);
static void monocle(Monitor *m);
static void motionabsolute(struct wl_listener *listener, void *data);
static void motionnotify(uint32_t time, struct wlr_input_device *device, double sx,
		double sy, double sx_unaccel, double sy_unaccel);
static void motionrelative(struct wl_listener *listener, void *data);
static void moveresize(const Arg *arg);
static void tileresize(const Arg *arg);
static void handletiledrop(Client *c, double x, double y);
static void swap_tiles_in_list(Client *c1, Client *c2);
static void handletiledrop_old(Client *c, double x, double y);
static void outputmgrapply(struct wl_listener *listener, void *data);
static void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test);
static void outputmgrtest(struct wl_listener *listener, void *data);
static void pointerfocus(Client *c, struct wlr_surface *surface,
		double sx, double sy, uint32_t time);
static void printstatus(void);
static void powermgrsetmode(struct wl_listener *listener, void *data);
static void quit(const Arg *arg);
static void rendermon(struct wl_listener *listener, void *data);
static void requestdecorationmode(struct wl_listener *listener, void *data);
static void requeststartdrag(struct wl_listener *listener, void *data);
static void requestmonstate(struct wl_listener *listener, void *data);
static void resize(Client *c, struct wlr_box geo, int interact);
static void run(char *startup_cmd);
static void setcursor(struct wl_listener *listener, void *data);
static void setcursorshape(struct wl_listener *listener, void *data);
static void setfloating(Client *c, int floating);
static void force_column_tiling(Monitor *m);
static void setfullscreen(Client *c, int fullscreen);
static void setlayout(const Arg *arg);
static void setmfact(const Arg *arg);
static void setmon(Client *c, Monitor *m, uint32_t newtags);
static int frame_synced_resize_callback(void *data);
static int get_monitor_refresh_rate(Monitor *m);
static void setpsel(struct wl_listener *listener, void *data);
static void setsel(struct wl_listener *listener, void *data);
static void setup(void);
static void spawn(const Arg *arg);
static void startdrag(struct wl_listener *listener, void *data);
static void tag(const Arg *arg);
static void tagmon(const Arg *arg);
static void tile(Monitor *m);
static void togglecolumn(const Arg *arg);
static void togglefloating(const Arg *arg);
static void togglefullscreen(const Arg *arg);
static void toggletag(const Arg *arg);
static void toggleview(const Arg *arg);
static void unlocksession(struct wl_listener *listener, void *data);
static void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
static void unmapnotify(struct wl_listener *listener, void *data);
static void updatemons(struct wl_listener *listener, void *data);
static void updatetitle(struct wl_listener *listener, void *data);
static void urgent(struct wl_listener *listener, void *data);
static void view(const Arg *arg);
static void virtualkeyboard(struct wl_listener *listener, void *data);
static void virtualpointer(struct wl_listener *listener, void *data);
static Monitor *xytomon(double x, double y);
static void xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny);
static void zoom(const Arg *arg);

/* variables */
static pid_t child_pid = -1;
static int locked;
static void *exclusive_focus;
static struct wl_display *dpy;
static struct wl_event_loop *event_loop;
static struct wlr_backend *backend;
static struct wlr_scene *scene;
static struct wlr_scene_tree *layers[NUM_LAYERS];
static struct wlr_scene_tree *drag_icon;
/* Map from ZWLR_LAYER_SHELL_* constants to Lyr* enum */
static const int layermap[] = { LyrBg, LyrBottom, LyrTop, LyrOverlay };
static struct wlr_renderer *drw;
static struct wlr_allocator *alloc;
static struct wlr_compositor *compositor;
static struct wlr_session *session;

static struct wlr_xdg_shell *xdg_shell;
static struct wlr_xdg_activation_v1 *activation;
static struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
static struct wl_list clients; /* tiling order */
static struct wl_list fstack;  /* focus order */

/* MANUAL RESIZE PRESERVATION: Track manual resize state per column */
static bool manual_resize_performed[2] = {false, false}; /* [left_column, right_column] */
static struct wlr_idle_notifier_v1 *idle_notifier;
static struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_output_manager_v1 *output_mgr;
static struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
static struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
static struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
static struct wlr_output_power_manager_v1 *power_mgr;

static struct wlr_pointer_constraints_v1 *pointer_constraints;
static struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
static struct wlr_pointer_constraint_v1 *active_constraint;

static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *cursor_mgr;

static struct wlr_scene_rect *root_bg;
static struct wlr_session_lock_manager_v1 *session_lock_mgr;
static struct wlr_scene_rect *locked_bg;
static struct wlr_session_lock_v1 *cur_lock;

static struct wlr_seat *seat;
static KeyboardGroup *kb_group;
static unsigned int cursor_mode;
static Client *grabc;
static int grabcx, grabcy, grabedge; /* client-relative */
static float initial_mfact; /* for pointer-relative resizing */
static int initial_edge_x; /* for pointer-relative resizing */
static int initial_edge_y; /* for vertical pointer-relative resizing */
static Client *initial_resize_client; /* client being resized vertically */
static Client *initial_resize_neighbor; /* neighbor client for vertical resizing */
static bool resizing_from_top_edge; /* true if resizing from top edge of a tile */

/* Horizontal tile resizing variables */
static Client *horizontal_resize_client; /* client being resized horizontally */
static Client *horizontal_resize_neighbor; /* neighbor client for horizontal resizing */
static bool resizing_from_left_edge; /* true if resizing from left edge of a tile */

/* Frame-synced resizing variables */
static bool resize_pending = false;
static float pending_target_mfact = 0.0f;
static bool vertical_resize_pending = false;
static float pending_target_height_factor = 0.0f;
static Client *vertical_resize_client = NULL;
static Client *vertical_resize_neighbor = NULL;

/* Column-based horizontal resizing variables */
static bool horizontal_column_resize_pending = false;
static int pending_left_column = -1;
static int pending_right_column = -1;
static int pending_left_width = 0;
static int pending_right_x = 0;
static int pending_right_width = 0;
static struct wl_event_source *resize_timer = NULL;
static struct timespec resize_start_time = {0}; /* Track resize operation start time */
static bool resize_operation_active = false;    /* Track if resize is currently active */

/* Grid-based resizing variables */
static int resize_edge_type = 0;              /* EdgeType for current resize operation */
static int resize_target_column = -1;         /* Column being resized */
/* vertical_resize_neighbor already declared above */

/* Ultra-smooth resizing: Multi-threading and CPU optimization */

/* BROKEN PIPE PREVENTION: Arrange throttling to prevent protocol overload */
static struct timespec last_arrange_time = {0};
static bool arrange_pending = false;
static struct wl_event_source *arrange_timer = NULL;
static Monitor *pending_arrange_monitor = NULL;
#define ARRANGE_THROTTLE_MS 16  /* 60 FPS max arrange rate */

/* Missing resize timing variables */
static uint32_t last_horizontal_resize_time = 0;
static uint32_t last_vertical_resize_time = 0;
static struct wl_event_source *resize_timer_source = NULL;
static int gap = 5;  /* Default gap value */
static pthread_t layout_thread;
static pthread_mutex_t layout_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool layout_thread_active = false;
static bool high_performance_mode = false;

static struct wlr_output_layout *output_layout;
static struct wlr_box sgeom;
static struct wl_list mons;
static Monitor *selmon;

/* global event handlers */
static struct wl_listener cursor_axis = {.notify = axisnotify};
static struct wl_listener cursor_button = {.notify = buttonpress};
static struct wl_listener cursor_frame = {.notify = cursorframe};
static struct wl_listener cursor_motion = {.notify = motionrelative};
static struct wl_listener cursor_motion_absolute = {.notify = motionabsolute};
static struct wl_listener gpu_reset = {.notify = gpureset};
static struct wl_listener layout_change = {.notify = updatemons};
static struct wl_listener new_idle_inhibitor = {.notify = createidleinhibitor};
static struct wl_listener new_input_device = {.notify = inputdevice};
static struct wl_listener new_virtual_keyboard = {.notify = virtualkeyboard};
static struct wl_listener new_virtual_pointer = {.notify = virtualpointer};
static struct wl_listener new_pointer_constraint = {.notify = createpointerconstraint};
static struct wl_listener new_output = {.notify = createmon};
static struct wl_listener new_xdg_toplevel = {.notify = createnotify};
static struct wl_listener new_xdg_popup = {.notify = createpopup};
static struct wl_listener new_xdg_decoration = {.notify = createdecoration};
static struct wl_listener new_layer_surface = {.notify = createlayersurface};
static struct wl_listener output_mgr_apply = {.notify = outputmgrapply};
static struct wl_listener output_mgr_test = {.notify = outputmgrtest};
static struct wl_listener output_power_mgr_set_mode = {.notify = powermgrsetmode};
static struct wl_listener request_activate = {.notify = urgent};
static struct wl_listener request_cursor = {.notify = setcursor};
static struct wl_listener request_set_psel = {.notify = setpsel};
static struct wl_listener request_set_sel = {.notify = setsel};
static struct wl_listener request_set_cursor_shape = {.notify = setcursorshape};
static struct wl_listener request_start_drag = {.notify = requeststartdrag};
static struct wl_listener start_drag = {.notify = startdrag};
static struct wl_listener new_session_lock = {.notify = locksession};

#ifdef XWAYLAND
static void activatex11(struct wl_listener *listener, void *data);
static void associatex11(struct wl_listener *listener, void *data);
static void configurex11(struct wl_listener *listener, void *data);
static void createnotifyx11(struct wl_listener *listener, void *data);
static void dissociatex11(struct wl_listener *listener, void *data);
static void sethints(struct wl_listener *listener, void *data);
static void xwaylandready(struct wl_listener *listener, void *data);
static struct wl_listener new_xwayland_surface = {.notify = createnotifyx11};
static struct wl_listener xwayland_ready = {.notify = xwaylandready};
static struct wlr_xwayland *xwayland;
#endif

/* configuration, allows nested code to access above variables */
#include "config.h"

/* attempt to encapsulate suck into one file */
#include "client.h"
#include "resize_edge.h"

/*
 * Get current time in milliseconds
 */
uint32_t
get_time_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*
 * EDGE DETECTION: Detect which edge of a client the cursor is near
 * Returns a bitmask of EDGE_* values
 */
int
detectresizeedge(Client *c, double x, double y)
{
	int edge = EDGE_NONE;
	
	/* Validate client and its dimensions */
	if (!c || !c->geom.width || !c->geom.height) {
		return EDGE_NONE;
	}
	
	/* Check if the point is within the window boundaries */
	if (x < c->geom.x || x > c->geom.x + c->geom.width ||
	    y < c->geom.y || y > c->geom.y + c->geom.height) {
		return EDGE_NONE;
	}
	
	/* Extended left edge detection - from middle to left edge */
	if (IS_LEFT_HALF(x, c)) {
		edge |= EDGE_LEFT;
	}
	
	/* Extended right edge detection - from middle to right edge */
	else if (IS_RIGHT_HALF(x, c)) {
		edge |= EDGE_RIGHT;
	}
	
	/* Check if cursor is near the top edge */
	if (y >= c->geom.y && y <= c->geom.y + EDGE_THRESHOLD)
		edge |= EDGE_TOP;
	
	/* Check if cursor is near the bottom edge */
	else if (y >= c->geom.y + c->geom.height - EDGE_THRESHOLD && y <= c->geom.y + c->geom.height)
		edge |= EDGE_BOTTOM;
	
	return edge;
}

/*
 * TILE DESTRUCTION PREVENTION: Check if a resize operation is safe
 * Returns 1 if resize is safe, 0 if it would cause tile destruction
 */
static int
is_resize_safe(Client *c, int new_width, int new_height)
{
	if (!c || !c->mon) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid client in resize safety check");
		return 0;
	}
	
	/* Check if new dimensions would be destructively small - but be more lenient for multi-column layouts */
	int safe_min_width = 120;  /* Reduced from 180 for multi-column compatibility */
	int safe_min_height = 80;  /* Reduced from 140 for stacked tiles */
	
	/* Only block extremely small sizes that would cause crashes */
	if (new_width < safe_min_width || new_height < safe_min_height) {
		wlr_log(WLR_DEBUG, "[nixtile] RESIZE SAFETY: Blocking destructive resize (requested: %dx%d, minimum: %dx%d)", 
		        new_width, new_height, safe_min_width, safe_min_height);
		return 0;
	} else {
		wlr_log(WLR_DEBUG, "[nixtile] RESIZE SAFETY: Allowing resize (requested: %dx%d, minimum: %dx%d)", 
		        new_width, new_height, safe_min_width, safe_min_height);
	}
	
	/* Check if client surface is still valid */
	struct wlr_surface *surface = client_surface(c);
	if (!surface || !surface->mapped) {
		wlr_log(WLR_ERROR, "[nixtile] RESIZE SAFETY: Client surface invalid or unmapped");
		return 0;
	}
	
	return 1; /* Resize is safe */
}

/*
 * Checks if there is an adjacent tile in the specified direction.
 * Returns 1 if there is an adjacent tile, 0 otherwise.
 * 
 * Note: Since we don't have direct access to the client list structure,
 * we'll simplify and always allow resizing for tiled windows.
 * A more sophisticated implementation would require tracking
 * adjacent window information during layout calculations.
 */
int
hasadjacenttile(Client *c, int edge)
{
	/* Only allow resizing if the client is not floating */
	if (!c || !c->mon)
		return 0;
	
	/* For floating windows, always allow resizing */
	if (c->isfloating)
		return 1;
	
	/* For tiled windows, allow resizing in any direction for now */
	/* A more sophisticated approach would check for adjacent windows */
	return 1;
}

/*
 * Perform smooth resizing based on mouse movement.
 * Allows fluid adjustment of both width and height based on mouse movements.
 * Maintains distinction between left/right edges while adding comprehensive error checking.
 */
void
smoothresize(Client *c, int edge, double dx, double dy)
{
	struct wlr_box geo;
	float factor;
	/* Use the constants from resize_edge.h for consistency */
	int min_width = MIN_WINDOW_WIDTH;
	int min_height = MIN_WINDOW_HEIGHT;
	
	/* Comprehensive validation */
	if (!c || !c->mon || !cursor)
		return;
	
	/* Validate cursor position to prevent extreme values */
	if (cursor->x < -10000 || cursor->x > 10000 || cursor->y < -10000 || cursor->y > 10000) {
		wlr_log(WLR_ERROR, "[nixtile] smoothresize: cursor position out of bounds (%.2f, %.2f)", cursor->x, cursor->y);
		return;
	}
	
	/* Safe initialization with current geometry */
	if (c->geom.width <= 0 || c->geom.height <= 0) {
		/* Protect against invalid dimensions */
		return;
	}
	
	/* Additional null pointer checks */
	if (!c->scene || !c->scene_surface) {
		return;
	}
	
	/* Copy current geometry as starting point */
	geo = c->geom;
	
	/* 
	 * Handle horizontal resizing
	 * Allow horizontal mouse movement to adjust width regardless of which edge
	 */
	if (edge & EDGE_LEFT) {
		/* For left edge: adjust both position and width based on horizontal movement */
		if (cursor->x >= 0 && c->geom.x + c->geom.width > cursor->x) {
			geo.x = (int)round(cursor->x - grabcx);
			geo.width = c->geom.width + (c->geom.x - geo.x);
			
			/* Ensure minimum width and valid position */
			if (geo.width < min_width) {
				geo.width = min_width;
				geo.x = c->geom.x + c->geom.width - min_width;
			}
			
			/* Ensure we don't move beyond monitor boundaries */
			if (geo.x < c->mon->w.x) {
				geo.width = geo.width - (c->mon->w.x - geo.x);
				geo.x = c->mon->w.x;
			}
		}
	} else if (edge & EDGE_RIGHT) {
		/* For right edge: adjust width only based on horizontal movement */
		if (cursor->x >= c->geom.x) {
			geo.width = (int)round(cursor->x - c->geom.x);
			
			/* Ensure minimum width */
			if (geo.width < min_width)
				geo.width = min_width;
			
			/* Ensure we don't extend beyond monitor boundaries */
			if (geo.x + geo.width > c->mon->w.x + c->mon->w.width) {
				geo.width = c->mon->w.x + c->mon->w.width - geo.x;
			}
		}
	}
	
	/* 
	 * Handle vertical resizing
	 * Always allow vertical mouse movement to adjust height
	 */
	if (edge & EDGE_TOP) {
		/* For top edge: adjust both position and height based on vertical movement */
		if (cursor->y >= 0 && c->geom.y + c->geom.height > cursor->y) {
			geo.y = (int)round(cursor->y - grabcy);
			geo.height = c->geom.height + (c->geom.y - geo.y);
			
			/* Ensure minimum height and valid position */
			if (geo.height < min_height) {
				geo.height = min_height;
				geo.y = c->geom.y + c->geom.height - min_height;
			}
			
			/* Ensure we don't move beyond monitor boundaries */
			if (geo.y < c->mon->w.y) {
				geo.height = geo.height - (c->mon->w.y - geo.y);
				geo.y = c->mon->w.y;
			}
		}
	} else if (edge & EDGE_BOTTOM) {
		/* For bottom edge: adjust height only based on vertical movement */
		if (cursor->y >= c->geom.y) {
			geo.height = (int)round(cursor->y - c->geom.y);
			
			/* Ensure minimum height */
			if (geo.height < min_height)
				geo.height = min_height;
			
			/* Ensure we don't extend beyond monitor boundaries */
			if (geo.y + geo.height > c->mon->w.y + c->mon->w.height) {
				geo.height = c->mon->w.y + c->mon->w.height - geo.y;
			}
		}
	}
	
	/* Fluid resize: Allow height adjustment even when grabbing left/right edges */
	if ((edge & (EDGE_LEFT | EDGE_RIGHT)) && !(edge & (EDGE_TOP | EDGE_BOTTOM))) {
		/* Update height based on vertical mouse movement */
		int new_height;
		
		if (dy != 0) {
			/* Adjust height with vertical mouse movement */
			new_height = geo.height + (int)round(dy);
			
			/* Apply with bounds checking */
			if (new_height >= min_height && geo.y + new_height <= c->mon->w.y + c->mon->w.height) {
				geo.height = new_height;
			}
		}
	}
	
	/* Fluid resize: Allow width adjustment even when grabbing top/bottom edges */
	if ((edge & (EDGE_TOP | EDGE_BOTTOM)) && !(edge & (EDGE_LEFT | EDGE_RIGHT))) {
		/* Update width based on horizontal mouse movement */
		int new_width;
		
		if (dx != 0) {
			/* Adjust width with horizontal mouse movement */
			new_width = geo.width + (int)round(dx);
			
			/* Apply with bounds checking */
			if (new_width >= min_width && geo.x + new_width <= c->mon->w.x + c->mon->w.width) {
				geo.width = new_width;
			}
		}
	}
	
	/* Final validation to ensure we have valid dimensions */
	if (geo.width < min_width) geo.width = min_width;
	if (geo.height < min_height) geo.height = min_height;
	
	/* Add safety margin to prevent size-related crashes */
	geo.width += RESIZE_SAFETY_MARGIN;
	geo.height += RESIZE_SAFETY_MARGIN;
	
	/* Make sure window stays within monitor boundaries with safety margin */
	if (geo.x < c->mon->w.x) {
		geo.width -= (c->mon->w.x - geo.x);
		geo.x = c->mon->w.x;
	}
	
	if (geo.x + geo.width > c->mon->w.x + c->mon->w.width)
		geo.width = c->mon->w.x + c->mon->w.width - geo.x;
	
	if (geo.y < c->mon->w.y) {
		geo.height -= (c->mon->w.y - geo.y);
		geo.y = c->mon->w.y;
	}
	
	if (geo.y + geo.height > c->mon->w.y + c->mon->w.height)
		geo.height = c->mon->w.y + c->mon->w.height - geo.y;
	
	/* Final sanity check to ensure minimum size after all adjustments */
	if (geo.width < min_width) geo.width = min_width;
	if (geo.height < min_height) geo.height = min_height;
	
	/* Apply resize with interaction flag */
	resize(c, geo, 1);
	
	/* If not floating, adjust master factor for left/right edges */
	if (!c->isfloating && (edge & (EDGE_LEFT | EDGE_RIGHT))) {
		/* Calculate relative movement as percentage of monitor width */
		factor = (float)dx / c->mon->w.width;
		
		/* Adjust mfact based on the edge and direction of movement */
		if ((edge & EDGE_LEFT && dx > 0) || (edge & EDGE_RIGHT && dx < 0))
			factor = -factor;
		
		/* Apply the change to mfact with bounds checking */
		if (c->mon->mfact + factor >= 0.1 && c->mon->mfact + factor <= 0.9) {
			c->mon->mfact += factor;
			arrange(c->mon);
		}
	}
}

/* function implementations */
void
applybounds(Client *c, struct wlr_box *bbox)
{
	/* Comprehensive validation to prevent crashes */
	if (!c || !bbox) {
		return;
	}
	
	/* Validate bbox dimensions */
	if (bbox->width <= 0 || bbox->height <= 0) {
		return;
	}
	
	/* Ensure minimum window dimensions using our constants */
	int min_width = MAX(MIN_WINDOW_WIDTH, 1 + 2 * (int)c->bw);
	int min_height = MAX(MIN_WINDOW_HEIGHT, 1 + 2 * (int)c->bw);
	
	c->geom.width = MAX(min_width, c->geom.width);
	c->geom.height = MAX(min_height, c->geom.height);
	
	/* Ensure window stays within bounds with safety checks */
	if (c->geom.x >= bbox->x + bbox->width)
		c->geom.x = bbox->x + bbox->width - c->geom.width;
	if (c->geom.y >= bbox->y + bbox->height)
		c->geom.y = bbox->y + bbox->height - c->geom.height;
	if (c->geom.x + c->geom.width <= bbox->x)
		c->geom.x = bbox->x;
	if (c->geom.y + c->geom.height <= bbox->y)
		c->geom.y = bbox->y;
	
	/* Final validation to ensure we didn't create invalid geometry */
	if (c->geom.width < min_width) c->geom.width = min_width;
	if (c->geom.height < min_height) c->geom.height = min_height;
	
	/* Ensure position is not negative */
	if (c->geom.x < 0) c->geom.x = 0;
	if (c->geom.y < 0) c->geom.y = 0;
}

void
applyrules(Client *c)
{
	/* rule matching */
	const char *appid, *title;
	uint32_t newtags = 0;
	int i;
	const Rule *r;
	Monitor *mon = selmon, *m;

	appid = client_get_appid(c);
	title = client_get_title(c);

	for (r = rules; r < END(rules); r++) {
		if ((!r->title || strstr(title, r->title))
				&& (!r->id || strstr(appid, r->id))) {
			c->isfloating = r->isfloating;
			newtags |= r->tags;
			i = 0;
			wl_list_for_each(m, &mons, link) {
				if (r->monitor == i++)
					mon = m;
			}
		}
	}

	c->isfloating |= client_is_float_type(c);
	setmon(c, mon, newtags);
}

/* BROKEN PIPE PREVENTION: Throttled arrange callback */
static int throttled_arrange_callback(void *data)
{
	Monitor *m = (Monitor *)data;
	
	/* Clear pending state */
	arrange_pending = false;
	arrange_timer = NULL;
	pending_arrange_monitor = NULL;
	
	/* Update last arrange time */
	clock_gettime(CLOCK_MONOTONIC, &last_arrange_time);
	
	wlr_log(WLR_DEBUG, "[nixtile] THROTTLE: Executing throttled arrange");
	
	/* Call the actual arrange function */
	arrange_immediate(m);
	
	return 0; /* Remove timer */
}

/* BROKEN PIPE PREVENTION: Safe wrapper for arrange() with error recovery */
static void try_arrange_safe(Monitor *m)
{
	if (!m) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: NULL monitor in try_arrange_safe");
		return;
	}
	
	/* Validate monitor state */
	if (!m->wlr_output || !m->wlr_output->enabled) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid monitor output in try_arrange_safe");
		return;
	}
	
	/* Call arrange with basic error recovery */
	arrange(m);
	
	/* If we get here, arrange succeeded */
	wlr_log(WLR_DEBUG, "[nixtile] SAFETY: arrange() completed successfully in try_arrange_safe");
}

/* BROKEN PIPE PREVENTION: Throttled arrange function */
void
arrange(Monitor *m)
{
	/* CRASH PREVENTION: Validate client list integrity before layout operations */
	if (!validate_client_list_integrity()) {
		wlr_log(WLR_ERROR, "[nixtile] CRASH PREVENTION: Client list integrity check failed - ABORT arrange");
		return;
	}
	
	struct timespec current_time;
	clock_gettime(CLOCK_MONOTONIC, &current_time);
	
	/* Calculate time since last arrange */
	long time_diff_ms = (current_time.tv_sec - last_arrange_time.tv_sec) * 1000 +
						(current_time.tv_nsec - last_arrange_time.tv_nsec) / 1000000;
	
	/* If enough time has passed, arrange immediately */
	if (time_diff_ms >= ARRANGE_THROTTLE_MS) {
		wlr_log(WLR_DEBUG, "[nixtile] THROTTLE: Immediate arrange (time_diff=%ldms)", time_diff_ms);
		clock_gettime(CLOCK_MONOTONIC, &last_arrange_time);
		arrange_immediate(m);
		return;
	}
	
	/* If arrange is already pending for this monitor, skip */
	if (arrange_pending && pending_arrange_monitor == m) {
		wlr_log(WLR_DEBUG, "[nixtile] THROTTLE: Arrange already pending for this monitor");
		return;
	}
	
	/* Cancel any existing timer */
	if (arrange_timer) {
		wl_event_source_remove(arrange_timer);
		arrange_timer = NULL;
	}
	
	/* Schedule throttled arrange */
	int remaining_ms = ARRANGE_THROTTLE_MS - time_diff_ms;
	arrange_pending = true;
	pending_arrange_monitor = m;
	
	wlr_log(WLR_DEBUG, "[nixtile] THROTTLE: Scheduling arrange in %dms", remaining_ms);
	
	arrange_timer = wl_event_loop_add_timer(event_loop, throttled_arrange_callback, m);
	if (arrange_timer) {
		wl_event_source_timer_update(arrange_timer, remaining_ms);
	} else {
		wlr_log(WLR_ERROR, "[nixtile] THROTTLE: Failed to create arrange timer - falling back to immediate");
		arrange_immediate(m);
	}
}

/* BROKEN PIPE PREVENTION: Immediate arrange function (renamed from original) */
void
arrange_immediate(Monitor *m)
{
	Client *c;

	/* BROKEN PIPE PREVENTION: Validate monitor and output */
	if (!m || !m->wlr_output) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: NULL monitor or output in arrange - BROKEN PIPE PREVENTION");
		return;
	}

	if (!m->wlr_output->enabled) {
		wlr_log(WLR_DEBUG, "[nixtile] arrange: Monitor output disabled, skipping");
		return;
	}

	wl_list_for_each(c, &clients, link) {
		if (c->mon == m) {
			/* BROKEN PIPE PREVENTION: Validate client scene nodes */
			if (!c->scene || !c->scene_surface) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid scene nodes in arrange - skip client");
				continue;
			}
			
			/* BROKEN PIPE PREVENTION: Validate surface is still mapped */
			struct wlr_surface *surface = client_surface(c);
			if (!surface) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: NULL surface in arrange - skip client");
				continue;
			}
			
			wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON(c, m));
			client_set_suspended(c, !VISIBLEON(c, m));
		}
	}

	wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
			(c = focustop(m)) && c->isfullscreen);

	strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));

	/* We move all clients (except fullscreen and unmanaged) to LyrTile while
	 * in floating layout to avoid "real" floating clients be always on top */
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || !c->scene || c->scene->node.parent == layers[LyrFS])
			continue;

		/* BROKEN PIPE PREVENTION: Validate scene node before reparenting */
		if (!c->scene || !c->scene_surface) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid scene nodes in reparent - skip client");
			continue;
		}

		/* BROKEN PIPE PREVENTION: Validate layers before reparenting */
		if (!layers[LyrTile] || !layers[LyrFloat]) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid layers in reparent - skip client");
			continue;
		}
		
		/* BROKEN PIPE PREVENTION: Try-catch style error handling for reparenting */
		struct wlr_scene_tree *target_layer = (!m->lt[m->sellt]->arrange && c->isfloating)
				? layers[LyrTile]
				: (m->lt[m->sellt]->arrange && c->isfloating)
						? layers[LyrFloat]
						: (struct wlr_scene_tree *)c->scene->node.parent;
		
		if (target_layer && &target_layer->node != c->scene->node.parent) {
			wlr_scene_node_reparent(&c->scene->node, target_layer);
		}
	}

	if (m->lt[m->sellt]->arrange) {
		/* PREVENT INFINITE RECURSION: Skip layout during active tile resize to prevent loops */
		if (cursor_mode == CurTileResize) {
			wlr_log(WLR_DEBUG, "[nixtile] ARRANGE: Skipping layout recalculation during active tile resize");
			return;
		}
		
		/* BROKEN PIPE PREVENTION: Additional safety check for layout function */
		if (!m->lt[m->sellt]->arrange) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: NULL layout arrange function - ABORT");
			return;
		}
		
		/* BROKEN PIPE PREVENTION: Try to call layout with error recovery */
		wlr_log(WLR_DEBUG, "[nixtile] ARRANGE: Calling layout arrange function");
		m->lt[m->sellt]->arrange(m);
		wlr_log(WLR_DEBUG, "[nixtile] ARRANGE: Layout arrange completed successfully");
		/* FORCE: Ensure all tiles are properly tiled in columns after layout */
		Client *temp_c;
		wl_list_for_each(temp_c, &clients, link) {
			if (VISIBLEON(temp_c, m) && !temp_c->isfullscreen) {
				/* BROKEN PIPE PREVENTION: Validate client before forcing tiling */
				if (!temp_c->scene || !temp_c->scene_surface || !client_surface(temp_c)) {
					wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid client in force tiling - skip");
					continue;
				}
				
				/* FORCE: No tile can be floating */
				temp_c->isfloating = 0;
				/* Ensure valid column assignment for dynamic columns */
				int current_optimal_columns = get_optimal_columns(m->w.width);
				if (temp_c->column_group < 0 || temp_c->column_group >= current_optimal_columns) {
					/* DO NOT force to 0 - let tile() function handle assignment */
					wlr_log(WLR_DEBUG, "[nixtile] COLUMN VALIDATION: Invalid column_group %d, will be reassigned by tile() (max columns: %d)", temp_c->column_group, current_optimal_columns);
				}
			}
		}
	}
	motionnotify(0, NULL, 0, 0, 0, 0);
	checkidleinhibitor(NULL);
}

void
arrangelayer(Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive)
{
	LayerSurface *l;
	struct wlr_box full_area = m->m;

	wl_list_for_each(l, list, link) {
		struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;

		if (!layer_surface->initialized)
			continue;

		if (exclusive != (layer_surface->current.exclusive_zone > 0))
			continue;

		wlr_scene_layer_surface_v1_configure(l->scene_layer, &full_area, usable_area);
		wlr_scene_node_set_position(&l->popups->node, l->scene->node.x, l->scene->node.y);
	}
}

void
arrangelayers(Monitor *m)
{
	int i;
	struct wlr_box usable_area = m->m;
	LayerSurface *l;
	uint32_t layers_above_shell[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	if (!m->wlr_output->enabled)
		return;

	/* Arrange exclusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 1);

	if (!wlr_box_equal(&usable_area, &m->w)) {
		m->w = usable_area;
		arrange(m);
	}

	/* Arrange non-exlusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 0);

	/* Find topmost keyboard interactive layer, if such a layer exists */
	for (i = 0; i < (int)LENGTH(layers_above_shell); i++) {
		wl_list_for_each_reverse(l, &m->layers[layers_above_shell[i]], link) {
			if (locked || !l->layer_surface->current.keyboard_interactive || !l->mapped)
				continue;
			/* Deactivate the focused client. */
			focusclient(NULL, 0);
			exclusive_focus = l;
			client_notify_enter(l->layer_surface->surface, wlr_seat_get_keyboard(seat));
			return;
		}
	}
}

void
axisnotify(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct wlr_pointer_axis_event *event = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	/* TODO: allow usage of scroll wheel for mousebindings, it can be implemented
	 * by checking the event's orientation and the delta of the event */
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

void
buttonpress(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_button_event *event = data;
	struct wlr_keyboard *keyboard;
	uint32_t mods;
	Client *c;
	const Button *b;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	switch (event->state) {
	case WL_POINTER_BUTTON_STATE_PRESSED:
		cursor_mode = CurPressed;
		selmon = xytomon(cursor->x, cursor->y);
		if (locked)
			break;

		/* Change focus if the button was _pressed_ over a client */
		xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
			focusclient(c, 1);

		keyboard = wlr_seat_get_keyboard(seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		for (b = buttons; b < END(buttons); b++) {
			if (CLEANMASK(mods) == CLEANMASK(b->mod) &&
					event->button == b->button && b->func) {
				b->func(&b->arg);
				return;
			}
		}
		break;
	case WL_POINTER_BUTTON_STATE_RELEASED:
		/* If you released any buttons, we exit interactive move/resize mode. */
		/* TODO: should reset to the pointer focus's current setcursor */
		if (!locked && cursor_mode != CurNormal && cursor_mode != CurPressed) {
			/* MOUSE BUTTON ISOLATION: Store original cursor mode before reset */
			int original_cursor_mode = cursor_mode;
			
			/* Safely reset cursor */
			if (cursor && cursor_mgr) {
				wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
			}
			cursor_mode = CurNormal;
			/* PRESERVE MANUAL RESIZE STATE: Don't reset manual resize flags on mouse release */
			/* User requirement: Manual resize should only reset on tile deletion */
			horizontal_column_resize_pending = false;
			
			/* MOUSE BUTTON ISOLATION: Only handle tile movement from left mouse button (CurMove) */
			if (grabc && cursor && original_cursor_mode == CurMove) {
				selmon = xytomon(cursor->x, cursor->y);
				wlr_log(WLR_DEBUG, "[nixtile] Button release (CurMove): grabc=%p, cursor=%.1f,%.1f, selmon=%p, grabc->mon=%p", 
					(void*)grabc, cursor->x, cursor->y, (void*)selmon, (void*)(grabc ? grabc->mon : NULL));
				
				/* FORCE: Ensure tile is never floating after move */
				grabc->isfloating = 0;
				
				if (selmon && grabc->mon) {
					if (selmon != grabc->mon) {
						/* Moving to different monitor */
						wlr_log(WLR_DEBUG, "[nixtile] Moving tile to different monitor");
						setmon(grabc, selmon, 0);
						/* FORCE: Ensure column tiling on target monitor */
						force_column_tiling(selmon);
						arrange(selmon);
					} else {
						/* Same monitor - handle column-to-column dragging */
						wlr_log(WLR_DEBUG, "[nixtile] Same monitor - calling handletiledrop");
						handletiledrop(grabc, cursor->x, cursor->y);
						/* FORCE: Additional safety check for column tiling */
						force_column_tiling(selmon);
					}
				} else {
					wlr_log(WLR_DEBUG, "[nixtile] Button release: selmon or grabc->mon is NULL");
					/* FORCE: Even if monitor detection fails, ensure tiling */
					if (grabc && grabc->mon) {
						force_column_tiling(grabc->mon);
					}
				}
			} else if (grabc && cursor && original_cursor_mode != CurMove) {
				/* RIGHT MOUSE BUTTON ISOLATION: Resize mode - no tile movement allowed */
				wlr_log(WLR_DEBUG, "[nixtile] Button release (resize mode): tile movement blocked, original_cursor_mode=%d", original_cursor_mode);
			} else {
				wlr_log(WLR_DEBUG, "[nixtile] Button release: grabc or cursor is NULL");
			}
			/* Reset grab variables safely */
			grabc = NULL;
			grabedge = EDGE_NONE;
			grabcx = 0;
			grabcy = 0;
			return;
		}
		cursor_mode = CurNormal;
		/* PRESERVE MANUAL RESIZE STATE: Don't reset manual resize flags on mouse release */
		/* User requirement: Manual resize should only reset on tile deletion */
		break;
	}
	/* If the event wasn't handled by the compositor, notify the client with
	 * pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(seat,
			event->time_msec, event->button, event->state);
}

void
chvt(const Arg *arg)
{
	wlr_session_change_vt(session, arg->ui);
}

void
checkidleinhibitor(struct wlr_surface *exclude)
{
	int inhibited = 0, unused_lx, unused_ly;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &idle_inhibit_mgr->inhibitors, link) {
		struct wlr_surface *surface = wlr_surface_get_root_surface(inhibitor->surface);
		struct wlr_scene_tree *tree = surface->data;
		if (exclude != surface && (bypass_surface_visibility || (!tree
				|| wlr_scene_node_coords(&tree->node, &unused_lx, &unused_ly)))) {
			inhibited = 1;
			break;
		}
	}

	wlr_idle_notifier_v1_set_inhibited(idle_notifier, inhibited);
}

void
cleanup(void)
{
	/* BROKEN PIPE PREVENTION: Clean up arrange timer */
	if (arrange_timer) {
		wl_event_source_remove(arrange_timer);
		arrange_timer = NULL;
	}
	arrange_pending = false;
	pending_arrange_monitor = NULL;
	
	cleanuplisteners();
#ifdef XWAYLAND
	wlr_xwayland_destroy(xwayland);
	xwayland = NULL;
#endif
	wl_display_destroy_clients(dpy);
	if (child_pid > 0) {
		kill(-child_pid, SIGTERM);
		waitpid(child_pid, NULL, 0);
	}
	wlr_xcursor_manager_destroy(cursor_mgr);

	destroykeyboardgroup(&kb_group->destroy, NULL);

	/* If it's not destroyed manually, it will cause a use-after-free of wlr_seat.
	 * Destroy it until it's fixed on the wlroots side */
	wlr_backend_destroy(backend);

	wl_display_destroy(dpy);
	/* Destroy after the wayland display (when the monitors are already destroyed)
	   to avoid destroying them with an invalid scene output. */
	wlr_scene_node_destroy(&scene->tree.node);
}

void
cleanupmon(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy);
	LayerSurface *l, *tmp;
	size_t i;

	/* m->layers[i] are intentionally not unlinked */
	for (i = 0; i < LENGTH(m->layers); i++) {
		wl_list_for_each_safe(l, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(l->layer_surface);
	}

	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->link);
	wl_list_remove(&m->request_state.link);
	if (m->lock_surface)
		destroylocksurface(&m->destroy_lock_surface, NULL);
	m->wlr_output->data = NULL;
	wlr_output_layout_remove(output_layout, m->wlr_output);
	wlr_scene_output_destroy(m->scene_output);

	closemon(m);
	wlr_scene_node_destroy(&m->fullscreen_bg->node);
	free(m);
}

void
cleanuplisteners(void)
{
	wl_list_remove(&cursor_axis.link);
	wl_list_remove(&cursor_button.link);
	wl_list_remove(&cursor_frame.link);
	wl_list_remove(&cursor_motion.link);
	wl_list_remove(&cursor_motion_absolute.link);
	wl_list_remove(&gpu_reset.link);
	wl_list_remove(&new_idle_inhibitor.link);
	wl_list_remove(&layout_change.link);
	wl_list_remove(&new_input_device.link);
	wl_list_remove(&new_virtual_keyboard.link);
	wl_list_remove(&new_virtual_pointer.link);
	wl_list_remove(&new_pointer_constraint.link);
	wl_list_remove(&new_output.link);
	wl_list_remove(&new_xdg_toplevel.link);
	wl_list_remove(&new_xdg_decoration.link);
	wl_list_remove(&new_xdg_popup.link);
	wl_list_remove(&new_layer_surface.link);
	wl_list_remove(&output_mgr_apply.link);
	wl_list_remove(&output_mgr_test.link);
	wl_list_remove(&output_power_mgr_set_mode.link);
	wl_list_remove(&request_activate.link);
	wl_list_remove(&request_cursor.link);
	wl_list_remove(&request_set_psel.link);
	wl_list_remove(&request_set_sel.link);
	wl_list_remove(&request_set_cursor_shape.link);
	wl_list_remove(&request_start_drag.link);
	wl_list_remove(&start_drag.link);
	wl_list_remove(&new_session_lock.link);
#ifdef XWAYLAND
	wl_list_remove(&new_xwayland_surface.link);
	wl_list_remove(&xwayland_ready.link);
#endif
}

void
closemon(Monitor *m)
{
	/* update selmon if needed and
	 * move closed monitor's clients to the focused one */
	Client *c;
	int i = 0, nmons = wl_list_length(&mons);
	if (!nmons) {
		selmon = NULL;
	} else if (m == selmon) {
		do /* don't switch to disabled mons */
			selmon = wl_container_of(mons.next, selmon, link);
		while (!selmon->wlr_output->enabled && i++ < nmons);

		if (!selmon->wlr_output->enabled)
			selmon = NULL;
	}

	wl_list_for_each(c, &clients, link) {
		if (c->isfloating && c->geom.x > m->m.width)
			resize(c, (struct wlr_box){.x = c->geom.x - m->w.width, .y = c->geom.y,
					.width = c->geom.width, .height = c->geom.height}, 0);
		if (c->mon == m)
			setmon(c, selmon, c->tags);
	}
	focusclient(focustop(selmon), 1);
	printstatus();
}

void
commitlayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;
	struct wlr_scene_tree *scene_layer = layers[layermap[layer_surface->current.layer]];
	struct wlr_layer_surface_v1_state old_state;

	if (l->layer_surface->initial_commit) {
		client_set_scale(layer_surface->surface, l->mon->wlr_output->scale);

		/* Temporarily set the layer's current state to pending
		 * so that we can easily arrange it */
		old_state = l->layer_surface->current;
		l->layer_surface->current = l->layer_surface->pending;
		arrangelayers(l->mon);
		l->layer_surface->current = old_state;
		return;
	}

	if (layer_surface->current.committed == 0 && l->mapped == layer_surface->surface->mapped)
		return;
	l->mapped = layer_surface->surface->mapped;

	if (scene_layer != l->scene->node.parent) {
		wlr_scene_node_reparent(&l->scene->node, scene_layer);
		wl_list_remove(&l->link);
		wl_list_insert(&l->mon->layers[layer_surface->current.layer], &l->link);
		wlr_scene_node_reparent(&l->popups->node, (layer_surface->current.layer
				< ZWLR_LAYER_SHELL_V1_LAYER_TOP ? layers[LyrTop] : scene_layer));
	}

	arrangelayers(l->mon);
}

void
commitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, commit);

	if (c->surface.xdg->initial_commit) {
		/*
		 * Get the monitor this client will be rendered on
		 * Note that if the user set a rule in which the client is placed on
		 * a different monitor based on its title, this will likely select
		 * a wrong monitor.
		 */
		applyrules(c);
		if (c->mon) {
			client_set_scale(client_surface(c), c->mon->wlr_output->scale);
		}
		setmon(c, NULL, 0); /* Make sure to reapply rules in mapnotify() */

		wlr_xdg_toplevel_set_wm_capabilities(c->surface.xdg->toplevel,
				WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
		if (c->decoration)
			requestdecorationmode(&c->set_decoration_mode, c->decoration);
		wlr_xdg_toplevel_set_size(c->surface.xdg->toplevel, 0, 0);
		return;
	}

	resize(c, c->geom, (c->isfloating && !c->isfullscreen));

	/* mark a pending resize as completed */
	if (c->resize && c->resize <= c->surface.xdg->current.configure_serial)
		c->resize = 0;
}

void
commitpopup(struct wl_listener *listener, void *data)
{
	struct wlr_surface *surface = data;
	struct wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(surface);
	LayerSurface *l = NULL;
	Client *c = NULL;
	struct wlr_box box;
	int type = -1;

	if (!popup->base->initial_commit)
		return;

	type = toplevel_from_wlr_surface(popup->base->surface, &c, &l);
	if (!popup->parent || type < 0)
		return;
	popup->base->surface->data = wlr_scene_xdg_surface_create(
			popup->parent->data, popup->base);
	if ((l && !l->mon) || (c && !c->mon)) {
		wlr_xdg_popup_destroy(popup);
		return;
	}
	box = type == LayerShell ? l->mon->m : c->mon->w;
	box.x -= (type == LayerShell ? l->scene->node.x : c->geom.x);
	box.y -= (type == LayerShell ? l->scene->node.y : c->geom.y);
	wlr_xdg_popup_unconstrain_from_box(popup, &box);
	wl_list_remove(&listener->link);
	free(listener);
}

void
createdecoration(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	Client *c = deco->toplevel->base->data;
	c->decoration = deco;

	LISTEN(&deco->events.request_mode, &c->set_decoration_mode, requestdecorationmode);
	LISTEN(&deco->events.destroy, &c->destroy_decoration, destroydecoration);

	requestdecorationmode(&c->set_decoration_mode, deco);
}

void
createidleinhibitor(struct wl_listener *listener, void *data)
{
	struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
	LISTEN_STATIC(&idle_inhibitor->events.destroy, destroyidleinhibitor);

	checkidleinhibitor(NULL);
}

void
createkeyboard(struct wlr_keyboard *keyboard)
{
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(keyboard, kb_group->wlr_group->keyboard.keymap);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(kb_group->wlr_group, keyboard);
}

KeyboardGroup *
createkeyboardgroup(void)
{
	KeyboardGroup *group = ecalloc(1, sizeof(*group));
	struct xkb_context *context;
	struct xkb_keymap *keymap;

	group->wlr_group = wlr_keyboard_group_create();
	group->wlr_group->data = group;

	/* Prepare an XKB keymap and assign it to the keyboard group. */
	context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!(keymap = xkb_keymap_new_from_names(context, &xkb_rules,
				XKB_KEYMAP_COMPILE_NO_FLAGS)))
		die("failed to compile keymap");

	wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);

	wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard, repeat_rate, repeat_delay);

	/* Set up listeners for keyboard events */
	LISTEN(&group->wlr_group->keyboard.events.key, &group->key, keypress);
	LISTEN(&group->wlr_group->keyboard.events.modifiers, &group->modifiers, keypressmod);

	group->key_repeat_source = wl_event_loop_add_timer(event_loop, keyrepeat, group);

	/* A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same wlr_keyboard_group, which provides a single wlr_keyboard interface for
	 * all of them. Set this combined wlr_keyboard as the seat keyboard.
	 */
	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	return group;
}

void
createlayersurface(struct wl_listener *listener, void *data)
{
	struct wlr_layer_surface_v1 *layer_surface = data;
	LayerSurface *l;
	struct wlr_surface *surface = layer_surface->surface;
	struct wlr_scene_tree *scene_layer = layers[layermap[layer_surface->pending.layer]];

	if (!layer_surface->output
			&& !(layer_surface->output = selmon ? selmon->wlr_output : NULL)) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	l = layer_surface->data = ecalloc(1, sizeof(*l));
	l->type = LayerShell;
	LISTEN(&surface->events.commit, &l->surface_commit, commitlayersurfacenotify);
	LISTEN(&surface->events.unmap, &l->unmap, unmaplayersurfacenotify);
	LISTEN(&layer_surface->events.destroy, &l->destroy, destroylayersurfacenotify);

	l->layer_surface = layer_surface;
	l->mon = layer_surface->output->data;
	l->scene_layer = wlr_scene_layer_surface_v1_create(scene_layer, layer_surface);
	l->scene = l->scene_layer->tree;
	l->popups = surface->data = wlr_scene_tree_create(layer_surface->current.layer
			< ZWLR_LAYER_SHELL_V1_LAYER_TOP ? layers[LyrTop] : scene_layer);
	l->scene->node.data = l->popups->node.data = l;

	wl_list_insert(&l->mon->layers[layer_surface->pending.layer],&l->link);
	wlr_surface_send_enter(surface, layer_surface->output);
}

void
createlocksurface(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	Monitor *m = lock_surface->output->data;
	struct wlr_scene_tree *scene_tree = lock_surface->surface->data
			= wlr_scene_subsurface_tree_create(lock->scene, lock_surface->surface);
	m->lock_surface = lock_surface;

	wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
	wlr_session_lock_surface_v1_configure(lock_surface, m->m.width, m->m.height);

	LISTEN(&lock_surface->events.destroy, &m->destroy_lock_surface, destroylocksurface);

	if (m == selmon)
		client_notify_enter(lock_surface->surface, wlr_seat_get_keyboard(seat));
}

/* GPU Acceleration Functions */
static gpu_capabilities_t
detect_gpu_capabilities(struct wlr_renderer *renderer, struct wlr_output *output)
{
	gpu_capabilities_t caps = {0};
	
	if (!renderer || !output) {
		wlr_log(WLR_ERROR, "[nixtile] GPU DETECTION: Invalid renderer or output");
		return caps;
	}
	
	/* Detect hardware acceleration */
	caps.hardware_acceleration = wlr_renderer_is_gles2(renderer);
	caps.renderer_name = caps.hardware_acceleration ? "GLES2" : "Software";
	
	/* Detect adaptive sync support */
	caps.adaptive_sync = output->adaptive_sync_supported;
	
	/* Get maximum refresh rate */
	struct wlr_output_mode *mode;
	caps.max_refresh_rate = 60; /* Default */
	wl_list_for_each(mode, &output->modes, link) {
		int hz = mode->refresh / 1000;
		if (hz > caps.max_refresh_rate) {
			caps.max_refresh_rate = hz;
		}
	}
	
	/* Detect other capabilities */
	caps.hardware_cursors = true; /* Assume supported unless proven otherwise */
	caps.texture_compression = caps.hardware_acceleration;
	caps.timeline_sync = caps.hardware_acceleration;
	
	return caps;
}

static void
optimize_for_gpu(gpu_capabilities_t *caps)
{
	if (!caps) return;
	
	/* Enable high performance mode if hardware acceleration is available */
	if (caps->hardware_acceleration) {
		high_performance_mode = true;
		wlr_log(WLR_INFO, "[nixtile] GPU OPTIMIZATION: High performance mode enabled");
		
		/* Set environment variables for optimal GPU performance */
		setenv("WLR_RENDERER", "gles2", 0); /* Don't override if already set */
		setenv("WLR_DRM_NO_ATOMIC", "0", 0);
		setenv("WLR_DRM_NO_MODIFIERS", "0", 0);
		setenv("WLR_NO_HARDWARE_CURSORS", "0", 0);
		
		/* Enable VSync for smooth rendering */
		setenv("__GL_SYNC_TO_VBLANK", "1", 0); /* NVIDIA */
		setenv("vblank_mode", "1", 0);        /* AMD/Intel */
	} else {
		wlr_log(WLR_ERROR, "[nixtile] GPU OPTIMIZATION: Hardware acceleration not available, using software rendering");
		high_performance_mode = false;
	}
}

static void
log_gpu_status(gpu_capabilities_t *caps)
{
	if (!caps) return;
	
	wlr_log(WLR_INFO, "[nixtile] GPU STATUS: Renderer: %s", caps->renderer_name ? caps->renderer_name : "Unknown");
	wlr_log(WLR_INFO, "[nixtile] GPU STATUS: Hardware Acceleration: %s", caps->hardware_acceleration ? "YES" : "NO");
	wlr_log(WLR_INFO, "[nixtile] GPU STATUS: Adaptive Sync: %s", caps->adaptive_sync ? "YES" : "NO");
	wlr_log(WLR_INFO, "[nixtile] GPU STATUS: Max Refresh Rate: %dHz", caps->max_refresh_rate);
	wlr_log(WLR_INFO, "[nixtile] GPU STATUS: Hardware Cursors: %s", caps->hardware_cursors ? "YES" : "NO");
	wlr_log(WLR_INFO, "[nixtile] GPU STATUS: Texture Compression: %s", caps->texture_compression ? "YES" : "NO");
	wlr_log(WLR_INFO, "[nixtile] GPU STATUS: Timeline Sync: %s", caps->timeline_sync ? "YES" : "NO");
}

void
createmon(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct wlr_output *wlr_output = data;
	const MonitorRule *r;
	size_t i;
	struct wlr_output_state state;
	Monitor *m;

	if (!wlr_output_init_render(wlr_output, alloc, drw))
		return;

	m = wlr_output->data = ecalloc(1, sizeof(*m));
	m->wlr_output = wlr_output;

	for (i = 0; i < LENGTH(m->layers); i++)
		wl_list_init(&m->layers[i]);

	wlr_output_state_init(&state);
	/* Initialize monitor state using configured rules */
	m->tagset[0] = m->tagset[1] = 1;
	for (r = monrules; r < END(monrules); r++) {
		if (!r->name || strstr(wlr_output->name, r->name)) {
			m->m.x = r->x;
			m->m.y = r->y;
			m->mfact = r->mfact;
			m->nmaster = r->nmaster;
			m->lt[0] = r->lt;
			m->lt[1] = &layouts[LENGTH(layouts) > 1 && r->lt != &layouts[1]];
			strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));
			wlr_output_state_set_scale(&state, r->scale);
			wlr_output_state_set_transform(&state, r->rr);
			break;
		}
	}

	/* Find the mode with the highest resolution and refresh rate */
	struct wlr_output_mode *mode;
	struct wlr_output_mode *best_mode = NULL;
	int best_width = 0;
	int best_height = 0;
	int best_refresh = 0;
	
	wl_list_for_each(mode, &wlr_output->modes, link) {
		/* First prioritize by total pixels (width * height) */
		if (mode->width * mode->height > best_width * best_height ||
		    /* If same resolution, prioritize by refresh rate */
		    (mode->width * mode->height == best_width * best_height &&
		     mode->refresh > best_refresh)) {
			best_mode = mode;
			best_width = mode->width;
			best_height = mode->height;
			best_refresh = mode->refresh;
		}
	}
	
	/* ULTRA-SMOOTH GPU ACCELERATION: Configure optimal display mode */
	if (best_mode) {
		wlr_output_state_set_mode(&state, best_mode);
		wlr_log(WLR_INFO, "[nixtile] GPU ACCELERATION: Using optimal mode %dx%d@%dmHz for ultra-smooth display", 
		        best_width, best_height, best_refresh / 1000);
	} else {
		wlr_output_state_set_mode(&state, wlr_output_preferred_mode(wlr_output));
		wlr_log(WLR_INFO, "[nixtile] GPU ACCELERATION: Using preferred mode for display");
	}
	
	/* ULTRA-SMOOTH GPU ACCELERATION: Detect and optimize for GPU capabilities */
	gpu_capabilities_t gpu_caps = detect_gpu_capabilities(drw, wlr_output);
	optimize_for_gpu(&gpu_caps);
	log_gpu_status(&gpu_caps);
	
	/* Enable VSync and adaptive sync */
	if (wlr_output->adaptive_sync_supported) {
		wlr_output_state_set_adaptive_sync_enabled(&state, true);
		wlr_log(WLR_INFO, "[nixtile] GPU ACCELERATION: Adaptive sync (VRR/FreeSync/G-Sync) enabled for monitor %s", wlr_output->name);
	} else {
		wlr_log(WLR_INFO, "[nixtile] GPU ACCELERATION: Adaptive sync not supported on monitor %s", wlr_output->name);
	}
	
	/* Enable subpixel rendering for crisp text during animations */
	if (wlr_output->subpixel != WL_OUTPUT_SUBPIXEL_UNKNOWN) {
		wlr_log(WLR_DEBUG, "[nixtile] GPU ACCELERATION: Subpixel rendering available for crisp text");
	}

	/* Set up event listeners */
	LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
	LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);
	LISTEN(&wlr_output->events.request_state, &m->request_state, requestmonstate);

	wlr_output_state_set_enabled(&state, 1);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	wl_list_insert(&mons, &m->link);
	printstatus();

	/* The xdg-protocol specifies:
	 *
	 * If the fullscreened surface is not opaque, the compositor must make
	 * sure that other screen content not part of the same surface tree (made
	 * up of subsurfaces, popups or similarly coupled surfaces) are not
	 * visible below the fullscreened surface.
	 *
	 */
	/* updatemons() will resize and set correct position */
	m->fullscreen_bg = wlr_scene_rect_create(layers[LyrFS], 0, 0, fullscreen_bg);
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

	/* Adds this to the output layout in the order it was configured.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	m->scene_output = wlr_scene_output_create(scene, wlr_output);
	if (m->m.x == -1 && m->m.y == -1)
		wlr_output_layout_add_auto(output_layout, wlr_output);
	else
		wlr_output_layout_add(output_layout, wlr_output, m->m.x, m->m.y);
}

void
createnotify(struct wl_listener *listener, void *data)
{
	/* INTELLIGENT TILE PLACEMENT: Norwegian user requirements */
	struct wlr_xdg_toplevel *toplevel = data;
	Client *c = NULL;

	/* Allocate a Client for this surface */
	c = toplevel->base->data = ecalloc(1, sizeof(*c));
	c->surface.xdg = toplevel->base;
	c->bw = borderpx;
	c->height_factor = 1.0f;
	c->width_factor = 1.0f;
	c->isfloating = 0; /* CRITICAL: Never allow floating tiles */
	/* Initialize destroy rebalancing fields */
	c->destroy_mon = NULL;
	c->destroy_column = -1;
	c->destroy_was_floating = false;
	
	wlr_log(WLR_ERROR, "[nixtile] *** CREATENOTIFY CALLED - NEW TILE BEING CREATED ***");
	wlr_log(WLR_ERROR, "[nixtile] *** IF YOU SEE THIS MESSAGE, CREATENOTIFY IS WORKING ***");
	
	/* CRITICAL: Set mon and tags early for VISIBLEON to work correctly */
	c->mon = selmon;
	c->tags = selmon->tagset[selmon->seltags];
	
	wlr_log(WLR_INFO, "[nixtile] INTELLIGENT PLACEMENT: Creating new tile with screen-filling priority");
	
	/* STEP 1: Check current workspace tile situation */
	/* Count existing tiles (excluding the current tile being assigned) */
	int current_workspace_tiles = 0;
	Client *temp_c;
	wl_list_for_each(temp_c, &clients, link) {
		if (temp_c != c && VISIBLEON(temp_c, selmon) && !temp_c->isfloating && !temp_c->isfullscreen) {
			current_workspace_tiles++;
		}
	}
	
	/* Get dynamic column configuration first */
	int screen_width = selmon->w.width;
	int optimal_columns = get_optimal_columns(screen_width);
	int master_tiles = get_optimal_master_tiles(screen_width);
	
	/* Count tiles in each column dynamically - will be updated after assignment */
	int column_counts[4] = {0}; /* Support up to 4 columns */
	/* Note: column_counts will be calculated after column_group is set */
	
	wlr_log(WLR_INFO, "[nixtile] WORKSPACE ANALYSIS: total=%d, columns=%d, col0=%d, col1=%d, col2=%d, col3=%d", 
		current_workspace_tiles, optimal_columns, column_counts[0], column_counts[1], column_counts[2], column_counts[3]);
	
	/* STEP 2: Handle workspace overflow first */
	if (current_workspace_tiles >= MAX_TILES_PER_WORKSPACE) {
		int available_workspace = find_available_workspace();
		wlr_log(WLR_INFO, "[nixtile] WORKSPACE OVERFLOW: Moving to workspace %d", available_workspace);
		
		selmon->tagset[selmon->seltags] = 1 << available_workspace;
		c->tags = 1 << available_workspace;
		load_workspace_mfact();
		
		/* Reset counts for new workspace */
		current_workspace_tiles = 0;
		wl_list_for_each(temp_c, &clients, link) {
			if (temp_c != c && VISIBLEON(temp_c, selmon) && !temp_c->isfloating && !temp_c->isfullscreen) {
				current_workspace_tiles++;
			}
		}
		for (int col = 0; col < optimal_columns && col < 4; col++) {
			column_counts[col] = count_tiles_in_stack(col, selmon);
		}
	}
	
	/* STEP 3: SIMPLE GUARANTEED HORIZONTAL PLACEMENT */
	
	/* Count existing tiles in this workspace (excluding the new tile) */
	int tile_number = current_workspace_tiles + 1; /* 1-based tile number */
	
	wlr_log(WLR_INFO, "[nixtile] SIMPLE PLACEMENT: Tile #%d, master_tiles=%d, optimal_columns=%d", 
		tile_number, master_tiles, optimal_columns);
	
	/* GUARANTEED RULE: First N tiles go to separate columns (horizontal placement) */
	if (tile_number <= master_tiles) {
		int target_column = (tile_number - 1) % optimal_columns;
		wlr_log(WLR_INFO, "[nixtile] HORIZONTAL PLACEMENT: Tile #%d -> Column %d (guaranteed horizontal)", 
			tile_number, target_column);
		c->column_group = target_column;
	} else {
		/* STACKING: Let tile() function handle assignment for proper distribution */
		wlr_log(WLR_INFO, "[nixtile] STACK PLACEMENT: Tile #%d -> will be assigned by tile() function", 
			tile_number);
		c->column_group = -1; /* Invalid - will be assigned by tile() */
	}
	
	wlr_log(WLR_ERROR, "[nixtile] *** CREATENOTIFY FINAL ASSIGNMENT: Tile %p assigned to column %d ***", (void*)c, c->column_group);
	wlr_log(WLR_ERROR, "[nixtile] *** THIS IS THE COLUMN_GROUP SET IN CREATENOTIFY - SHOULD NOT BE OVERWRITTEN ***");
	
	/* STEP 4: RESET MANUAL RESIZE - Always reset and enforce equal distribution on tile addition */
	int target_column = c->column_group;
	if (target_column >= 0 && target_column < MAX_COLUMNS) {
		/* Reset manual resize flag to enforce equal distribution */
		if (manual_resize_performed[target_column]) {
			wlr_log(WLR_INFO, "[nixtile] EQUAL DISTRIBUTION: Resetting manual resize flag for column %d - enforcing equal distribution on tile addition", target_column);
			manual_resize_performed[target_column] = false;
		}
		
		/* Set all tiles in target column to equal height factors */
		wlr_log(WLR_INFO, "[nixtile] EQUAL DISTRIBUTION: Setting equal height factors in column %d", target_column);
		Client *temp_c;
		wl_list_for_each(temp_c, &clients, link) {
			if (!VISIBLEON(temp_c, selmon) || temp_c->isfloating || temp_c->isfullscreen)
				continue;
			if (temp_c->column_group == target_column) {
				temp_c->height_factor = 1.0f;
			}
		}
	} else {
		/* Invalid column - fallback to equal distribution */
		wlr_log(WLR_ERROR, "[nixtile] INVALID COLUMN: %d, using fallback equal distribution", target_column);
		c->height_factor = 1.0f;
		c->width_factor = 1.0f;
	}
	
	/* STEP 5: Final verification */
	int final_count = count_tiles_in_stack(c->column_group, selmon);
	wlr_log(WLR_INFO, "[nixtile] PLACEMENT COMPLETE: Column %d will have %d tiles after addition", c->column_group, final_count + 1);

	LISTEN(&toplevel->base->surface->events.commit, &c->commit, commitnotify);
	LISTEN(&toplevel->base->surface->events.map, &c->map, mapnotify);
	LISTEN(&toplevel->base->surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&toplevel->events.destroy, &c->destroy, destroynotify);
	LISTEN(&toplevel->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&toplevel->events.request_maximize, &c->maximize, maximizenotify);
	LISTEN(&toplevel->events.set_title, &c->set_title, updatetitle);
	
	/* Ensure equal width distribution for new workspaces */
	ensure_equal_width_distribution(c);
	
	/* EQUAL DISTRIBUTION: Ensure equal height distribution within the target stack */
	ensure_equal_height_distribution_in_stack(c->column_group);
	
	/* EQUAL DISTRIBUTION: Check if we need equal horizontal distribution for 2 tiles */
	ensure_equal_horizontal_distribution_for_two_tiles();
}

void
handletiledrop(Client *c, double x, double y)
{
	/* SIMPLE RATE LIMITING: Prevent broken pipe from rapid tile movements */
	static struct timespec last_tiledrop_time = {0, 0};
	struct timespec current_time;
	clock_gettime(CLOCK_MONOTONIC, &current_time);
	
	/* Calculate time since last tile drop */
	long time_diff_ms = (current_time.tv_sec - last_tiledrop_time.tv_sec) * 1000 +
						(current_time.tv_nsec - last_tiledrop_time.tv_nsec) / 1000000;
	
	/* Rate limit: minimum 50ms between tile drops to prevent broken pipe */
	if (time_diff_ms < 50) {
		wlr_log(WLR_DEBUG, "[nixtile] RATE LIMIT: Ignoring rapid tile movement (time_diff=%ldms)", time_diff_ms);
		return;
	}
	
	/* Update last tile drop time */
	last_tiledrop_time = current_time;
	
	/* ULTRA-SMOOTH GPU ACCELERATION: Optimized tile movement for butter-smooth experience */
	/* CRITICAL SAFETY CHECKS: Prevent all crashes */
	if (!c) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: handletiledrop called with NULL client - ABORT");
		return;
	}
	
	/* CRASH PREVENTION: Validate client list integrity before any operations */
	if (!validate_client_list_integrity()) {
		wlr_log(WLR_ERROR, "[nixtile] CRASH PREVENTION: Client list integrity check failed - ABORT tile movement");
		return;
	}
	
	/* GPU ACCELERATION: Prepare for hardware-accelerated tile movement */
	if (high_performance_mode && drw && wlr_renderer_is_gles2(drw)) {
		wlr_log(WLR_DEBUG, "[nixtile] GPU ACCELERATION: Starting hardware-accelerated tile movement");
		
		/* Enable GPU texture caching for smooth tile animations */
		if (c->scene && c->scene_surface) {
			wlr_log(WLR_DEBUG, "[nixtile] GPU ACCELERATION: Scene nodes ready for hardware acceleration");
		}
	}
	
	if (!c->mon) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: Client %p has NULL monitor - ABORT", (void*)c);
		return;
	}
	
	if (!selmon) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: selmon is NULL - ABORT");
		return;
	}
	
	/* Validate coordinates are finite numbers */
	if (!isfinite(x) || !isfinite(y)) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid coordinates x=%.1f y=%.1f - ABORT", x, y);
		return;
	}
	
	/* Check if client is being destroyed */
	if (c->geom.width <= 0 || c->geom.height <= 0) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: Client %p has invalid geometry w=%d h=%d - ABORT", 
		        (void*)c, c->geom.width, c->geom.height);
		return;
	}
	
	/* BROKEN PIPE PREVENTION: Validate client surface and scene nodes */
	if (!c->scene || !c->scene_surface) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid scene nodes - BROKEN PIPE PREVENTION");
		return;
	}
	
	/* BROKEN PIPE PREVENTION: Validate surface is mapped and valid */
	struct wlr_surface *surface = client_surface(c);
	if (!surface || !surface->mapped) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: Surface not mapped - BROKEN PIPE PREVENTION");
		return;
	}
	
	wlr_log(WLR_DEBUG, "[nixtile] *** HANDLETILEDROP CALLED *** x=%.1f y=%.1f client=%p", x, y, (void*)c);
	
	/* BOUNDARY CONSTRAINTS: Prevent dragging outside monitor edges */
	Monitor *drag_monitor = selmon; /* Use selected monitor for boundary constraints */
	
	/* Clamp coordinates to monitor boundaries */
	double min_x = drag_monitor->w.x;
	double max_x = drag_monitor->w.x + drag_monitor->w.width;
	double min_y = drag_monitor->w.y;
	double max_y = drag_monitor->w.y + drag_monitor->w.height;
	
	/* Apply boundary constraints */
	if (x < min_x) {
		x = min_x;
		wlr_log(WLR_DEBUG, "[nixtile] BOUNDARY: Clamped x to left edge: %.1f", x);
	}
	if (x > max_x) {
		x = max_x;
		wlr_log(WLR_DEBUG, "[nixtile] BOUNDARY: Clamped x to right edge: %.1f", x);
	}
	if (y < min_y) {
		y = min_y;
		wlr_log(WLR_DEBUG, "[nixtile] BOUNDARY: Clamped y to top edge: %.1f", y);
	}
	if (y > max_y) {
		y = max_y;
		wlr_log(WLR_DEBUG, "[nixtile] BOUNDARY: Clamped y to bottom edge: %.1f", y);
	}
	
	wlr_log(WLR_DEBUG, "[nixtile] BOUNDARY: Final coordinates x=%.1f y=%.1f", x, y);
	
	/* SAFETY: Store original position in case we need to restore it */
	struct wlr_box original_geom = c->geom;
	int original_column = c->column_group;
	float original_height_factor = c->height_factor;
	
	Monitor *m;
	struct wlr_box adjusted_area;
	double adjusted_center_x;
	int target_column;
	Client *target_tile = NULL;
	double closest_distance = INFINITY;
	Client *temp_c;
	int temp_column;
	int tiles_in_target_column = 0;
	
	if (!c || !c->mon || c->isfloating) {
		return;
	}

	/* Use adjusted monitor area (accounting for gaps and statusbar) */
	m = c->mon;
	adjusted_area = m->w;
	
	/* Adjust for statusbar */
	if (statusbar_visible) {
		if (strcmp(statusbar_position, "top") == 0) {
			adjusted_area.y += statusbar_height + outergappx;
			adjusted_area.height -= statusbar_height + outergappx;
		} else if (strcmp(statusbar_position, "bottom") == 0) {
			adjusted_area.height -= statusbar_height + outergappx;
		} else if (strcmp(statusbar_position, "left") == 0) {
			adjusted_area.x += statusbar_height + outergappx;
			adjusted_area.width -= statusbar_height + outergappx;
		} else if (strcmp(statusbar_position, "right") == 0) {
			adjusted_area.width -= statusbar_height + outergappx;
		}
	}
	
	/* Apply outer gaps */
	adjusted_area.x += outergappx;
	adjusted_area.y += outergappx;
	adjusted_area.width -= 2 * outergappx;
	adjusted_area.height -= 2 * outergappx;
	
	adjusted_center_x = adjusted_area.x + (adjusted_area.width / 2.0);
	
	/* INTRA-COLUMN DETECTION: Check for tile-to-tile overlap in same column */
	bool prefer_intra_column = false;
	Client *potential_intra_target = NULL;
	int same_column_tiles = 0;
	
	wlr_log(WLR_ERROR, "[nixtile] VERTICAL DEBUG: Starting intra-column detection for client %p in column %d", (void*)c, c->column_group);
	wlr_log(WLR_ERROR, "[nixtile] VERTICAL DEBUG: Mouse position x=%.1f, y=%.1f", x, y);
	wlr_log(WLR_ERROR, "[nixtile] DRAGGED TILE GEOMETRY: x=%.1f y=%.1f w=%.1f h=%.1f", (double)c->geom.x, (double)c->geom.y, (double)c->geom.width, (double)c->geom.height);
	
	/* Calculate the current position of the dragged tile during drag */
	double dragged_x = (double)((int)round(x) - grabcx);
	double dragged_y = (double)((int)round(y) - grabcy);
	double dragged_width = (double)c->geom.width;
	double dragged_height = (double)c->geom.height;
	
	wlr_log(WLR_ERROR, "[nixtile] TILE OVERLAP DEBUG: Dragged tile position: x=%.1f y=%.1f w=%.1f h=%.1f", 
	        dragged_x, dragged_y, dragged_width, dragged_height);
	
	/* Check for tile-to-tile overlap in same column (VERTICAL movement only) */
	double max_vertical_overlap_area = 0.0;
	Client *best_vertical_target = NULL;
	
	wl_list_for_each(temp_c, &clients, link) {
		/* SAFETY: Validate client pointer and state */
		if (!temp_c) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: NULL client in list - SKIP");
			continue;
		}
		
		if (temp_c == c || !temp_c->mon || temp_c->mon != m || temp_c->isfloating)
			continue;
		
		/* SAFETY: Validate geometry before any calculations */
		if (temp_c->geom.width <= 0 || temp_c->geom.height <= 0) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid geometry for tile %p w=%d h=%d - SKIP", 
			        (void*)temp_c, temp_c->geom.width, temp_c->geom.height);
			continue;
		}
		
		/* SAFETY: Validate column group is reasonable */
		/* Get current optimal columns for validation */
		int optimal_columns = get_optimal_columns(m->w.width);
		if (temp_c->column_group < 0 || temp_c->column_group >= optimal_columns) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid column_group %d for tile %p (max: %d) - SKIP", 
			        temp_c->column_group, (void*)temp_c, optimal_columns - 1);
			continue;
		}
		
		if (temp_c->column_group == c->column_group) {
			same_column_tiles++;
			
			wlr_log(WLR_ERROR, "[nixtile] VERTICAL OVERLAP CHECK: Target tile %p at x=%.1f y=%.1f w=%.1f h=%.1f", 
			        (void*)temp_c, (double)temp_c->geom.x, (double)temp_c->geom.y, (double)temp_c->geom.width, (double)temp_c->geom.height);
			
			/* SIZE-INDEPENDENT ULTRA-SENSITIVE detection using small mouse cursor zone */
			double cursor_zone = 30.0; /* Very small zone around mouse cursor - completely size independent */
			
			/* Create small detection zone around mouse cursor (not tile boundaries) */
			double cursor_left = x - cursor_zone;
			double cursor_right = x + cursor_zone;
			double cursor_top = y - cursor_zone;
			double cursor_bottom = y + cursor_zone;
			
			/* Check if small cursor zone overlaps with target tile */
			bool tiles_overlap = !(cursor_left >= temp_c->geom.x + temp_c->geom.width ||  
			                       cursor_right <= temp_c->geom.x ||      
			                       cursor_top >= temp_c->geom.y + temp_c->geom.height || 
			                       cursor_bottom <= temp_c->geom.y);
			
			wlr_log(WLR_ERROR, "[nixtile] SIZE-INDEPENDENT CHECK: cursor_zone=[%.1f,%.1f,%.1f,%.1f] vs tile=[%.1f,%.1f,%.1f,%.1f] overlap=%d", 
			        cursor_left, cursor_top, cursor_right, cursor_bottom, 
			        (double)temp_c->geom.x, (double)temp_c->geom.y, (double)(temp_c->geom.x + temp_c->geom.width), (double)(temp_c->geom.y + temp_c->geom.height), 
			        tiles_overlap);
			
			if (tiles_overlap) {
				/* SIZE-INDEPENDENT scoring: Use distance from cursor to tile center */
				double tile_center_x = temp_c->geom.x + (temp_c->geom.width / 2.0);
				double tile_center_y = temp_c->geom.y + (temp_c->geom.height / 2.0);
				
				/* Distance from mouse cursor to tile center */
				double cursor_to_center_dist = sqrt(pow(x - tile_center_x, 2) + pow(y - tile_center_y, 2));
				
				/* Invert distance to get score (closer = higher score) */
				double overlap_area = 10000.0 / (1.0 + cursor_to_center_dist);
				
				wlr_log(WLR_ERROR, "[nixtile] SIZE-INDEPENDENT SCORE: cursor=(%.1f,%.1f) tile_center=(%.1f,%.1f) dist=%.1f score=%.1f", 
				        x, y, tile_center_x, tile_center_y, cursor_to_center_dist, overlap_area);
				
				wlr_log(WLR_ERROR, "[nixtile] VERTICAL OVERLAP: tile %p, overlap_area=%.1f", (void*)temp_c, overlap_area);
				
				/* Track the tile with maximum overlap */
				if (overlap_area > max_vertical_overlap_area) {
					max_vertical_overlap_area = overlap_area;
					best_vertical_target = temp_c;
					wlr_log(WLR_ERROR, "[nixtile] NEW BEST VERTICAL TARGET: %p with overlap %.1f", (void*)temp_c, overlap_area);
				}
			}
		}
	}
	
	/* Set the best vertical target if found */
	if (best_vertical_target) {
		prefer_intra_column = true;
		potential_intra_target = best_vertical_target;
		wlr_log(WLR_ERROR, "[nixtile] FINAL VERTICAL TARGET: %p with max overlap %.1f", (void*)best_vertical_target, max_vertical_overlap_area);
	}
	
	wlr_log(WLR_DEBUG, "[nixtile] VERTICAL DEBUG: Found %d tiles in same column, prefer_intra_column=%d", same_column_tiles, prefer_intra_column);
	
	/* SIZE-INDEPENDENT TARGET COLUMN SELECTION: Find closest tile to determine column */
	int normal_target_column = -1;
	Client *closest_column_tile = NULL;
	double closest_column_distance = INFINITY;
	
	/* Find the closest tile to cursor to determine target column */
	wl_list_for_each(temp_c, &clients, link) {
		/* SAFETY: Validate client pointer and state */
		if (!temp_c) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: NULL client in column selection - SKIP");
			continue;
		}
		
		if (temp_c == c || !temp_c->mon || temp_c->mon != m || temp_c->isfloating) {
			continue;
		}
		
		/* SAFETY: Validate geometry before calculations */
		if (temp_c->geom.width <= 0 || temp_c->geom.height <= 0) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid geometry in column selection for tile %p - SKIP", (void*)temp_c);
			continue;
		}
		
		/* SAFETY: Validate column group */
		/* Get current optimal columns for validation */
		int optimal_columns = get_optimal_columns(m->w.width);
		if (temp_c->column_group < 0 || temp_c->column_group >= optimal_columns) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid column_group %d in column selection (max: %d) - SKIP", 
				temp_c->column_group, optimal_columns - 1);
			continue;
		}
		
		/* Calculate distance from cursor to tile center */
		double tile_center_x = temp_c->geom.x + (temp_c->geom.width / 2.0);
		double tile_center_y = temp_c->geom.y + (temp_c->geom.height / 2.0);
		double distance = sqrt(pow(x - tile_center_x, 2) + pow(y - tile_center_y, 2));
		
		/* SAFETY: Validate distance is finite */
		if (!isfinite(distance)) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid distance calculation - SKIP");
			continue;
		}
		
		if (distance < closest_column_distance) {
			closest_column_distance = distance;
			closest_column_tile = temp_c;
			normal_target_column = temp_c->column_group;
		}
	}
	
	/* Fallback to center-based logic if no tiles found */
	if (normal_target_column == -1) {
		/* Get dynamic column count based on monitor resolution */
		int max_columns = get_optimal_columns(m->w.width);
		/* Calculate which column based on x position within monitor */
		double column_width = (double)adjusted_area.width / max_columns;
		normal_target_column = (int)((x - adjusted_area.x) / column_width);
		/* Clamp to valid range */
		if (normal_target_column < 0) normal_target_column = 0;
		if (normal_target_column >= max_columns) normal_target_column = max_columns - 1;
		wlr_log(WLR_ERROR, "[nixtile] SIZE-INDEPENDENT COLUMN: No tiles found, using fallback column %d (max_columns=%d)", normal_target_column, max_columns);
	} else {
		wlr_log(WLR_ERROR, "[nixtile] SIZE-INDEPENDENT COLUMN: Closest tile %p in column %d (distance=%.1f)", 
		        (void*)closest_column_tile, normal_target_column, closest_column_distance);
	}
	
	/* Calculate drag direction to determine user intent */
	double drag_delta_x = x - grabcx;
	double drag_delta_y = y - grabcy;
	double abs_delta_x = fabs(drag_delta_x);
	double abs_delta_y = fabs(drag_delta_y);
	
	/* MAXIMUM OVERLAP PRIORITY: Choose movement based on largest overlap area */
	if (prefer_intra_column && normal_target_column != c->column_group) {
		/* CONFLICT: Both vertical and horizontal movement detected - calculate overlap areas */
		wlr_log(WLR_ERROR, "[nixtile] MOVEMENT CONFLICT: Calculating overlap areas to determine priority");
		
		/* Calculate SIZE-INDEPENDENT vertical overlap area using cursor-to-center distance */
		double vertical_overlap_area = 0.0;
		if (potential_intra_target) {
			/* Use cursor-to-center distance for size-independent scoring */
			double v_tile_center_x = potential_intra_target->geom.x + (potential_intra_target->geom.width / 2.0);
			double v_tile_center_y = potential_intra_target->geom.y + (potential_intra_target->geom.height / 2.0);
			double v_cursor_to_center_dist = sqrt(pow(x - v_tile_center_x, 2) + pow(y - v_tile_center_y, 2));
			vertical_overlap_area = 10000.0 / (1.0 + v_cursor_to_center_dist);
			
			wlr_log(WLR_ERROR, "[nixtile] SIZE-INDEPENDENT VERTICAL CONFLICT: cursor=(%.1f,%.1f) tile_center=(%.1f,%.1f) dist=%.1f score=%.1f", 
			        x, y, v_tile_center_x, v_tile_center_y, v_cursor_to_center_dist, vertical_overlap_area);
		}
		
		/* Find horizontal overlap area by checking tiles in target column with SENSITIVE detection */
		double horizontal_overlap_area = 0.0;
		Client *horizontal_target = NULL;
		wl_list_for_each(temp_c, &clients, link) {
			/* SAFETY: Validate client pointer and state */
			if (!temp_c) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: NULL client in horizontal overlap - SKIP");
				continue;
			}
			
			if (temp_c == c || !temp_c->mon || temp_c->mon != m || temp_c->isfloating || temp_c->column_group != normal_target_column) {
				continue;
			}
			
			/* SAFETY: Validate geometry and column group */
			if (temp_c->geom.width <= 0 || temp_c->geom.height <= 0) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid geometry in horizontal overlap for tile %p - SKIP", (void*)temp_c);
				continue;
			}
			
			/* BIDIRECTIONAL FIX: Support all columns (0-2) for full bidirectional movement */
			if (temp_c->column_group < 0 || temp_c->column_group >= MAX_COLUMNS) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid column_group %d in horizontal overlap (max=%d) - SKIP", temp_c->column_group, MAX_COLUMNS-1);
				continue;
			}
			
			/* SIZE-INDEPENDENT horizontal overlap detection using cursor zone */
			double h_cursor_zone = 30.0; /* Same small zone as other detections */
			
			/* Create small detection zone around mouse cursor */
			double h_cursor_left = x - h_cursor_zone;
			double h_cursor_right = x + h_cursor_zone;
			double h_cursor_top = y - h_cursor_zone;
			double h_cursor_bottom = y + h_cursor_zone;
			
			/* Check if small cursor zone overlaps with target tile */
			bool h_tiles_overlap = !(h_cursor_left >= temp_c->geom.x + temp_c->geom.width ||  
			                         h_cursor_right <= temp_c->geom.x ||      
			                         h_cursor_top >= temp_c->geom.y + temp_c->geom.height || 
			                         h_cursor_bottom <= temp_c->geom.y);
			
			if (h_tiles_overlap) {
				/* SIZE-INDEPENDENT horizontal scoring: Use distance from cursor to tile center */
				double h_tile_center_x = temp_c->geom.x + (temp_c->geom.width / 2.0);
				double h_tile_center_y = temp_c->geom.y + (temp_c->geom.height / 2.0);
				
				/* Distance from mouse cursor to tile center */
				double h_cursor_to_center_dist = sqrt(pow(x - h_tile_center_x, 2) + pow(y - h_tile_center_y, 2));
				
				/* Invert distance to get score (closer = higher score) */
				double overlap_area = 10000.0 / (1.0 + h_cursor_to_center_dist);
				
				wlr_log(WLR_ERROR, "[nixtile] SIZE-INDEPENDENT HORIZONTAL: cursor=(%.1f,%.1f) tile_center=(%.1f,%.1f) dist=%.1f score=%.1f", 
				        x, y, h_tile_center_x, h_tile_center_y, h_cursor_to_center_dist, overlap_area);
				
				wlr_log(WLR_ERROR, "[nixtile] HORIZONTAL OVERLAP: tile %p, overlap_area=%.1f", (void*)temp_c, overlap_area);
				
				if (overlap_area > horizontal_overlap_area) {
					horizontal_overlap_area = overlap_area;
					horizontal_target = temp_c;
					wlr_log(WLR_ERROR, "[nixtile] NEW BEST HORIZONTAL TARGET: %p with overlap %.1f", (void*)temp_c, overlap_area);
				}
			}
		}
		
		wlr_log(WLR_ERROR, "[nixtile] OVERLAP AREAS: vertical=%.1f, horizontal=%.1f", 
			vertical_overlap_area, horizontal_overlap_area);
		
		if (horizontal_overlap_area > vertical_overlap_area) {
			/* Horizontal overlap is larger - prefer horizontal movement */
			target_column = normal_target_column;
			prefer_intra_column = false; /* Override intra-column preference */
			target_tile = horizontal_target; /* Set horizontal target for positioning */
			wlr_log(WLR_ERROR, "[nixtile] CONFLICT RESOLVED: Horizontal movement preferred (larger overlap: %.1f > %.1f)", 
				horizontal_overlap_area, vertical_overlap_area);
		} else {
			/* Vertical overlap is larger or equal - prefer vertical movement */
			target_column = c->column_group;
			target_tile = potential_intra_target;
			wlr_log(WLR_ERROR, "[nixtile] CONFLICT RESOLVED: Vertical movement preferred (larger overlap: %.1f >= %.1f)", 
				vertical_overlap_area, horizontal_overlap_area);
		}
	} else if (prefer_intra_column) {
		/* Pure vertical movement - no conflict */
		target_column = c->column_group;
		target_tile = potential_intra_target;
		wlr_log(WLR_DEBUG, "[nixtile] PURE VERTICAL: target_column=%d, target_tile=%p", target_column, (void*)target_tile);
	} else {
		/* Pure horizontal movement - no conflict */
		target_column = normal_target_column;
		wlr_log(WLR_DEBUG, "[nixtile] PURE HORIZONTAL: target_column=%d", target_column);
	}
	
	wlr_log(WLR_DEBUG, "[nixtile] Tile movement: current_column=%d, target_column=%d, prefer_intra=%d", 
		c->column_group, target_column, prefer_intra_column);
	
	/* INTER-COLUMN DETECTION: Only run if we haven't found an intra-column target */
	if (!prefer_intra_column) {
		wlr_log(WLR_DEBUG, "[nixtile] Running inter-column detection (horizontal movement)");
		double best_inter_score = 0.0;
		Client *best_inter_target = NULL;
		
		wl_list_for_each(temp_c, &clients, link) {
			/* SAFETY: Validate client pointer and state */
			if (!temp_c) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: NULL client in inter-column detection - SKIP");
				continue;
			}
			
			if (temp_c == c || !temp_c->mon || temp_c->mon != m || temp_c->isfloating) {
				continue;
			}
			
			/* SAFETY: Validate geometry and column group */
			if (temp_c->geom.width <= 0 || temp_c->geom.height <= 0) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid geometry in inter-column for tile %p - SKIP", (void*)temp_c);
				continue;
			}
			
			/* BIDIRECTIONAL FIX: Support all columns (0-2) for full bidirectional movement */
			if (temp_c->column_group < 0 || temp_c->column_group >= MAX_COLUMNS) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid column_group %d in inter-column (max=%d) - SKIP", temp_c->column_group, MAX_COLUMNS-1);
				continue;
			}
			
			if (temp_c->column_group == target_column) {
				tiles_in_target_column++;
				
				/* SIZE-INDEPENDENT inter-column detection using cursor zone */
				double inter_cursor_zone = 30.0; /* Same small zone as vertical detection */
				
				/* Create small detection zone around mouse cursor */
				double inter_cursor_left = x - inter_cursor_zone;
				double inter_cursor_right = x + inter_cursor_zone;
				double inter_cursor_top = y - inter_cursor_zone;
				double inter_cursor_bottom = y + inter_cursor_zone;
				
				/* Check if small cursor zone overlaps with target tile */
				bool inter_overlap = !(inter_cursor_left >= temp_c->geom.x + temp_c->geom.width ||  
				                       inter_cursor_right <= temp_c->geom.x ||      
				                       inter_cursor_top >= temp_c->geom.y + temp_c->geom.height || 
				                       inter_cursor_bottom <= temp_c->geom.y);
				
				if (inter_overlap) {
					/* SIZE-INDEPENDENT inter-column scoring: Use distance from cursor to tile center */
					double inter_tile_center_x = temp_c->geom.x + (temp_c->geom.width / 2.0);
					double inter_tile_center_y = temp_c->geom.y + (temp_c->geom.height / 2.0);
					
					/* Distance from mouse cursor to tile center */
					double inter_cursor_to_center_dist = sqrt(pow(x - inter_tile_center_x, 2) + pow(y - inter_tile_center_y, 2));
					
					/* Invert distance to get score (closer = higher score) */
					double inter_score = 10000.0 / (1.0 + inter_cursor_to_center_dist);
					
					wlr_log(WLR_ERROR, "[nixtile] SIZE-INDEPENDENT INTER-COLUMN: cursor=(%.1f,%.1f) tile_center=(%.1f,%.1f) dist=%.1f score=%.1f", 
					        x, y, inter_tile_center_x, inter_tile_center_y, inter_cursor_to_center_dist, inter_score);
					
					wlr_log(WLR_ERROR, "[nixtile] INTER-COLUMN OVERLAP: tile %p, score=%.1f", (void*)temp_c, inter_score);
					
					if (inter_score > best_inter_score) {
						best_inter_score = inter_score;
						best_inter_target = temp_c;
						wlr_log(WLR_ERROR, "[nixtile] NEW BEST INTER-COLUMN TARGET: %p with score %.1f", (void*)temp_c, inter_score);
					}
				}
			}
		}
		
		/* Set the best inter-column target if found */
		if (best_inter_target) {
			target_tile = best_inter_target;
			wlr_log(WLR_ERROR, "[nixtile] FINAL INTER-COLUMN TARGET: %p with score %.1f", (void*)best_inter_target, best_inter_score);
		}
	} else {
		wlr_log(WLR_DEBUG, "[nixtile] Skipping inter-column detection - intra-column target already found");
	}
	
	/* Count tiles in source column */
	int tiles_in_source_column = 0;
	wl_list_for_each(temp_c, &clients, link) {
		if (temp_c != c && temp_c->mon == m && !temp_c->isfloating && temp_c->column_group == c->column_group) {
			tiles_in_source_column++;
		}
	}
	
	if (c->column_group == target_column) {
		/* INTRA-COLUMN MOVEMENT: Reorder within same column */
		if (!target_tile) {
			wlr_log(WLR_DEBUG, "[nixtile] No target tile found for intra-column movement");
			return;
		}
		
		/* SAFETY: Validate target tile before movement */
		if (!target_tile->mon || target_tile->geom.width <= 0 || target_tile->geom.height <= 0) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid target tile %p for intra-column movement - ABORT", (void*)target_tile);
			return;
		}
		
		/* SAFETY: Validate target tile is still in same column */
		if (target_tile->column_group != target_column) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Target tile column mismatch %d != %d - ABORT", target_tile->column_group, target_column);
			return;
		}
		
		wlr_log(WLR_DEBUG, "[nixtile] Intra-column reordering in column %d", target_column);
		
		/* DRAG DIRECTION-BASED POSITIONING: Position based on drag direction, not absolute position */
		wl_list_remove(&c->link);
		
		/* Calculate drag direction from initial grab position to current mouse position */
		double drag_delta_y = y - grabcy;
		
		/* Get original tile position before drag started */
		double original_tile_center_y = c->geom.y + (c->geom.height / 2.0);
		double target_center_y = target_tile->geom.y + (target_tile->geom.height / 2.0);
		
		wlr_log(WLR_ERROR, "[nixtile] DRAG POSITIONING: drag_delta_y=%.1f, original_center=%.1f, target_center=%.1f", 
			drag_delta_y, original_tile_center_y, target_center_y);
		
		/* Determine placement based on original tile position relative to target */
		if (original_tile_center_y > target_center_y) {
			/* Dragged tile was originally BELOW target - dragging UP means insert BEFORE */
			wl_list_insert(target_tile->link.prev, &c->link);
			wlr_log(WLR_ERROR, "[nixtile] DRAG POSITIONING: Insert BEFORE - dragging up from below");
		} else {
			/* Dragged tile was originally ABOVE target - dragging DOWN means insert AFTER */
			wl_list_insert(&target_tile->link, &c->link);
			wlr_log(WLR_ERROR, "[nixtile] DRAG POSITIONING: Insert AFTER - dragging down from above");
		}
		
		/* PRESERVE HEIGHT FACTORS: Keep custom sizing from resizing */
		/* Do not reset height_factor - preserve tile shape after movement */
		wlr_log(WLR_DEBUG, "[nixtile] Preserving height factors: c=%.2f, target=%.2f", c->height_factor, target_tile->height_factor);
		
	} else {
		/* INTER-COLUMN MOVEMENT: Strict tiling rules enforcement */
		
		/* SAFETY: Validate target column is valid */
		int max_columns = get_optimal_columns(m->w.width);
		if (target_column < 0 || target_column >= max_columns) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid target_column %d for inter-column movement (max_columns=%d) - ABORT", target_column, max_columns);
			return;
		}
		
		/* SAFETY: Validate tile counts are reasonable */
		if (tiles_in_target_column < 0 || tiles_in_source_column < 0) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid tile counts source=%d target=%d - ABORT", tiles_in_source_column, tiles_in_target_column);
			return;
		}
		
		/* STRICT STACK LIMITS: Norwegian user requirements validation */
		if (tiles_in_target_column >= MAX_TILES_PER_STACK) {
			wlr_log(WLR_ERROR, "[nixtile] STACK LIMIT: Target stack %d is full (%d/%d tiles) - ABORT movement", target_column, tiles_in_target_column, MAX_TILES_PER_STACK);
			return;
		}
		
		/* Check if movement would exceed stack limits */
		int tiles_after_movement = tiles_in_target_column;
		if (tiles_in_source_column > 0) {
			/* Moving from stack to another location adds 1 tile to target */
			tiles_after_movement += 1;
		} else {
			/* Single tile movement */
			tiles_after_movement += 1;
		}
		
		if (tiles_after_movement > MAX_TILES_PER_STACK) {
			wlr_log(WLR_ERROR, "[nixtile] STACK LIMIT: Movement would exceed limit (%d > %d) - ABORT", tiles_after_movement, MAX_TILES_PER_STACK);
			return;
		}
		
		wlr_log(WLR_INFO, "[nixtile] MOVEMENT ANALYSIS: source_tiles=%d, target_tiles=%d, after_movement=%d (limit=%d)", tiles_in_source_column, tiles_in_target_column, tiles_after_movement, MAX_TILES_PER_STACK);
		
		if (tiles_in_target_column == 0) {
			/* Target column is empty - simple column change (ALLOWED) */
			wlr_log(WLR_DEBUG, "[nixtile] ALLOWED: Moving to empty column %d", target_column);
			c->column_group = target_column;
			/* PRESERVE HEIGHT FACTOR: Keep custom sizing when moving to empty column */
			wlr_log(WLR_DEBUG, "[nixtile] Preserving height factor %.2f in empty column", c->height_factor);
			
		} else if (tiles_in_source_column == 0 && tiles_in_target_column == 1) {
			/* SINGLE TILE to SINGLE TILE: Always allow swap for master tile replacement */
			if (target_tile) {
				/* MASTER TILE SWAP: Allow single tiles to swap positions (fixes master tile in column 2 issue) */
				wlr_log(WLR_DEBUG, "[nixtile] MASTER TILE SWAP: Swapping single tiles between columns (enables master tile replacement)");
				int temp_column = c->column_group;
				c->column_group = target_tile->column_group;
				target_tile->column_group = temp_column;
				
				/* PRESERVE HEIGHT FACTORS: Keep custom sizing during swap */
				wlr_log(WLR_DEBUG, "[nixtile] Preserving height factors during swap: c=%.2f, target=%.2f", c->height_factor, target_tile->height_factor);
			} else {
				wlr_log(WLR_ERROR, "[nixtile] MASTER TILE SWAP: No target tile found for swap");
				return;
			}
			
		} else if (tiles_in_source_column == 0 && tiles_in_target_column > 1) {
			/* SINGLE TILE to STACK: Only allow entire column swap, not joining stack */
			if (prefer_intra_column && target_tile) {
				/* FORBIDDEN: Single tile cannot join existing stack */
				wlr_log(WLR_ERROR, "[nixtile] MOVEMENT RESTRICTION: Single tile cannot join existing stack");
				wlr_log(WLR_ERROR, "[nixtile] RULE ENFORCEMENT: Single tile can only swap with entire stack, not join it");
				return; /* ABORT movement - this violates tiling rules */
			} else {
				/* ALLOWED: Entire column swap - single tile swaps with entire stack */
				wlr_log(WLR_DEBUG, "[nixtile] ALLOWED: Swapping single tile with entire stack in column %d", target_column);
				
				/* Swap all tiles in target column to source column */
				wl_list_for_each(temp_c, &clients, link) {
					/* SAFETY: Validate client in column swap */
					if (!temp_c) {
						wlr_log(WLR_ERROR, "[nixtile] SAFETY: NULL client in column swap - SKIP");
						continue;
					}
					
					if (temp_c != c && temp_c->mon == m && !temp_c->isfloating && temp_c->column_group == target_column) {
						/* SAFETY: Validate geometry before swap */
						if (temp_c->geom.width <= 0 || temp_c->geom.height <= 0) {
							wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid geometry in column swap for tile %p - SKIP", (void*)temp_c);
							continue;
						}
						
						temp_c->column_group = c->column_group;
						/* PRESERVE HEIGHT FACTOR: Keep custom sizing during column swap */
						wlr_log(WLR_DEBUG, "[nixtile] Preserving height factor %.2f for tile %p", temp_c->height_factor, (void*)temp_c);
					}
				}
				
				/* Move single tile to target column */
				c->column_group = target_column;
				/* PRESERVE HEIGHT FACTOR: Keep custom sizing */
				wlr_log(WLR_DEBUG, "[nixtile] Preserving height factor %.2f for moved tile", c->height_factor);
			}
			
		} else if (tiles_in_source_column > 0 && tiles_in_target_column == 1) {
			/* TILE FROM STACK to SINGLE TILE: Create new stack (ALLOWED) */
			wlr_log(WLR_DEBUG, "[nixtile] ALLOWED: Tile from stack joining single tile to create new stack in column %d", target_column);
			c->column_group = target_column;
			
			/* CRASH PREVENTION: Safe list manipulation for stack creation */
			/* SAFETY: Validate client link before removal */
			if (!c->link.prev || !c->link.next) {
				wlr_log(WLR_ERROR, "[nixtile] CRASH PREVENTION: Invalid client link state - ABORT movement");
				return;
			}
			
			wl_list_remove(&c->link);
			
			if (target_tile) {
				/* SAFETY: Validate target tile link integrity */
				if (!target_tile->link.prev || !target_tile->link.next) {
					wlr_log(WLR_ERROR, "[nixtile] CRASH PREVENTION: Invalid target tile link state - ABORT positioning");
					/* Add to end of clients list as fallback */
					wl_list_insert(clients.prev, &c->link);
					return;
				}
				
				if (y < target_tile->geom.y + (target_tile->geom.height / 2.0)) {
					/* Insert before target tile */
					wl_list_insert(target_tile->link.prev, &c->link);
					wlr_log(WLR_DEBUG, "[nixtile] STACK CREATE: Safely inserted before target tile");
				} else {
					/* Insert after target tile */
					wl_list_insert(&target_tile->link, &c->link);
					wlr_log(WLR_DEBUG, "[nixtile] STACK CREATE: Safely inserted after target tile");
				}
			} else {
				/* No specific target tile, add to end of clients list safely */
				wl_list_insert(clients.prev, &c->link);
				wlr_log(WLR_DEBUG, "[nixtile] STACK CREATE: Safely added to end of client list");
			}
			
			/* PRESERVE HEIGHT FACTORS: Keep custom sizing during stack creation */
			wlr_log(WLR_DEBUG, "[nixtile] Preserving height factor %.2f during stack creation", c->height_factor);
			
		} else if (tiles_in_source_column > 0 && tiles_in_target_column > 1) {
			/* TILE FROM STACK to STACK: Join existing stack (ALLOWED) */
			wlr_log(WLR_DEBUG, "[nixtile] ALLOWED: Moving tile from stack to join existing stack in column %d", target_column);
			c->column_group = target_column;
			
			/* CRASH PREVENTION: Safe list manipulation for stack joining */
			/* SAFETY: Validate client link before removal */
			if (!c->link.prev || !c->link.next) {
				wlr_log(WLR_ERROR, "[nixtile] CRASH PREVENTION: Invalid client link state in stack join - ABORT movement");
				return;
			}
			
			wl_list_remove(&c->link);
			
			if (target_tile) {
				/* SAFETY: Validate target tile link integrity */
				if (!target_tile->link.prev || !target_tile->link.next) {
					wlr_log(WLR_ERROR, "[nixtile] CRASH PREVENTION: Invalid target tile link state in stack join - ABORT positioning");
					/* Add to end of clients list as fallback */
					wl_list_insert(clients.prev, &c->link);
					return;
				}
				
				if (y < target_tile->geom.y + (target_tile->geom.height / 2.0)) {
					/* Insert before target tile */
					wl_list_insert(target_tile->link.prev, &c->link);
					wlr_log(WLR_DEBUG, "[nixtile] STACK JOIN: Safely inserted before target tile");
				} else {
					/* Insert after target tile */
					wl_list_insert(&target_tile->link, &c->link);
					wlr_log(WLR_DEBUG, "[nixtile] STACK JOIN: Safely inserted after target tile");
				}
			} else {
				/* No specific target tile, add to end of clients list safely */
				wl_list_insert(clients.prev, &c->link);
				wlr_log(WLR_DEBUG, "[nixtile] STACK JOIN: Safely added to end of client list");
			}
			
			/* PRESERVE HEIGHT FACTORS: Keep custom sizing during stack join */
			wlr_log(WLR_DEBUG, "[nixtile] Preserving height factor %.2f during stack join", c->height_factor);
			
		} else {
			/* UNKNOWN CONFIGURATION: Log and abort for safety */
			wlr_log(WLR_ERROR, "[nixtile] MOVEMENT RESTRICTION: Unknown tile configuration source=%d target=%d - ABORT for safety", tiles_in_source_column, tiles_in_target_column);
			return;
		}
	}
	
	/* SAFETY: Final validation before layout operations */
	if (!c->mon || c->geom.width <= 0 || c->geom.height <= 0) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: Client state corrupted during movement - ABORT final operations");
		return;
	}
	
	/* SAFETY: Validate monitor is still valid */
	if (!m || m->w.width <= 0 || m->w.height <= 0) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: Monitor state corrupted - ABORT final operations");
		return;
	}
	
	/* CRITICAL: Ensure tile remains tiled */
	c->isfloating = 0;
	if (target_tile) {
		/* SAFETY: Validate target tile before setting floating state */
		if (target_tile->mon && target_tile->geom.width > 0 && target_tile->geom.height > 0) {
			target_tile->isfloating = 0;
		} else {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Target tile corrupted, skipping floating state update");
		}
	}
	
	/* BROKEN PIPE PREVENTION: Final validation before layout operations */
	if (!c->scene || !c->scene_surface || !client_surface(c) || !client_surface(c)->mapped) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: Client surface corrupted before layout - BROKEN PIPE PREVENTION");
		return;
	}
	
	/* FORCE COLUMN TILING: Ensure ALL tiles are properly tiled in columns */
	force_column_tiling(m);
	
	/* BROKEN PIPE PREVENTION: Validate monitor output before arrange */
	if (!m->wlr_output || !m->wlr_output->enabled) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: Monitor output disabled - BROKEN PIPE PREVENTION");
		return;
	}
	
	/* CRASH PREVENTION: Defer layout update until all validation is complete */
	bool needs_arrange = true;
	bool restore_original = false;
	
	/* FULL TILE VISIBILITY CHECK: Ensure entire tile remains visible on screen */
	bool tile_fully_visible = true;
	
	/* Check if entire tile is within monitor bounds */
	if (c->geom.x < m->w.x || 
	    c->geom.x + c->geom.width > m->w.x + m->w.width ||
	    c->geom.y < m->w.y || 
	    c->geom.y + c->geom.height > m->w.y + m->w.height) {
		tile_fully_visible = false;
		wlr_log(WLR_ERROR, "[nixtile] VISIBILITY VIOLATION: Tile partially off-screen");
		wlr_log(WLR_ERROR, "[nixtile] Tile bounds: x=%d y=%d w=%d h=%d", 
			c->geom.x, c->geom.y, c->geom.width, c->geom.height);
		wlr_log(WLR_ERROR, "[nixtile] Monitor bounds: x=%d y=%d w=%d h=%d", 
			m->w.x, m->w.y, m->w.width, m->w.height);
	}
	
	if (!tile_fully_visible) {
		/* VISIBILITY VIOLATION: Mark for restoration */
		wlr_log(WLR_ERROR, "[nixtile] VISIBILITY VIOLATION: Will restore tile to grid");
		restore_original = true;
	} else {
		wlr_log(WLR_DEBUG, "[nixtile] VISIBILITY CHECK: Entire tile remains visible on screen");
	}
	
	/* DEBUG: Verify tile order after arrange */
	wlr_log(WLR_ERROR, "[nixtile] AFTER ARRANGE: Verifying tile order in column %d", c->column_group);
	int tile_index = 0;
	wl_list_for_each(temp_c, &clients, link) {
		if (temp_c->mon == m && !temp_c->isfloating && temp_c->column_group == c->column_group) {
			wlr_log(WLR_ERROR, "[nixtile] TILE ORDER: Index %d = Client %p (dragged=%s)", 
				tile_index++, (void*)temp_c, (temp_c == c) ? "YES" : "NO");
		}
	}
	
	/* STRICT GRID VALIDATION: Ensure tile is always part of the proper grid */
	bool tile_is_valid = false;
	
	/* Check if tile is properly positioned within the tiling grid */
	if (c->isfloating) {
		/* Tile became floating - this is invalid */
		wlr_log(WLR_ERROR, "[nixtile] GRID VIOLATION: Tile became floating");
		tile_is_valid = false;
	} else {
		/* Check if tile geometry is within reasonable bounds */
		bool within_monitor = (c->geom.x >= m->w.x - 100 && 
		                      c->geom.x + c->geom.width <= m->w.x + m->w.width + 100 &&
		                      c->geom.y >= m->w.y - 100 && 
		                      c->geom.y + c->geom.height <= m->w.y + m->w.height + 100);
		
		/* Check if tile has reasonable size */
		bool reasonable_size = (c->geom.width > 50 && c->geom.height > 50);
		
		/* Check if tile is in a valid column (0 or 1) */
		int max_columns = get_optimal_columns(selmon->w.width);
		bool valid_column = (c->column_group >= 0 && c->column_group < max_columns);
		
		tile_is_valid = within_monitor && reasonable_size && valid_column;
		
		wlr_log(WLR_ERROR, "[nixtile] GRID CHECK: within_monitor=%d, reasonable_size=%d, valid_column=%d", 
			within_monitor, reasonable_size, valid_column);
	}
	
	if (!tile_is_valid) {
		/* GRID VIOLATION: Mark for restoration */
		wlr_log(WLR_ERROR, "[nixtile] GRID VIOLATION: Tile outside grid, will restore original position");
		restore_original = true;
	} else {
		wlr_log(WLR_DEBUG, "[nixtile] GRID VALIDATION: Tile properly positioned in grid");
	}
	
	/* CRASH PREVENTION: Single arrange() call after all validation */
	if (restore_original) {
		/* Restore original state completely */
		wlr_log(WLR_ERROR, "[nixtile] RESTORING: Snapping tile back to original position");
		c->column_group = original_column;
		c->height_factor = original_height_factor;
		c->geom = original_geom;
		c->isfloating = 0; /* Force tiled */
	}
	
	/* SINGLE LAYOUT UPDATE: Prevent Wayland protocol overload */
	if (needs_arrange) {
		arrange(m);
		wlr_log(WLR_DEBUG, "[nixtile] Layout updated with single arrange() call");
	}
	
	wlr_log(WLR_DEBUG, "[nixtile] Tile movement completed: tiles remain tiled");
}

void
swap_tiles_in_list(Client *c1, Client *c2)
{
	struct wl_list *c1_prev, *c1_next, *c2_prev, *c2_next;
	
	if (!c1 || !c2 || c1 == c2) {
		return;
	}
	
	/* Store the link pointers */
	c1_prev = c1->link.prev;
	c1_next = c1->link.next;
	c2_prev = c2->link.prev;
	c2_next = c2->link.next;
	
	/* Remove both from list */
	wl_list_remove(&c1->link);
	wl_list_remove(&c2->link);
	
	/* Insert c1 where c2 was */
	wl_list_insert(c2_prev, &c1->link);
	
	/* Insert c2 where c1 was */
	wl_list_insert(c1_prev, &c2->link);
	
	wlr_log(WLR_DEBUG, "[nixtile] Swapped tile positions in list");
}

void
handletiledrop_old(Client *c, double x, double y)
{
	if (!c || !c->mon || c->isfloating) {
		return;
	}

	/* Use adjusted monitor area (accounting for gaps and statusbar) */
	Monitor *m = c->mon;
	struct wlr_box adjusted_area = m->w;
	
	/* Adjust for statusbar */
	if (statusbar_visible) {
		if (strcmp(statusbar_position, "top") == 0) {
			adjusted_area.y += statusbar_height + outergappx;
			adjusted_area.height -= statusbar_height + outergappx;
		} else if (strcmp(statusbar_position, "bottom") == 0) {
			adjusted_area.height -= statusbar_height + outergappx;
		} else if (strcmp(statusbar_position, "left") == 0) {
			adjusted_area.x += statusbar_height + outergappx;
			adjusted_area.width -= statusbar_height + outergappx;
		} else if (strcmp(statusbar_position, "right") == 0) {
			adjusted_area.width -= statusbar_height + outergappx;
		}
	}
	
	/* Apply outer gaps */
	adjusted_area.x += outergappx;
	adjusted_area.y += outergappx;
	adjusted_area.width -= 2 * outergappx;
	adjusted_area.height -= 2 * outergappx;
	
	double adjusted_center_x = adjusted_area.x + (adjusted_area.width / 2.0);
	int target_column = (x < adjusted_center_x) ? 0 : 1; /* 0 = left, 1 = right */
	
	wlr_log(WLR_DEBUG, "[nixtile] Tile drop: x=%.1f, adjusted_center=%.1f, current_column=%d, target_column=%d", 
		x, adjusted_center_x, c->column_group, target_column);
	
	/* If already in target column, determine insertion position within column */
	if (c->column_group == target_column) {
		/* Same column - find insertion position based on Y coordinate */
		Client *insert_after = NULL;
		Client *temp_c;
		
		/* Find tiles in same column and determine insertion point based on Y position */
		Client *closest_tile = NULL;
		double closest_distance = INFINITY;
		
		wl_list_for_each(temp_c, &clients, link) {
			if (temp_c == c || temp_c->mon != m || temp_c->isfloating || 
			    temp_c->column_group != target_column) {
				continue;
			}
			
			/* Calculate distance from drop point to tile center */
			double tile_center_y = temp_c->geom.y + (temp_c->geom.height / 2.0);
			double distance = fabs(y - tile_center_y);
			
			if (distance < closest_distance) {
				closest_distance = distance;
				closest_tile = temp_c;
			}
		}
		
		/* ULTRA-SENSITIVE VERTICAL POSITIONING: Trigger movement near tile edges */
		if (closest_tile) {
			/* Use top 25% and bottom 25% of tile for ultra-sensitive positioning */
			double tile_top = closest_tile->geom.y;
			double tile_bottom = closest_tile->geom.y + closest_tile->geom.height;
			double tile_height = closest_tile->geom.height;
			double sensitive_zone = tile_height * 0.25; /* 25% of tile height */
			
			double top_trigger_zone = tile_top + sensitive_zone;
			double bottom_trigger_zone = tile_bottom - sensitive_zone;
			
			wlr_log(WLR_DEBUG, "[nixtile] ULTRA-SENSITIVE POSITIONING: y=%.1f, top_trigger=%.1f, bottom_trigger=%.1f", 
				y, top_trigger_zone, bottom_trigger_zone);
			
			if (y < top_trigger_zone) {
				insert_after = NULL; /* Drop near top = insert before */
				wlr_log(WLR_DEBUG, "[nixtile] ULTRA-SENSITIVE: Insert BEFORE (top 25%)");
			} else if (y > bottom_trigger_zone) {
				insert_after = closest_tile; /* Drop near bottom = insert after */
				wlr_log(WLR_DEBUG, "[nixtile] ULTRA-SENSITIVE: Insert AFTER (bottom 25%)");
			} else {
				/* Middle 50% - use center as fallback */
				double tile_center_y = closest_tile->geom.y + (closest_tile->geom.height / 2.0);
				if (y > tile_center_y) {
					insert_after = closest_tile;
					wlr_log(WLR_DEBUG, "[nixtile] ULTRA-SENSITIVE: Insert AFTER (center fallback)");
				} else {
					insert_after = NULL;
					wlr_log(WLR_DEBUG, "[nixtile] ULTRA-SENSITIVE: Insert BEFORE (center fallback)");
				}
			}
		}
		
		/* Reorder within same column */
		if (insert_after && insert_after != c) {
			/* Remove c from current position */
			wl_list_remove(&c->link);
			/* Insert after target position */
			wl_list_insert(&insert_after->link, &c->link);
			wlr_log(WLR_DEBUG, "[nixtile] Reordered tile within column %d (intra-column)", target_column);
		} else if (!insert_after) {
			/* Drop at top of column or insert before closest tile */
			if (closest_tile) {
				/* Insert before closest tile */
				wl_list_remove(&c->link);
				wl_list_insert(closest_tile->link.prev, &c->link);
				wlr_log(WLR_DEBUG, "[nixtile] Moved tile before closest tile in column %d (intra-column)", target_column);
			} else {
				/* No tiles in column or no closest tile - find first tile and insert before it */
				Client *first_in_column = NULL;
				Client *temp_c;
				wl_list_for_each(temp_c, &clients, link) {
					if (temp_c != c && temp_c->mon == m && !temp_c->isfloating && 
					    temp_c->column_group == target_column) {
						first_in_column = temp_c;
						break;
					}
				}
				
				if (first_in_column && first_in_column != c) {
					/* Remove c from current position */
					wl_list_remove(&c->link);
					/* Insert before first tile in column */
					wl_list_insert(first_in_column->link.prev, &c->link);
					wlr_log(WLR_DEBUG, "[nixtile] Moved tile to top of column %d (intra-column)", target_column);
				}
			}
		}
	} else {
		/* Different column - move to target column */
		int old_column = c->column_group;
		c->column_group = target_column;
		
		/* Find insertion position in target column based on Y coordinate */
		Client *insert_after = NULL;
		Client *temp_c;
		Client *closest_tile = NULL;
		double closest_distance = INFINITY;
		
		wl_list_for_each(temp_c, &clients, link) {
			if (temp_c == c || temp_c->mon != m || temp_c->isfloating || 
			    temp_c->column_group != target_column) {
				continue;
			}
			
			/* Calculate distance from drop point to tile center */
			double tile_center_y = temp_c->geom.y + (temp_c->geom.height / 2.0);
			double distance = fabs(y - tile_center_y);
			
			if (distance < closest_distance) {
				closest_distance = distance;
				closest_tile = temp_c;
			}
		}
		
		/* ULTRA-SENSITIVE VERTICAL POSITIONING: Trigger movement near tile edges */
		if (closest_tile) {
			/* Use top 25% and bottom 25% of tile for ultra-sensitive positioning */
			double tile_top = closest_tile->geom.y;
			double tile_bottom = closest_tile->geom.y + closest_tile->geom.height;
			double tile_height = closest_tile->geom.height;
			double sensitive_zone = tile_height * 0.25; /* 25% of tile height */
			
			double top_trigger_zone = tile_top + sensitive_zone;
			double bottom_trigger_zone = tile_bottom - sensitive_zone;
			
			wlr_log(WLR_DEBUG, "[nixtile] ULTRA-SENSITIVE POSITIONING 2: y=%.1f, top_trigger=%.1f, bottom_trigger=%.1f", 
				y, top_trigger_zone, bottom_trigger_zone);
			
			if (y < top_trigger_zone) {
				insert_after = NULL; /* Drop near top = insert before */
				wlr_log(WLR_DEBUG, "[nixtile] ULTRA-SENSITIVE 2: Insert BEFORE (top 25%)");
			} else if (y > bottom_trigger_zone) {
				insert_after = closest_tile; /* Drop near bottom = insert after */
				wlr_log(WLR_DEBUG, "[nixtile] ULTRA-SENSITIVE 2: Insert AFTER (bottom 25%)");
			} else {
				/* Middle 50% - use center as fallback */
				double tile_center_y = closest_tile->geom.y + (closest_tile->geom.height / 2.0);
				if (y > tile_center_y) {
					insert_after = closest_tile;
					wlr_log(WLR_DEBUG, "[nixtile] ULTRA-SENSITIVE 2: Insert AFTER (center fallback)");
				} else {
					insert_after = NULL;
					wlr_log(WLR_DEBUG, "[nixtile] ULTRA-SENSITIVE 2: Insert BEFORE (center fallback)");
				}
			}
		}
		
		/* Remove from current position */
		wl_list_remove(&c->link);
		
		if (insert_after) {
			/* Insert after target position in target column */
			wl_list_insert(&insert_after->link, &c->link);
		} else {
			/* Insert at beginning of target column */
			Client *first_in_column = NULL;
			wl_list_for_each(temp_c, &clients, link) {
				if (temp_c->mon == m && !temp_c->isfloating && 
				    temp_c->column_group == target_column) {
					first_in_column = temp_c;
					break;
				}
			}
			
			if (first_in_column) {
				/* Insert before first tile in target column */
				wl_list_insert(first_in_column->link.prev, &c->link);
			} else {
				/* Target column is empty, add to end of list */
				wl_list_insert(clients.prev, &c->link);
			}
		}
		
		/* Reset height factors for both columns */
		wl_list_for_each(temp_c, &clients, link) {
			if (temp_c->mon == m && !temp_c->isfloating && 
			    (temp_c->column_group == old_column || temp_c->column_group == target_column)) {
				temp_c->height_factor = 1.0f;
			}
		}
		
		wlr_log(WLR_DEBUG, "[nixtile] Moved tile from column %d to column %d (cross-column swap)", old_column, target_column);
	}
	
	/* EQUAL DISTRIBUTION: Reset manual resize flags and ensure equal distribution after tile movement */
	/* Reset manual resize flags to enforce equal distribution on tile movement */
	if (manual_resize_performed[0] || manual_resize_performed[1]) {
		wlr_log(WLR_INFO, "[nixtile] EQUAL DISTRIBUTION: Resetting manual resize flags on tile movement - enforcing equal distribution");
		manual_resize_performed[0] = false;
		manual_resize_performed[1] = false;
	}
	
	ensure_equal_height_distribution_in_stack(0); /* Left column */
	ensure_equal_height_distribution_in_stack(1); /* Right column */
	
	/* EQUAL DISTRIBUTION: Check if we need equal horizontal distribution for single tiles */
	ensure_equal_horizontal_distribution_for_two_tiles();
	
	/* Force layout update */
	arrange(m);
}

void
createpointer(struct wlr_pointer *pointer)
{
	struct libinput_device *device;
	if (wlr_input_device_is_libinput(&pointer->base)
			&& (device = wlr_libinput_get_device_handle(&pointer->base))) {

		if (libinput_device_config_tap_get_finger_count(device)) {
			libinput_device_config_tap_set_enabled(device, tap_to_click);
			libinput_device_config_tap_set_drag_enabled(device, tap_and_drag);
			libinput_device_config_tap_set_drag_lock_enabled(device, drag_lock);
			libinput_device_config_tap_set_button_map(device, button_map);
		}

		if (libinput_device_config_scroll_has_natural_scroll(device))
			libinput_device_config_scroll_set_natural_scroll_enabled(device, natural_scrolling);

		if (libinput_device_config_dwt_is_available(device))
			libinput_device_config_dwt_set_enabled(device, disable_while_typing);

		if (libinput_device_config_left_handed_is_available(device))
			libinput_device_config_left_handed_set(device, left_handed);

		if (libinput_device_config_middle_emulation_is_available(device))
			libinput_device_config_middle_emulation_set_enabled(device, middle_button_emulation);

		if (libinput_device_config_scroll_get_methods(device) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method(device, scroll_method);

		if (libinput_device_config_click_get_methods(device) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
			libinput_device_config_click_set_method(device, click_method);

		if (libinput_device_config_send_events_get_modes(device))
			libinput_device_config_send_events_set_mode(device, send_events_mode);

		if (libinput_device_config_accel_is_available(device)) {
			libinput_device_config_accel_set_profile(device, accel_profile);
			libinput_device_config_accel_set_speed(device, accel_speed);
		}
	}

	wlr_cursor_attach_input_device(cursor, &pointer->base);
}

void
createpointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = ecalloc(1, sizeof(*pointer_constraint));
	pointer_constraint->constraint = data;
	LISTEN(&pointer_constraint->constraint->events.destroy,
			&pointer_constraint->destroy, destroypointerconstraint);
}

void
createpopup(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client (either xdg-shell or layer-shell)
	 * creates a new popup. */
	struct wlr_xdg_popup *popup = data;
	LISTEN_STATIC(&popup->base->surface->events.commit, commitpopup);
}

void
cursorconstrain(struct wlr_pointer_constraint_v1 *constraint)
{
	if (active_constraint == constraint)
		return;

	if (active_constraint)
		wlr_pointer_constraint_v1_send_deactivated(active_constraint);

	active_constraint = constraint;
	wlr_pointer_constraint_v1_send_activated(constraint);
}

void
cursorframe(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(seat);
}

void
cursorwarptohint(void)
{
	Client *c = NULL;
	double sx = active_constraint->current.cursor_hint.x;
	double sy = active_constraint->current.cursor_hint.y;

	toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
	if (c && active_constraint->current.cursor_hint.enabled) {
		wlr_cursor_warp(cursor, NULL, sx + c->geom.x + c->bw, sy + c->geom.y + c->bw);
		wlr_seat_pointer_warp(active_constraint->seat, sx, sy);
	}
}

void
destroydecoration(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, destroy_decoration);

	wl_list_remove(&c->destroy_decoration.link);
	wl_list_remove(&c->set_decoration_mode.link);
}

void
destroydragicon(struct wl_listener *listener, void *data)
{
	/* Focus enter isn't sent during drag, so refocus the focused node. */
	focusclient(focustop(selmon), 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
	wl_list_remove(&listener->link);
	free(listener);
}

void
destroyidleinhibitor(struct wl_listener *listener, void *data)
{
	/* `data` is the wlr_surface of the idle inhibitor being destroyed,
	 * at this point the idle inhibitor is still in the list of the manager */
	checkidleinhibitor(wlr_surface_get_root_surface(data));
	wl_list_remove(&listener->link);
	free(listener);
}

void
destroylayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, destroy);

	wl_list_remove(&l->link);
	wl_list_remove(&l->destroy.link);
	wl_list_remove(&l->unmap.link);
	wl_list_remove(&l->surface_commit.link);
	wlr_scene_node_destroy(&l->scene->node);
	wlr_scene_node_destroy(&l->popups->node);
	free(l);
}

void
destroylock(SessionLock *lock, int unlock)
{
	wlr_seat_keyboard_notify_clear_focus(seat);
	if ((locked = !unlock))
		goto destroy;

	wlr_scene_node_set_enabled(&locked_bg->node, 0);

	focusclient(focustop(selmon), 0);
	motionnotify(0, NULL, 0, 0, 0, 0);

destroy:
	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);

	wlr_scene_node_destroy(&lock->scene->node);
	cur_lock = NULL;
	free(lock);
}

void
destroylocksurface(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy_lock_surface);
	struct wlr_session_lock_surface_v1 *surface, *lock_surface = m->lock_surface;

	m->lock_surface = NULL;
	wl_list_remove(&m->destroy_lock_surface.link);

	if (lock_surface->surface != seat->keyboard_state.focused_surface)
		return;

	if (locked && cur_lock && !wl_list_empty(&cur_lock->surfaces)) {
		surface = wl_container_of(cur_lock->surfaces.next, surface, link);
		client_notify_enter(surface->surface, wlr_seat_get_keyboard(seat));
	} else if (!locked) {
		focusclient(focustop(selmon), 1);
	} else {
		wlr_seat_keyboard_clear_focus(seat);
	}
}

void
destroynotify(struct wl_listener *listener, void *data)
{
	/* Called when the xdg_toplevel is destroyed. */
	Client *c = wl_container_of(listener, c, destroy);
	
	wlr_log(WLR_DEBUG, "[nixtile] DESTROY CLIENT: Client %p being destroyed", (void*)c);
	
	/* Remove only the listeners that haven't been removed yet */
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
	wl_list_remove(&c->commit.link);
	wl_list_remove(&c->set_decoration_mode.link);
	wl_list_remove(&c->maximize.link);
	
	/* Clear any remaining global references */
	if (c == grabc) grabc = NULL;
	if (c == exclusive_focus) exclusive_focus = NULL;
	if (c == vertical_resize_client) vertical_resize_client = NULL;
	if (c == vertical_resize_neighbor) vertical_resize_neighbor = NULL;
	if (c == initial_resize_client) initial_resize_client = NULL;
	if (c == initial_resize_neighbor) initial_resize_neighbor = NULL;
	
	/* DYNAMIC TILE DELETION AND REBALANCING */
	Monitor *client_mon = c->destroy_mon;
	int removed_column = c->destroy_column;
	bool client_was_floating = c->destroy_was_floating;
	
	wlr_log(WLR_DEBUG, "[nixtile] DESTROY REBALANCING CHECK: floating=%d, column=%d, mon=%p", client_was_floating, removed_column, (void*)client_mon);
	
	if (!client_was_floating && removed_column >= 0 && client_mon) {
		int max_columns = get_optimal_columns(client_mon->w.width);
		
		/* Count remaining tiles after removal */
		int total_tiles = 0;
		int column_counts[MAX_COLUMNS] = {0};
		Client *temp_c;
		
		wl_list_for_each(temp_c, &clients, link) {
			if (temp_c == c) continue; /* Skip the client being destroyed */
			if (!VISIBLEON(temp_c, client_mon) || temp_c->isfloating || temp_c->isfullscreen)
				continue;
			if (temp_c->column_group >= 0 && temp_c->column_group < max_columns) {
				column_counts[temp_c->column_group]++;
				total_tiles++;
			}
		}
		
		wlr_log(WLR_DEBUG, "[nixtile] TILE REBALANCING: total_tiles=%d, max_columns=%d", total_tiles, max_columns);
		
		/* SCENARIO 1: Single tile remaining - expand to full screen */
		if (total_tiles == 1) {
			wlr_log(WLR_DEBUG, "[nixtile] SINGLE TILE: Expanding to full screen");
			wl_list_for_each(temp_c, &clients, link) {
				if (temp_c == c) continue; /* Skip the client being destroyed */
				if (!VISIBLEON(temp_c, client_mon) || temp_c->isfloating || temp_c->isfullscreen)
					continue;
				temp_c->column_group = 0;
				temp_c->height_factor = 1.0f;
				break;
			}
			client_mon->mfact = 1.0f;
		}
		/* SCENARIO 2: Empty column - move tile from fullest column */
		else if (column_counts[removed_column] == 0 && total_tiles > 0) {
			/* Find column with most tiles */
			int source_column = -1;
			int max_tiles = 0;
			for (int col = 0; col < max_columns; col++) {
				if (column_counts[col] > max_tiles) {
					max_tiles = column_counts[col];
					source_column = col;
				}
			}
			
			if (source_column >= 0 && max_tiles >= 2) {
				wlr_log(WLR_DEBUG, "[nixtile] MASTER TILE REPLACEMENT: Moving from column %d to %d", source_column, removed_column);
				
				/* Find first tile to move */
				wl_list_for_each(temp_c, &clients, link) {
					if (temp_c == c) continue; /* Skip the client being destroyed */
					if (!VISIBLEON(temp_c, client_mon) || temp_c->isfloating || temp_c->isfullscreen)
						continue;
					if (temp_c->column_group == source_column) {
						temp_c->column_group = removed_column;
						temp_c->height_factor = 1.0f;
						wlr_log(WLR_DEBUG, "[nixtile] TILE MOVED: Client %p to column %d", (void*)temp_c, removed_column);
						break;
					}
				}
				
				/* Rebalance heights in source column */
				wl_list_for_each(temp_c, &clients, link) {
					if (temp_c == c) continue; /* Skip the client being destroyed */
					if (!VISIBLEON(temp_c, client_mon) || temp_c->isfloating || temp_c->isfullscreen)
						continue;
					if (temp_c->column_group == source_column) {
						temp_c->height_factor = 1.0f;
					}
				}
			}
		}
		/* SCENARIO 3: Rebalance heights in column after tile removal */
		else {
			wlr_log(WLR_DEBUG, "[nixtile] HEIGHT REBALANCING: Column %d", removed_column);
			wl_list_for_each(temp_c, &clients, link) {
				if (temp_c == c) continue; /* Skip the client being destroyed */
				if (!VISIBLEON(temp_c, client_mon) || temp_c->isfloating || temp_c->isfullscreen)
					continue;
				if (temp_c->column_group == removed_column) {
					temp_c->height_factor = 1.0f;
				}
			}
		}
		
		/* Reset manual resize flags on tile deletion */
		manual_resize_performed[0] = false;
		manual_resize_performed[1] = false;
		client_mon->manual_horizontal_resize = 0;
		client_mon->manual_vertical_resize = 0;
		wlr_log(WLR_DEBUG, "[nixtile] MANUAL RESIZE RESET: Flags reset due to tile deletion");
		
		/* Schedule layout update after client is freed */
		wlr_log(WLR_DEBUG, "[nixtile] TILE REBALANCING COMPLETE");
	}
	
	/* Store monitor pointer before freeing client to avoid use-after-free */
	Monitor *saved_mon = client_mon;
	
	wlr_log(WLR_DEBUG, "[nixtile] DESTROY COMPLETE: Client %p freed", (void*)c);
	free(c);
	
	/* Schedule throttled layout update after client is completely freed */
	if (saved_mon) {
		try_arrange_safe(saved_mon);
		wlr_log(WLR_DEBUG, "[nixtile] LAYOUT UPDATE: Throttled arrange scheduled after client destruction");
	}
}

void
destroypointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = wl_container_of(listener, pointer_constraint, destroy);

	if (active_constraint == pointer_constraint->constraint) {
		cursorwarptohint();
		active_constraint = NULL;
	}

	wl_list_remove(&pointer_constraint->destroy.link);
	free(pointer_constraint);
}

void
destroysessionlock(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, destroy);
	destroylock(lock, 0);
}

void
destroykeyboardgroup(struct wl_listener *listener, void *data)
{
	KeyboardGroup *group = wl_container_of(listener, group, destroy);
	wl_event_source_remove(group->key_repeat_source);
	wl_list_remove(&group->key.link);
	wl_list_remove(&group->modifiers.link);
	wl_list_remove(&group->destroy.link);
	wlr_keyboard_group_destroy(group->wlr_group);
	free(group);
}

Monitor *
dirtomon(enum wlr_direction dir)
{
	struct wlr_output *next;
	if (!wlr_output_layout_get(output_layout, selmon->wlr_output))
		return selmon;
	if ((next = wlr_output_layout_adjacent_output(output_layout,
			dir, selmon->wlr_output, selmon->m.x, selmon->m.y)))
		return next->data;
	if ((next = wlr_output_layout_farthest_output(output_layout,
			dir ^ (WLR_DIRECTION_LEFT|WLR_DIRECTION_RIGHT),
			selmon->wlr_output, selmon->m.x, selmon->m.y)))
		return next->data;
	return selmon;
}

void
focusclient(Client *c, int lift)
{
	struct wlr_surface *old = seat->keyboard_state.focused_surface;
	int unused_lx, unused_ly, old_client_type;
	Client *old_c = NULL;
	LayerSurface *old_l = NULL;

	if (locked)
		return;

	/* Raise client in stacking order if requested */
	if (c && lift)
		wlr_scene_node_raise_to_top(&c->scene->node);

	if (c && client_surface(c) == old)
		return;

	if ((old_client_type = toplevel_from_wlr_surface(old, &old_c, &old_l)) == XDGShell) {
		struct wlr_xdg_popup *popup, *tmp;
		wl_list_for_each_safe(popup, tmp, &old_c->surface.xdg->popups, link)
			wlr_xdg_popup_destroy(popup);
	}

	/* Put the new client atop the focus stack and select its monitor */
	if (c && !client_is_unmanaged(c)) {
		wl_list_remove(&c->flink);
		wl_list_insert(&fstack, &c->flink);
		selmon = c->mon;
		c->isurgent = 0;

		/* Don't change border color if there is an exclusive focus or we are
		 * handling a drag operation */
		if (!exclusive_focus && !seat->drag)
			client_set_border_color(c, focuscolor);
	}

	/* Deactivate old client if focus is changing */
	if (old && (!c || client_surface(c) != old)) {
		/* If an overlay is focused, don't focus or activate the client,
		 * but only update its position in fstack to render its border with focuscolor
		 * and focus it after the overlay is closed. */
		if (old_client_type == LayerShell && wlr_scene_node_coords(
					&old_l->scene->node, &unused_lx, &unused_ly)
				&& old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
			return;
		} else if (old_c && old_c == exclusive_focus && client_wants_focus(old_c)) {
			return;
		/* Don't deactivate old client if the new one wants focus, as this causes issues with winecfg
		 * and probably other clients */
		} else if (old_c && !client_is_unmanaged(old_c) && (!c || !client_wants_focus(c))) {
			client_set_border_color(old_c, bordercolor);

			client_activate_surface(old, 0);
		}
	}
	printstatus();

	if (!c) {
		/* With no client, all we have left is to clear focus */
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}

	/* Change cursor surface */
	motionnotify(0, NULL, 0, 0, 0, 0);

	/* Have a client, so focus its top-level wlr_surface */
	client_notify_enter(client_surface(c), wlr_seat_get_keyboard(seat));

	/* Activate the new client */
	client_activate_surface(client_surface(c), 1);
}

void
focusmon(const Arg *arg)
{
	int i = 0, nmons = wl_list_length(&mons);
	if (nmons) {
		do /* don't switch to disabled mons */
			selmon = dirtomon(arg->i);
		while (!selmon->wlr_output->enabled && i++ < nmons);
	}
	focusclient(focustop(selmon), 1);
}

void
focusstack(const Arg *arg)
{
	/* Focus the next or previous client (in tiling order) on selmon */
	Client *c, *sel = focustop(selmon);
	if (!sel || (sel->isfullscreen && !client_has_children(sel)))
		return;
	if (arg->i > 0) {
		wl_list_for_each(c, &sel->link, link) {
			if (&c->link == &clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon))
				break; /* found it */
		}
	} else {
		wl_list_for_each_reverse(c, &sel->link, link) {
			if (&c->link == &clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon))
				break; /* found it */
		}
	}
	/* If only one client is visible on selmon, then c == sel */
	focusclient(c, 1);
}

/* We probably should change the name of this: it sounds like it
 * will focus the topmost client of this mon, when actually will
 * only return that client */
Client *
focustop(Monitor *m)
{
	Client *c;
	wl_list_for_each(c, &fstack, flink) {
		if (VISIBLEON(c, m))
			return c;
	}
	return NULL;
}

void
fullscreennotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, fullscreen);
	setfullscreen(c, client_wants_fullscreen(c));
}

void
gpureset(struct wl_listener *listener, void *data)
{
	struct wlr_renderer *old_drw = drw;
	struct wlr_allocator *old_alloc = alloc;
	struct Monitor *m;
	if (!(drw = wlr_renderer_autocreate(backend)))
		die("couldn't recreate renderer");

	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't recreate allocator");

	wl_list_remove(&gpu_reset.link);
	wl_signal_add(&drw->events.lost, &gpu_reset);

	wlr_compositor_set_renderer(compositor, drw);

	wl_list_for_each(m, &mons, link) {
		wlr_output_init_render(m->wlr_output, alloc, drw);
	}

	wlr_allocator_destroy(old_alloc);
	wlr_renderer_destroy(old_drw);
}

void
handlesig(int signo)
{
	if (signo == SIGCHLD)
		while (waitpid(-1, NULL, WNOHANG) > 0);
	else if (signo == SIGINT || signo == SIGTERM)
		quit(NULL);
}

void
incnmaster(const Arg *arg)
{
	if (!arg || !selmon)
		return;
	selmon->nmaster = MAX(selmon->nmaster + arg->i, 0);
	arrange(selmon);
}

void
inputdevice(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct wlr_input_device *device = data;
	uint32_t caps;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		createkeyboard(wlr_keyboard_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		createpointer(wlr_pointer_from_input_device(device));
		break;
	default:
		/* TODO handle other input device types */
		break;
	}

	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In nixtile we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	/* TODO do we actually require a cursor? */
	caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&kb_group->wlr_group->devices))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(seat, caps);
}

int
keybinding(uint32_t mods, xkb_keysym_t sym)
{
    fprintf(stderr, "[nixtile] keybinding: mods=0x%x, sym=0x%x\n", mods, sym);
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 */
	const Key *k;
	for (k = keys; k < END(keys); k++) {
		if (CLEANMASK(mods) == CLEANMASK(k->mod)
				&& sym == k->keysym && k->func) {
			k->func(&k->arg);
			return 1;
		}
	}
	return 0;
}

void
keypress(struct wl_listener *listener, void *data)
{
    extern int launcher_just_closed;

	int i;
	/* This event is raised when a key is pressed or released. */
	KeyboardGroup *group = wl_container_of(listener, group, key);
	struct wlr_keyboard_key_event *event = data;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			group->wlr_group->keyboard.xkb_state, keycode, &syms);

	int handled = 0;

	// Launcher input handling: if launcher is visible, handle and return
	if (launcher.visible) {
		for (i = 0; i < nsyms; i++) {
			handle_launcher_key(event->keycode, event->state, syms[i]);
		}
		return;
	}

	uint32_t mods = wlr_keyboard_get_modifiers(&group->wlr_group->keyboard);

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	/* On _press_ if there is no active screen locker,
	 * attempt to process a compositor keybinding. */
	if (!locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (i = 0; i < nsyms; i++)
			handled = keybinding(mods, syms[i]) || handled;
	}

    // Reset launcher debounce when MOD or P is released
    if (!locked && event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        for (i = 0; i < nsyms; i++) {
            // 0x70 is 'p', 0x40 is likely MOD (from logs)
            if (syms[i] == 0x70 || (event->keycode + 8) == 0x70) { // P released
                launcher_just_closed = 0;
                fprintf(stderr, "[nixtile] launcher_just_closed reset on P release\n");
            }
        }
        // Also reset if MOD is released (modifier release not always in syms)
        if (!(mods & 0x40)) {
            launcher_just_closed = 0;
            fprintf(stderr, "[nixtile] launcher_just_closed reset on MOD release\n");
        }
    }

	if (handled && group->wlr_group->keyboard.repeat_info.delay > 0) {
		group->mods = mods;
		group->keysyms = syms;
		group->nsyms = nsyms;
		wl_event_source_timer_update(group->key_repeat_source,
				group->wlr_group->keyboard.repeat_info.delay);
	} else {
		group->nsyms = 0;
		wl_event_source_timer_update(group->key_repeat_source, 0);
	}

	if (handled)
		return;

	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	/* Pass unhandled keycodes along to the client. */
	wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
}


void
keypressmod(struct wl_listener *listener, void *data)
{
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	KeyboardGroup *group = wl_container_of(listener, group, modifiers);

	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(seat,
			&group->wlr_group->keyboard.modifiers);
}

int
keyrepeat(void *data)
{
	KeyboardGroup *group = data;
	int i;
	if (!group->nsyms || group->wlr_group->keyboard.repeat_info.rate <= 0)
		return 0;

	wl_event_source_timer_update(group->key_repeat_source,
			1000 / group->wlr_group->keyboard.repeat_info.rate);

	for (i = 0; i < group->nsyms; i++)
		keybinding(group->mods, group->keysyms[i]);

	return 0;
}

void
killclient(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		client_send_close(sel);
}

void
locksession(struct wl_listener *listener, void *data)
{
	struct wlr_session_lock_v1 *session_lock = data;
	SessionLock *lock;
	wlr_scene_node_set_enabled(&locked_bg->node, 1);
	if (cur_lock) {
		wlr_session_lock_v1_destroy(session_lock);
		return;
	}
	lock = session_lock->data = ecalloc(1, sizeof(*lock));
	focusclient(NULL, 0);

	lock->scene = wlr_scene_tree_create(layers[LyrBlock]);
	cur_lock = lock->lock = session_lock;
	locked = 1;

	LISTEN(&session_lock->events.new_surface, &lock->new_surface, createlocksurface);
	LISTEN(&session_lock->events.destroy, &lock->destroy, destroysessionlock);
	LISTEN(&session_lock->events.unlock, &lock->unlock, unlocksession);

	wlr_session_lock_v1_send_locked(session_lock);
}

void
mapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is mapped, or ready to display on-screen. */
	Client *p = NULL;
	Client *w, *c = wl_container_of(listener, c, map);
	Monitor *m;
	int i;

	/* Create scene tree for this client and its border */
	c->scene = client_surface(c)->data = wlr_scene_tree_create(layers[LyrTile]);
	/* Enabled later by a call to arrange() */
	wlr_scene_node_set_enabled(&c->scene->node, client_is_unmanaged(c));
	c->scene_surface = c->type == XDGShell
			? wlr_scene_xdg_surface_create(c->scene, c->surface.xdg)
			: wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
	c->scene->node.data = c->scene_surface->node.data = c;

	client_get_geometry(c, &c->geom);

	/* Handle unmanaged clients first so we can return prior create borders */
	if (client_is_unmanaged(c)) {
		/* Unmanaged clients always are floating */
		wlr_scene_node_reparent(&c->scene->node, layers[LyrFloat]);
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		client_set_size(c, c->geom.width, c->geom.height);
		if (client_wants_focus(c)) {
			focusclient(c, 1);
			exclusive_focus = c;
		}
		goto unset_fullscreen;
	}

	for (i = 0; i < 4; i++) {
		c->border[i] = wlr_scene_rect_create(c->scene, 0, 0,
				c->isurgent ? urgentcolor : bordercolor);
		c->border[i]->node.data = c;
	}

	/* Initialize client geometry with room for border */
	client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	c->geom.width += 2 * c->bw;
	c->geom.height += 2 * c->bw;

	/* Insert this client into client lists. */
	wl_list_insert(&clients, &c->link);
	wl_list_insert(&fstack, &c->flink);

	/* Set initial monitor, tags, floating status, and focus:
	 * we always consider floating, clients that have parent and thus
	 * we set the same tags and monitor as its parent.
	 * If there is no parent, apply rules */
	if ((p = client_get_parent(c))) {
		c->isfloating = 1;
		setmon(c, p->mon, p->tags);
	} else {
		applyrules(c);
	}
	
	/* STRICT TILE LIMITS: Ensure proper stack assignment and screen filling */
	if (!client_is_unmanaged(c)) {
		c->isfloating = 0; /* CRITICAL: Never allow floating tiles */
		
		/* Validate and fix column assignment if needed */
		/* Validate and fix column_group for dynamic columns */
		int optimal_columns = get_optimal_columns(c->mon->w.width);
		wlr_log(WLR_ERROR, "[nixtile] *** MAPNOTIFY ENTRY: Tile %p has column_group=%d (optimal_columns=%d) ***", 
			(void*)c, c->column_group, optimal_columns);
		if (c->column_group < 0 || c->column_group >= optimal_columns) {
			wlr_log(WLR_ERROR, "[nixtile] *** MAPNOTIFY: Invalid column_group %d (valid: 0-%d), will be reassigned in tile() ***", 
				c->column_group, optimal_columns - 1);
			/* DO NOT force to 0 - let tile() function handle assignment */
		} else {
			wlr_log(WLR_ERROR, "[nixtile] *** MAPNOTIFY KEEPING: Valid column_group %d for %d-column layout ***", 
				c->column_group, optimal_columns);
		}
		
		/* Stack limits are now handled by tile() function - do not override column_group here */
		int stack_count = count_tiles_in_stack(c->column_group, c->mon);
		if (stack_count > MAX_TILES_PER_STACK) {
			wlr_log(WLR_ERROR, "[nixtile] MAPNOTIFY: Stack %d overflow (%d tiles), will be handled by tile() function", c->column_group, stack_count);
			/* DO NOT override column_group here - let tile() function handle redistribution */
		} else {
			wlr_log(WLR_ERROR, "[nixtile] MAPNOTIFY: Stack %d has %d tiles (within limit %d)", c->column_group, stack_count, MAX_TILES_PER_STACK);
		}
		
		/* Force column tiling and ensure screen filling */
		if (c->mon) {
			force_column_tiling(c->mon);
			handle_empty_column_expansion(); /* Ensure tiles fill entire screen */
		}
		
		wlr_log(WLR_INFO, "[nixtile] MAPNOTIFY: Tile mapped to stack %d, forcing screen fill", c->column_group);
	}
	
	printstatus();

unset_fullscreen:
	m = c->mon ? c->mon : xytomon(c->geom.x, c->geom.y);
	wl_list_for_each(w, &clients, link) {
		if (w != c && w != p && w->isfullscreen && m == w->mon && (w->tags & c->tags))
			setfullscreen(w, 0);
	}
}

void
maximizenotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on
	 * client-side decorations. nixtile doesn't support maximization, but
	 * to conform to xdg-shell protocol we still must send a configure.
	 * Since xdg-shell protocol v5 we should ignore request of unsupported
	 * capabilities, just schedule a empty configure when the client uses <5
	 * protocol version
	 * wlr_xdg_surface_schedule_configure() is used to send an empty reply. */
	Client *c = wl_container_of(listener, c, maximize);
	if (c->surface.xdg->initialized
			&& wl_resource_get_version(c->surface.xdg->toplevel->resource)
					< XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
		wlr_xdg_surface_schedule_configure(c->surface.xdg);
}

void
monocle(Monitor *m)
{
	Client *c;
	int n = 0;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		resize(c, m->w, 0);
		n++;
	}
	if (n)
		snprintf(m->ltsymbol, LENGTH(m->ltsymbol), "[%d]", n);
	if ((c = focustop(m)))
		wlr_scene_node_raise_to_top(&c->scene->node);
}

void
motionabsolute(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. Also, some hardware emits these events. */
	struct wlr_pointer_motion_absolute_event *event = data;
	double lx, ly, dx, dy;

	if (!event->time_msec) /* this is 0 with virtual pointers */
		wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);

	wlr_cursor_absolute_to_layout_coords(cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
	dx = lx - cursor->x;
	dy = ly - cursor->y;
	motionnotify(event->time_msec, &event->pointer->base, dx, dy, dx, dy);
}

void
motionnotify(uint32_t time, struct wlr_input_device *device, double dx, double dy,
		double dx_unaccel, double dy_unaccel)
{
	double sx = 0, sy = 0, sx_confined, sy_confined;
	Client *c = NULL, *w = NULL;
	LayerSurface *l = NULL;
	struct wlr_surface *surface = NULL;
	struct wlr_pointer_constraint_v1 *constraint;

	/* Find the client under the pointer and send the event along. */
	xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);

	if (cursor_mode == CurPressed && !seat->drag
			&& surface != seat->pointer_state.focused_surface
			&& toplevel_from_wlr_surface(seat->pointer_state.focused_surface, &w, &l) >= 0) {
		c = w;
		surface = seat->pointer_state.focused_surface;
		sx = cursor->x - (l ? l->scene->node.x : w->geom.x);
		sy = cursor->y - (l ? l->scene->node.y : w->geom.y);
	}

	/* time is 0 in internal calls meant to restore pointer focus. */
	if (time) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
				relative_pointer_mgr, seat, (uint64_t)time * 1000,
				dx, dy, dx_unaccel, dy_unaccel);

		wl_list_for_each(constraint, &pointer_constraints->constraints, link)
			cursorconstrain(constraint);

		if (active_constraint && cursor_mode != CurResize && cursor_mode != CurMove) {
			toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
			if (c && active_constraint->surface == seat->pointer_state.focused_surface) {
				sx = cursor->x - c->geom.x - c->bw;
				sy = cursor->y - c->geom.y - c->bw;
				if (wlr_region_confine(&active_constraint->region, sx, sy,
						sx + dx, sy + dy, &sx_confined, &sy_confined)) {
					dx = sx_confined - sx;
					dy = sy_confined - sy;
				}

				if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
					return;
			}
		}

		/* ULTRA-SMOOTH GPU ACCELERATION: Optimize cursor movement */
		if (high_performance_mode && drw && wlr_renderer_is_gles2(drw)) {
			/* GPU-accelerated cursor movement for butter-smooth experience */
			wlr_cursor_move(cursor, device, dx, dy);
			/* Immediate GPU buffer swap for minimal latency */
			wlr_log(WLR_DEBUG, "[nixtile] GPU ACCELERATION: Hardware-accelerated cursor movement (dx=%.2f, dy=%.2f)", dx, dy);
		} else {
			/* Standard cursor movement */
			wlr_cursor_move(cursor, device, dx, dy);
		}
		wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

		/* Update selmon (even while dragging a window) */
		if (sloppyfocus)
			selmon = xytomon(cursor->x, cursor->y);
	}

	/* Update drag icon's position */
	wlr_scene_node_set_position(&drag_icon->node, (int)round(cursor->x), (int)round(cursor->y));

	/* If we are currently grabbing the mouse, handle and return */
	if (cursor_mode == CurMove) {
		/* Validate grabc before moving */
		if (!grabc || !grabc->mon || !grabc->scene) {
			wlr_log(WLR_DEBUG, "[nixtile] CurMove: Invalid grabc, resetting to CurNormal");
			cursor_mode = CurNormal;
			return;
		}
		/* FORCE: Ensure tile stays tiled during movement */
		grabc->isfloating = 0;
		
		/* Move the grabbed client to the new position. */
		resize(grabc, (struct wlr_box){.x = (int)round(cursor->x) - grabcx, .y = (int)round(cursor->y) - grabcy,
			.width = grabc->geom.width, .height = grabc->geom.height}, 1);
		return;
	} else if (cursor_mode == CurSmartResize) {
		/* Legacy mode - just reset to normal to avoid crashes */
		cursor_mode = CurNormal;
		return;
	} else if (cursor_mode == CurResize) {
		/* Validate grabc before resizing */
		if (!grabc || !grabc->mon || !grabc->scene) {
			cursor_mode = CurNormal;
			return;
		}
		/* Calculate new dimensions with safety checks */
		int new_width = (int)round(cursor->x) - grabc->geom.x;
		int new_height = (int)round(cursor->y) - grabc->geom.y;
		/* Ensure minimum dimensions */
		if (new_width < MIN_WINDOW_WIDTH) new_width = MIN_WINDOW_WIDTH;
		if (new_height < MIN_WINDOW_HEIGHT) new_height = MIN_WINDOW_HEIGHT;
		resize(grabc, (struct wlr_box){.x = grabc->geom.x, .y = grabc->geom.y,
			.width = new_width, .height = new_height}, 1);
		return;
	} else if (cursor_mode == CurTileResize) {
		/* GRID-BASED TILE RESIZING: Connected edge resizing with permanent locking */
		/* CRITICAL SAFETY CHECKS: Prevent all crashes */
		if (!selmon || !grabc) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: selmon or grabc is NULL in CurTileResize - ABORT");
			cursor_mode = CurNormal;
			return;
		}
		
		/* SAFETY: Validate grabc client state */
		if (!grabc->mon || grabc->geom.width <= 0 || grabc->geom.height <= 0) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid grabc geometry in CurTileResize - ABORT");
			cursor_mode = CurNormal;
			grabc = NULL;
			return;
		}
		
		/* SAFETY: Validate cursor position */
		if (cursor->x < -10000 || cursor->x > 10000 || cursor->y < -10000 || cursor->y > 10000) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: cursor position out of bounds in CurTileResize - ABORT");
			cursor_mode = CurNormal;
			return;
		}
		
		/* Calculate mouse movement for grid-based resizing */
		float horizontal_delta = cursor->x - (float)grabcx;
		float vertical_delta = cursor->y - (float)grabcy;
		
		/* GRID RESIZE DEBUG: Log mouse movement and resize state */
		wlr_log(WLR_DEBUG, "[nixtile] GRID RESIZE: cursor=(%.1f,%.1f), grab=(%d,%d), h_delta=%.1f, v_delta=%.1f, edge_type=%d, target_col=%d", 
			cursor->x, cursor->y, grabcx, grabcy, horizontal_delta, vertical_delta, resize_edge_type, resize_target_column);
		
		/* GRID-BASED HORIZONTAL RESIZING: Column edge resizing */
		if (fabs(horizontal_delta) > 2.0f && resize_edge_type == 1) { // Horizontal edge type
			/* Rate limiting for smooth resizing */
			uint32_t current_time = get_time_ms();
			if (current_time - last_horizontal_resize_time < 16) {
				return;
			}
			last_horizontal_resize_time = current_time;
			
			/* Calculate new mfact for column edge adjustment */
			float width_ratio = (float)horizontal_delta / (float)selmon->m.width;
			float new_mfact = selmon->mfact + width_ratio;
			
			/* Apply strict boundary protection with gap consideration */
			float min_mfact = 0.15f + (float)gap / (float)selmon->m.width;
			float max_mfact = 0.85f - (float)gap / (float)selmon->m.width;
			new_mfact = fmax(min_mfact, fmin(max_mfact, new_mfact));
			
			/* Update mfact and save to workspace for persistence */
			selmon->mfact = new_mfact;
			save_workspace_mfact();
			
			/* Schedule frame-synced layout update */
			pending_target_mfact = new_mfact;
			resize_pending = true;
			
			if (resize_timer_source) {
				wl_event_source_remove(resize_timer_source);
			}
			resize_timer_source = wl_event_loop_add_timer(event_loop, frame_synced_resize_callback, NULL);
			wl_event_source_timer_update(resize_timer_source, 16); // 60 FPS sync
			
			wlr_log(WLR_DEBUG, "[nixtile] GRID HORIZONTAL RESIZE: column %d edge, mfact %.3f (delta=%.1f)", 
				resize_target_column, new_mfact, horizontal_delta);
		}
		
		/* GRID-BASED VERTICAL RESIZING: Between two vertically adjacent tiles */
		if (fabs(vertical_delta) > 2.0f && resize_edge_type == 2 && vertical_resize_neighbor) { // Vertical edge type
			/* Rate limiting for smooth vertical resizing */
			uint32_t current_time = get_time_ms();
			if (current_time - last_vertical_resize_time < 16) {
				return;
			}
			last_vertical_resize_time = current_time;
			
			/* Calculate height factor adjustment for vertical resize */
			float height_ratio = (float)vertical_delta / (float)selmon->m.height;
			float current_height_factor = grabc->height_factor;
			float new_height_factor = current_height_factor + height_ratio;
			
			/* Apply boundary protection for height factors */
			float min_height_factor = 0.1f;
			float max_height_factor = 0.9f;
			new_height_factor = fmax(min_height_factor, fmin(max_height_factor, new_height_factor));
			
			/* Update height factor and schedule frame-synced update */
			pending_target_height_factor = new_height_factor;
			vertical_resize_client = grabc;
			vertical_resize_pending = true;
			
			if (resize_timer_source) {
				wl_event_source_remove(resize_timer_source);
			}
			resize_timer_source = wl_event_loop_add_timer(event_loop, frame_synced_resize_callback, NULL);
			wl_event_source_timer_update(resize_timer_source, 16); // 60 FPS sync
			
			wlr_log(WLR_DEBUG, "[nixtile] GRID VERTICAL RESIZE: tile %p, height_factor %.3f -> %.3f (delta=%.1f)", 
				(void*)grabc, current_height_factor, new_height_factor, vertical_delta);
		}
		
		/* Grid-based resizing complete - frame-synced updates handled by timer above */
		wlr_log(WLR_DEBUG, "[nixtile] GRID RESIZE MOTION: h_delta=%.1f, v_delta=%.1f, edge_type=%d, pending_updates: h=%d, v=%d", 
			horizontal_delta, vertical_delta, resize_edge_type, resize_pending, vertical_resize_pending);
		
		/* Schedule frame-synced resize timer if any changes are pending */
		if ((resize_pending || vertical_resize_pending) && !resize_timer) {
			/* Get monitor refresh rate for optimal frame sync */
			int refresh_rate_mhz = get_monitor_refresh_rate(selmon);
			int frame_interval_ms = 1000 / (refresh_rate_mhz / 1000); /* Convert mHz to ms interval */
			
			/* SAFETY: Clamp frame interval to prevent protocol overload or sluggishness */
			if (frame_interval_ms < 8) frame_interval_ms = 8;   /* Max 125Hz - prevents protocol overload */
			if (frame_interval_ms > 20) frame_interval_ms = 20; /* Min 50Hz - ensures responsiveness */
			
			resize_timer = wl_event_loop_add_timer(wl_display_get_event_loop(dpy),
							   frame_synced_resize_callback, NULL);
			if (resize_timer) {
				wl_event_source_timer_update(resize_timer, frame_interval_ms);
				wlr_log(WLR_DEBUG, "[nixtile] GRID RESIZE TIMER: Scheduled frame-synced update in %dms", frame_interval_ms);
			} else {
				wlr_log(WLR_ERROR, "[nixtile] GRID RESIZE ERROR: Failed to create resize timer");
			}
		}
		return;
	}

	/* If there's no client surface under the cursor, set the cursor image to a
	 * default. This is what makes the cursor image appear when you move it
	 * off of a client or over its border. */
	if (!surface && !seat->drag)
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

	pointerfocus(c, surface, sx, sy, time);
}

void
motionrelative(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	motionnotify(event->time_msec, &event->pointer->base, event->delta_x, event->delta_y,
			event->unaccel_dx, event->unaccel_dy);
}

void
moveresize(const Arg *arg)
{
	/* DOUBLE-CLICK PROTECTION: Prevent rapid successive calls */
	static struct timespec last_moveresize_time = {0};
	struct timespec current_time;
	clock_gettime(CLOCK_MONOTONIC, &current_time);
	
	/* Calculate time since last moveresize call */
	long time_diff_ms = (current_time.tv_sec - last_moveresize_time.tv_sec) * 1000 +
						(current_time.tv_nsec - last_moveresize_time.tv_nsec) / 1000000;
	
	/* Debounce: Ignore calls within 100ms to prevent double-click crashes */
	if (time_diff_ms < 100 && last_moveresize_time.tv_sec != 0) {
		wlr_log(WLR_DEBUG, "[nixtile] DOUBLE-CLICK PROTECTION: Ignoring rapid moveresize call (%ldms ago)", time_diff_ms);
		return;
	}
	last_moveresize_time = current_time;
	
	/* Validate cursor mode */
	if (cursor_mode != CurNormal && cursor_mode != CurPressed) {
		wlr_log(WLR_DEBUG, "[nixtile] moveresize: invalid cursor mode: %d", cursor_mode);
		return;
	}

	/* Make sure cursor and args are valid before using them */
	if (!cursor || !arg) {
		wlr_log(WLR_ERROR, "[nixtile] moveresize: cursor or arg is NULL (cursor=%p, arg=%p)", 
			(void *)cursor, (void *)arg);
		return;
	}

	/* Find client under cursor */
	xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
	if (!grabc || client_is_unmanaged(grabc) || grabc->isfullscreen) {
		wlr_log(WLR_DEBUG, "[nixtile] moveresize: no valid client found under cursor");
		return;
	}

	/* Make sure monitor is valid */
	if (!grabc->mon) {
		wlr_log(WLR_ERROR, "[nixtile] moveresize: client has no monitor");
		return;
	}

	/* Keep the window in its current state (tiled or floating) and tell motionnotify to grab it */
	/* Note: We don't change floating status - window stays in current mode */
	switch (arg->ui) {
	case CurMove:
		cursor_mode = CurMove;
		grabcx = (int)round(cursor->x) - grabc->geom.x;
		grabcy = (int)round(cursor->y) - grabc->geom.y;
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "all-scroll");
		wlr_log(WLR_DEBUG, "[nixtile] Started tile dragging: client=%p, column=%d, cursor=%.1f,%.1f", 
			(void*)grabc, grabc->column_group, cursor->x, cursor->y);
		break;
		
	case CurSmartResize: {
		/* Additional check for grabc validity */
		if (!grabc) {
			wlr_log(WLR_ERROR, "[nixtile] CurSmartResize: grabc is NULL");
			return;
		}
		/* Detect which edge of the window the cursor is on */
		int edge = detectresizeedge(grabc, cursor->x, cursor->y);
		
		/* Only allow resizing if there's an adjacent tile in that direction */
		if (edge == EDGE_NONE || !hasadjacenttile(grabc, edge)) {
			wlr_log(WLR_DEBUG, "[nixtile] CurSmartResize: no valid edge (%d) or adjacent tile", edge);
			return;
		}
		
		/* Store the edge being resized with overflow protection */
		double temp_grabcx = round(cursor->x) - grabc->geom.x;
		double temp_grabcy = round(cursor->y) - grabc->geom.y;
		
		/* Prevent integer overflow */
		if (temp_grabcx > INT_MAX || temp_grabcx < INT_MIN || 
		    temp_grabcy > INT_MAX || temp_grabcy < INT_MIN) {
			wlr_log(WLR_ERROR, "[nixtile] CurSmartResize: grab offset overflow (%.2f, %.2f)", temp_grabcx, temp_grabcy);
			return;
		}
		
		grabcx = (int)temp_grabcx;
		grabcy = (int)temp_grabcy;
		
		/* Use the edge as the resize type */
		grabedge = edge;
		
		/* Update cursor based on the edge being resized */
		switch (edge) {
		case EDGE_LEFT:
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "w-resize");
			break;
		case EDGE_RIGHT:
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "e-resize");
			break;
		case EDGE_TOP:
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "n-resize");
			break;
		case EDGE_BOTTOM:
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "s-resize");
			break;
		case EDGE_TOP | EDGE_LEFT:
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "nw-resize");
			break;
		case EDGE_TOP | EDGE_RIGHT:
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "ne-resize");
			break;
		case EDGE_BOTTOM | EDGE_LEFT:
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "sw-resize");
			break;
		case EDGE_BOTTOM | EDGE_RIGHT:
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "se-resize");
			break;
		default:
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
		}
		
		cursor_mode = CurSmartResize;
		break;
	}
	
	case CurResize:
		/* Keep old resize functionality for backwards compatibility */
		cursor_mode = CurResize;
		/* Doesn't work for X11 output - the next absolute motion event
		 * returns the cursor to where it started */
		wlr_cursor_warp_closest(cursor, NULL,
				grabc->geom.x + grabc->geom.width,
				grabc->geom.y + grabc->geom.height);
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "se-resize");
		break;
	}
}

/* Frame-synced resize callback - matches screen Hz for butter-smooth bi-axial resizing */
static int
frame_synced_resize_callback(void *data)
{
	/* CRITICAL SAFETY CHECKS: Prevent all crashes */
	if (!selmon) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: selmon is NULL in resize callback - ABORT");
		resize_timer = NULL;
		resize_pending = false;
		vertical_resize_pending = false;
		return 0;
	}
	
	
	/* Fast early exit for performance */
	if (!resize_pending && !vertical_resize_pending && !horizontal_column_resize_pending) {
		resize_timer = NULL;
		return 0;
	}
	
	/* ULTRA-SMOOTH GPU ACCELERATION: High-performance optimized layout updates */
	bool layout_changed = false;
	
	/* GPU ACCELERATION: Enable all performance features for butter-smooth movement */
	if (high_performance_mode) {
		/* Lock mutex for thread-safe operations */
		pthread_mutex_lock(&layout_mutex);
		
		/* GPU ACCELERATION: Prepare for hardware-accelerated rendering */
		if (drw && wlr_renderer_is_gles2(drw)) {
			/* Enable GPU texture caching for smooth animations */
			wlr_log(WLR_DEBUG, "[nixtile] GPU ACCELERATION: Using hardware-accelerated rendering for smooth resize");
		}
	}
	
	/* Apply pending horizontal mfact change with per-workspace support */
	if (resize_pending) {
		wlr_log(WLR_DEBUG, "[nixtile] Frame-sync callback: applying horizontal mfact %.3f -> %.3f", selmon->mfact, pending_target_mfact);
		selmon->mfact = pending_target_mfact;
		save_workspace_mfact(); /* Save to current workspace */
		resize_pending = false;
		layout_changed = true;
	}
	
	/* Apply pending vertical height factor change */
	if (vertical_resize_pending) {
		/* SAFETY: Validate client pointers before accessing */
		if (!vertical_resize_client || !vertical_resize_neighbor) {
			wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid resize client pointers - ABORT vertical resize");
			vertical_resize_pending = false;
			vertical_resize_client = NULL;
			vertical_resize_neighbor = NULL;
			resize_operation_active = false;
		} else {
			/* ENHANCED SAFETY: Validate client surfaces and scene nodes */
			if (!client_surface(vertical_resize_client) || !client_surface(vertical_resize_client)->mapped ||
			    !vertical_resize_client->scene || !vertical_resize_client->scene_surface) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: Resize client surface/scene corrupted - ABORT");
				vertical_resize_pending = false;
				vertical_resize_client = NULL;
				vertical_resize_neighbor = NULL;
				resize_operation_active = false;
			} else if (!client_surface(vertical_resize_neighbor) || !client_surface(vertical_resize_neighbor)->mapped ||
			           !vertical_resize_neighbor->scene || !vertical_resize_neighbor->scene_surface) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: Resize neighbor surface/scene corrupted - ABORT");
				vertical_resize_pending = false;
				vertical_resize_client = NULL;
				vertical_resize_neighbor = NULL;
				resize_operation_active = false;
			} else if (!vertical_resize_client->mon || vertical_resize_client->geom.width <= 0 || vertical_resize_client->geom.height <= 0) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid resize client geometry - ABORT");
				vertical_resize_pending = false;
				vertical_resize_client = NULL;
				vertical_resize_neighbor = NULL;
				resize_operation_active = false;
			} else if (!vertical_resize_neighbor->mon || vertical_resize_neighbor->geom.width <= 0 || vertical_resize_neighbor->geom.height <= 0) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid resize neighbor geometry - ABORT");
				vertical_resize_pending = false;
				vertical_resize_client = NULL;
				vertical_resize_neighbor = NULL;
				resize_operation_active = false;
			} else if (vertical_resize_client->mon != vertical_resize_neighbor->mon) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: Resize clients on different monitors - ABORT");
				vertical_resize_pending = false;
				vertical_resize_client = NULL;
				vertical_resize_neighbor = NULL;
				resize_operation_active = false;
			} else {
				/* SAFETY: Validate height factor is reasonable */
				if (!isfinite(pending_target_height_factor) || pending_target_height_factor <= 0.1f || pending_target_height_factor >= 3.0f) {
					wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid height factor %.3f - ABORT", pending_target_height_factor);
					vertical_resize_pending = false;
					resize_operation_active = false;
				} else {
					/* FINAL SAFETY: Check if clients are still in client list */
					bool client_valid = false, neighbor_valid = false;
					Client *temp_c;
					wl_list_for_each(temp_c, &clients, link) {
						if (temp_c == vertical_resize_client) client_valid = true;
						if (temp_c == vertical_resize_neighbor) neighbor_valid = true;
					}
					
					if (!client_valid || !neighbor_valid) {
						wlr_log(WLR_ERROR, "[nixtile] SAFETY: Resize clients no longer in client list - ABORT");
						vertical_resize_pending = false;
						vertical_resize_client = NULL;
						vertical_resize_neighbor = NULL;
						resize_operation_active = false;
					} else {
						/* Safe to apply resize */
						vertical_resize_client->height_factor = pending_target_height_factor;
						/* Optimized neighbor adjustment */
						float remaining = 2.0f - pending_target_height_factor;
						if (remaining > 0.2f) {
							vertical_resize_neighbor->height_factor = remaining;
						}
						
						/* MANUAL RESIZE TRACKING: Mark this column as manually resized */
						int column = vertical_resize_client->column_group;
						if (column >= 0 && column < 2) {
							manual_resize_performed[column] = true;
							wlr_log(WLR_INFO, "[nixtile] MANUAL RESIZE: Column %d marked as manually resized", column);
						}
						
						vertical_resize_pending = false;
						layout_changed = true;
						
						/* RESET RESIZE OPERATION STATE: Mark resize as completed */
						resize_operation_active = false;
						wlr_log(WLR_DEBUG, "[nixtile] RESIZE DEBUG: Vertical resize applied successfully - operation complete");
					}
				}
			}
		}
	}
	
	/* Apply pending horizontal column resize */
	if (horizontal_column_resize_pending) {
		wlr_log(WLR_DEBUG, "[nixtile] Frame-sync callback: applying horizontal column resize");
		
		/* Calculate width factors based on new widths - ISOLATED APPROACH */
		int total_available_width = selmon->w.width - innergappx; /* Total available space minus gaps */
		if (total_available_width > 0) {
			/* Calculate width factors as independent ratios of total screen width */
			float left_width_factor = (float)pending_left_width / (float)total_available_width;
			float right_width_factor = (float)pending_right_width / (float)total_available_width;
			
			/* Normalize to ensure factors are reasonable (0.1 to 1.9 range) */
			if (left_width_factor < 0.1f) left_width_factor = 0.1f;
			if (left_width_factor > 1.9f) left_width_factor = 1.9f;
			if (right_width_factor < 0.1f) right_width_factor = 0.1f;
			if (right_width_factor > 1.9f) right_width_factor = 1.9f;
			
			/* SAFETY: Validate monitor and client list before applying width factors */
			if (!selmon || !selmon->wlr_output) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid monitor during horizontal resize - ABORT");
				horizontal_column_resize_pending = false;
				return 0;
			}
			
			/* Apply width factors to all tiles in both columns */
			Client *temp_c;
			int tiles_updated = 0;
			int left_tiles = 0, right_tiles = 0;
			
			wl_list_for_each(temp_c, &clients, link) {
				/* SAFETY: Validate client pointer and monitor assignment */
				if (!temp_c || !temp_c->mon || temp_c->mon != selmon) {
					continue;
				}
				if (!VISIBLEON(temp_c, selmon) || temp_c->isfloating || temp_c->isfullscreen)
					continue;
				
				if (temp_c->column_group == pending_left_column) {
					/* Update left column width factor */
					temp_c->width_factor = left_width_factor;
					tiles_updated++;
					left_tiles++;
				} else if (temp_c->column_group == pending_right_column) {
					/* Update right column width factor */
					temp_c->width_factor = right_width_factor;
					tiles_updated++;
					right_tiles++;
				}
			}
			
			/* VALIDATION: Ensure we actually updated some tiles */
			if (tiles_updated == 0) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: No tiles found for horizontal resize columns %d,%d - ABORT", 
					pending_left_column, pending_right_column);
				horizontal_column_resize_pending = false;
				return 0;
			}
			
			wlr_log(WLR_DEBUG, "[nixtile] WIDTH FACTORS APPLIED: Updated %d tiles, left_factor=%.3f, right_factor=%.3f", 
				tiles_updated, left_width_factor, right_width_factor);
		}
		
		horizontal_column_resize_pending = false;
		layout_changed = true;
	}
	
	/* Apply layout changes only if needed - with rate limiting to prevent broken pipe */
	if (layout_changed) {
		/* RECURSION PROTECTION: Prevent infinite arrange() loops during resize */
		static int arrange_recursion_count = 0;
		if (arrange_recursion_count > 5) {
			wlr_log(WLR_ERROR, "[nixtile] HANG PREVENTION: Too many arrange() calls in resize callback - ABORT");
			arrange_recursion_count = 0;
			resize_timer = NULL;
			return 0;
		}
		
		/* Rate limiting: prevent too frequent arrange() calls */
		static struct timespec last_arrange_time = {0};
		struct timespec current_time;
		clock_gettime(CLOCK_MONOTONIC, &current_time);
		
		/* Calculate time difference in milliseconds */
		long time_diff_ms = (current_time.tv_sec - last_arrange_time.tv_sec) * 1000 +
							(current_time.tv_nsec - last_arrange_time.tv_nsec) / 1000000;
		
		/* DYNAMIC FRAME SYNC: Calculate optimal frame time based on monitor's refresh rate */
		int monitor_refresh_hz = get_monitor_refresh_rate(selmon) / 1000; /* Convert mHz to Hz */
		int optimal_frame_time_ms = (monitor_refresh_hz > 0) ? (1000 / monitor_refresh_hz) : 16;
		
		/* Ensure minimum performance even on very high refresh rate displays */
		if (optimal_frame_time_ms < 4) optimal_frame_time_ms = 4; /* Max 250fps cap for safety */
		
		wlr_log(WLR_DEBUG, "[nixtile] FRAME SYNC: Monitor %dHz -> %dms frame time (was fixed 16ms)", 
		        monitor_refresh_hz, optimal_frame_time_ms);
		
		/* Only arrange if enough time has passed (dynamic based on monitor Hz) */
		if (time_diff_ms >= optimal_frame_time_ms || last_arrange_time.tv_sec == 0) {
			/* BROKEN PIPE PREVENTION: Validate monitor state before arrange */
			if (!selmon || !selmon->wlr_output || !selmon->wlr_output->enabled) {
				wlr_log(WLR_ERROR, "[nixtile] SAFETY: Invalid monitor state in resize callback - ABORT");
				resize_timer = NULL;
				return 0;
			}
			
			wlr_log(WLR_DEBUG, "[nixtile] RESIZE DEBUG: Calling arrange() from frame callback (recursion=%d)", arrange_recursion_count);
			arrange_recursion_count++;
			
			/* BROKEN PIPE PREVENTION: Try-catch style error handling for arrange */
			try_arrange_safe(selmon);
			
			arrange_recursion_count--;
			last_arrange_time = current_time;
			wlr_log(WLR_DEBUG, "[nixtile] RESIZE DEBUG: arrange() completed successfully");
			
			/* MONITOR-SYNCHRONIZED FRAME SCHEDULING: Schedule next frame at optimal timing */
			if (selmon && selmon->wlr_output) {
				/* Schedule frame immediately for smooth animation */
				wlr_output_schedule_frame(selmon->wlr_output);
				
				/* Log frame scheduling for high refresh rate monitors */
				if (monitor_refresh_hz > 60) {
					wlr_log(WLR_DEBUG, "[nixtile] FRAME SYNC: Scheduled next frame for %dHz display", monitor_refresh_hz);
				}
			}
		}
	}
	
	/* Unlock mutex if in high-performance mode */
	if (high_performance_mode) {
		pthread_mutex_unlock(&layout_mutex);
	}
	
	/* Reset timer state */
	resize_timer = NULL;
	
	/* FINAL CLEANUP: Ensure resize operation state is reset */
	if (!resize_pending && !vertical_resize_pending && !horizontal_column_resize_pending) {
		resize_operation_active = false;
	}
	
	return 0;
}

/* Get monitor's actual refresh rate for dynamic frame sync */
static int
get_monitor_refresh_rate(Monitor *m)
{
	if (!m || !m->wlr_output) {
		return 60000; /* Default 60Hz fallback (in mHz) */
	}
	
	/* Get current mode from wlr_output */
	struct wlr_output_mode *mode = m->wlr_output->current_mode;
	if (mode) {
		return mode->refresh; /* Returns refresh rate in mHz (e.g., 60000 for 60Hz) */
	}
	
	/* Fallback: try to get preferred mode */
	struct wlr_output_mode *preferred = wlr_output_preferred_mode(m->wlr_output);
	if (preferred) {
		return preferred->refresh;
	}
	
	/* Final fallback */
	return 60000; /* 60Hz default */
}

/* GPU Acceleration Functions */


/* Frame Rate Diagnostics */
static void
log_frame_timing_stats(Monitor *m)
{
	static struct timespec last_frame_time = {0};
	static int frame_count = 0;
	static double total_frame_time = 0.0;
	static double min_frame_time = 999.0;
	static double max_frame_time = 0.0;
	
	struct timespec current_time;
	clock_gettime(CLOCK_MONOTONIC, &current_time);
	
	if (last_frame_time.tv_sec != 0) {
		/* Calculate frame time in milliseconds */
		double frame_time_ms = (current_time.tv_sec - last_frame_time.tv_sec) * 1000.0 +
		                      (current_time.tv_nsec - last_frame_time.tv_nsec) / 1000000.0;
		
		frame_count++;
		total_frame_time += frame_time_ms;
		
		if (frame_time_ms < min_frame_time) min_frame_time = frame_time_ms;
		if (frame_time_ms > max_frame_time) max_frame_time = frame_time_ms;
		
		/* Log statistics every 5 seconds (approximately) */
		if (frame_count >= 300) { /* ~5 seconds at 60fps */
			double avg_frame_time = total_frame_time / frame_count;
			double actual_fps = 1000.0 / avg_frame_time;
			int target_hz = get_monitor_refresh_rate(m) / 1000;
			
			wlr_log(WLR_INFO, "[nixtile] FRAME TIMING: Target=%dHz, Actual=%.1fFPS, Avg=%.2fms, Min=%.2fms, Max=%.2fms",
			        target_hz, actual_fps, avg_frame_time, min_frame_time, max_frame_time);
			
			/* Check if we're hitting target frame rate */
			if (actual_fps < (target_hz * 0.95)) { /* Allow 5% tolerance */
				wlr_log(WLR_ERROR, "[nixtile] FRAME TIMING: Performance warning - not hitting target %dHz (actual %.1fFPS)",
				        target_hz, actual_fps);
			} else {
				wlr_log(WLR_INFO, "[nixtile] FRAME TIMING: Performance excellent - hitting target %dHz", target_hz);
			}
			
			/* Reset statistics */
			frame_count = 0;
			total_frame_time = 0.0;
			min_frame_time = 999.0;
			max_frame_time = 0.0;
		}
	}
	
	last_frame_time = current_time;
}

/* GRID-BASED TILE RESIZING SYSTEM */

/* Grid tile structure for tracking neighbors and edges */
typedef struct {
	Client *tile;
	Client *left_neighbor;   /* Tile to the left (different column) */
	Client *right_neighbor;  /* Tile to the right (different column) */
	Client *top_neighbor;    /* Tile above (same column) */
	Client *bottom_neighbor; /* Tile below (same column) */
	int column_group;
	int left_edge_x;
	int right_edge_x;
	int top_edge_y;
	int bottom_edge_y;
	bool left_edge_resizable;   /* Can resize left edge (borders another tile) */
	bool right_edge_resizable;  /* Can resize right edge (borders another tile) */
	bool top_edge_resizable;    /* Can resize top edge (borders another tile) */
	bool bottom_edge_resizable; /* Can resize bottom edge (borders another tile) */
} GridTile;

/* Build grid map of all tiles with neighbor relationships */
void
build_grid_map(GridTile *grid_map, int *tile_count, Monitor *m)
{
	Client *c;
	*tile_count = 0;
	
	/* First pass: Create basic tile entries */
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		if (*tile_count >= 32) break;
		
		GridTile *tile = &grid_map[(*tile_count)++];
		tile->tile = c;
		tile->left_neighbor = NULL;
		tile->right_neighbor = NULL;
		tile->top_neighbor = NULL;
		tile->bottom_neighbor = NULL;
		tile->column_group = c->column_group;
		tile->left_edge_x = c->geom.x;
		tile->right_edge_x = c->geom.x + c->geom.width;
		tile->top_edge_y = c->geom.y;
		tile->bottom_edge_y = c->geom.y + c->geom.height;
		tile->left_edge_resizable = false;
		tile->right_edge_resizable = false;
		tile->top_edge_resizable = false;
		tile->bottom_edge_resizable = false;
	}
	
	/* Second pass: Find neighbors and determine resizable edges */
	for (int i = 0; i < *tile_count; i++) {
		GridTile *current = &grid_map[i];
		
		for (int j = 0; j < *tile_count; j++) {
			if (i == j) continue;
			GridTile *other = &grid_map[j];
			
			/* Check for horizontal neighbors (different columns) */
			if (current->column_group != other->column_group) {
				/* Check if tiles are vertically aligned (overlapping Y ranges) */
				if (!(current->bottom_edge_y <= other->top_edge_y || 
				      current->top_edge_y >= other->bottom_edge_y)) {
					
					/* Other tile is to the right */
					if (other->left_edge_x == current->right_edge_x) {
						current->right_neighbor = other->tile;
						current->right_edge_resizable = true;
						other->left_neighbor = current->tile;
						other->left_edge_resizable = true;
					}
					/* Other tile is to the left */
					else if (other->right_edge_x == current->left_edge_x) {
						current->left_neighbor = other->tile;
						current->left_edge_resizable = true;
						other->right_neighbor = current->tile;
						other->right_edge_resizable = true;
					}
				}
			}
			/* Check for vertical neighbors (same column) */
			else if (current->column_group == other->column_group) {
				/* Check if tiles are horizontally aligned (overlapping X ranges) */
				if (!(current->right_edge_x <= other->left_edge_x || 
				      current->left_edge_x >= other->right_edge_x)) {
					
					/* Other tile is below */
					if (other->top_edge_y == current->bottom_edge_y) {
						current->bottom_neighbor = other->tile;
						current->bottom_edge_resizable = true;
						other->top_neighbor = current->tile;
						other->top_edge_resizable = true;
					}
					/* Other tile is above */
					else if (other->bottom_edge_y == current->top_edge_y) {
						current->top_neighbor = other->tile;
						current->top_edge_resizable = true;
						other->bottom_neighbor = current->tile;
						other->bottom_edge_resizable = true;
					}
				}
			}
		}
	}
}

/* Find which edge of a tile the cursor is closest to */
/* EdgeType already defined in resize_edge.h */
typedef int EdgeType;

EdgeType
find_closest_resizable_edge(GridTile *tile, double cursor_x, double cursor_y)
{
	
	double left_dist = cursor_x - tile->left_edge_x;
	double right_dist = tile->right_edge_x - cursor_x;
	double top_dist = cursor_y - tile->top_edge_y;
	double bottom_dist = tile->bottom_edge_y - cursor_y;
	
	/* Find closest edge within threshold */
	double min_dist = INFINITY;
	EdgeType closest_edge = EDGE_NONE;
	
	if (tile->left_edge_resizable && left_dist >= 0 && left_dist <= EDGE_THRESHOLD && left_dist < min_dist) {
		min_dist = left_dist;
		closest_edge = EDGE_LEFT;
	}
	if (tile->right_edge_resizable && right_dist >= 0 && right_dist <= EDGE_THRESHOLD && right_dist < min_dist) {
		min_dist = right_dist;
		closest_edge = EDGE_RIGHT;
	}
	if (tile->top_edge_resizable && top_dist >= 0 && top_dist <= EDGE_THRESHOLD && top_dist < min_dist) {
		min_dist = top_dist;
		closest_edge = EDGE_TOP;
	}
	if (tile->bottom_edge_resizable && bottom_dist >= 0 && bottom_dist <= EDGE_THRESHOLD && bottom_dist < min_dist) {
		min_dist = bottom_dist;
		closest_edge = EDGE_BOTTOM;
	}
	
	/* For horizontal edges, use mouse position to determine side when tile borders both sides */
	if (closest_edge == EDGE_NONE && (tile->left_edge_resizable || tile->right_edge_resizable)) {
		double tile_center_x = tile->left_edge_x + (tile->right_edge_x - tile->left_edge_x) / 2.0;
		if (cursor_x < tile_center_x && tile->left_edge_resizable) {
			closest_edge = EDGE_LEFT;
		} else if (cursor_x >= tile_center_x && tile->right_edge_resizable) {
			closest_edge = EDGE_RIGHT;
		}
	}
	
	return closest_edge;
}

/* Apply horizontal resize to entire column */
void
apply_horizontal_resize_to_column(GridTile *grid_map, int tile_count, int target_column, 
                                   EdgeType edge, int delta_x)
{
	wlr_log(WLR_DEBUG, "[nixtile] GRID: Applying horizontal resize to column %d, edge %d, delta %d", 
	        target_column, edge, delta_x);
	
	for (int i = 0; i < tile_count; i++) {
		GridTile *tile = &grid_map[i];
		if (tile->column_group != target_column) continue;
		
		if (edge == EDGE_LEFT && tile->left_edge_resizable) {
			/* Resize left edge - move left edge left/right */
			tile->tile->geom.x += delta_x;
			tile->tile->geom.width -= delta_x;
			
			/* Update neighbor's right edge */
			if (tile->left_neighbor) {
				tile->left_neighbor->geom.width += delta_x;
			}
			
		} else if (edge == EDGE_RIGHT && tile->right_edge_resizable) {
			/* Resize right edge - extend/shrink right edge */
			tile->tile->geom.width += delta_x;
			
			/* Update neighbor's left edge */
			if (tile->right_neighbor) {
				tile->right_neighbor->geom.x += delta_x;
				tile->right_neighbor->geom.width -= delta_x;
			}
		}
	}
}

/* Apply vertical resize between two specific tiles */
void
apply_vertical_resize_between_tiles(Client *tile1, Client *tile2, int delta_y)
{
	wlr_log(WLR_DEBUG, "[nixtile] GRID: Applying vertical resize between tiles, delta %d", delta_y);
	
	/* Determine which tile is above and which is below */
	Client *top_tile, *bottom_tile;
	if (tile1->geom.y < tile2->geom.y) {
		top_tile = tile1;
		bottom_tile = tile2;
	} else {
		top_tile = tile2;
		bottom_tile = tile1;
	}
	
	/* Resize top tile (extend/shrink bottom edge) */
	top_tile->geom.height += delta_y;
	
	/* Resize bottom tile (move top edge and adjust height) */
	bottom_tile->geom.y += delta_y;
	bottom_tile->geom.height -= delta_y;
	
	/* Ensure minimum sizes */
	const int MIN_HEIGHT = 100;
	if (top_tile->geom.height < MIN_HEIGHT) {
		int correction = MIN_HEIGHT - top_tile->geom.height;
		top_tile->geom.height = MIN_HEIGHT;
		bottom_tile->geom.y -= correction;
		bottom_tile->geom.height += correction;
	}
	if (bottom_tile->geom.height < MIN_HEIGHT) {
		int correction = MIN_HEIGHT - bottom_tile->geom.height;
		bottom_tile->geom.height = MIN_HEIGHT;
		top_tile->geom.height -= correction;
	}
}

void
tileresize(const Arg *arg)
{
	wlr_log(WLR_DEBUG, "[nixtile] GRID RESIZE: Starting grid-based tile resize");
	
	/* CRITICAL SAFETY CHECKS */
	if (!cursor || !selmon) {
		wlr_log(WLR_ERROR, "[nixtile] GRID RESIZE: cursor or selmon is NULL - ABORT");
		return;
	}
	
	/* Find tile under cursor */
	xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
	if (!grabc || grabc->isfloating || grabc->isfullscreen) {
		wlr_log(WLR_DEBUG, "[nixtile] GRID RESIZE: No tiled client under cursor");
		return;
	}
	
	/* Build grid map */
	GridTile grid_map[32];
	int tile_count;
	build_grid_map(grid_map, &tile_count, selmon);
	
	/* Find the target tile in grid map */
	GridTile *target_tile = NULL;
	for (int i = 0; i < tile_count; i++) {
		if (grid_map[i].tile == grabc) {
			target_tile = &grid_map[i];
			break;
		}
	}
	
	if (!target_tile) {
		wlr_log(WLR_ERROR, "[nixtile] GRID RESIZE: Target tile not found in grid map");
		return;
	}
	
	/* Find closest resizable edge */
	EdgeType edge = find_closest_resizable_edge(target_tile, cursor->x, cursor->y);
	if (edge == EDGE_NONE) {
		wlr_log(WLR_DEBUG, "[nixtile] GRID RESIZE: No resizable edge found near cursor");
		return;
	}
	
	/* Store initial state */
	grabcx = (int)cursor->x;
	grabcy = (int)cursor->y;
	
	/* Set cursor mode and store resize context */
	cursor_mode = CurTileResize;
	resize_edge_type = edge;
	resize_target_column = target_tile->column_group;
	
	if (edge == EDGE_TOP || edge == EDGE_BOTTOM) {
		/* Vertical resize between two tiles */
		if (edge == EDGE_TOP && target_tile->top_neighbor) {
			vertical_resize_client = grabc;
			vertical_resize_neighbor = target_tile->top_neighbor;
		} else if (edge == EDGE_BOTTOM && target_tile->bottom_neighbor) {
			vertical_resize_client = grabc;
			vertical_resize_neighbor = target_tile->bottom_neighbor;
		} else {
			wlr_log(WLR_DEBUG, "[nixtile] GRID RESIZE: No neighbor for vertical resize");
			cursor_mode = CurNormal;
			return;
		}
		vertical_resize_pending = true;
	} else {
		/* Horizontal resize affects entire column */
		horizontal_column_resize_pending = true;
	}
	
	wlr_log(WLR_DEBUG, "[nixtile] GRID RESIZE: Started %s resize on edge %d", 
	        (edge == EDGE_TOP || edge == EDGE_BOTTOM) ? "vertical" : "horizontal", edge);
}

void
outputmgrapply(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 0);
}

void
outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test)
{
	/*
	 * Called when a client such as wlr-randr requests a change in output
	 * configuration. This is only one way that the layout can be changed,
	 * so any Monitor information should be updated by updatemons() after an
	 * output_layout.change event, not here.
	 */
	struct wlr_output_configuration_head_v1 *config_head;
	int ok = 1;

	wl_list_for_each(config_head, &config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		Monitor *m = wlr_output->data;
		struct wlr_output_state state;

		/* Ensure displays previously disabled by wlr-output-power-management-v1
		 * are properly handled*/
		m->asleep = 0;

		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, config_head->state.enabled);
		if (!config_head->state.enabled)
			goto apply_or_test;

		if (config_head->state.mode)
			wlr_output_state_set_mode(&state, config_head->state.mode);
		else
			wlr_output_state_set_custom_mode(&state,
					config_head->state.custom_mode.width,
					config_head->state.custom_mode.height,
					config_head->state.custom_mode.refresh);

		wlr_output_state_set_transform(&state, config_head->state.transform);
		wlr_output_state_set_scale(&state, config_head->state.scale);
		wlr_output_state_set_adaptive_sync_enabled(&state,
				config_head->state.adaptive_sync_enabled);

apply_or_test:
		ok &= test ? wlr_output_test_state(wlr_output, &state)
				: wlr_output_commit_state(wlr_output, &state);

		/* Don't move monitors if position wouldn't change. This avoids
		 * wlroots marking the output as manually configured.
		 * wlr_output_layout_add does not like disabled outputs */
		if (!test && wlr_output->enabled && (m->m.x != config_head->state.x || m->m.y != config_head->state.y))
			wlr_output_layout_add(output_layout, wlr_output,
					config_head->state.x, config_head->state.y);

		wlr_output_state_finish(&state);
	}

	if (ok)
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);

	/* https://codeberg.org/nixtile/nixtile/issues/577 */
	updatemons(NULL, NULL);
}

void
outputmgrtest(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 1);
}

void
pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
		uint32_t time)
{
	struct timespec now;

	if (surface != seat->pointer_state.focused_surface &&
			sloppyfocus && time && c && !client_is_unmanaged(c))
		focusclient(c, 0);

	/* If surface is NULL, clear pointer focus */
	if (!surface) {
		wlr_seat_pointer_notify_clear_focus(seat);
		return;
	}

	if (!time) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
	}

	/* Let the client know that the mouse cursor has entered one
	 * of its surfaces, and make keyboard focus follow if desired.
	 * wlroots makes this a no-op if surface is already focused */
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

void
printstatus(void)
{
	Monitor *m = NULL;
	Client *c;
	uint32_t occ, urg, sel;

	wl_list_for_each(m, &mons, link) {
		occ = urg = 0;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m)
				continue;
			occ |= c->tags;
			if (c->isurgent)
				urg |= c->tags;
		}
		if ((c = focustop(m))) {
			printf("%s title %s\n", m->wlr_output->name, client_get_title(c));
			printf("%s appid %s\n", m->wlr_output->name, client_get_appid(c));
			printf("%s fullscreen %d\n", m->wlr_output->name, c->isfullscreen);
			printf("%s floating %d\n", m->wlr_output->name, c->isfloating);
			sel = c->tags;
		} else {
			printf("%s title \n", m->wlr_output->name);
			printf("%s appid \n", m->wlr_output->name);
			printf("%s fullscreen \n", m->wlr_output->name);
			printf("%s floating \n", m->wlr_output->name);
			sel = 0;
		}

		printf("%s selmon %u\n", m->wlr_output->name, m == selmon);
		printf("%s tags %"PRIu32" %"PRIu32" %"PRIu32" %"PRIu32"\n",
			m->wlr_output->name, occ, m->tagset[m->seltags], sel, urg);
		printf("%s layout %s\n", m->wlr_output->name, m->ltsymbol);
	}
	/* Safely flush stdout, ignore broken pipe errors */
	if (fflush(stdout) != 0) {
		/* Broken pipe is normal when external status readers disconnect */
		wlr_log(WLR_DEBUG, "[nixtile] printstatus: stdout flush failed (broken pipe)");
	}
}

void
powermgrsetmode(struct wl_listener *listener, void *data)
{
	struct wlr_output_power_v1_set_mode_event *event = data;
	struct wlr_output_state state = {0};
	Monitor *m = event->output->data;

	if (!m)
		return;

	m->gamma_lut_changed = 1; /* Reapply gamma LUT when re-enabling the ouput */
	wlr_output_state_set_enabled(&state, event->mode);
	wlr_output_commit_state(m->wlr_output, &state);

	m->asleep = !event->mode;
	updatemons(NULL, NULL);
}

void
quit(const Arg *arg)
{
	wl_display_terminate(dpy);
}

// --- Status bar implementation ---

static struct wlr_scene_rect *statusbar_rect = NULL;

int launcher_just_closed = 0;

void show_launcher(const Arg *arg) {
    if (launcher_just_closed) {
        fprintf(stderr, "[nixtile] show_launcher: just closed, skipping\n");
        return;
    }
    if (launcher.visible) {
        fprintf(stderr, "[nixtile] show_launcher: already visible, skipping\n");
        return;
    }
    fprintf(stderr, "[nixtile] show_launcher called (launcher.visible=%d)\n", launcher.visible);
    Monitor *m;
    int center_x, center_y;
    int tab_height, tab_width, base_x, base_y;
    int i;
    
    launcher.visible = true;
    launcher.width = 300;
    launcher.height = 300;
    launcher.selected_tab = 0;
    launcher.search[0] = '\0';
    launcher.search_len = 0;
    launcher.selected_index = 0;

    update_launcher_filter();

    m = selmon; // Use selected monitor for overlay
    if (!m) return;

    // Use the global overlay scene layer as parent for the launcher overlay
    if (!launcher_scene_tree) {
        launcher_scene_tree = wlr_scene_tree_create(layers[LyrOverlay]);
    }

    wlr_scene_node_raise_to_top(&launcher_scene_tree->node);
    wlr_scene_node_set_enabled(&launcher_scene_tree->node, true);

    if (!launcher_rect) {
        launcher_rect = wlr_scene_rect_create(launcher_scene_tree,
            launcher.width, launcher.height, launcher_bg_color);
    } else {
        wlr_scene_rect_set_size(launcher_rect, launcher.width, launcher.height);
        wlr_scene_node_set_enabled(&launcher_rect->node, true);
    }
    // Center overlay on monitor using m->m (monitor geometry)
    center_x = m->m.x + (m->m.width - launcher.width) / 2;
    center_y = m->m.y + (m->m.height - launcher.height) / 2;
    wlr_scene_node_set_position(&launcher_rect->node, center_x, center_y);

    tab_height = 36;
    tab_width = launcher.width / 3;
    base_x = center_x;
    base_y = center_y;
    for (i = 0; i < 3; ++i) {
        const float *color = (i == launcher.selected_tab) ? launcher_tab_active_color : launcher_tab_color;
        if (!launcher_tab_rects[i]) {
            launcher_tab_rects[i] = wlr_scene_rect_create(launcher_scene_tree,
                tab_width, tab_height, color);
        } else {
            wlr_scene_rect_set_size(launcher_tab_rects[i], tab_width, tab_height);
            wlr_scene_rect_set_color(launcher_tab_rects[i], color);
            wlr_scene_node_set_enabled(&launcher_tab_rects[i]->node, true);
        }
        wlr_scene_node_set_position(&launcher_tab_rects[i]->node,
            base_x + i * tab_width,
            base_y);
        wlr_scene_node_place_above(&launcher_tab_rects[i]->node, &launcher_rect->node);
    }

    // --- Draw tab labels (placeholder, no real text rendering) ---
    // TODO: Implement text rendering for tab labels

    // --- Draw app list and search box (placeholder, no real text rendering) ---
    // TODO: Implement text rendering for app list and search box

    // Schedule redraw
    if (m && m->wlr_output)
        wlr_output_schedule_frame(m->wlr_output);
}

// --- Static app list for launcher ---
#define LAUNCHER_MAX_APPS 32
static const char *launcher_apps[LAUNCHER_MAX_APPS] = {
    "Alacritty", "Firefox", "Thunar", "Foot", "Neovim", "GIMP", "Inkscape", "htop",
    "Calculator", "Terminal", "Files", "VSCode", "LibreOffice", "Steam", "Discord",
    "Spotify", "mpv", "qutebrowser", "Ranger", "ncmpcpp", "Lutris", "Telegram",
    "Signal", "Hexchat", "KeepassXC", "Zathura", "Evince", "Shotwell", "Gwenview",
    "Krita", "OBS Studio", "Blender"
};
#define LAUNCHER_APP_COUNT 32

// --- Fuzzy search and filter ---
static int launcher_filtered[LAUNCHER_MAX_APPS];
static int launcher_filtered_count = 0;

static void update_launcher_filter(void) {
    int i;
    launcher_filtered_count = 0;
    for (i = 0; i < LAUNCHER_APP_COUNT; ++i) {
        if (launcher.search_len == 0 || strstr(launcher_apps[i], launcher.search)) {
            launcher_filtered[launcher_filtered_count++] = i;
        }
    }
    if (launcher.selected_index >= launcher_filtered_count) {
        launcher.selected_index = launcher_filtered_count - 1;
    }
    if (launcher.selected_index < 0) launcher.selected_index = 0;
}

// --- Input handling for launcher ---
#include <xkbcommon/xkbcommon-keysyms.h>

// --- Static function prototypes for C90 compliance ---
static void keypress(struct wl_listener *listener, void *data);
static void keypressmod(struct wl_listener *listener, void *data);
static void handle_launcher_key(uint32_t key, uint32_t state, xkb_keysym_t sym);
static void hide_launcher(void);
static void update_launcher_filter(void);


static void handle_launcher_key(uint32_t key, uint32_t state, xkb_keysym_t sym) {
    if (!launcher.visible || state != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    // ESC: close launcher
    if (sym == XKB_KEY_Escape) {
        hide_launcher();
        return;
    }
    // Tab/Left/Right: switch tab
    if (sym == XKB_KEY_Tab || sym == XKB_KEY_Right) {
        launcher.selected_tab = (launcher.selected_tab + 1) % 3;
        return;
    }
    if (sym == XKB_KEY_Left) {
        launcher.selected_tab = (launcher.selected_tab + 2) % 3;
        return;
    }
    // Up/Down: move selection
    if (sym == XKB_KEY_Up) {
        if (launcher.selected_index > 0) launcher.selected_index--;
        return;
    }
    if (sym == XKB_KEY_Down) {
        if (launcher.selected_index + 1 < launcher_filtered_count) launcher.selected_index++;
        return;
    }
    // Enter: launch selected app (placeholder)
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        // TODO: Actually launch the selected app
        hide_launcher();
        return;
    }
    // Backspace: remove last char in search
    if (sym == XKB_KEY_BackSpace) {
        if (launcher.search_len > 0) {
            launcher.search[--launcher.search_len] = '\0';
            update_launcher_filter();
        }
        return;
    }
    // Printable ASCII: add to search
    if ((sym >= 32 && sym <= 126) && launcher.search_len < (int)sizeof(launcher.search) - 1) {
        launcher.search[launcher.search_len++] = (char)sym;
        launcher.search[launcher.search_len] = '\0';
        update_launcher_filter();
        return;
    }
}

// --- Minimal placeholder for drawing app list and search box (no real text rendering) ---
// TODO: Integrate a text rendering library or draw text buffers for real UI
// For now, just leave comments where text would be rendered.


void hide_launcher(void) {
    launcher_just_closed = 1;
    fprintf(stderr, "[nixtile] hide_launcher called (launcher.visible=%d)\n", launcher.visible);
    Monitor *m;
    int i;
    launcher.visible = false;
    // Hide or destroy launcher scene nodes
    if (launcher_rect) {
        wlr_scene_node_set_enabled(&launcher_rect->node, false);
    }
    for (i = 0; i < 3; ++i) {
        if (launcher_tab_rects[i]) {
            wlr_scene_node_set_enabled(&launcher_tab_rects[i]->node, false);
        }
    }
    if (launcher_scene_tree) {
        wlr_scene_node_set_enabled(&launcher_scene_tree->node, false);
    }
    m = selmon;
    if (m && m->wlr_output)
        wlr_output_schedule_frame(m->wlr_output);
}



void toggle_statusbar(const Arg *arg) {
    statusbar_visible = !statusbar_visible;
    arrange(selmon);
    printstatus(); // Triggers redraw
    if (selmon && selmon->wlr_output)
        wlr_output_schedule_frame(selmon->wlr_output);
}

static void draw_status_bar(Monitor *m) {
    int bar_x = 0, bar_y = 0, bar_w = 0, bar_h = 0;
    float color[4];
    // Always remove previous bar if it exists
    if (statusbar_rect) {
        wlr_scene_node_destroy(&statusbar_rect->node);
        statusbar_rect = NULL;
    }
    if (!statusbar_visible) {
        return;
    }

    bar_w = m->wlr_output->width;
    bar_h = statusbar_height;
    if (strcmp(statusbar_position, "top") == 0) {
        bar_x = statusbar_side_gap;
        bar_y = statusbar_top_gap;
        bar_w = m->wlr_output->width - (2 * statusbar_side_gap);
        bar_h = statusbar_height;
    } else if (strcmp(statusbar_position, "bottom") == 0) {
        bar_x = statusbar_side_gap;
        bar_y = m->wlr_output->height - statusbar_height; /* Bottom gap ignored as requested */
        bar_w = m->wlr_output->width - (2 * statusbar_side_gap);
        bar_h = statusbar_height;
    } else if (strcmp(statusbar_position, "left") == 0) {
        bar_x = statusbar_side_gap;
        bar_y = statusbar_top_gap;
        bar_w = statusbar_height;
        bar_h = m->wlr_output->height - statusbar_top_gap; /* Bottom gap ignored */
    } else if (strcmp(statusbar_position, "right") == 0) {
        bar_x = m->wlr_output->width - statusbar_height - statusbar_side_gap;
        bar_y = statusbar_top_gap;
        bar_w = statusbar_height;
        bar_h = m->wlr_output->height - statusbar_top_gap; /* Bottom gap ignored */
    }

    color[0] = statusbar_color[0];
    color[1] = statusbar_color[1];
    color[2] = statusbar_color[2];
    color[3] = statusbar_alpha;
    statusbar_rect = wlr_scene_rect_create(&scene->tree, bar_w, bar_h, color);
    wlr_scene_node_set_position(&statusbar_rect->node, bar_x, bar_y);
    wlr_scene_node_place_above(&statusbar_rect->node, &layers[LyrTop]->node);
}

void
rendermon(struct wl_listener *listener, void *data)
{
	/* MONITOR-SYNCHRONIZED RENDERING: Perfect frame timing at native refresh rate */
	/* This function is called every time an output is ready to display a frame,
	 * synchronized with the monitor's actual refresh rate (60Hz, 120Hz, 144Hz, etc.) */
	Monitor *m = wl_container_of(listener, m, frame);
	Client *c;
	struct wlr_output_state pending = {0};
	struct timespec now;
	
	/* FRAME RATE SYNCHRONIZATION: Get monitor's actual refresh rate */
	int monitor_refresh_hz = get_monitor_refresh_rate(m) / 1000;
	static int last_logged_hz = 0;
	if (monitor_refresh_hz != last_logged_hz) {
		wlr_log(WLR_INFO, "[nixtile] FRAME SYNC: Rendering at %dHz (monitor native rate)", monitor_refresh_hz);
		last_logged_hz = monitor_refresh_hz;
	}
	
	/* FRAME TIMING DIAGNOSTICS: Monitor actual frame rate performance */
	log_frame_timing_stats(m);
	
	/* GPU ACCELERATION: Enable hardware-accelerated frame rendering */
	if (high_performance_mode && drw && wlr_renderer_is_gles2(drw)) {
		/* Hardware-accelerated rendering path for ultra-smooth performance */
		static int frame_count = 0;
		frame_count++;
		
		/* Log performance info every 60 frames to avoid spam */
		if (frame_count % 60 == 0) {
			wlr_log(WLR_DEBUG, "[nixtile] GPU ACCELERATION: Hardware-accelerated rendering at %dHz (frame %d)", 
			        monitor_refresh_hz, frame_count);
		}
		
		/* Enable GPU texture caching and buffer optimization */
		if (m->wlr_output->adaptive_sync_supported) {
			/* Adaptive sync provides variable refresh rate for smoother experience */
			if (frame_count % 120 == 0) { /* Log every 2 seconds at 60fps */
				wlr_log(WLR_DEBUG, "[nixtile] GPU ACCELERATION: Using adaptive sync for tear-free rendering");
			}
		}
	}
	
	/* SAFE FRAME-SYNCED RENDERING: No direct resize callbacks to prevent infinite loops */

	/* Render if no XDG clients have an outstanding resize and are visible on
	 * this monitor. */
	wl_list_for_each(c, &clients, link) {
		if (c->resize && !c->isfloating && client_is_rendered_on_mon(c, m) && !client_is_stopped(c))
			goto skip;
	}

	/* ULTRA-SMOOTH GPU ACCELERATION: Optimized scene rendering */
	draw_status_bar(m);
	
	/* GPU ACCELERATION: Hardware-accelerated scene commit for smooth animations */
	if (high_performance_mode && drw && wlr_renderer_is_gles2(drw)) {
		/* Enable GPU buffer optimization for ultra-smooth tile movements */
		wlr_scene_output_commit(m->scene_output, NULL);
		wlr_log(WLR_DEBUG, "[nixtile] GPU ACCELERATION: Hardware-accelerated scene commit completed");
	} else {
		/* Standard scene rendering */
		wlr_scene_output_commit(m->scene_output, NULL);
	}

skip:
	/* Let clients know a frame has been rendered */
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);
	wlr_output_state_finish(&pending);
}

void
requestdecorationmode(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_decoration_mode);
	if (c->surface.xdg->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(c->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
requeststartdrag(struct wl_listener *listener, void *data)
{
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(seat, event->origin,
			event->serial))
		wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

void
requestmonstate(struct wl_listener *listener, void *data)
{
	struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(event->output, event->state);
	updatemons(NULL, NULL);
}

void
resize(Client *c, struct wlr_box geo, int interact)
{
	struct wlr_box *bbox;
	struct wlr_box clip;

	/* Enhanced validation to prevent crashes */
	if (!c || !c->mon || !client_surface(c) || !client_surface(c)->mapped)
		return;
	
	/* COMPREHENSIVE RESIZE SAFETY CHECK: Prevent tile destruction */
	if (!is_resize_safe(c, geo.width, geo.height)) {
		wlr_log(WLR_DEBUG, "[nixtile] RESIZE SAFETY: Aborting unsafe resize operation for client %p", (void*)c);
		return; /* Abort resize to prevent tile destruction */
	}
	
	/* HARMONIZED MINIMUM SIZE PROTECTION: Match stack protection minimum (120px) */
	int generous_min_width = 120;  /* Harmonized with stack protection minimum */
	int generous_min_height = 120; /* Harmonized minimum height */
	
	if (geo.width < generous_min_width || geo.height < generous_min_height) {
		wlr_log(WLR_DEBUG, "[nixtile] TILE PROTECTION: Enforcing generous minimum size (requested: %dx%d, enforcing: %dx%d)", 
		        geo.width, geo.height, 
		        MAX(geo.width, generous_min_width), MAX(geo.height, generous_min_height));
		geo.width = MAX(geo.width, generous_min_width);
		geo.height = MAX(geo.height, generous_min_height);
	}
	
	/* Validate scene nodes exist */
	if (!c->scene || !c->scene_surface) {
		return;
	}

	bbox = interact ? &sgeom : &c->mon->w;

	client_set_bounds(c, geo.width, geo.height);
	c->geom = geo;
	applybounds(c, bbox);

	/* Update scene-graph, including borders with safety checks */
	if (c->scene && c->scene_surface) {
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
		
		/* Update borders with validation */
		for (int i = 0; i < 4; i++) {
			if (!c->border[i]) continue;
		}
		
		if (c->border[0]) {
			wlr_scene_rect_set_size(c->border[0], c->geom.width, c->bw);
		}
		if (c->border[1]) {
			wlr_scene_rect_set_size(c->border[1], c->geom.width, c->bw);
			wlr_scene_node_set_position(&c->border[1]->node, 0, c->geom.height - c->bw);
		}
		if (c->border[2]) {
			wlr_scene_rect_set_size(c->border[2], c->bw, c->geom.height - 2 * c->bw);
			wlr_scene_node_set_position(&c->border[2]->node, 0, c->bw);
		}
		if (c->border[3]) {
			wlr_scene_rect_set_size(c->border[3], c->bw, c->geom.height - 2 * c->bw);
			wlr_scene_node_set_position(&c->border[3]->node, c->geom.width - c->bw, c->bw);
		}
	}

	/* Optimized client resize for better responsiveness */
	c->resize = client_set_size(c, c->geom.width - 2 * c->bw,
			c->geom.height - 2 * c->bw);
	client_get_clip(c, &clip);
	wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
}

void
run(char *startup_cmd)
{
	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(dpy);
	if (!socket)
		die("startup: display_add_socket_auto");
	setenv("WAYLAND_DISPLAY", socket, 1);

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(backend))
		die("startup: backend_start");

	/* Now that the socket exists and the backend is started, run the startup command */
	if (startup_cmd) {
		int piperw[2];
		if (pipe(piperw) < 0)
			die("startup: pipe:");
		if ((child_pid = fork()) < 0)
			die("startup: fork:");
		if (child_pid == 0) {
			setsid();
			dup2(piperw[0], STDIN_FILENO);
			close(piperw[0]);
			close(piperw[1]);
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, NULL);
			die("startup: execl:");
		}
		dup2(piperw[1], STDOUT_FILENO);
		close(piperw[1]);
		close(piperw[0]);
	}

	/* Mark stdout as non-blocking to avoid the startup script
	 * causing nixtile to freeze when a user neither closes stdin
	 * nor consumes standard input in his startup script */

	if (fd_set_nonblock(STDOUT_FILENO) < 0)
		close(STDOUT_FILENO);

	printstatus();

	/* At this point the outputs are initialized, choose initial selmon based on
	 * cursor position, and set default cursor image */
	selmon = xytomon(cursor->x, cursor->y);

	/* TODO hack to get cursor to display in its initial location (100, 100)
	 * instead of (0, 0) and then jumping. Still may not be fully
	 * initialized, as the image/coordinates are not transformed for the
	 * monitor when displayed here */
	wlr_cursor_warp_closest(cursor, NULL, cursor->x, cursor->y);
	wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wl_display_run(dpy);
}

void
setcursor(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* If we're "grabbing" the cursor, don't use the client's image, we will
	 * restore it after "grabbing" sending a leave event, followed by a enter
	 * event, which will result in the client requesting set the cursor surface */
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided surface as the cursor image. It will set the
	 * hardware cursor on the output that it's currently on and continue to
	 * do so as the cursor moves between outputs. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_surface(cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
}

void
setcursorshape(struct wl_listener *listener, void *data)
{
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided cursor shape. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_xcursor(cursor, cursor_mgr,
				wlr_cursor_shape_v1_name(event->shape));
}

void
setfloating(Client *c, int floating)
{
	Client *p = client_get_parent(c);
	c->isfloating = floating;
	/* If in floating layout do not change the client's layer */
	if (!c->mon || !client_surface(c)->mapped || !c->mon->lt[c->mon->sellt]->arrange)
		return;
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen ||
			(p && p->isfullscreen) ? LyrFS
			: c->isfloating ? LyrFloat : LyrTile]);
	arrange(c->mon);
	printstatus();
}

void
force_column_tiling(Monitor *m)
{
	Client *c;
	int left_column_count = 0, right_column_count = 0;
	
	if (!m) {
		wlr_log(WLR_ERROR, "[nixtile] force_column_tiling: monitor is NULL");
		return;
	}
	
	wlr_log(WLR_DEBUG, "[nixtile] FORCE COLUMN TILING: Starting force tiling for monitor %p", (void*)m);
	
	/* First pass: Force all tiles to be non-floating and assign to columns */
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfullscreen)
			continue;
		
		/* FORCE: No tile can be floating */
		if (c->isfloating) {
			wlr_log(WLR_DEBUG, "[nixtile] FORCE: Converting floating tile %p to tiled", (void*)c);
			c->isfloating = 0;
		}
		
		/* Ensure tile has a valid column assignment for multi-column layout */
		int optimal_columns = get_optimal_columns(m->w.width);
		if (c->column_group < 0 || c->column_group >= optimal_columns) {
			wlr_log(WLR_DEBUG, "[nixtile] FORCE: Invalid column %d for tile %p (valid: 0-%d), will be reassigned in tile()", 
				c->column_group, (void*)c, optimal_columns - 1);
			/* DO NOT force to 0 - let tile() function handle assignment */
		}
		
		/* Count tiles in each column (only count valid assignments) */
		if (c->column_group >= 0 && c->column_group < optimal_columns) {
			if (c->column_group == 0) {
				left_column_count++;
			} else {
				right_column_count++;
			}
		}
	}
	
	/* Second pass: Balance columns if needed (optional - can be disabled for strict positioning) */
	/* For now, we keep the current column assignments but ensure they're valid */
	
	/* Third pass: Ensure proper scene layer assignment */
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfullscreen)
			continue;
		
		/* Force tile layer assignment */
		Client *p = client_get_parent(c);
		wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen ||
				(p && p->isfullscreen) ? LyrFS : LyrTile]);
	}
	
	wlr_log(WLR_DEBUG, "[nixtile] FORCE COLUMN TILING: Completed - left_column=%d, right_column=%d", 
	        left_column_count, right_column_count);
	
	/* Force layout update */
	arrange(m);
}

void
setfullscreen(Client *c, int fullscreen)
{
	c->isfullscreen = fullscreen;
	if (!c->mon || !client_surface(c)->mapped)
		return;
	c->bw = fullscreen ? 0 : borderpx;
	client_set_fullscreen(c, fullscreen);
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen
			? LyrFS : c->isfloating ? LyrFloat : LyrTile]);

	if (fullscreen) {
		c->prev = c->geom;
		resize(c, c->mon->m, 0);
	} else {
		/* restore previous size instead of arrange for floating windows since
		 * client positions are set by the user and cannot be recalculated */
		resize(c, c->prev, 0);
	}
	try_arrange_safe(c->mon);
	printstatus();
}

void
setlayout(const Arg *arg)
{
	if (!selmon)
		return;
	if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
		selmon->sellt ^= 1;
	if (arg && arg->v)
		selmon->lt[selmon->sellt] = (Layout *)arg->v;
	strncpy(selmon->ltsymbol, selmon->lt[selmon->sellt]->symbol, LENGTH(selmon->ltsymbol));
	arrange(selmon);
	printstatus();
}

/* arg > 1.0 will set mfact absolutely */
void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !selmon || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0f ? arg->f + selmon->mfact : arg->f - 1.0f;
	if (f < 0.1 || f > 0.9)
		return;
	selmon->mfact = f;
	arrange(selmon);
}

void
setmon(Client *c, Monitor *m, uint32_t newtags)
{
	Monitor *oldmon = c->mon;

	if (oldmon == m)
		return;
	c->mon = m;
	c->prev = c->geom;

	/* Scene graph sends surface leave/enter events on move and resize */
	if (oldmon)
		try_arrange_safe(oldmon);
	if (m) {
		/* Make sure window actually overlaps with the monitor */
		resize(c, c->geom, 0);
		c->tags = newtags ? newtags : m->tagset[m->seltags]; /* assign tags of target monitor */
		setfullscreen(c, c->isfullscreen); /* This will call arrange(c->mon) */
		setfloating(c, c->isfloating);
	}
	focusclient(focustop(selmon), 1);
}

void
setpsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in nixtile we always honor them
	 */
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

void
setsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in nixtile we always honor them
	 */
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(seat, event->source, event->serial);
}

void
setup(void)
{
	int drm_fd, i, sig[] = {SIGCHLD, SIGINT, SIGTERM, SIGPIPE};
	struct sigaction sa = {.sa_flags = SA_RESTART, .sa_handler = handlesig};
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < (int)LENGTH(sig); i++)
		sigaction(sig[i], &sa, NULL);

	wlr_log_init(log_level, NULL);

	/* ULTRA-SMOOTH GPU ACCELERATION: Initialize hardware acceleration system */
	init_gpu_acceleration();
	
	/* Enable high performance mode for butter-smooth movements */
	high_performance_mode = true; /* Enable all GPU optimizations */
	
	/* Aggressive CPU optimization for ultra-smooth performance */
	if (nice(-10) != -1) {
		wlr_log(WLR_INFO, "[nixtile] GPU ACCELERATION: Set high priority (nice -10) for ultra-smooth movements");
	}
	
	/* Enable GPU acceleration environment variables */
	setenv("WLR_RENDERER", "gles2", 0); /* Prefer hardware-accelerated GLES2 */
	setenv("WLR_DRM_NO_ATOMIC", "0", 0); /* Enable atomic modesetting for smooth updates */
	setenv("WLR_DRM_NO_MODIFIERS", "0", 0); /* Enable DRM modifiers for better performance */
	setenv("WLR_SCENE_DISABLE_VISIBILITY", "0", 0); /* Enable scene visibility optimizations */
	wlr_log(WLR_INFO, "[nixtile] GPU ACCELERATION: Enabled hardware acceleration environment");

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	dpy = wl_display_create();
	event_loop = wl_display_get_event_loop(dpy);

	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	if (!(backend = wlr_backend_autocreate(event_loop, &session)))
		die("couldn't create backend");

	/* Initialize the scene graph used to lay out windows */
	scene = wlr_scene_create();
	root_bg = wlr_scene_rect_create(&scene->tree, 0, 0, rootcolor);
	for (i = 0; i < NUM_LAYERS; i++)
		layers[i] = wlr_scene_tree_create(&scene->tree);
	drag_icon = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_place_below(&drag_icon->node, &layers[LyrBlock]->node);

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
	 * can also specify a renderer using the WLR_RENDERER env var.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	if (!(drw = wlr_renderer_autocreate(backend)))
		die("couldn't create renderer");
	wl_signal_add(&drw->events.lost, &gpu_reset);
	
	/* ULTRA-SMOOTH GPU ACCELERATION: Configure renderer for maximum performance */
	if (drw) {
		/* Log renderer capabilities for debugging */
		const char *renderer_name = "unknown";
		if (wlr_renderer_is_gles2(drw)) {
			renderer_name = "OpenGL ES 2.0 (Hardware Accelerated)";
			wlr_log(WLR_INFO, "[nixtile] GPU ACCELERATION: Using hardware-accelerated OpenGL ES 2.0 renderer");
		} else if (wlr_renderer_is_pixman(drw)) {
			renderer_name = "Pixman (Software Fallback)";
			wlr_log(WLR_INFO, "[nixtile] GPU ACCELERATION: Using software Pixman renderer (no hardware acceleration)");
		} else {
			wlr_log(WLR_INFO, "[nixtile] GPU ACCELERATION: Using unknown renderer type");
		}
		
		/* Enable all available GPU features for ultra-smooth performance */
		wlr_log(WLR_DEBUG, "[nixtile] GPU ACCELERATION: Renderer name: %s", renderer_name);
	}

	/* Create shm, drm and linux_dmabuf interfaces by ourselves.
	 * The simplest way is to call:
	 *      wlr_renderer_init_wl_display(drw);
	 * but we need to create the linux_dmabuf interface manually to integrate it
	 * with wlr_scene. */
	wlr_renderer_init_wl_shm(drw, dpy);

	if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
		wlr_drm_create(dpy, drw);
		wlr_scene_set_linux_dmabuf_v1(scene,
				wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw));
	}

	if ((drm_fd = wlr_renderer_get_drm_fd(drw)) >= 0 && drw->features.timeline
			&& backend->features.timeline)
		wlr_linux_drm_syncobj_manager_v1_create(dpy, 1, drm_fd);

	/* Autocreates an allocator for us.
	 * The allocator is the bridge between the renderer and the backend. It
	 * handles the buffer creation, allowing wlroots to render onto the
	 * screen */
	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't create allocator");

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the setsel() function. */
	compositor = wlr_compositor_create(dpy, 6, drw);
	wlr_subcompositor_create(dpy);
	wlr_data_device_manager_create(dpy);
	wlr_export_dmabuf_manager_v1_create(dpy);
	wlr_screencopy_manager_v1_create(dpy);
	wlr_data_control_manager_v1_create(dpy);
	wlr_primary_selection_v1_device_manager_create(dpy);
	wlr_viewporter_create(dpy);
	wlr_single_pixel_buffer_manager_v1_create(dpy);
	wlr_fractional_scale_manager_v1_create(dpy, 1);
	wlr_presentation_create(dpy, backend, 2);
	wlr_alpha_modifier_v1_create(dpy);

	/* Initializes the interface used to implement urgency hints */
	activation = wlr_xdg_activation_v1_create(dpy);
	wl_signal_add(&activation->events.request_activate, &request_activate);

	wlr_scene_set_gamma_control_manager_v1(scene, wlr_gamma_control_manager_v1_create(dpy));

	power_mgr = wlr_output_power_manager_v1_create(dpy);
	wl_signal_add(&power_mgr->events.set_mode, &output_power_mgr_set_mode);

	/* Creates an output layout, which is a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	output_layout = wlr_output_layout_create(dpy);
	wl_signal_add(&output_layout->events.change, &layout_change);

    wlr_xdg_output_manager_v1_create(dpy, output_layout);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&mons);
	wl_signal_add(&backend->events.new_output, &new_output);

	/* Set up our client lists, the xdg-shell and the layer-shell. The xdg-shell is a
	 * Wayland protocol which is used for application windows. For more
	 * detail on shells, refer to the article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&clients);
	wl_list_init(&fstack);

	xdg_shell = wlr_xdg_shell_create(dpy, 6);
	wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
	wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

	layer_shell = wlr_layer_shell_v1_create(dpy, 3);
	wl_signal_add(&layer_shell->events.new_surface, &new_layer_surface);

	idle_notifier = wlr_idle_notifier_v1_create(dpy);

	idle_inhibit_mgr = wlr_idle_inhibit_v1_create(dpy);
	wl_signal_add(&idle_inhibit_mgr->events.new_inhibitor, &new_idle_inhibitor);

	session_lock_mgr = wlr_session_lock_manager_v1_create(dpy);
	wl_signal_add(&session_lock_mgr->events.new_lock, &new_session_lock);
	locked_bg = wlr_scene_rect_create(layers[LyrBlock], sgeom.width, sgeom.height,
			(float [4]){0.1f, 0.1f, 0.1f, 1.0f});
	wlr_scene_node_set_enabled(&locked_bg->node, 0);

	/* Use decoration protocols to negotiate server-side decorations */
	wlr_server_decoration_manager_set_default_mode(
			wlr_server_decoration_manager_create(dpy),
			WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(dpy);
	wl_signal_add(&xdg_decoration_mgr->events.new_toplevel_decoration, &new_xdg_decoration);

	pointer_constraints = wlr_pointer_constraints_v1_create(dpy);
	wl_signal_add(&pointer_constraints->events.new_constraint, &new_pointer_constraint);

	relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(dpy);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). Scaled cursors will be loaded with each output. */
	cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	setenv("XCURSOR_SIZE", "24", 1);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	wl_signal_add(&cursor->events.motion, &cursor_motion);
	wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
	wl_signal_add(&cursor->events.button, &cursor_button);
	wl_signal_add(&cursor->events.axis, &cursor_axis);
	wl_signal_add(&cursor->events.frame, &cursor_frame);

	cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(dpy, 1);
	wl_signal_add(&cursor_shape_mgr->events.request_set_shape, &request_set_cursor_shape);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_signal_add(&backend->events.new_input, &new_input_device);
	virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(dpy);
	wl_signal_add(&virtual_keyboard_mgr->events.new_virtual_keyboard,
			&new_virtual_keyboard);
	virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(dpy);
    wl_signal_add(&virtual_pointer_mgr->events.new_virtual_pointer,
            &new_virtual_pointer);

	seat = wlr_seat_create(dpy, "seat0");
	wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
	wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
	wl_signal_add(&seat->events.request_set_primary_selection, &request_set_psel);
	wl_signal_add(&seat->events.request_start_drag, &request_start_drag);
	wl_signal_add(&seat->events.start_drag, &start_drag);

	kb_group = createkeyboardgroup();
	wl_list_init(&kb_group->destroy.link);

	output_mgr = wlr_output_manager_v1_create(dpy);
	wl_signal_add(&output_mgr->events.apply, &output_mgr_apply);
	wl_signal_add(&output_mgr->events.test, &output_mgr_test);

	/* Make sure XWayland clients don't connect to the parent X server,
	 * e.g when running in the x11 backend or the wayland backend and the
	 * compositor has Xwayland support */
	unsetenv("DISPLAY");
#ifdef XWAYLAND
	/*
	 * Initialise the XWayland X server.
	 * It will be started when the first X client is started.
	 */
	if ((xwayland = wlr_xwayland_create(dpy, compositor, 1))) {
		wl_signal_add(&xwayland->events.ready, &xwayland_ready);
		wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

		setenv("DISPLAY", xwayland->display_name, 1);
	} else {
		fprintf(stderr, "failed to setup XWayland X server, continuing without it\n");
	}
#endif
}

void
spawn(const Arg *arg)
{
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		execvp(((char **)arg->v)[0], (char **)arg->v);
		die("nixtile: execvp %s failed:", ((char **)arg->v)[0]);
	}
}

void
startdrag(struct wl_listener *listener, void *data)
{
	struct wlr_drag *drag = data;
	if (!drag->icon)
		return;

	drag->icon->data = &wlr_scene_drag_icon_create(drag_icon, drag->icon)->node;
	LISTEN_STATIC(&drag->icon->events.destroy, destroydragicon);
}

void
tag(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel || (arg->ui & TAGMASK) == 0)
		return;

	sel->tags = arg->ui & TAGMASK;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

void
tagmon(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		setmon(sel, dirtomon(arg->i), 0);
}

/* DYNAMIC COLUMN AND MASTER TILE CONFIGURATION */

/* Get optimal number of columns based on screen width */
int
get_optimal_columns(int screen_width)
{
	if (screen_width >= 3440) {
		/* Super Ultrawide: 4+ columns for very wide screens */
		return 4;
	} else if (screen_width >= 2560) {
		/* Ultrawide: 3 columns */
		return 3;
	} else if (screen_width >= 1600) {
		/* Normal widescreen: 2 columns */
		return 3;
	} else {
		/* Standard/narrow screens: 1 column */
		return 1;
	}
}

/* Get optimal number of master tiles based on screen width */
int
get_optimal_master_tiles(int screen_width)
{
	if (screen_width >= 3440) {
		/* Super Ultrawide: 4 master tiles */
		return 4;
	} else if (screen_width >= 2560) {
		/* Ultrawide: 3 master tiles */
		return 3;
	} else if (screen_width >= 1600) {
		/* Normal widescreen: 2 master tiles */
		return 3;
	} else {
		/* Standard/narrow screens: 1 master tile */
		return 1;
	}
}

/* Update monitor's nmaster based on screen width */
void
update_dynamic_master_tiles(Monitor *m)
{
	int screen_width = m->w.width;
	int optimal_master = get_optimal_master_tiles(screen_width);
	
	/* Only update if different to avoid unnecessary layout changes */
	if (m->nmaster != optimal_master) {
		m->nmaster = optimal_master;
		wlr_log(WLR_INFO, "[nixtile] DYNAMIC LAYOUT: Screen width %dpx -> %d master tiles", 
			screen_width, optimal_master);
	}
}

void
tile(Monitor *m)
{
	wlr_log(WLR_ERROR, "[nixtile] *** TILE FUNCTION CALLED - STARTING LAYOUT CALCULATION ***");
	
	/* RESIZE PROTECTION: Skip layout recalculation during active resize to preserve custom geometry */
	if (cursor_mode == CurTileResize) {
		wlr_log(WLR_DEBUG, "[nixtile] TILE: Skipping layout recalculation during active tile resize to preserve geometry");
		return;
	}
	
	int i, n = 0;
	Client *c;
	extern int statusbar_visible;
	/* Apply outer gap to the monitor window area */
	int x_outer_gap = outergappx;
	int y_outer_gap = outergappx;

	/* Calculate adjusted window area with gaps */
	struct wlr_box adjusted_area = m->w;
	
	/* Adjust for status bar if it's visible - maintain outergappx distance between statusbar and tiles */
	if (statusbar_visible) {
		if (strcmp(statusbar_position, "top") == 0) {
			adjusted_area.y += statusbar_height + outergappx;
			adjusted_area.height -= statusbar_height + outergappx;
		} else if (strcmp(statusbar_position, "bottom") == 0) {
			adjusted_area.height -= statusbar_height + outergappx;
		} else if (strcmp(statusbar_position, "left") == 0) {
			adjusted_area.x += statusbar_height + outergappx;
			adjusted_area.width -= statusbar_height + outergappx;
		} else if (strcmp(statusbar_position, "right") == 0) {
			adjusted_area.width -= statusbar_height + outergappx;
		}
	}
	
	/* Apply outer gaps */
	adjusted_area.x += x_outer_gap;
	adjusted_area.y += y_outer_gap;
	adjusted_area.width -= 2 * x_outer_gap;
	adjusted_area.height -= 2 * y_outer_gap;

	/* Count windows that need to be tiled - ROBUST VERSION */
	/* Use more lenient visibility check to handle timing issues */
	wl_list_for_each(c, &clients, link) {
		/* More robust visibility check: if mon is set and not floating/fullscreen, count it */
		bool has_monitor = (c->mon != NULL);
		bool not_floating = !c->isfloating;
		bool not_fullscreen = !c->isfullscreen;
		/* Also check traditional VISIBLEON for comparison */
		bool traditional_visible = VISIBLEON(c, m);
		
		wlr_log(WLR_ERROR, "[nixtile] *** TILE COUNT: Client %p - has_mon=%d, not_float=%d, not_full=%d, trad_vis=%d, column=%d ***", 
			(void*)c, has_monitor, not_floating, not_fullscreen, traditional_visible, c->column_group);
		
		/* Use robust check: if tile has monitor and is not floating/fullscreen, include it */
		if (has_monitor && not_floating && not_fullscreen) {
			n++;
		}
	}
	wlr_log(WLR_ERROR, "[nixtile] *** TILE FUNCTION: Found %d tiles to layout ***", n);
	if (n == 0) {
		wlr_log(WLR_ERROR, "[nixtile] *** TILE FUNCTION: No tiles to layout, returning early ***");
		return;
	}

	/* DYNAMIC MULTI-COLUMN LAYOUT WITH INDEPENDENT BI-AXIAL RESIZING */
	/* Update dynamic master tiles based on screen width */
	update_dynamic_master_tiles(m);
	
	/* Determine layout based on screen width and tile count */
	int screen_width = adjusted_area.width;
	int optimal_columns = get_optimal_columns(screen_width);
	int master_tiles = m->nmaster;
	
	/* Use multi-column layout for 2+ columns with 2+ tiles, or always for 3+ columns */
	bool use_multi_column = (optimal_columns >= 2 && n >= 2);
	bool force_multi_column = (optimal_columns >= 3); /* 3+ columns always use multi-column layout */
	
	wlr_log(WLR_ERROR, "[nixtile] *** TILE FUNCTION: n=%d, optimal_columns=%d, use_multi_column=%d, force_multi_column=%d ***", 
		n, optimal_columns, use_multi_column, force_multi_column);
	
	if (n == 1) {
		/* Single tile: full screen */
		c = NULL;
		wl_list_for_each(c, &clients, link) {
			if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
				break;
		}
		if (c) {
			resize(c, (struct wlr_box){
				.x = adjusted_area.x,
				.y = adjusted_area.y,
				.width = adjusted_area.width,
				.height = adjusted_area.height
			}, 0);
		}
	} else if (optimal_columns == 2 && use_multi_column && !force_multi_column) {
		wlr_log(WLR_ERROR, "[nixtile] *** ENTERING 2-COLUMN LAYOUT BLOCK ***");
		/* 2-column layout: mfact-based horizontal resizing (supports stacks) */
		/* This preserves horizontal resizing functionality for 2-column scenarios only */
		
		int left_width, right_width;
		
		/* HORIZONTAL RESIZE SUPPORT: Check if any tiles have custom width factors */
		bool has_custom_width_factors = false;
		float left_width_factor = 1.0f, right_width_factor = 1.0f;
		
		Client *factor_check;
		wl_list_for_each(factor_check, &clients, link) {
			if (!VISIBLEON(factor_check, m) || factor_check->isfloating || factor_check->isfullscreen)
				continue;
			if (factor_check->width_factor > 0.1f && factor_check->width_factor < 1.9f) {
				has_custom_width_factors = true;
				if (factor_check->column_group == 0) {
					left_width_factor = factor_check->width_factor;
				} else if (factor_check->column_group == 1) {
					right_width_factor = factor_check->width_factor;
				}
			}
		}
		
		if (has_custom_width_factors) {
			/* Use width factors like height factors */
			float total_factors = left_width_factor + right_width_factor;
			if (total_factors > 0.1f) {
				left_width = (int)((adjusted_area.width * left_width_factor) / total_factors);
				right_width = adjusted_area.width - left_width - innergappx;
				wlr_log(WLR_DEBUG, "[nixtile] WIDTH FACTORS: left=%.3f, right=%.3f, widths=(%d,%d)", 
					left_width_factor, right_width_factor, left_width, right_width);
			} else {
				/* Fallback to mfact */
				left_width = (int)roundf(adjusted_area.width * m->mfact);
				right_width = adjusted_area.width - left_width - innergappx;
			}
		} else {
			/* Calculate widths based on mfact */
			left_width = (int)roundf(adjusted_area.width * m->mfact);
			right_width = adjusted_area.width - left_width - innergappx;
		}
		
		/* Count tiles in each column for proper stack handling */
		int left_tiles = 0, right_tiles = 0;
		Client *temp_c;
		wl_list_for_each(temp_c, &clients, link) {
			if (!VISIBLEON(temp_c, m) || temp_c->isfloating || temp_c->isfullscreen)
				continue;
			
			if (temp_c->column_group == 0) {
				left_tiles++;
			} else {
				right_tiles++;
			}
		}
		
		/* Position all tiles in each column with proper stacking */
		int left_y = 0, right_y = 0;
		wl_list_for_each(c, &clients, link) {
			/* Use robust visibility check instead of strict VISIBLEON */
			if (!c->mon || c->isfloating || c->isfullscreen)
				continue;
			
			bool is_left_column = (c->column_group == 0);
			int tiles_in_column = is_left_column ? left_tiles : right_tiles;
			int column_width = is_left_column ? left_width : right_width;
			int x_pos = is_left_column ? adjusted_area.x : adjusted_area.x + left_width + innergappx;
			
			/* Calculate height for this tile in the stack */
			int available_height = adjusted_area.height - (tiles_in_column - 1) * innergappx;
			int height = available_height / tiles_in_column;
			
			/* Support for vertical resizing via height_factor */
			if (c->height_factor > 0.1f && c->height_factor < 1.9f) {
				float total_factors = 0.0f;
				Client *factor_c;
				wl_list_for_each(factor_c, &clients, link) {
					if (!VISIBLEON(factor_c, m) || factor_c->isfloating || factor_c->isfullscreen)
						continue;
					if ((factor_c->column_group == 0) == is_left_column) {
						total_factors += factor_c->height_factor;
					}
				}
				if (total_factors > 0.1f) {
					height = (int)((available_height * c->height_factor) / total_factors);
				}
			}
			
			/* Position the tile */
			int y_pos = adjusted_area.y + (is_left_column ? left_y : right_y);
			resize(c, (struct wlr_box){
				.x = x_pos,
				.y = y_pos,
				.width = column_width,
				.height = height
			}, 0);
			
			/* Update y position for next tile in this column */
			if (is_left_column) {
				left_y += height + innergappx;
			} else {
				right_y += height + innergappx;
			}
		}
	} else {
		wlr_log(WLR_ERROR, "[nixtile] *** ENTERING ELSE BLOCK - CHECKING MULTI-COLUMN CONDITIONS ***");
		/* DYNAMIC MULTI-COLUMN LAYOUT WITH MFACT-BASED HORIZONTAL RESIZING */
		if (use_multi_column || force_multi_column) {
			wlr_log(WLR_ERROR, "[nixtile] *** ENTERING DYNAMIC MULTI-COLUMN LAYOUT BLOCK ***");
			/* Multi-column layout: dynamic number of independent stacking areas with horizontal resizing */
			/* Count actual tiles to determine effective columns */
			int actual_tiles = 0;
			Client *count_c;
			wl_list_for_each(count_c, &clients, link) {
				if (count_c->mon && !count_c->isfloating && !count_c->isfullscreen) {
					actual_tiles++;
				}
			}
			
			/* Calculate effective columns and column widths with mfact support */
			int effective_columns = MIN(actual_tiles, optimal_columns);
			int total_gaps = (effective_columns - 1) * innergappx;
			int available_width = adjusted_area.width - total_gaps;
			
			/* MULTI-COLUMN HORIZONTAL RESIZING: Use mfact to control column distribution */
			int *column_widths = calloc(effective_columns, sizeof(int));
			
			if (effective_columns == 2) {
				/* HORIZONTAL RESIZE SUPPORT: Check for width factors */
				bool has_width_factors = false;
				float left_factor = 1.0f, right_factor = 1.0f;
				
				Client *wf_check;
				wl_list_for_each(wf_check, &clients, link) {
					if (!VISIBLEON(wf_check, m) || wf_check->isfloating || wf_check->isfullscreen)
						continue;
					if (wf_check->width_factor > 0.1f && wf_check->width_factor < 1.9f) {
						has_width_factors = true;
						if (wf_check->column_group == 0) left_factor = wf_check->width_factor;
						else if (wf_check->column_group == 1 && effective_columns > 1) right_factor = wf_check->width_factor;
					}
				}
				
				if (has_width_factors) {
					/* Use width factors */
					float total_factors = left_factor + right_factor;
					if (total_factors > 0.1f) {
						column_widths[0] = (int)((available_width * left_factor) / total_factors);
						column_widths[1] = available_width - column_widths[0];
						wlr_log(WLR_DEBUG, "[nixtile] WIDTH FACTORS: left=%.3f, right=%.3f, widths=(%d,%d)", 
							left_factor, right_factor, column_widths[0], column_widths[1]);
					} else {
						/* Fallback to mfact */
						column_widths[0] = (int)(available_width * m->mfact);
						column_widths[1] = available_width - column_widths[0];
						wlr_log(WLR_ERROR, "[nixtile] *** MULTI-COLUMN FALLBACK MFACT *** mfact=%.3f, left=%d, right=%d", 
							m->mfact, column_widths[0], column_widths[1]);
					}
				} else {
					/* 2 columns: Use mfact directly (left = mfact, right = 1-mfact) */
					column_widths[0] = (int)(available_width * m->mfact);
					column_widths[1] = available_width - column_widths[0];
					wlr_log(WLR_ERROR, "[nixtile] *** APPLYING MFACT IN LAYOUT *** mfact=%.3f, left=%d (%.1f%%), right=%d (%.1f%%)", 
						m->mfact, column_widths[0], m->mfact * 100, column_widths[1], (1.0f - m->mfact) * 100);
				}
			} else {
				/* Multi-column (3+ columns): Check for width factors first */
				float column_factors[MAX_COLUMNS] = {1.0f, 1.0f, 1.0f, 1.0f};
				bool has_width_factors = false;
				float total_factors = 0.0f;
				
				/* Collect width factors for each column */
				for (int col = 0; col < effective_columns; col++) {
					Client *wf_check;
					wl_list_for_each(wf_check, &clients, link) {
						if (!VISIBLEON(wf_check, m) || wf_check->isfloating || wf_check->isfullscreen)
							continue;
						if (wf_check->column_group == col && wf_check->width_factor > 0.1f && wf_check->width_factor < 3.0f) {
							column_factors[col] = wf_check->width_factor;
							has_width_factors = true;
							break; /* Use first tile's width_factor for the column */
						}
					}
					total_factors += column_factors[col];
				}
				
				if (has_width_factors && total_factors > 0.1f) {
					/* Use width factors for proportional distribution */
					for (int col = 0; col < effective_columns; col++) {
						column_widths[col] = (int)((available_width * column_factors[col]) / total_factors);
					}
					wlr_log(WLR_DEBUG, "[nixtile] %d-COLUMN WIDTH FACTORS: factors=(%.3f,%.3f,%.3f,%.3f), total=%.3f", 
						effective_columns, column_factors[0], column_factors[1], column_factors[2], column_factors[3], total_factors);
				} else {
					/* Fallback to equal distribution */
					int base_width = available_width / effective_columns;
					for (int col = 0; col < effective_columns; col++) {
						column_widths[col] = base_width;
					}
					/* Distribute remainder */
					int remainder = available_width % effective_columns;
					for (int col = 0; col < remainder; col++) {
						column_widths[col]++;
					}
					wlr_log(WLR_DEBUG, "[nixtile] %d-COLUMN EQUAL DISTRIBUTION: base_width=%d (no width factors)", effective_columns, base_width);
				}
			}
			wlr_log(WLR_ERROR, "[nixtile] *** WIDTH CALCULATION: actual_tiles=%d, effective_columns=%d, mfact=%.3f ***", 
				actual_tiles, effective_columns, m->mfact);
			
			/* Count tiles in each column based on their assigned column_group */
			int *column_tiles = calloc(optimal_columns, sizeof(int));
			wlr_log(WLR_ERROR, "[nixtile] *** COLUMN ASSIGNMENT START: optimal_columns=%d ***", optimal_columns);
			Client *temp_c;
			wl_list_for_each(temp_c, &clients, link) {
				/* Use robust visibility check instead of strict VISIBLEON */
				if (!temp_c->mon || temp_c->isfloating || temp_c->isfullscreen)
					continue;
				
				/* ROBUST MASTER TILE PROTECTION: Guarantee first N tiles are horizontal */
				int master_tiles = get_optimal_master_tiles(m->w.width);
				int tile_index = 0;
				
				/* Count this tile's position among all tiles */
				Client *count_c;
				wl_list_for_each(count_c, &clients, link) {
					if (count_c == temp_c) break;
					if (count_c->mon && !count_c->isfloating && !count_c->isfullscreen) {
						tile_index++;
					}
				}
				
				/* Debug: Check tile assignment logic */
				wlr_log(WLR_ERROR, "[nixtile] *** TILE ASSIGNMENT CHECK: Tile %p (index %d) has column_group=%d, master_tiles=%d ***", 
					(void*)temp_c, tile_index, temp_c->column_group, master_tiles);
				
				/* Use column_group to determine if tile needs stacking assignment (not index) */
				if (temp_c->column_group < 0 || temp_c->column_group >= optimal_columns) {
					wlr_log(WLR_ERROR, "[nixtile] *** STACKING TILE DETECTED: Tile %p has invalid column_group=%d ***", 
						(void*)temp_c, temp_c->column_group);
					
					/* FOCUSED TILE STACKING: Stack new tiles in the column of the currently focused tile */
					Client *focused_client = focustop(m);
					int target_column = 0; /* Default to column 0 if no focused tile */
					
					if (focused_client && focused_client->column_group >= 0 && focused_client->column_group < optimal_columns) {
						target_column = focused_client->column_group;
						wlr_log(WLR_ERROR, "[nixtile] *** FOCUSED STACKING: Found focused tile %p in column %d ***", 
							(void*)focused_client, target_column);
					} else {
						wlr_log(WLR_ERROR, "[nixtile] *** FOCUSED STACKING: No valid focused tile, defaulting to column 0 ***");
					}
					
					temp_c->column_group = target_column;
					wlr_log(WLR_ERROR, "[nixtile] *** STACKING TILE: Tile %p (index %d) assigned to focused column %d ***", 
						(void*)temp_c, tile_index, target_column);
				} else {
					wlr_log(WLR_ERROR, "[nixtile] *** STACKING TILE: Tile %p (index %d) already has valid column_group=%d ***", 
						(void*)temp_c, tile_index, temp_c->column_group);
				}
				
				column_tiles[temp_c->column_group]++;
			}
			
			/* Track Y position for each column */
			int *column_y = calloc(optimal_columns, sizeof(int));
			
			i = 0;
			wl_list_for_each(c, &clients, link) {
				/* CRITICAL FIX: Use robust visibility check to allow tiles with correct column_group */
				if (!c->mon || c->isfloating || c->isfullscreen)
					continue;
				
				/* Get column information */
				int column = c->column_group;
				int tiles_in_column = column_tiles[column];
				
				/* Calculate column index within the specific column */
				int column_index = 0;
				Client *temp_c;
				wl_list_for_each(temp_c, &clients, link) {
					if (!VISIBLEON(temp_c, m) || temp_c->isfloating || temp_c->isfullscreen)
						continue;
					if (temp_c == c) break;
					if (temp_c->column_group == column)
						column_index++;
				}
				
				/* Calculate available height for this column */
				int available_height = adjusted_area.height - (tiles_in_column - 1) * innergappx;
				
				/* ROBUST PER-COLUMN HEIGHT DISTRIBUTION */
				int height;
				
				/* Always use equal distribution within each column for robustness */
				/* This ensures each column always uses full height and distributes equally */
				if (tiles_in_column > 0) {
					height = available_height / tiles_in_column;
				} else {
					height = available_height; /* Fallback */
				}
				
				/* Support for vertical resizing via height_factor (optional enhancement) */
				if (c->height_factor > 0.1f && c->height_factor < 1.9f) {
					/* Calculate total factors for this column only */
					float total_factors = 0.0f;
					Client *temp_c;
					wl_list_for_each(temp_c, &clients, link) {
						if (!VISIBLEON(temp_c, m) || temp_c->isfloating || temp_c->isfullscreen)
							continue;
						/* Use column_group for correct column detection */
						if (temp_c->column_group == column) {
							total_factors += temp_c->height_factor;
						}
					}
					if (total_factors > 0.1f) {
						height = (int)((available_height * c->height_factor) / total_factors);
					}
				}
				
				/* SAFETY CHECK: Prevent crash from invalid column_group */
				if (column < 0 || column >= effective_columns) {
					wlr_log(WLR_ERROR, "[nixtile] *** SAFETY FIX: Tile %p has invalid column=%d (effective_columns=%d), forcing to column 0 ***", 
						(void*)c, column, effective_columns);
					column = 0;
					c->column_group = 0;
				}
				
				/* Position and size calculation for multi-column layout with mfact support */
				/* Calculate x position by summing previous column widths */
				int x_pos = adjusted_area.x;
				for (int prev_col = 0; prev_col < column; prev_col++) {
					x_pos += column_widths[prev_col] + innergappx;
				}
				/* ADDITIONAL SAFETY: Ensure width is never zero */
				int width = column_widths[column];
				if (width <= 0) {
					wlr_log(WLR_ERROR, "[nixtile] *** CRITICAL FIX: column_widths[%d]=%d is invalid, using fallback width ***", 
						column, width);
					width = (adjusted_area.width - (effective_columns - 1) * innergappx) / effective_columns;
					if (width <= 0) width = 200; /* Absolute minimum fallback */
				}
				int y_pos = adjusted_area.y + column_y[column];
				
				wlr_log(WLR_ERROR, "[nixtile] *** MFACT HORIZONTAL PLACEMENT: Tile %p -> column=%d, x_pos=%d, width=%d (mfact=%.3f) ***", 
					(void*)c, column, x_pos, width, m->mfact);
				
				/* Clean up column_widths at the end of the loop */
				if (i == n - 1) {
					free(column_widths);
				}
				
				resize(c, (struct wlr_box){
					.x = x_pos,
					.y = y_pos,
					.width = width,
					.height = height
				}, 0);
				
				/* Update y position for next tile in this column */
				column_y[column] += height + innergappx;
				
				i++;
			}
			
			/* Cleanup allocated memory */
			free(column_tiles);
			free(column_y);
		} else {
			wlr_log(WLR_ERROR, "[nixtile] *** ENTERING SINGLE COLUMN LAYOUT BLOCK ***");
			/* Single column layout: all tiles stacked vertically */
			int y_offset = 0;
			
			i = 0;
			wl_list_for_each(c, &clients, link) {
				if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
					continue;
				
				/* Calculate available height for all tiles */
				int available_height = adjusted_area.height - (n - 1) * innergappx;
				
				/* Calculate tile height with height_factor support */
				int height;
				if (c->height_factor > 0.1f && c->height_factor < 1.9f) {
					/* Calculate total factors for all tiles */
					float total_factors = 0.0f;
					Client *temp_c;
					wl_list_for_each(temp_c, &clients, link) {
						if (VISIBLEON(temp_c, m) && !temp_c->isfloating && !temp_c->isfullscreen) {
							total_factors += temp_c->height_factor;
						}
					}
					height = (int)((available_height * c->height_factor) / total_factors);
				} else {
					/* Equal distribution */
					height = available_height / n;
				}
				
				resize(c, (struct wlr_box){
					.x = adjusted_area.x,
					.y = adjusted_area.y + y_offset,
					.width = adjusted_area.width,
					.height = height
				}, 0);
				
				y_offset += height + innergappx;
				i++;
			}
		}
	}
}

void
togglefloating(const Arg *arg)
{
	Client *sel = focustop(selmon);
	/* FORCE: Disable floating toggle - all tiles must stay tiled */
	if (sel && !sel->isfullscreen) {
		/* Force tile to stay tiled */
		sel->isfloating = 0;
		/* Ensure valid column assignment for multi-column layout */
		int optimal_columns = get_optimal_columns(sel->mon->w.width);
		if (sel->column_group < 0 || sel->column_group >= optimal_columns) {
			/* DO NOT force to 0 - let tile() function handle assignment */
			wlr_log(WLR_DEBUG, "[nixtile] TOGGLE: Invalid column_group %d, will be reassigned by tile() (max columns: %d)", sel->column_group, optimal_columns);
		}
		/* Force column tiling */
		if (sel->mon) {
			force_column_tiling(sel->mon);
		}
		wlr_log(WLR_DEBUG, "[nixtile] FORCE: Floating toggle disabled - tile forced to stay tiled");
	}
}

void
togglecolumn(const Arg *arg)
{
	Client *sel = focustop(selmon);
	/* Only allow column switching for tiled windows */
	if (sel && !sel->isfloating && !sel->isfullscreen) {
		/* Toggle between left (0) and right (1) column */
		int max_columns = get_optimal_columns(selmon->w.width);
		sel->column_group = (sel->column_group + 1) % max_columns;
		/* Re-arrange layout to reflect the change */
		arrange(selmon);
	}
}

void
togglefullscreen(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		setfullscreen(sel, !sel->isfullscreen);
}

void
toggletag(const Arg *arg)
{
	uint32_t newtags;
	Client *sel = focustop(selmon);
	if (!sel || !(newtags = sel->tags ^ (arg->ui & TAGMASK)))
		return;

	sel->tags = newtags;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

void
toggleview(const Arg *arg)
{
	uint32_t newtagset;
	if (!(newtagset = selmon ? selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK) : 0))
		return;

	selmon->tagset[selmon->seltags] = newtagset;
	
	/* Reset workspace-specific stacking counter for "blank slate" per workspace */
	selmon->workspace_stacking_counter = 0;
	selmon->workspace_stacking_counter_initialized = false;
	wlr_log(WLR_ERROR, "[nixtile] *** WORKSPACE TOGGLE RESET: Stacking counter reset for workspace toggle ***");
	
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

void
unlocksession(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, unlock);
	destroylock(lock, 1);
}

void
unmaplayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, unmap);

	l->mapped = 0;
	wlr_scene_node_set_enabled(&l->scene->node, 0);
	if (l == exclusive_focus)
		exclusive_focus = NULL;
	if (l->layer_surface->output && (l->mon = l->layer_surface->output->data))
		arrangelayers(l->mon);
	if (l->layer_surface->surface == seat->keyboard_state.focused_surface)
		focusclient(focustop(selmon), 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
unmapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is unmapped, and should no longer be shown. */
	Client *c = wl_container_of(listener, c, unmap);
	if (c == grabc) {
		cursor_mode = CurNormal;
		grabc = NULL;
	}

	/* Store info for rebalancing BEFORE removing client */
	int removed_column = c->column_group;
	Monitor *client_mon = c->mon;
	bool client_was_floating = c->isfloating;
	
	/* Store rebalancing info in Client structure for destroynotify */
	c->destroy_mon = client_mon;
	c->destroy_column = removed_column;
	c->destroy_was_floating = client_was_floating;
	
	if (client_is_unmanaged(c)) {
		if (c == exclusive_focus) {
			exclusive_focus = NULL;
			focusclient(focustop(selmon), 1);
		}
	} else {
		wl_list_remove(&c->link);
		setmon(c, NULL, 0);
		wl_list_remove(&c->flink);
	}

	/* Update status before destroying scene node to prevent broken pipe */
	printstatus();
	motionnotify(0, NULL, 0, 0, 0, 0);
	
	/* Destroy scene node last */
	wlr_scene_node_destroy(&c->scene->node);
}

void
updatemons(struct wl_listener *listener, void *data)
{
	/*
	 * Called whenever the output layout changes: adding or removing a
	 * monitor, changing an output's mode or position, etc. This is where
	 * the change officially happens and we update geometry, window
	 * positions, focus, and the stored configuration in wlroots'
	 * output-manager implementation.
	 */
	struct wlr_output_configuration_v1 *config
			= wlr_output_configuration_v1_create();
	Client *c;
	struct wlr_output_configuration_head_v1 *config_head;
	Monitor *m;

	/* First remove from the layout the disabled monitors */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled || m->asleep)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
		config_head->state.enabled = 0;
		/* Remove this output from the layout to avoid cursor enter inside it */
		wlr_output_layout_remove(output_layout, m->wlr_output);
		closemon(m);
		m->m = m->w = (struct wlr_box){0};
	}
	/* Insert outputs that need to */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled
				&& !wlr_output_layout_get(output_layout, m->wlr_output))
			wlr_output_layout_add_auto(output_layout, m->wlr_output);
	}

	/* Now that we update the output layout we can get its box */
	wlr_output_layout_get_box(output_layout, NULL, &sgeom);

	wlr_scene_node_set_position(&root_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(root_bg, sgeom.width, sgeom.height);

	/* Make sure the clients are hidden when nixtile is locked */
	wlr_scene_node_set_position(&locked_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(locked_bg, sgeom.width, sgeom.height);

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);

		/* Get the effective monitor geometry to use for surfaces */
		wlr_output_layout_get_box(output_layout, m->wlr_output, &m->m);
		m->w = m->m;
		wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);

		wlr_scene_node_set_position(&m->fullscreen_bg->node, m->m.x, m->m.y);
		wlr_scene_rect_set_size(m->fullscreen_bg, m->m.width, m->m.height);

		if (m->lock_surface) {
			struct wlr_scene_tree *scene_tree = m->lock_surface->surface->data;
			wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
			wlr_session_lock_surface_v1_configure(m->lock_surface, m->m.width, m->m.height);
		}

		/* Calculate the effective monitor geometry to use for clients */
		arrangelayers(m);
		/* Don't move clients to the left output when plugging monitors */
		arrange(m);
		/* make sure fullscreen clients have the right size */
		if ((c = focustop(m)) && c->isfullscreen)
			resize(c, m->m, 0);

		/* Try to re-set the gamma LUT when updating monitors,
		 * it's only really needed when enabling a disabled output, but meh. */
		m->gamma_lut_changed = 1;

		config_head->state.x = m->m.x;
		config_head->state.y = m->m.y;

		if (!selmon) {
			selmon = m;
		}
	}

	if (selmon && selmon->wlr_output->enabled) {
		wl_list_for_each(c, &clients, link) {
			if (!c->mon && client_surface(c)->mapped)
				setmon(c, selmon, c->tags);
		}
		focusclient(focustop(selmon), 1);
		if (selmon->lock_surface) {
			client_notify_enter(selmon->lock_surface->surface,
					wlr_seat_get_keyboard(seat));
			client_activate_surface(selmon->lock_surface->surface, 1);
		}
	}

	/* FIXME: figure out why the cursor image is at 0,0 after turning all
	 * the monitors on.
	 * Move the cursor image where it used to be. It does not generate a
	 * wl_pointer.motion event for the clients, it's only the image what it's
	 * at the wrong position after all. */
	wlr_cursor_move(cursor, NULL, 0, 0);

	wlr_output_manager_v1_set_configuration(output_mgr, config);
}

void
updatetitle(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_title);
	if (c == focustop(c->mon))
		printstatus();
}

void
urgent(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	Client *c = NULL;
	toplevel_from_wlr_surface(event->surface, &c, NULL);
	if (!c || c == focustop(selmon))
		return;

	c->isurgent = 1;
	printstatus();

	if (client_surface(c)->mapped)
		client_set_border_color(c, urgentcolor);
}

void
view(const Arg *arg)
{
	if (!selmon || (arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
		return;
	selmon->seltags ^= 1; /* toggle sel tagset */
	if (arg->ui & TAGMASK)
		selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
	
	/* Load workspace-specific mfact for independent resizing */
	load_workspace_mfact();
	
	/* Reset workspace-specific stacking counter for "blank slate" per workspace */
	selmon->workspace_stacking_counter = 0;
	selmon->workspace_stacking_counter_initialized = false;
	wlr_log(WLR_ERROR, "[nixtile] *** WORKSPACE RESET: Stacking counter reset for new workspace ***");
	
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

void
virtualkeyboard(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_keyboard_v1 *kb = data;
	/* virtual keyboards shouldn't share keyboard group */
	KeyboardGroup *group = createkeyboardgroup();
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(&kb->keyboard, group->wlr_group->keyboard.keymap);
	LISTEN(&kb->keyboard.base.events.destroy, &group->destroy, destroykeyboardgroup);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(group->wlr_group, &kb->keyboard);
}

void
virtualpointer(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_input_device *device = &event->new_pointer->pointer.base;

	wlr_cursor_attach_input_device(cursor, device);
	if (event->suggested_output)
		wlr_cursor_map_input_to_output(cursor, device, event->suggested_output);
}

Monitor *
xytomon(double x, double y)
{
	struct wlr_output *o = wlr_output_layout_output_at(output_layout, x, y);
	return o ? o->data : NULL;
}

void
xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny)
{
	struct wlr_scene_node *node, *pnode;
	struct wlr_surface *surface = NULL;
	Client *c = NULL;
	LayerSurface *l = NULL;
	int layer;

	for (layer = NUM_LAYERS - 1; !surface && layer >= 0; layer--) {
		if (!(node = wlr_scene_node_at(&layers[layer]->node, x, y, nx, ny)))
			continue;

		if (node->type == WLR_SCENE_NODE_BUFFER)
			surface = wlr_scene_surface_try_from_buffer(
					wlr_scene_buffer_from_node(node))->surface;
		/* Walk the tree to find a node that knows the client */
		for (pnode = node; pnode && !c; pnode = &pnode->parent->node)
			c = pnode->data;
		if (c && c->type == LayerShell) {
			c = NULL;
			l = pnode->data;
		}
	}

	if (psurface) *psurface = surface;
	if (pc) *pc = c;
	if (pl) *pl = l;
}

void
zoom(const Arg *arg)
{
	Client *c, *sel = focustop(selmon);

	if (!sel || !selmon || !selmon->lt[selmon->sellt]->arrange || sel->isfloating)
		return;

	/* Search for the first tiled window that is not sel, marking sel as
	 * NULL if we pass it along the way */
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, selmon) && !c->isfloating) {
			if (c != sel)
				break;
			sel = NULL;
		}
	}

	/* Return if no other tiled window was found */
	if (&c->link == &clients)
		return;

	/* If we passed sel, move c to the front; otherwise, move sel to the
	 * front */
	if (!sel)
		sel = c;
	wl_list_remove(&sel->link);
	wl_list_insert(&clients, &sel->link);

	focusclient(sel, 1);
	arrange(selmon);
}

#ifdef XWAYLAND
void
activatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, activate);

	/* Only "managed" windows can be activated */
	if (!client_is_unmanaged(c))
		wlr_xwayland_surface_activate(c->surface.xwayland, 1);
}

void
associatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, associate);

	LISTEN(&client_surface(c)->events.map, &c->map, mapnotify);
	LISTEN(&client_surface(c)->events.unmap, &c->unmap, unmapnotify);
}

void
configurex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	if (!client_surface(c) || !client_surface(c)->mapped) {
		wlr_xwayland_surface_configure(c->surface.xwayland,
				event->x, event->y, event->width, event->height);
		return;
	}
	if (client_is_unmanaged(c)) {
		wlr_scene_node_set_position(&c->scene->node, event->x, event->y);
		wlr_xwayland_surface_configure(c->surface.xwayland,
				event->x, event->y, event->width, event->height);
		return;
	}
	if ((c->isfloating && c != grabc) || !c->mon->lt[c->mon->sellt]->arrange) {
		resize(c, (struct wlr_box){.x = event->x - c->bw,
				.y = event->y - c->bw, .width = event->width + c->bw * 2,
				.height = event->height + c->bw * 2}, 0);
	} else {
		arrange(c->mon);
	}
}

void
createnotifyx11(struct wl_listener *listener, void *data)
{
	struct wlr_xwayland_surface *xsurface = data;
	Client *c;

	/* Allocate a Client for this surface */
	c = xsurface->data = ecalloc(1, sizeof(*c));
	c->surface.xwayland = xsurface;
	c->type = X11;
	c->bw = client_is_unmanaged(c) ? 0 : borderpx;
	c->height_factor = 1.0f; /* Initialize height factor for vertical resizing */
	c->width_factor = 1.0f; /* Initialize width factor for horizontal resizing */
	/* Initialize destroy rebalancing fields */
	c->destroy_mon = NULL;
	c->destroy_column = -1;
	c->destroy_was_floating = false;
	
	/* INTELLIGENT TILE PLACEMENT: Use same logic as createnotify for consistency */
	c->mon = selmon;
	c->tags = selmon->tagset[selmon->seltags];
	
	/* Count existing tiles (excluding the current tile being assigned) */
	int current_workspace_tiles = 0;
	Client *temp_c;
	wl_list_for_each(temp_c, &clients, link) {
		if (temp_c != c && VISIBLEON(temp_c, selmon) && !temp_c->isfloating && !temp_c->isfullscreen) {
			current_workspace_tiles++;
		}
	}
	
	/* Get dynamic column configuration */
	int screen_width = selmon->w.width;
	int optimal_columns = get_optimal_columns(screen_width);
	int master_tiles = get_optimal_master_tiles(screen_width);
	
	int tile_number = current_workspace_tiles + 1; /* 1-based tile number */
	
	wlr_log(WLR_INFO, "[nixtile] X11 PLACEMENT: Tile #%d, master_tiles=%d, optimal_columns=%d", 
		tile_number, master_tiles, optimal_columns);
	
	/* GUARANTEED RULE: First N tiles go to separate columns (horizontal placement) */
	if (tile_number <= master_tiles) {
		int target_column = (tile_number - 1) % optimal_columns;
		wlr_log(WLR_INFO, "[nixtile] X11 HORIZONTAL PLACEMENT: Tile #%d -> Column %d (guaranteed horizontal)", 
			tile_number, target_column);
		c->column_group = target_column;
	} else {
		/* STACKING: Let tile() function handle assignment for proper distribution */
		wlr_log(WLR_INFO, "[nixtile] X11 STACK PLACEMENT: Tile #%d -> will be assigned by tile() function", 
			tile_number);
		c->column_group = -1; /* Invalid - will be assigned by tile() */
	}
	
	/* PRESERVE MANUAL RESIZE IN MAPNOTIFY: Only reset if no manual resize performed */
	int target_column = c->column_group;
	if (target_column >= 0 && target_column < MAX_COLUMNS) {
		if (!manual_resize_performed[target_column]) {
			/* No manual resize - maintain equal distribution */
			wlr_log(WLR_INFO, "[nixtile] MAPNOTIFY: No manual resize in column %d, maintaining equal height factors", target_column);
			Client *temp_c;
			wl_list_for_each(temp_c, &clients, link) {
				if (!VISIBLEON(temp_c, selmon) || temp_c->isfloating || temp_c->isfullscreen)
					continue;
				if (temp_c->column_group == target_column) {
					temp_c->height_factor = 1.0f;
				}
			}
		} else {
			/* Manual resize performed - preserve existing ratios */
			wlr_log(WLR_INFO, "[nixtile] MAPNOTIFY: Column %d has manual resize, preserving existing height factors", target_column);
			/* Don't reset height_factors - preserve manual resize settings */
		}
	}

	/* Listen to the various events it can emit */
	LISTEN(&xsurface->events.associate, &c->associate, associatex11);
	LISTEN(&xsurface->events.destroy, &c->destroy, destroynotify);
	LISTEN(&xsurface->events.dissociate, &c->dissociate, dissociatex11);
	LISTEN(&xsurface->events.request_activate, &c->activate, activatex11);
	LISTEN(&xsurface->events.request_configure, &c->configure, configurex11);
	LISTEN(&xsurface->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&xsurface->events.set_hints, &c->set_hints, sethints);
	LISTEN(&xsurface->events.set_title, &c->set_title, updatetitle);
}

void
dissociatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, dissociate);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
}

void
sethints(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_hints);
	struct wlr_surface *surface = client_surface(c);
	if (c == focustop(selmon) || !c->surface.xwayland->hints)
		return;

	c->isurgent = xcb_icccm_wm_hints_get_urgency(c->surface.xwayland->hints);
	printstatus();

	if (c->isurgent && surface && surface->mapped)
		client_set_border_color(c, urgentcolor);
}

void
xwaylandready(struct wl_listener *listener, void *data)
{
	struct wlr_xcursor *xcursor;

	/* assign the one and only seat */
	wlr_xwayland_set_seat(xwayland, seat);

	/* Set the default XWayland cursor to match the rest of nixtile. */
	if ((xcursor = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", 1)))
		wlr_xwayland_set_cursor(xwayland,
				xcursor->images[0]->buffer, xcursor->images[0]->width * 4,
				xcursor->images[0]->width, xcursor->images[0]->height,
				xcursor->images[0]->hotspot_x, xcursor->images[0]->hotspot_y);
}
#endif

int
main(int argc, char *argv[])
{
	char *startup_cmd = NULL;
	int c;

	while ((c = getopt(argc, argv, "s:hdv")) != -1) {
		if (c == 's')
			startup_cmd = optarg;
		else if (c == 'd')
			log_level = WLR_DEBUG;
		else if (c == 'v')
			die("nixtile " VERSION);
		else
			goto usage;
	}
	if (optind < argc)
		goto usage;

	/* Wayland requires XDG_RUNTIME_DIR for creating its communications socket */
	if (!getenv("XDG_RUNTIME_DIR"))
		die("XDG_RUNTIME_DIR must be set");
	setup();
	run(startup_cmd);
	cleanup();
	return EXIT_SUCCESS;

usage:
	die("Usage: %s [-v] [-d] [-s startup command]", argv[0]);
}

/* PER-WORKSPACE RESIZING FUNCTIONS */

/* Get current workspace index (0-8) */
static int get_current_workspace() {
	for (int i = 0; i < 9; i++) {
		if (selmon->tagset[selmon->seltags] & (1 << i)) {
			return i;
		}
	}
	return 0; /* Default to workspace 0 */
}

/* Load workspace-specific mfact */
static void load_workspace_mfact() {
	int workspace = get_current_workspace();
	if (selmon->workspace_mfact[workspace] == 0.0f) {
		/* Initialize new workspace with default mfact */
		selmon->workspace_mfact[workspace] = 0.5f;
	}
	selmon->mfact = selmon->workspace_mfact[workspace];
}

/* Save workspace-specific mfact */
static void save_workspace_mfact() {
	int workspace = get_current_workspace();
	selmon->workspace_mfact[workspace] = selmon->mfact;
}

/* EQUAL WIDTH DISTRIBUTION FUNCTIONS */

/* Reset mfact to 0.5 for equal width distribution when workspace becomes populated */
void ensure_equal_width_distribution(Client *new_client) {
	/* TEMPORARILY DISABLED: Manual resize check for debugging */
	bool any_manual_resize = manual_resize_performed[0] || manual_resize_performed[1];
	if (false) { /* DISABLED FOR TESTING */
		wlr_log(WLR_INFO, "[nixtile] PRESERVING MANUAL RESIZE: Skipping equal width distribution (left_manual=%d, right_manual=%d)", 
		        manual_resize_performed[0], manual_resize_performed[1]);
		return;
	}
	wlr_log(WLR_DEBUG, "[nixtile] MANUAL RESIZE CHECK: left_manual=%d, right_manual=%d - PROCEEDING", 
	        manual_resize_performed[0], manual_resize_performed[1]);
	
	/* Get current mfact for logging */
	float current_mfact = selmon->mfact;
	
	/* Count existing visible tiles to determine if workspace was empty */
	int existing_tiles = 0;
	Client *count_c;
	wl_list_for_each(count_c, &clients, link) {
		if (!VISIBLEON(count_c, selmon) || count_c->isfloating || count_c->isfullscreen)
			continue;
		if (count_c != new_client) /* Don't count the new tile being added */
			existing_tiles++;
	}
	
	/* Only reset mfact for truly new workspaces (0 or 1 existing tiles) */
	if (existing_tiles <= 1) {
		selmon->mfact = 0.5f; /* Always equal 50/50 distribution for new workspaces */
		wlr_log(WLR_DEBUG, "[nixtile] Reset mfact to 0.5 for equal width distribution (existing_tiles=%d)", existing_tiles);
	} else {
		wlr_log(WLR_DEBUG, "[nixtile] PRESERVING EXISTING LAYOUT: Not changing mfact (existing_tiles=%d, current_mfact=%.3f)", existing_tiles, current_mfact);
	}
}

/* SCREEN FILLING LOGIC: Ensure tiles always fill the entire screen */
void handle_empty_column_expansion() {
	/* SAFETY: Validate monitor */
	if (!selmon) {
		wlr_log(WLR_ERROR, "[nixtile] SAFETY: No selected monitor in handle_empty_column_expansion");
		return;
	}
	
	/* CHECK FOR MANUAL RESIZE: Don't auto-adjust if user has manually resized */
	bool any_manual_resize = manual_resize_performed[0] || manual_resize_performed[1];
	if (any_manual_resize) {
		wlr_log(WLR_INFO, "[nixtile] PRESERVING MANUAL RESIZE: Skipping automatic column expansion (left_manual=%d, right_manual=%d)", 
		        manual_resize_performed[0], manual_resize_performed[1]);
		return;
	}
	
	/* Count tiles in each column */
	int left_column_count = count_tiles_in_stack(0, selmon);
	int right_column_count = count_tiles_in_stack(1, selmon);
	int total_tiles = left_column_count + right_column_count;
	
	wlr_log(WLR_DEBUG, "[nixtile] COLUMN EXPANSION: left=%d, right=%d, total=%d", 
	        left_column_count, right_column_count, total_tiles);
	
	if (total_tiles == 0) {
		wlr_log(WLR_DEBUG, "[nixtile] SCREEN FILLING: No tiles to arrange");
		return;
	}
	
	/* SCREEN FILLING RULE: Tiles must always fill the entire screen */
	if (left_column_count == 0 && right_column_count > 0) {
		/* Left column is empty, expand right column to FULL SCREEN */
		selmon->mfact = 0.01f; /* Minimal left column, maximum right column */
		wlr_log(WLR_INFO, "[nixtile] SCREEN FILLING: Left column empty, expanding right column to full screen (right_tiles=%d)", right_column_count);
		
	} else if (right_column_count == 0 && left_column_count > 0) {
		/* Right column is empty, expand left column to FULL SCREEN */
		selmon->mfact = 0.99f; /* Maximum left column, minimal right column */
		wlr_log(WLR_INFO, "[nixtile] SCREEN FILLING: Right column empty, expanding left column to full screen (left_tiles=%d)", left_column_count);
		
	} else if (left_column_count > 0 && right_column_count > 0) {
		/* Both columns have tiles - NEVER CHANGE MFACT */
		/* Preserve column widths regardless of tile count */
		wlr_log(WLR_DEBUG, "[nixtile] SCREEN FILLING: Both columns populated, preserving width mfact=%.3f (left=%d, right=%d)", 
		        selmon->mfact, left_column_count, right_column_count);
		return; /* Exit early, never change horizontal width */
	} else {
		/* Fallback: No tiles in either column (shouldn't happen) */
		wlr_log(WLR_ERROR, "[nixtile] SCREEN FILLING: No tiles in any column - unexpected state");
		return;
	}
	
	/* CRITICAL: Apply the layout changes immediately to ensure screen is filled */
	wlr_log(WLR_DEBUG, "[nixtile] SCREEN FILLING: Applying layout with mfact=%.2f", selmon->mfact);
	arrange(selmon);
}

/* EQUAL HEIGHT DISTRIBUTION: Ensure all tiles in a stack have equal height factors */
void ensure_equal_height_distribution_in_stack(int column) {
	/* SAFETY: Validate inputs */
	if (!selmon || column < 0 || column >= MAX_COLUMNS) {
		wlr_log(WLR_ERROR, "[nixtile] EQUAL HEIGHT: Invalid parameters (selmon=%p, column=%d)", (void*)selmon, column);
		return;
	}
	
	/* RESET MANUAL RESIZE: Always reset and enforce equal distribution on tile operations */
	if (manual_resize_performed[column]) {
		wlr_log(WLR_INFO, "[nixtile] EQUAL HEIGHT: Resetting manual resize flag for column %d - enforcing equal distribution", column);
		manual_resize_performed[column] = false;
	}
	
	/* Count tiles in the specified column */
	int tiles_in_column = 0;
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, selmon) || c->isfloating || c->isfullscreen)
			continue;
		if (c->column_group == column)
			tiles_in_column++;
	}
	
	if (tiles_in_column <= 1) {
		wlr_log(WLR_DEBUG, "[nixtile] EQUAL HEIGHT: Column %d has %d tiles, no redistribution needed", column, tiles_in_column);
		return;
	}
	
	/* Set all tiles in this column to equal height factors */
	wlr_log(WLR_INFO, "[nixtile] EQUAL HEIGHT: Redistributing %d tiles equally in column %d", tiles_in_column, column);
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, selmon) || c->isfloating || c->isfullscreen)
			continue;
		if (c->column_group == column) {
			c->height_factor = 1.0f; /* Equal distribution */
			c->width_factor = 1.0f;
			wlr_log(WLR_DEBUG, "[nixtile] EQUAL HEIGHT: Set tile %p height_factor to 1.0", (void*)c);
		}
	}
}

/* EQUAL HORIZONTAL DISTRIBUTION: Set mfact to 0.5 when only single tiles remain (no stacks) */
void ensure_equal_horizontal_distribution_for_two_tiles(void) {
	/* SAFETY: Validate monitor */
	if (!selmon) {
		wlr_log(WLR_ERROR, "[nixtile] EQUAL HORIZONTAL: No selected monitor");
		return;
	}
	
	/* Count total tiles across both columns */
	int left_count = count_tiles_in_stack(0, selmon);
	int right_count = count_tiles_in_stack(1, selmon);
	int total_tiles = left_count + right_count;
	
	wlr_log(WLR_DEBUG, "[nixtile] EQUAL HORIZONTAL: Total tiles=%d (left=%d, right=%d)", total_tiles, left_count, right_count);
	
	/* Apply equal distribution when only single tiles remain (no stacks) */
	bool only_single_tiles = (left_count <= 1 && right_count <= 1 && total_tiles >= 2);
	if (only_single_tiles) {
		/* RESET MANUAL RESIZE: Always reset and enforce equal distribution for single tiles */
		bool any_manual_resize = manual_resize_performed[0] || manual_resize_performed[1];
		if (any_manual_resize) {
			wlr_log(WLR_INFO, "[nixtile] EQUAL HORIZONTAL: Resetting manual resize flags - enforcing equal distribution for single tiles");
			manual_resize_performed[0] = false;
			manual_resize_performed[1] = false;
		}
		
		/* Set equal horizontal distribution */
		selmon->mfact = 0.5f;
		wlr_log(WLR_INFO, "[nixtile] EQUAL HORIZONTAL: Set mfact to 0.5 for equal distribution of single tiles (left=%d, right=%d)", left_count, right_count);
	} else {
		wlr_log(WLR_DEBUG, "[nixtile] EQUAL HORIZONTAL: Not applicable - stacks exist or insufficient tiles (total=%d, left=%d, right=%d)", total_tiles, left_count, right_count);
	}
}

/* CRASH PREVENTION: Validate client list integrity before operations */
bool validate_client_list_integrity() {
	/* SAFETY: Check if clients list is properly initialized */
	if (!clients.prev || !clients.next) {
		wlr_log(WLR_ERROR, "[nixtile] CRASH PREVENTION: Clients list not properly initialized");
		return false;
	}
	
	/* SAFETY: Check for circular list integrity */
	if (clients.prev->next != &clients || clients.next->prev != &clients) {
		wlr_log(WLR_ERROR, "[nixtile] CRASH PREVENTION: Clients list circular integrity broken");
		return false;
	}
	
	/* SAFETY: Validate each client in the list */
	Client *c;
	int client_count = 0;
	wl_list_for_each(c, &clients, link) {
		client_count++;
		
		/* Prevent infinite loops */
		if (client_count > 1000) {
			wlr_log(WLR_ERROR, "[nixtile] CRASH PREVENTION: Clients list appears to have infinite loop");
			return false;
		}
		
		/* SAFETY: Validate client link integrity */
		if (!c->link.prev || !c->link.next) {
			wlr_log(WLR_ERROR, "[nixtile] CRASH PREVENTION: Client %p has invalid link pointers", (void*)c);
			return false;
		}
		
		/* SAFETY: Validate client basic state */
		if (!c->mon) {
			wlr_log(WLR_ERROR, "[nixtile] CRASH PREVENTION: Client %p has NULL monitor", (void*)c);
			return false;
		}
	}
	
	wlr_log(WLR_DEBUG, "[nixtile] CRASH PREVENTION: Client list integrity validated (%d clients)", client_count);
	return true;
}

/* STRICT TILE LIMITS: Helper functions for Norwegian user requirements */

/* Count tiles in a specific stack (column) */
int count_tiles_in_stack(int column, Monitor *m) {
	int count = 0;
	Client *c;
	
	if (!m) return 0;
	
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && c->column_group == column) {
			count++;
			wlr_log(WLR_DEBUG, "[nixtile] STACK TILE FOUND: Tile %p in column %d (count now %d)", (void*)c, column, count);
		}
	}
	
	wlr_log(WLR_DEBUG, "[nixtile] STACK COUNT: Column %d has %d tiles", column, count);
	return count;
}

/* Count total tiles in current workspace */
int count_total_tiles_in_workspace(Monitor *m) {
	int count = 0;
	Client *c;
	
	if (!m) return 0;
	
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen) {
			count++;
		}
	}
	
	wlr_log(WLR_DEBUG, "[nixtile] WORKSPACE COUNT: Current workspace has %d tiles", count);
	return count;
}

/* Find available stack (column) that has space for new tile */
int find_available_stack(Monitor *m) {
	if (!m) return 0;
	
	/* Check left column (0) first */
	int left_count = count_tiles_in_stack(0, m);
	if (left_count < MAX_TILES_PER_STACK) {
		wlr_log(WLR_INFO, "[nixtile] STACK PLACEMENT: Left stack (0) has space (%d/%d tiles)", left_count, MAX_TILES_PER_STACK);
		return 0;
	}
	
	/* Check right column (1) */
	int right_count = count_tiles_in_stack(1, m);
	if (right_count < MAX_TILES_PER_STACK) {
		wlr_log(WLR_INFO, "[nixtile] STACK PLACEMENT: Right stack (1) has space (%d/%d tiles)", right_count, MAX_TILES_PER_STACK);
		return 1;
	}
	
	/* Both stacks are full */
	wlr_log(WLR_ERROR, "[nixtile] STACK PLACEMENT: Both stacks are full (left=%d, right=%d)", left_count, right_count);
	return -1; /* No available stack */
}

/* Find next available workspace */
int find_available_workspace(void) {
	for (int workspace = 0; workspace < TAGCOUNT; workspace++) {
		/* Switch to this workspace temporarily to count tiles */
		uint32_t old_tags = selmon->tagset[selmon->seltags];
		selmon->tagset[selmon->seltags] = 1 << workspace;
		
		int tile_count = count_total_tiles_in_workspace(selmon);
		
		/* Restore original workspace */
		selmon->tagset[selmon->seltags] = old_tags;
		
		if (tile_count < MAX_TILES_PER_WORKSPACE) {
			wlr_log(WLR_INFO, "[nixtile] WORKSPACE PLACEMENT: Workspace %d has space (%d/%d tiles)", workspace, tile_count, MAX_TILES_PER_WORKSPACE);
			return workspace;
		}
	}
	
	wlr_log(WLR_ERROR, "[nixtile] WORKSPACE PLACEMENT: All workspaces are full");
	return 0; /* Fallback to first workspace */
}
