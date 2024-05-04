#include <X11/X.h>
#include <X11/extensions/Xrender.h>

#include "types.h"

char const title[] = "xpaint";

u32 const MAX_COLORS = 9;
u32 const TCS_NUM = 3;
char const FONT_NAME[] = "monospace:size=10";
// lag prevention. only one drag event per period will be done
u32 const DRAG_PERIOD_US = 10000;
i32 const PNG_SAVE_COMPRESSION = 4;

XRenderColor const SCHEMES[SchmLast][2] = {
    // fg, bg (rgba premultiplied)
    [SchmNorm] =
        {{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}, {0x1818, 0x1818, 0x1818, 0xFFFF}},
    [SchmFocus] = {{0xFFFF, 0, 0, 0xFFFF}, {0x9999, 0x1818, 0x1818, 0xFFFF}},
};

struct {
    u32 background_argb;
    Pair max_launch_size;
} const WINDOW = {
    .background_argb = 0xFF181818,
    .max_launch_size = {1000, 1000},
};

struct {
    u32 padding_bottom;
} const STATUSLINE = {
    .padding_bottom = 4,
};

struct {
    u32 outer_r_px;
    u32 inner_r_px;
    u32 line_w;
    i32 line_style;
    i32 cap_style;
    i32 join_style;
} const SELECTION_CIRCLE = {
    .outer_r_px = 225,
    .inner_r_px = 40,
    .line_w = 2,
    .line_style = LineSolid,
    .cap_style = CapNotLast,
    .join_style = JoinMiter,
};

struct {
    u32 line_w;
    i32 line_style;
    i32 cap_style;
    i32 join_style;
    u32 drag_argb;
} const SELECTION_TOOL = {
    .line_w = 2,
    .line_style = LineOnOffDash,
    .cap_style = CapNotLast,
    .join_style = JoinMiter,
    .drag_argb = 0xFFFF0000,
};

struct {
    u32 default_line_w;
} const TOOLS = {
    .default_line_w = 5,
};

struct {
    u32 background_argb;
    u32 default_width;
    u32 default_height;
    u32 max_zoom;
} const CANVAS = {
    .background_argb = 0xFFAA0000,
    .default_width = 500,
    .default_height = 800,
    .max_zoom = 10,
};
