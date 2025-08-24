/* Taken from https://github.com/djpohly/dwl/issues/466 */
#define COLOR(hex)    { ((hex >> 24) & 0xFF) / 255.0f, \
                        ((hex >> 16) & 0xFF) / 255.0f, \
                        ((hex >> 8) & 0xFF) / 255.0f, \
                        (hex & 0xFF) / 255.0f }
/* appearance */
static const int sloppyfocus               = 1;  /* focus follows mouse */
static const int bypass_surface_visibility = 0;  /* 1 means idle inhibitors will disable idle tracking even if it's surface isn't visible  */
static const unsigned int borderpx         = 1;  /* border pixel of windows */
static const unsigned int innergappx       = 4;  /* inner gap in pixels between windows */
static const unsigned int outergappx       = 4;  /* outer gap in pixels between windows and screen edge/statusbar */
static const unsigned int statusbar_side_gap = 4;  /* gap between statusbar and screen sides */
static const unsigned int statusbar_top_gap  = 4;  /* gap between statusbar and screen top */
static const float rootcolor[]             = COLOR(0x222222ff);
static const float bordercolor[]           = COLOR(0x444444ff);
static const float focuscolor[]            = COLOR(0x005577ff);
static const float urgentcolor[]           = COLOR(0xff0000ff);
/* Status bar configuration */
static const char *statusbar_position      = "top";   /* "top", "bottom", "left", "right" */
static const unsigned int statusbar_height = 30;      /* Height in pixels (for top/bottom), width for left/right) */
static const float statusbar_alpha         = 0.4f;    /* Transparency (0.0 = fully transparent, 1.0 = opaque) */
static const float statusbar_color[]       = COLOR(0x222222ff); /* Default bar background color */
/* This conforms to the xdg-protocol. Set the alpha to zero to restore the old behavior */
static const float fullscreen_bg[]         = {0.1f, 0.1f, 0.1f, 1.0f}; /* You can also use glsl colors */

/* tagging - TAGCOUNT must be no greater than 31 */
#define TAGCOUNT (9)

/* logging */
static int log_level = WLR_ERROR;

/* NOTE: ALWAYS keep a rule declared even if you don't use rules (e.g leave at least one example) */
static const Rule rules[] = {
	/* app_id             title       tags mask     isfloating   monitor */
	/* examples: */
	{ "Gimp_EXAMPLE",     NULL,       0,            1,           -1 }, /* Start on currently visible tags floating, not tiled */
	{ "firefox_EXAMPLE",  NULL,       1 << 8,       0,           -1 }, /* Start on ONLY tag "9" */
};

/* layout(s) */
static const Layout layouts[] = {
	/* symbol     arrange function */
	{ "[]=",      tile },
	{ "><>",      NULL },    /* no layout function means floating behavior */
	{ "[M]",      monocle },
};

/* monitors */
/* (x=-1, y=-1) is reserved as an "autoconfigure" monitor position indicator
 * WARNING: negative values other than (-1, -1) cause problems with Xwayland clients
 * https://gitlab.freedesktop.org/xorg/xserver/-/issues/899
*/
/* NOTE: ALWAYS add a fallback rule, even if you are completely sure it won't be used */
static const MonitorRule monrules[] = {
	/* name       mfact  nmaster scale layout       rotate/reflect                x    y */
	/* example of a HiDPI laptop monitor:
	{ "eDP-1",    0.5f,  1,      2,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1 },
	*/
	/* defaults */
	{ NULL,       0.5f,  1,      1,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1 },
};

/* keyboard */
static const struct xkb_rule_names xkb_rules = {
	/* can specify fields: rules, model, layout, variant, options */
	/* example:
	.options = "ctrl:nocaps",
	*/
	.options = NULL,
};

static const int repeat_rate = 25;
static const int repeat_delay = 600;

/* Trackpad */
static const int tap_to_click = 1;
static const int tap_and_drag = 1;
static const int drag_lock = 1;
static const int natural_scrolling = 0;
static const int disable_while_typing = 1;
static const int left_handed = 0;
static const int middle_button_emulation = 0;
/* You can choose between:
LIBINPUT_CONFIG_SCROLL_NO_SCROLL
LIBINPUT_CONFIG_SCROLL_2FG
LIBINPUT_CONFIG_SCROLL_EDGE
LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN
*/
static const enum libinput_config_scroll_method scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;

/* You can choose between:
LIBINPUT_CONFIG_CLICK_METHOD_NONE
LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS
LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER
*/
static const enum libinput_config_click_method click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;

/* You can choose between:
LIBINPUT_CONFIG_SEND_EVENTS_ENABLED
LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE
*/
static const uint32_t send_events_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;

/* You can choose between:
LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE
*/
static const enum libinput_config_accel_profile accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
static const double accel_speed = 0.0;

/* You can choose between:
LIBINPUT_CONFIG_TAP_MAP_LRM -- 1/2/3 finger tap maps to left/right/middle
LIBINPUT_CONFIG_TAP_MAP_LMR -- 1/2/3 finger tap maps to left/middle/right
*/
static const enum libinput_config_tap_button_map button_map = LIBINPUT_CONFIG_TAP_MAP_LRM;

/* If you want to use the windows key for MODKEY, use WLR_MODIFIER_LOGO */
#define MODKEY WLR_MODIFIER_LOGO

#define TAGKEYS(KEY,SKEY,TAG) \
	{ MODKEY,                    KEY,            view,            {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_CTRL,  KEY,            toggleview,      {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_SHIFT, SKEY,           tag,             {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,SKEY,toggletag, {.ui = 1 << TAG} }

/* helper for spawning shell commands in the pre dwm-5.0 fashion */
#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

/* commands */
static const char *termcmd[] = { "kitty", NULL };
static const char *filebrowsercmd[] = { "thunar", NULL };



void toggle_statusbar(const Arg *arg);
void show_launcher(const Arg *arg);

static const Key keys[] = {
    { .mod = MODKEY, .keysym = XKB_KEY_q, .func = killclient, .arg = { .i = 0 } },
    { .mod = MODKEY, .keysym = XKB_KEY_b, .func = toggle_statusbar, .arg = { .i = 0 } },
    { .mod = MODKEY, .keysym = XKB_KEY_p, .func = show_launcher, .arg = { .i = 0 } },
    { .mod = MODKEY, .keysym = XKB_KEY_Return, .func = spawn, .arg = { .v = termcmd } },
    { .mod = MODKEY, .keysym = XKB_KEY_e, .func = spawn, .arg = { .v = filebrowsercmd } },
    { .mod = MODKEY, .keysym = XKB_KEY_f, .func = togglefullscreen, .arg = { .i = 0 } },

    { .mod = MODKEY, .keysym = XKB_KEY_j, .func = focusstack, .arg = { .i = +1 } },
    { .mod = MODKEY, .keysym = XKB_KEY_k, .func = focusstack, .arg = { .i = -1 } },
    { .mod = MODKEY, .keysym = XKB_KEY_i, .func = incnmaster, .arg = { .i = +1 } },
    { .mod = MODKEY, .keysym = XKB_KEY_d, .func = incnmaster, .arg = { .i = -1 } },
    { .mod = MODKEY, .keysym = XKB_KEY_h, .func = setmfact, .arg = { .f = -0.05f } },
    { .mod = MODKEY, .keysym = XKB_KEY_l, .func = setmfact, .arg = { .f = +0.05f } },
    { .mod = MODKEY, .keysym = XKB_KEY_Tab, .func = view, .arg = { .i = 0 } },
    { .mod = MODKEY, .keysym = XKB_KEY_t, .func = setlayout, .arg = { .v = &layouts[0] } },
    { .mod = MODKEY, .keysym = XKB_KEY_f, .func = setlayout, .arg = { .v = &layouts[1] } },
    { .mod = MODKEY, .keysym = XKB_KEY_m, .func = setlayout, .arg = { .v = &layouts[2] } },
    { .mod = MODKEY, .keysym = XKB_KEY_space, .func = setlayout, .arg = { .i = 0 } },
    { .mod = MODKEY, .keysym = XKB_KEY_0, .func = view, .arg = { .ui = ~0 } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_parenright, .func = tag, .arg = { .ui = ~0 } },
    { .mod = MODKEY, .keysym = XKB_KEY_comma, .func = focusmon, .arg = { .i = WLR_DIRECTION_LEFT } },
    { .mod = MODKEY, .keysym = XKB_KEY_period, .func = focusmon, .arg = { .i = WLR_DIRECTION_RIGHT } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_less, .func = tagmon, .arg = { .i = WLR_DIRECTION_LEFT } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_greater, .func = tagmon, .arg = { .i = WLR_DIRECTION_RIGHT } },
    { .mod = MODKEY, .keysym = XKB_KEY_1, .func = view, .arg = { .ui = 1 << 0 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL, .keysym = XKB_KEY_1, .func = toggleview, .arg = { .ui = 1 << 0 } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_exclam, .func = tag, .arg = { .ui = 1 << 0 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_exclam, .func = toggletag, .arg = { .ui = 1 << 0 } },
    { .mod = MODKEY, .keysym = XKB_KEY_2, .func = view, .arg = { .ui = 1 << 1 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL, .keysym = XKB_KEY_2, .func = toggleview, .arg = { .ui = 1 << 1 } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_at, .func = tag, .arg = { .ui = 1 << 1 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_at, .func = toggletag, .arg = { .ui = 1 << 1 } },
    { .mod = MODKEY, .keysym = XKB_KEY_3, .func = view, .arg = { .ui = 1 << 2 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL, .keysym = XKB_KEY_3, .func = toggleview, .arg = { .ui = 1 << 2 } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_numbersign, .func = tag, .arg = { .ui = 1 << 2 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_numbersign, .func = toggletag, .arg = { .ui = 1 << 2 } },
    { .mod = MODKEY, .keysym = XKB_KEY_4, .func = view, .arg = { .ui = 1 << 3 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL, .keysym = XKB_KEY_4, .func = toggleview, .arg = { .ui = 1 << 3 } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_dollar, .func = tag, .arg = { .ui = 1 << 3 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_dollar, .func = toggletag, .arg = { .ui = 1 << 3 } },
    { .mod = MODKEY, .keysym = XKB_KEY_5, .func = view, .arg = { .ui = 1 << 4 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL, .keysym = XKB_KEY_5, .func = toggleview, .arg = { .ui = 1 << 4 } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_percent, .func = tag, .arg = { .ui = 1 << 4 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_percent, .func = toggletag, .arg = { .ui = 1 << 4 } },
    { .mod = MODKEY, .keysym = XKB_KEY_6, .func = view, .arg = { .ui = 1 << 5 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL, .keysym = XKB_KEY_6, .func = toggleview, .arg = { .ui = 1 << 5 } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_asciicircum, .func = tag, .arg = { .ui = 1 << 5 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_asciicircum, .func = toggletag, .arg = { .ui = 1 << 5 } },
    { .mod = MODKEY,                   .keysym = XKB_KEY_7, .func = view,       .arg = { .ui = 1 << 6 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL, .keysym = XKB_KEY_7, .func = toggleview, .arg = { .ui = 1 << 6 } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_ampersand, .func = tag, .arg = { .ui = 1 << 6 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_ampersand, .func = toggletag, .arg = { .ui = 1 << 6 } },
    { .mod = MODKEY,                   .keysym = XKB_KEY_8, .func = view,       .arg = { .ui = 1 << 7 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL, .keysym = XKB_KEY_8, .func = toggleview, .arg = { .ui = 1 << 7 } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_asterisk, .func = tag, .arg = { .ui = 1 << 7 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_asterisk, .func = toggletag, .arg = { .ui = 1 << 7 } },
    { .mod = MODKEY, .keysym = XKB_KEY_9, .func = view, .arg = { .ui = 1 << 8 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL, .keysym = XKB_KEY_9, .func = toggleview, .arg = { .ui = 1 << 8 } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_parenleft, .func = tag, .arg = { .ui = 1 << 8 } },
    { .mod = MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_parenleft, .func = toggletag, .arg = { .ui = 1 << 8 } },
    { .mod = MODKEY|WLR_MODIFIER_SHIFT, .keysym = XKB_KEY_Q, .func = quit, .arg = { .i = 0 } },

	/* Ctrl-Alt-Backspace and Ctrl-Alt-Fx used to be handled by X server */
	{ .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_Terminate_Server, .func = quit, .arg = { .i = 0 } },
	/* Ctrl-Alt-Fx is used to switch to another VT, if you don't know what a VT is
	 * do not remove them.
	 */
    { .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_XF86Switch_VT_1,  .func = chvt, .arg = { .ui = 1 } },
    { .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_XF86Switch_VT_2,  .func = chvt, .arg = { .ui = 2 } },
    { .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_XF86Switch_VT_3,  .func = chvt, .arg = { .ui = 3 } },
    { .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_XF86Switch_VT_4,  .func = chvt, .arg = { .ui = 4 } },
    { .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_XF86Switch_VT_5,  .func = chvt, .arg = { .ui = 5 } },
    { .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_XF86Switch_VT_6,  .func = chvt, .arg = { .ui = 6 } },
    { .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_XF86Switch_VT_7,  .func = chvt, .arg = { .ui = 7 } },
    { .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_XF86Switch_VT_8,  .func = chvt, .arg = { .ui = 8 } },
    { .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_XF86Switch_VT_9,  .func = chvt, .arg = { .ui = 9 } },
    { .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_XF86Switch_VT_10, .func = chvt, .arg = { .ui = 10 } },
    { .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_XF86Switch_VT_11, .func = chvt, .arg = { .ui = 11 } },
    { .mod = WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, .keysym = XKB_KEY_XF86Switch_VT_12, .func = chvt, .arg = { .ui = 12 } },
};

static const Button buttons[] = {
	{ MODKEY, BTN_LEFT,   moveresize,     {.ui = CurMove} },
	{ MODKEY, BTN_MIDDLE, togglefloating, {0} },
	{ MODKEY, BTN_RIGHT,  tileresize,     {0} },
};

/* DEBUG: Print button constants */
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#pragma message "BTN_LEFT = " TOSTRING(BTN_LEFT)
#pragma message "BTN_MIDDLE = " TOSTRING(BTN_MIDDLE) 
#pragma message "BTN_RIGHT = " TOSTRING(BTN_RIGHT)




