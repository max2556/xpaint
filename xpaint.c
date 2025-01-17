#include <X11/X.h>
#include <X11/Xatom.h>  // XA_*
#include <X11/Xft/Xft.h>
#include <X11/Xft/XftCompat.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xdbe.h>  // back buffer
#include <X11/extensions/Xrender.h>
#include <X11/extensions/render.h>
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/unistd.h>

// libs
#define INCBIN_PREFIX
#define INCBIN_STYLE INCBIN_STYLE_SNAKE
#include "lib/incbin.h"
#define STB_DS_IMPLEMENTATION
#include "lib/stb_ds.h"
#undef STB_DS_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "lib/stb_image.h"
#undef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "lib/stb_image_write.h"
#undef STB_IMAGE_WRITE_IMPLEMENTATION

#include "config.h"
#include "types.h"

// embeded data
INCBIN(u8, pic_tool_fill, "res/tool-fill.png");
INCBIN(u8, pic_tool_pencil, "res/tool-pencil.png");
INCBIN(u8, pic_tool_picker, "res/tool-picker.png");
INCBIN(u8, pic_tool_select, "res/tool-select.png");
INCBIN(u8, pic_tool_brush, "res/tool-brush.png");
INCBIN(u8, pic_tool_figure, "res/tool-figure.png");
INCBIN(u8, pic_unknown, "res/unknown.png");

/*
 * free -dyn vars with 'free' function
 * free -arr vars with 'arrfree' function
 * free -imdyn vars with 'stbi_image_free' function
 * free -xdyn vars with 'XFree' function
 * structs with t and d fields are tagged unions
 */

#define XLeftMouseBtn    Button1
#define XMiddleMouseBtn  Button2
#define XRightMouseBtn   Button3
#define XMouseScrollUp   Button4
#define XMouseScrollDown Button5
#define MAX(A, B)        ((A) > (B) ? (A) : (B))
#define MIN(A, B)        ((A) < (B) ? (A) : (B))
#define CLAMP(X, L, H)   (((X) < (L)) ? (L) : ((X) > (H)) ? (H) : (X))
#define LENGTH(X)        (sizeof(X) / sizeof(X)[0])
#define BETWEEN(X, A, B) ((A) <= (X) && (X) <= (B))
#define COALESCE(A, B)   ((A) ? (A) : (B))
#define PI               (3.141)
// default value for signed integers
#define NIL              (-1)
#define PNIL             ((Pair) {NIL, NIL})
#define ZOOM_SPEED       (1.2)

#define CURR_TC(p_ctx)     ((p_ctx)->tcarr[(p_ctx)->curr_tc])
// XXX workaround
#define COL_FG(p_dc, p_sc) ((p_dc)->schemes_dyn[(p_sc)].fg.pixel | 0xFF000000)
#define COL_BG(p_dc, p_sc) ((p_dc)->schemes_dyn[(p_sc)].bg.pixel | 0xFF000000)
#define HAS_SELECTION(p_tc) \
    ((p_tc)->t == Tool_Selection && (p_tc)->d.sel.end.x != NIL \
     && (p_tc)->d.sel.end.y != NIL && (p_tc)->d.sel.begin.x != NIL \
     && (p_tc)->d.sel.begin.y != NIL \
     && (p_tc)->d.sel.end.x != (p_tc)->d.sel.begin.x \
     && (p_tc)->d.sel.end.y != (p_tc)->d.sel.begin.y)
#define SELECTION_DRAGGING(p_tc) \
    ((p_tc)->t == Tool_Selection && (p_tc)->d.sel.drag_from.x != NIL \
     && (p_tc)->d.sel.drag_from.y != NIL)
#define UNREACHABLE() __builtin_unreachable()
#define ZOOM_C(dc_p)  (pow(ZOOM_SPEED, (double)(dc_p)->cv.zoom))

enum {
    A_Clipboard,
    A_Targets,
    A_Utf8string,
    A_ImagePng,
    A_Last,
};

enum Icon {
    I_Select,
    I_Pencil,
    I_Fill,
    I_Picker,
    I_Brush,
    I_Figure,
    I_Last,
};

struct IconData {
    u8 const* data;
    usize len;
};

enum ImageType {
    IMT_Png,
    IMT_Jpg,
    IMT_Unknown,
};

struct Ctx;
struct DrawCtx;
struct ToolCtx;

typedef void (*draw_fn)(struct Ctx* ctx, Pair p);

typedef u8 (*circle_get_alpha_fn)(
    struct Ctx* ctx,
    double circle_radius,
    Pair p  // point relative circle center
);

struct Ctx {
    struct DrawCtx {
        Display* dp;
        XVisualInfo vinfo;
        XIM xim;
        XIC xic;
        XRenderPictFormat* xrnd_pic_format;
        Colormap colmap;
        GC gc;
        GC screen_gc;
        Window window;
        u32 width;
        u32 height;
        XdbeBackBuffer back_buffer;  // double buffering
        i32 png_compression_level;  // FIXME find better place
        i32 jpg_quality_level;  // FIXME find better place
        struct Canvas {
            XImage* im;
            enum ImageType type;
            i32 zoom;  // 0 == no zoom
            Pair scroll;
        } cv;
        struct Fnt {
            XftFont* xfont;
            u32 h;
        } fnt;
        struct Scheme {
            XftColor fg;
            XftColor bg;
        }* schemes_dyn;  // must be len of SchmLast
        struct Cache {
            u32 pm_w;  // to validate pm
            u32 pm_h;
            Pixmap pm;  // pixel buffer to update screen
        } cache;
    } dc;

    struct Input {
        Pair prev_c;
        u32 holding_button;
        u64 last_proc_drag_ev_us;
        Bool is_holding;
        Bool is_dragging;
        Pair drag_from;

        enum InputTag {
            InputT_Interact,
            InputT_Color,
            InputT_Console,
        } t;
        union InputData {
            struct InputColorData {
                u32 current_digit;
            } col;
            struct InputConsoleData {
                char* cmdarr;
                char** compls_arr;
                Bool compls_valid;
                usize compls_curr;
            } cl;
        } d;
    } input;

    struct ToolCtx {
        void (*on_press)(struct Ctx*, XButtonPressedEvent const*);
        void (*on_release)(struct Ctx*, XButtonReleasedEvent const*);
        void (*on_drag)(struct Ctx*, XMotionEvent const*);
        void (*on_move)(struct Ctx*, XMotionEvent const*);

        struct ToolSharedData {
            argb* colarr;
            u32 curr_col;
            u32 prev_col;
            u32 line_w;
            Pair anchor;
        } sdata;

        enum ToolTag {
            Tool_Selection,
            Tool_Pencil,
            Tool_Fill,
            Tool_Picker,
            Tool_Brush,
            Tool_Figure,
        } t;
        union ToolData {
            struct SelectionData {
                // selection bounds
                Pair begin, end;
                // NIL if selection not dragging
                Pair drag_from, drag_to;
            } sel;
            // Tool_Pencil | Tool_Brush
            struct DrawerData {
                draw_fn fn;
            } drawer;
            struct FigureData {
                enum FigureType {
                    Figure_Circle,
                    Figure_Rectangle,
                    Figure_Triangle,
                } curr;
                Bool fill;
            } fig;
        } d;
    }* tcarr;
    u32 curr_tc;

    struct History {
        XImage* im;
    } *hist_prevarr, *hist_nextarr;

    struct SelectionCircle {
        Bool is_active;
        i32 x;
        i32 y;
        u32 item_count;
        struct Item {
            void (*on_select)(struct Ctx*);
            enum Icon icon;
        }* items;
    } sc;

    struct SelectionBuffer {
        XImage* im;
    } sel_buf;

    struct FileCtx {
        char* path_dyn;
    } finp, fout;
};

struct ClCommand {
    enum ClCTag {
        ClC_Echo = 0,
        ClC_Set,
        ClC_Exit,
        ClC_Save,
        ClC_Load,
        ClC_Last,
    } t;
    union ClCData {
        struct ClCDSet {
            enum ClCDSTag {
                ClCDS_LineW = 0,
                ClCDS_Col,
                ClCDS_Font,
                ClCDS_FInp,
                ClCDS_FOut,
                ClCDS_PngCompression,
                ClCDS_JpgQuality,
                ClCDS_Last,
            } t;
            union ClCDSData {
                struct ClCDSDLineW {
                    u32 value;
                } line_w;
                struct ClCDSDCol {
                    argb v;
                } col;
                struct ClCDSDFont {
                    char* name_dyn;
                } font;
                struct ClCDSDFInp {
                    char* path_dyn;
                } finp;
                struct ClCDSDFOut {
                    char* path_dyn;
                } fout;
                struct ClCDSDPngCpr {
                    i32 compression;
                } png_cpr;
                struct ClCDSDJpgQlt {
                    i32 quality;
                } jpg_qlt;
            } d;
        } set;
        struct ClCDEcho {
            char* msg_dyn;
        } echo;
        struct ClCDSave {
            enum ClCDSv {
                ClCDSv_Png = 0,
                ClCDSv_Jpg,
                ClCDSv_Last,
            } im_type;
            char* path_dyn;
        } save;
        struct ClCDLoad {
            char* path_dyn;
        } load;
    } d;
};

typedef struct {
    enum {
        ClCPrc_Msg = 0x1,  // wants to show message
        ClCPrc_Exit = 0x2,  // wants to exit application
    } bit_status;
    char* msg_dyn;  // NULL if not PCCR_MSG
} ClCPrcResult;

typedef struct {
    enum {
        ClCPrs_Ok,
        ClCPrs_ENoArg,
        ClCPrs_EInvArg,  // invalid
        ClCPrs_ENoSubArg,
        ClCPrs_EInvSubArg,
    } t;
    union {
        struct ClCommand ok;
        struct {
            char* arg_dyn;
        } invarg;
        struct {
            char* arg_dyn;
        } nosubarg;
        struct {
            char* arg_dyn;
            char* inv_val_dyn;
        } invsubarg;
    } d;
} ClCPrsResult;

// clang-format off
static void die(char const* errstr, ...);
static void trace(char const* fmt, ...);
static void* ecalloc(u32 n, u32 size);
static u32 digit_count(u32 number);
static void arrpoputf8(char const* strarr);
static usize first_dismatch(char const* restrict s1, char const* restrict s2);
static struct IconData get_icon_data(enum Icon icon);
static double brush_ease(double v);

// needs to be 'free'd after use
static char* str_new(char const* fmt, ...);
static char* str_new_va(char const* fmt, va_list args);
static void str_free(char** str_dyn);

static void tc_set_curr_col_num(struct ToolCtx* tc, u32 value);
static argb* tc_curr_col(struct ToolCtx* tc);
static void tc_set_tool(struct ToolCtx* tc, enum ToolTag type);
static char const* tc_get_tool_name(struct ToolCtx const* tc);
static void tc_free(struct ToolCtx* tc);

static Bool fnt_set(struct DrawCtx* dc, char const* font_name);
static void fnt_free(Display* dp, struct Fnt* fnt);
static void file_ctx_set(struct FileCtx* file_ctx, char const* file_path);
static void file_ctx_free(struct FileCtx* file_ctx);

static Pair point_from_cv_to_scr(struct DrawCtx const* dc, Pair p);
static Pair point_from_cv_to_scr_xy(struct DrawCtx const* dc, i32 x, i32 y);
static Pair point_from_cv_to_scr_no_move(struct DrawCtx const* dc, Pair p);
static Pair point_from_scr_to_cv_xy(struct DrawCtx const* dc, i32 x, i32 y);
static Bool point_in_rect(Pair p, Pair a1, Pair a2);

static enum ImageType file_type(char const* file_path);
static u8* ximage_to_rgb(XImage const* image, Bool rgba);
static argb blend_background(argb fg, argb bg, u32 a);
static XImage* read_file_from_memory(struct DrawCtx const* dc, u8 const* data, u32 len, argb bg);
static XImage* read_file_from_path(struct DrawCtx const* dc, char const* file_name, argb bg);
static Bool save_file(struct DrawCtx* dc, enum ImageType type, char const* file_path);

static ClCPrcResult cl_cmd_process(struct Ctx* ctx, struct ClCommand const* cl_cmd);
static ClCPrsResult cl_cmd_parse(struct Ctx* ctx, char const* cl);
static void cl_cmd_parse_res_free(ClCPrsResult* res);
static char* cl_cmd_get_str_dyn(struct InputConsoleData const* d_cl);
static char const* cl_cmd_from_enum(enum ClCTag t);
static char const* cl_set_prop_from_enum(enum ClCDSTag t);
static char const* cl_save_type_from_enum(enum ClCDSv t);
static enum ImageType cl_save_type_to_image_type(enum ClCDSv t);
static void cl_compls_update(struct InputConsoleData* cl);
static void cl_free(struct InputConsoleData* cl);
static void cl_compls_free(struct InputConsoleData* cl);
static void cl_push(struct InputConsoleData* cl, char c);
static void cl_pop(struct InputConsoleData* cl);

static void input_state_set(struct Input* input, enum InputTag is);

static void sel_circ_init(struct Ctx* ctx, i32 x, i32 y);
static void sel_circ_free(struct SelectionCircle* sel_circ);
static i32 sel_circ_curr_item(struct SelectionCircle const* sc, i32 x, i32 y);

// separate functions, because they are callbacks
static void tool_selection_on_press(struct Ctx* ctx, XButtonPressedEvent const* event);
static void tool_selection_on_release(struct Ctx* ctx, XButtonReleasedEvent const* event);
static void tool_selection_on_drag(struct Ctx* ctx, XMotionEvent const* event);
static void tool_drawer_on_press(struct Ctx* ctx, XButtonPressedEvent const* event);
static void tool_drawer_on_release(struct Ctx* ctx, XButtonReleasedEvent const* event);
static void tool_drawer_on_drag(struct Ctx* ctx, XMotionEvent const* event);
static void tool_figure_on_release(struct Ctx* ctx, XButtonReleasedEvent const* event);
static void tool_figure_on_drag(struct Ctx* ctx, XMotionEvent const* event);
static void tool_fill_on_release(struct Ctx* ctx, XButtonReleasedEvent const* event);
static void tool_picker_on_release(struct Ctx* ctx, XButtonReleasedEvent const* event);

static Bool history_move(struct Ctx* ctx, Bool forward);
static void history_push(struct History** hist, struct Ctx* ctx);
static void history_forward(struct Ctx* ctx);
static void history_apply(struct Ctx* ctx, struct History* hist);
static Bool history_restore(struct Ctx* ctx);
static struct History history_clone(struct History const* hist);
static void historyarr_clear(Display* dp, struct History** hist);

static Bool ximage_put_checked(XImage* im, u32 x, u32 y, argb col);
static void canvas_draw_fn_brush(struct Ctx* ctx, Pair c);
static void canvas_draw_fn_pencil(struct Ctx* ctx, Pair c);
static void canvas_figure(struct Ctx* ctx, Pair p1, Pair p2);
static void canvas_fill_rect(struct Ctx* ctx, Pair c, Pair dims, argb col);
static void canvas_rect(struct Ctx* ctx, Pair c, Pair dims, argb col, u32 w);
static void canvas_fill_triangle(struct Ctx* ctx, Pair c, Pair dims, argb col);
static void canvas_triangle(struct Ctx* ctx, Pair c, Pair dims, argb col, u32 w);
static void canvas_line(struct Ctx* ctx, Pair from, Pair to, draw_fn draw);
static void canvas_circle(struct Ctx* ctx, Pair c, u32 d, argb col, circle_get_alpha_fn get_a);
static void canvas_copy_region(struct Ctx* ctx, Pair from, Pair dims, Pair to, Bool clear_source);
static void canvas_fill(struct Ctx* ctx, argb col);
static void canvas_load(struct DrawCtx* dc, XImage* im, char const* file_path); // must be void
static void canvas_free(Display* dp, struct Canvas* cv);
static void canvas_change_zoom(struct DrawCtx* dc, Pair cursor, i32 delta);
static void canvas_resize(struct Ctx* ctx, i32 new_width, i32 new_height);

static u32 get_statusline_height(struct DrawCtx const* dc);
static void draw_string(struct DrawCtx* dc, char const* str, Pair c, enum Schm sc, Bool invert);
static void draw_int(struct DrawCtx* dc, i32 i, Pair c, enum Schm sc, Bool invert);
static int fill_rect(struct DrawCtx* dc, Pair p, Pair dim, argb col);
static int draw_rect(struct DrawCtx* dc, Pair p, Pair dim, argb col, u32 line_w, i32 line_st, i32 cap_st, i32 join_st);
static int draw_line(struct DrawCtx* dc, Pair from, Pair to, enum Schm sc, Bool invert);
static u32 get_int_width(struct DrawCtx const* dc, char const* format, u32 i);
static u32 get_string_width(struct DrawCtx const* dc, char const* str, u32 len);
static void draw_selection_circle(struct DrawCtx* dc, struct SelectionCircle const* sc, i32 pointer_x, i32 pointer_y);
static void update_screen(struct Ctx* ctx);
static void update_statusline(struct Ctx* ctx);
static void show_message(struct Ctx* ctx, char const* msg);
static void show_message_va(struct Ctx* ctx, char const* fmt, ...);

static struct Ctx ctx_init(Display* dp);
static void setup(Display* dp, struct Ctx* ctx);
static void run(struct Ctx* ctx);
static Bool button_press_hdlr(struct Ctx* ctx, XEvent* event);
static Bool button_release_hdlr(struct Ctx* ctx, XEvent* event);
static Bool destroy_notify_hdlr(struct Ctx* ctx, XEvent* event);
static Bool expose_hdlr(struct Ctx* ctx, XEvent* event);
static Bool key_press_hdlr(struct Ctx* ctx, XEvent* event);
static Bool mapping_notify_hdlr(struct Ctx* ctx, XEvent* event);
static Bool motion_notify_hdlr(struct Ctx* ctx, XEvent* event);
static Bool configure_notify_hdlr(struct Ctx* ctx, XEvent* event);
static Bool selection_request_hdlr(struct Ctx* ctx, XEvent* event);
static Bool selection_notify_hdlr(struct Ctx* ctx, XEvent* event);
static Bool client_message_hdlr(struct Ctx* ctx, XEvent* event);
static void cleanup(struct Ctx* ctx);
// clang-format on

static Bool is_verbose_output = False;
static Atom atoms[A_Last];
static XImage* images[I_Last];

static void
main_arg_bound_check(char const* cmd_name, i32 argc, char** argv, u32 pos);

i32 main(i32 argc, char** argv) {
    Display* display = XOpenDisplay(NULL);
    if (!display) {
        die("xpaint: cannot open X display");
    }

    struct Ctx ctx = ctx_init(display);

    for (i32 i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {  // main argument
            file_ctx_set(&ctx.finp, argv[i]);
            file_ctx_set(&ctx.fout, argv[i]);
        } else if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) {
            die("xpaint " VERSION);
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            is_verbose_output = True;
        } else if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--input")) {
            main_arg_bound_check("-i or --input", argc, argv, i);
            file_ctx_set(&ctx.finp, argv[++i]);
        } else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) {
            main_arg_bound_check("-o or --output", argc, argv, i);
            file_ctx_set(&ctx.fout, argv[++i]);
        } else if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--width")) {
            main_arg_bound_check("-w or --width", argc, argv, i);
            // ctx.dc.width == ctx.dc.cv.im->width at program start
            ctx.dc.width = strtol(argv[++i], NULL, 0);
            if (!ctx.dc.width) {
                die("xpaint: canvas width must be positive number");
            }
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--height")) {
            main_arg_bound_check("-h or --height", argc, argv, i);
            // ctx.dc.height == ctx.dc.cv.im->height at program start
            ctx.dc.height = strtol(argv[++i], NULL, 0);
            if (!ctx.dc.height) {
                die("xpaint: canvas height must be positive number");
            }
        } else {
            die("Usage: xpaint [OPTIONS] [FILE]\n"
                "\n"
                "Options:\n"
                "      --help                   Print help message\n"
                "  -V, --version                Print version\n"
                "  -v, --verbose                Use verbose output\n"
                "  -w, --width <canvas width>   Set canvas width\n"
                "  -h, --height <canvas height> Set canvas height\n"
                "  -i, --input <file path>      Set load file\n"
                "  -o, --output <file path>     Set save file");
        }
    }

    /* extentions support */ {
        i32 maj = NIL;
        i32 min = NIL;
        if (!XdbeQueryExtension(display, &maj, &min)) {
            die("no X Double Buffer Extention support");
        }
    }

    setup(display, &ctx);
    run(&ctx);
    cleanup(&ctx);
    XCloseDisplay(display);

    return EXIT_SUCCESS;
}

void main_arg_bound_check(
    char const* cmd_name,
    i32 argc,
    char** argv,
    u32 pos
) {
    if (pos + 1 == argc || argv[pos + 1][0] == '-') {
        die("xpaint: supply argument for %s", cmd_name);
    }
}

void die(char const* errstr, ...) {
    va_list ap;

    va_start(ap, errstr);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

void trace(char const* fmt, ...) {
    if (is_verbose_output) {
        va_list ap;

        va_start(ap, fmt);
        vfprintf(stdout, fmt, ap);
        fprintf(stdout, "\n");
        va_end(ap);
    }
}

void* ecalloc(u32 n, u32 size) {
    void* p = calloc(n, size);

    if (!p) {
        die("calloc:");
    }
    return p;
}

u32 digit_count(u32 number) {
    return (u32)floor(log10(number)) + 1;
}

void arrpoputf8(char const* strarr) {
    // from https://stackoverflow.com/a/37623867
    if (!arrlen(strarr)) {
        return;
    }

    char cp = arrpop(strarr);
    while (arrlen(strarr) && ((cp & 0x80) && !(cp & 0x40))) {
        cp = arrpop(strarr);
    }
}

usize first_dismatch(char const* restrict s1, char const* restrict s2) {
    if (!s1 || !s2) {
        return 0;
    }
    usize offset = 0;
    for (; s1[offset] && s1[offset] == s2[offset]; ++offset) {};
    return offset;
}

struct IconData get_icon_data(enum Icon icon) {
    typedef struct IconData D;
    switch (icon) {
        case I_Select: return (D) {pic_tool_select_data, RES_SZ_TOOL_SELECT};
        case I_Pencil: return (D) {pic_tool_pencil_data, RES_SZ_TOOL_PENCIL};
        case I_Fill: return (D) {pic_tool_fill_data, RES_SZ_TOOL_FILL};
        case I_Picker: return (D) {pic_tool_picker_data, RES_SZ_TOOL_PICKER};
        case I_Brush: return (D) {pic_tool_brush_data, RES_SZ_TOOL_BRUSH};
        case I_Figure: return (D) {pic_tool_figure_data, RES_SZ_TOOL_FIGURE};
        default: return (D) {pic_unknown_data, RES_SZ_UNKNOWN};
    }
}

double brush_ease(double v) {
    return (v == 1.0) ? v : 1 - pow(2, -10 * v);
}

char* str_new(char const* fmt, ...) {
    va_list ap1;
    va_start(ap1, fmt);
    char* result = str_new_va(fmt, ap1);
    va_end(ap1);
    return result;
}

char* str_new_va(char const* fmt, va_list args) {
    va_list ap2;
    va_copy(ap2, args);
    usize len = vsnprintf(NULL, 0, fmt, args);
    char* result = ecalloc(len + 1, sizeof(char));
    vsnprintf(result, len + 1, fmt, ap2);
    va_end(ap2);
    return result;
}

void str_free(char** str_dyn) {
    if (*str_dyn) {
        free(*str_dyn);
        *str_dyn = NULL;
    }
}

void tc_set_curr_col_num(struct ToolCtx* tc, u32 value) {
    tc->sdata.prev_col = tc->sdata.curr_col;
    tc->sdata.curr_col = value;
}

argb* tc_curr_col(struct ToolCtx* tc) {
    return &tc->sdata.colarr[tc->sdata.curr_col];
}

void tc_set_tool(struct ToolCtx* tc, enum ToolTag type) {
    struct ToolCtx new_tc = {
        .t = type,
        .sdata = tc->sdata,
    };
    tc->sdata = (struct ToolSharedData) {0};  // don't let sdata be freed
    switch (type) {
        case Tool_Selection:
            new_tc.on_press = &tool_selection_on_press;
            new_tc.on_release = &tool_selection_on_release;
            new_tc.on_drag = &tool_selection_on_drag;
            new_tc.d.sel = (struct SelectionData) {
                .begin = PNIL,
                .end = PNIL,
                .drag_from = PNIL,
                .drag_to = PNIL,
            };
            break;
        case Tool_Brush:
            new_tc.on_press = &tool_drawer_on_press;
            new_tc.on_release = &tool_drawer_on_release;
            new_tc.on_drag = &tool_drawer_on_drag;
            new_tc.d.drawer.fn = &canvas_draw_fn_brush;
            break;
        case Tool_Pencil:
            new_tc.on_press = &tool_drawer_on_press;
            new_tc.on_release = &tool_drawer_on_release;
            new_tc.on_drag = &tool_drawer_on_drag;
            new_tc.d.drawer.fn = &canvas_draw_fn_pencil;
            break;
        case Tool_Fill: new_tc.on_release = &tool_fill_on_release; break;
        case Tool_Picker: new_tc.on_release = &tool_picker_on_release; break;
        case Tool_Figure:
            new_tc.on_press = &tool_drawer_on_press;  // same behavior
            new_tc.on_release = &tool_figure_on_release;
            new_tc.on_drag = &tool_figure_on_drag;
            break;
    }
    tc_free(tc);
    *tc = new_tc;
}

char const* tc_get_tool_name(struct ToolCtx const* tc) {
    switch (tc->t) {
        case Tool_Selection: return "select ";
        case Tool_Pencil: return "pencil ";
        case Tool_Fill: return "fill   ";
        case Tool_Picker: return "picker ";
        case Tool_Brush: return "brush  ";
        case Tool_Figure:
            switch (tc->d.fig.curr) {
                case Figure_Circle: return "fig:cir";
                case Figure_Rectangle: return "fig:rct";
                case Figure_Triangle: return "fig:tri";
            }
    }
    UNREACHABLE();
}

void tc_free(struct ToolCtx* tc) {
    arrfree(tc->sdata.colarr);
}

Bool fnt_set(struct DrawCtx* dc, char const* font_name) {
    XftFont* xfont = XftFontOpenName(dc->dp, DefaultScreen(dc->dp), font_name);
    if (!xfont) {
        // FIXME never go there
        return False;
    }
    fnt_free(dc->dp, &dc->fnt);
    dc->fnt.xfont = xfont;
    dc->fnt.h = xfont->ascent + xfont->descent;
    return True;
}

void fnt_free(Display* dp, struct Fnt* fnt) {
    if (fnt->xfont) {
        XftFontClose(dp, fnt->xfont);
        fnt->xfont = NULL;
    }
}

void file_ctx_set(struct FileCtx* file_ctx, char const* file_path) {
    file_ctx_free(file_ctx);
    file_ctx->path_dyn = str_new("%s", file_path);
}

void file_ctx_free(struct FileCtx* file_ctx) {
    str_free(&file_ctx->path_dyn);
}

Pair point_from_cv_to_scr(struct DrawCtx const* dc, Pair p) {
    return point_from_cv_to_scr_xy(dc, p.x, p.y);
}

Pair point_from_cv_to_scr_xy(struct DrawCtx const* dc, i32 x, i32 y) {
    return (Pair) {
        .x = (i32)(x * ZOOM_C(dc) + dc->cv.scroll.x),
        .y = (i32)(y * ZOOM_C(dc) + dc->cv.scroll.y),
    };
}

Pair point_from_cv_to_scr_no_move(struct DrawCtx const* dc, Pair p) {
    return (Pair) {.x = (i32)(p.x * ZOOM_C(dc)), .y = (i32)(p.y * ZOOM_C(dc))};
}

Pair point_from_scr_to_cv_xy(struct DrawCtx const* dc, i32 x, i32 y) {
    return (Pair) {
        .x = (i32)((x - dc->cv.scroll.x) / ZOOM_C(dc)),
        .y = (i32)((y - dc->cv.scroll.y) / ZOOM_C(dc)),
    };
}

static Bool point_in_rect(Pair p, Pair a1, Pair a2) {
    return MIN(a1.x, a2.x) < p.x && p.x < MAX(a1.x, a2.x)
        && MIN(a1.y, a2.y) < p.y && p.y < MAX(a1.y, a2.y);
}

enum ImageType file_type(char const* file_path) {
    if (!file_path) {
        return IMT_Unknown;
    }
    static u32 const HEADER_SIZE = 8;  // read extra symbols for simplicity
    u8 h[HEADER_SIZE];
    enum ImageType result = IMT_Unknown;

    FILE* file = fopen(file_path, "r");
    if (!file) {
        return IMT_Unknown;
    }

    if (fread(h, sizeof(u8), HEADER_SIZE, file) != HEADER_SIZE) {
        fclose(file);
        return IMT_Unknown;
    }

    // jpeg SOI marker and another marker begin
    if (h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) {
        result = IMT_Jpg;
    } else
        // png header
        if (h[0] == 0x89 && h[1] == 0x50 && h[2] == 0x4E && h[3] == 0x47
            && h[4] == 0x0D && h[5] == 0x0A && h[6] == 0x1A && h[7] == 0x0A) {
            result = IMT_Png;
        }

    fclose(file);
    return result;
}

u8* ximage_to_rgb(XImage const* image, Bool rgba) {
    u32 w = image->width;
    u32 h = image->height;
    usize pixel_size = rgba ? 4 : 3;
    usize data_size = (size_t)w * h * pixel_size;
    u8* data = (u8*)ecalloc(1, data_size);
    if (data == NULL) {
        return NULL;
    }
    i32 ii = 0;
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            u64 pixel = XGetPixel((XImage*)image, x, y);
            data[ii + 0] = (pixel & 0xFF0000) >> 16U;
            data[ii + 1] = (pixel & 0xFF00) >> 8U;
            data[ii + 2] = (pixel & 0xFF);
            if (rgba) {
                data[ii + 3] = (pixel & 0xFF000000) >> 24U;
            }
            ii += (i32)pixel_size;
        }
    }
    return data;
}

argb blend_background(argb fg, argb bg, u32 a) {
    u32 const fgr = (fg >> 16) & 0xFF;
    u32 const fgg = (fg >> 8) & 0xFF;
    u32 const fgb = fg & 0xFF;
    u32 const bgr = (bg >> 16) & 0xFF;
    u32 const bgg = (bg >> 8) & 0xFF;
    u32 const bgb = bg & 0xFF;

    // https://stackoverflow.com/a/12016968
    // result = foreground * alpha + background * (1.0 - alpha)
    u32 const alpha = a + 1;
    u32 const inv_alpha = 256 - a;
    u32 const red = (alpha * fgr + inv_alpha * bgr) >> 8;
    u32 const green = (alpha * fgg + inv_alpha * bgg) >> 8;
    u32 const blue = (alpha * fgb + inv_alpha * bgb) >> 8;

    return 0xFF << 24 | red << 16 | green << 8 | blue;
}

static u32 argb_to_abgr(argb v) {
    u32 const a = v & 0xFF000000;
    u8 const red = (v & 0x00FF0000) >> (2 * 8);
    u32 const g = v & 0x0000FF00;
    u8 const blue = v & 0x000000FF;
    return a | blue << (2 * 8) | g | red;
}

static XImage* read_file_from_memory(
    struct DrawCtx const* dc,
    u8 const* data,
    u32 len,
    argb bg
) {
    i32 width = NIL;
    i32 height = NIL;
    i32 comp = NIL;
    stbi_uc* image_data =
        stbi_load_from_memory(data, (i32)len, &width, &height, &comp, 4);
    if (image_data == NULL) {
        return NULL;
    }
    // process image data
    argb* image = (argb*)image_data;
    for (i32 i = 0; i < (width * height); ++i) {
        if (bg) {
            image[i] = blend_background(image[i], bg, (image[i] >> 24) & 0xFF);
        }
        // https://stackoverflow.com/a/17030897
        image[i] = argb_to_abgr(image[i]);
    }
    XImage* result = XCreateImage(
        dc->dp,
        dc->vinfo.visual,
        dc->vinfo.depth,
        ZPixmap,
        0,
        (char*)image_data,
        width,
        height,
        32,  // FIXME what is it? (must be 32)
        width * 4
    );

    return result;
}

XImage*
read_file_from_path(struct DrawCtx const* dc, char const* file_name, argb bg) {
    int fd = open(file_name, O_RDONLY);
    off_t len = lseek(fd, 0, SEEK_END);
    void* data = mmap(0, len, PROT_READ, MAP_PRIVATE, fd, 0);

    XImage* result = read_file_from_memory(dc, data, len, bg);

    close(fd);

    return result;
}

Bool save_file(struct DrawCtx* dc, enum ImageType type, char const* file_path) {
    if (type == IMT_Unknown) {
        return False;
    }
    Bool result = False;

    i32 w = dc->cv.im->width;
    i32 h = dc->cv.im->height;
    u8* rgba_dyn = ximage_to_rgb(dc->cv.im, True);
    switch (type) {
        case IMT_Png: {
            stbi_write_png_compression_level = dc->png_compression_level;
            result = stbi_write_png(file_path, w, h, 4, rgba_dyn, 0);
        } break;
        case IMT_Jpg: {
            i32 quality = dc->jpg_quality_level;
            result = stbi_write_jpg(file_path, w, h, 4, rgba_dyn, quality);
        } break;
        case IMT_Unknown: UNREACHABLE();
    }
    free(rgba_dyn);
    return result;
}

ClCPrcResult cl_cmd_process(struct Ctx* ctx, struct ClCommand const* cl_cmd) {
    assert(cl_cmd);
    char* msg_to_show = NULL;  // counts as PCCR_Msg at func end
    usize bit_status = 0;
    switch (cl_cmd->t) {
        case ClC_Set: {
            switch (cl_cmd->d.set.t) {
                case ClCDS_LineW: {
                    CURR_TC(ctx).sdata.line_w = cl_cmd->d.set.d.line_w.value;
                } break;
                case ClCDS_Col: {
                    *tc_curr_col(&CURR_TC(ctx)) = cl_cmd->d.set.d.col.v;
                } break;
                case ClCDS_Font: {
                    char const* font = cl_cmd->d.set.d.font.name_dyn;
                    if (!fnt_set(&ctx->dc, font)) {
                        msg_to_show = str_new("invalid font name: '%s'", font);
                    }
                } break;
                case ClCDS_FInp: {
                    char const* path = cl_cmd->d.set.d.finp.path_dyn;
                    file_ctx_set(&ctx->finp, path);
                    msg_to_show = str_new("finp set to '%s'", path);
                } break;
                case ClCDS_FOut: {
                    char const* path = cl_cmd->d.set.d.fout.path_dyn;
                    file_ctx_set(&ctx->fout, path);
                    msg_to_show = str_new("fout set to '%s'", path);
                } break;
                case ClCDS_PngCompression: {
                    ctx->dc.png_compression_level =
                        cl_cmd->d.set.d.png_cpr.compression;
                } break;
                case ClCDS_JpgQuality: {
                    ctx->dc.jpg_quality_level = cl_cmd->d.set.d.jpg_qlt.quality;
                } break;
                case ClCDS_Last: assert(!"invalid tag");
            }
        } break;
        case ClC_Echo: {
            msg_to_show = str_new("%s", cl_cmd->d.echo.msg_dyn);
        } break;
        case ClC_Exit: {
            bit_status |= ClCPrc_Exit;
        } break;
        case ClC_Save: {
            char const* path =
                COALESCE(cl_cmd->d.save.path_dyn, ctx->fout.path_dyn);
            msg_to_show = str_new(
                save_file(
                    &ctx->dc,
                    cl_save_type_to_image_type(cl_cmd->d.save.im_type),
                    path
                )
                    ? "image saved to '%s'"
                    : "failed save image to '%s'",
                path
            );
        } break;
        case ClC_Load: {
            char const* path =
                COALESCE(cl_cmd->d.load.path_dyn, ctx->finp.path_dyn);
            XImage* loaded_image = read_file_from_path(&ctx->dc, path, 0);
            if (loaded_image) {
                history_forward(ctx);
                canvas_load(&ctx->dc, loaded_image, path);
                msg_to_show = str_new("image_loaded from '%s'", path);
            } else {
                msg_to_show = str_new("failed load image from '%s'", path);
            }
        } break;
        case ClC_Last: assert(!"invalid enum value");
    }
    bit_status |= msg_to_show ? ClCPrc_Msg : 0;
    return (ClCPrcResult) {.bit_status = bit_status, .msg_dyn = msg_to_show};
}

static ClCPrsResult cl_cmd_parse_helper(struct Ctx* ctx, char* cl) {
    // naive split by spaces (0x20) works on utf8
    char const* cmd = strtok(cl, " ");
    if (!cmd) {
        return (ClCPrsResult) {.t = ClCPrs_ENoArg};
    }
    if (!strcmp(cmd, "echo")) {
        char const* user_msg = strtok(NULL, "");
        return (ClCPrsResult
        ) {.t = ClCPrs_Ok,
           .d.ok.t = ClC_Echo,
           .d.ok.d.echo.msg_dyn = str_new("%s", COALESCE(user_msg, ""))};
    }
    if (!strcmp(cmd, cl_cmd_from_enum(ClC_Set))) {
        char const* prop = strtok(NULL, " ");
        if (!prop) {
            return (ClCPrsResult
            ) {.t = ClCPrs_ENoSubArg,
               .d.nosubarg.arg_dyn = str_new(cl_cmd_from_enum(ClC_Set))};
        }
        if (!strcmp(prop, cl_set_prop_from_enum(ClCDS_LineW))) {
            char const* args = strtok(NULL, "");
            return (ClCPrsResult
            ) {.t = ClCPrs_Ok,
               .d.ok.t = ClC_Set,
               .d.ok.d.set.t = ClCDS_LineW,
               .d.ok.d.set.d.line_w.value =
                   args ? MAX(0, strtol(args, NULL, 0)) : TOOLS.default_line_w};
        }
        if (!strcmp(prop, cl_set_prop_from_enum(ClCDS_Col))) {
            char const* arg = strtok(NULL, " ");
            return (ClCPrsResult
            ) {.t = ClCPrs_Ok,
               .d.ok.t = ClC_Set,
               .d.ok.d.set.t = ClCDS_Col,
               .d.ok.d.set.d.col.v =
                   arg ? (strtol(arg, NULL, 16) & 0xFFFFFF) | 0xFF000000 : 0};
        }
        if (!strcmp(prop, cl_set_prop_from_enum(ClCDS_Font))) {
            char const* font = strtok(NULL, " ");
            if (!font) {
                return (ClCPrsResult
                ) {.t = ClCPrs_ENoSubArg,
                   .d.nosubarg.arg_dyn =
                       str_new("%s", cl_set_prop_from_enum(ClCDS_Font))};
            }
            return (ClCPrsResult
            ) {.t = ClCPrs_Ok,
               .d.ok.t = ClC_Set,
               .d.ok.d.set.t = ClCDS_Font,
               .d.ok.d.set.d.font.name_dyn = str_new("%s", font)};
        }
        if (!strcmp(prop, cl_set_prop_from_enum(ClCDS_FInp))) {
            char const* path = strtok(NULL, "");  // user can load NULL
            return (ClCPrsResult
            ) {.t = ClCPrs_Ok,
               .d.ok.t = ClC_Set,
               .d.ok.d.set.t = ClCDS_FInp,
               .d.ok.d.set.d.finp.path_dyn = path ? str_new("%s", path) : NULL};
        }
        if (!strcmp(prop, cl_set_prop_from_enum(ClCDS_FOut))) {
            char const* path = strtok(NULL, "");  // user can load NULL
            return (ClCPrsResult
            ) {.t = ClCPrs_Ok,
               .d.ok.t = ClC_Set,
               .d.ok.d.set.t = ClCDS_FOut,
               .d.ok.d.set.d.fout.path_dyn = path ? str_new("%s", path) : NULL};
        }
        if (!strcmp(prop, cl_set_prop_from_enum(ClCDS_PngCompression))) {
            return (ClCPrsResult
            ) {.t = ClCPrs_Ok,
               .d.ok.t = ClC_Set,
               .d.ok.d.set.t = ClCDS_PngCompression,
               .d.ok.d.set.d.png_cpr.compression =
                   (i32)strtol(strtok(NULL, " "), NULL, 0)};
        }
        if (!strcmp(prop, cl_set_prop_from_enum(ClCDS_JpgQuality))) {
            return (ClCPrsResult
            ) {.t = ClCPrs_Ok,
               .d.ok.t = ClC_Set,
               .d.ok.d.set.t = ClCDS_JpgQuality,
               .d.ok.d.set.d.jpg_qlt.quality =
                   (i32)strtol(strtok(NULL, ""), NULL, 0)};
        }
        return (ClCPrsResult
        ) {.t = ClCPrs_EInvSubArg,
           .d.invsubarg.arg_dyn = str_new("%s", cl_cmd_from_enum(ClC_Set)),
           .d.invsubarg.inv_val_dyn = str_new("%s", prop)};
    }
    if (!strcmp(cmd, cl_cmd_from_enum(ClC_Exit))) {
        return (ClCPrsResult) {.t = ClCPrs_Ok, .d.ok.t = ClC_Exit};
    }
    if (!strcmp(cmd, cl_cmd_from_enum(ClC_Save))) {
        char const* type_str = strtok(NULL, " ");
        if (!type_str) {
            return (ClCPrsResult
            ) {.t = ClCPrs_ENoSubArg,
               .d.nosubarg.arg_dyn = str_new("%s", cl_cmd_from_enum(ClC_Save))};
        }
        enum ClCDSv type = 0;
        for (; type < ClCDSv_Last; ++type) {
            if (!strcmp(type_str, cl_save_type_from_enum(type))) {
                break;
            }
        }
        if (type == ClCDSv_Last) {
            return (ClCPrsResult
            ) {.t = ClCPrs_EInvSubArg,
               .d.invsubarg.arg_dyn = str_new("%s", cl_cmd_from_enum(ClC_Save)),
               .d.invsubarg.inv_val_dyn = str_new("%s", type_str)};
        }
        char const* path = strtok(NULL, "");  // include spaces
        return (ClCPrsResult
        ) {.t = ClCPrs_Ok,
           .d.ok.t = ClC_Save,
           .d.ok.d.save.im_type = type,
           .d.ok.d.save.path_dyn = path ? str_new("%s", path) : NULL};
    }
    if (!strcmp(cmd, cl_cmd_from_enum(ClC_Load))) {
        char const* path = strtok(NULL, "");  // path with spaces
        return (ClCPrsResult
        ) {.t = ClCPrs_Ok,
           .d.ok.t = ClC_Load,
           .d.ok.d.load.path_dyn = str_new("%s", path)};
    }
    return (ClCPrsResult
    ) {.t = ClCPrs_EInvArg, .d.invarg.arg_dyn = str_new("%s", cmd)};
}

ClCPrsResult cl_cmd_parse(struct Ctx* ctx, char const* cl) {
    assert(cl);
    char* cl_bufdyn = str_new("%s", cl);
    ClCPrsResult result = cl_cmd_parse_helper(ctx, cl_bufdyn);
    str_free(&cl_bufdyn);
    return result;
}

void cl_cmd_parse_res_free(ClCPrsResult* res) {
    switch (res->t) {
        case ClCPrs_Ok: {
            struct ClCommand* cl_cmd = &res->d.ok;
            switch (cl_cmd->t) {
                case ClC_Set:
                    switch (cl_cmd->d.set.t) {
                        case ClCDS_Font:
                            free(cl_cmd->d.set.d.font.name_dyn);
                            break;
                        case ClCDS_FInp:
                            free(cl_cmd->d.set.d.finp.path_dyn);
                            break;
                        case ClCDS_FOut:
                            free(cl_cmd->d.set.d.fout.path_dyn);
                            break;
                        case ClCDS_LineW:
                        case ClCDS_Col:
                        case ClCDS_PngCompression:
                        case ClCDS_JpgQuality:
                        case ClCDS_Last:
                            break;  // no default branch to enable warnings
                    }
                    break;
                case ClC_Save: free(cl_cmd->d.save.path_dyn); break;
                case ClC_Load: free(cl_cmd->d.load.path_dyn); break;
                case ClC_Echo: free(cl_cmd->d.echo.msg_dyn); break;
                case ClC_Exit: break;  // no default branch to enable warnings
                case ClC_Last: assert(!"invalid enum value");
            }
        } break;
        case ClCPrs_EInvArg: {
            str_free(&res->d.invarg.arg_dyn);
        } break;
        case ClCPrs_EInvSubArg: {
            str_free(&res->d.invsubarg.arg_dyn);
            str_free(&res->d.invsubarg.inv_val_dyn);
        } break;
        case ClCPrs_ENoSubArg: {
            str_free(&res->d.nosubarg.arg_dyn);
        } break;
        case ClCPrs_ENoArg: break;
    }
}

char* cl_cmd_get_str_dyn(struct InputConsoleData const* d_cl) {
    usize const cmd_len = arrlen(d_cl->cmdarr);
    char* cmd_dyn = ecalloc(cmd_len + 1, sizeof(char));
    memcpy(cmd_dyn, d_cl->cmdarr, cmd_len);
    cmd_dyn[cmd_len] = '\0';
    return cmd_dyn;
}

char const* cl_cmd_from_enum(enum ClCTag t) {
    switch (t) {
        case ClC_Echo: return "echo";
        case ClC_Exit: return "q";
        case ClC_Load: return "load";
        case ClC_Save: return "save";
        case ClC_Set: return "set";
        case ClC_Last: return "last";
    }
    UNREACHABLE();
}

static char const* cl_set_prop_from_enum(enum ClCDSTag t) {
    switch (t) {
        case ClCDS_Col: return "col";
        case ClCDS_FInp: return "finp";
        case ClCDS_FOut: return "fout";
        case ClCDS_Font: return "font";
        case ClCDS_LineW: return "line_w";
        case ClCDS_PngCompression: return "png_cmpr";
        case ClCDS_JpgQuality: return "jpg_qlty";
        case ClCDS_Last: return "last";
    }
    UNREACHABLE();
}

static char const* cl_save_type_from_enum(enum ClCDSv t) {
    switch (t) {
        case ClCDSv_Png: return "png";
        case ClCDSv_Jpg: return "jpg";
        case ClCDSv_Last: return "last";
    }
    UNREACHABLE();
}

enum ImageType cl_save_type_to_image_type(enum ClCDSv t) {
    switch (t) {
        case ClCDSv_Png: return IMT_Png;
        case ClCDSv_Jpg: return IMT_Jpg;
        case ClCDSv_Last: UNREACHABLE();
    }
    UNREACHABLE();
}

static void cl_compls_update_helper(
    char*** result,
    char const* token,
    char const* (*enum_to_str)(usize),
    usize enum_last
) {
    for (u32 e = 0; e < enum_last; ++e) {
        char const* enum_str = enum_to_str(e);
        usize offset = first_dismatch(enum_str, token);
        if (offset == strlen(token)) {
            arrpush(*result, str_new("%s", enum_str + offset));
        }
    }
}

void cl_compls_update(struct InputConsoleData* cl) {
    char* cl_buf_dyn = cl_cmd_get_str_dyn(cl);
    char** result = NULL;

    // XXX separator hardcoded
    char const* tok1 = strtok(cl_buf_dyn, " ");
    char const* tok2 = strtok(NULL, " ");  // can be NULL
    tok1 = COALESCE(tok1, "");  // don't do strtok in macro
    tok2 = COALESCE(tok2, "");

    typedef char const* (*cast)(usize);
    // subcommands with own completions
    if (!strcmp(tok1, cl_cmd_from_enum(ClC_Set))) {
        cl_compls_update_helper(
            &result,
            tok2,
            (cast)&cl_set_prop_from_enum,
            ClCDS_Last
        );
    } else if (!strcmp(tok1, cl_cmd_from_enum(ClC_Save))) {
        cl_compls_update_helper(
            &result,
            tok2,
            (cast)&cl_save_type_from_enum,
            ClCDSv_Last
        );
    } else {  // first token comletion
        cl_compls_update_helper(
            &result,
            tok1,
            (cast)&cl_cmd_from_enum,
            ClC_Last
        );
    }

    free(cl_buf_dyn);

    cl_compls_free(cl);
    cl->compls_arr = result;
    cl->compls_valid = True;
    cl->compls_curr = 0;  // prevent out-of-range errors
}

void cl_free(struct InputConsoleData* cl) {
    arrfree(cl->cmdarr);
    cl_compls_free(cl);
}

void cl_compls_free(struct InputConsoleData* cl) {
    if (cl->compls_arr) {
        for (u32 i = 0; i < arrlen(cl->compls_arr); ++i) {
            str_free(&cl->compls_arr[i]);
        }
        arrfree(cl->compls_arr);
        cl->compls_arr = NULL;
    }
}

void cl_push(struct InputConsoleData* cl, char c) {
    arrpush(cl->cmdarr, c);
    cl->compls_valid = False;
    cl_compls_free(cl);
}

void cl_pop(struct InputConsoleData* cl) {
    if (!cl->cmdarr) {
        return;
    }
    usize const size = arrlen(cl->cmdarr);
    if (size) {
        arrpoputf8(cl->cmdarr);
    }
    cl->compls_valid = False;
    cl_compls_free(cl);
}

void input_state_set(struct Input* input, enum InputTag const is) {
    switch (input->t) {
        case InputT_Console: cl_free(&input->d.cl); break;
        default: break;
    }

    input->t = is;

    switch (is) {
        case InputT_Color:
            input->d.col = (struct InputColorData) {.current_digit = 0};
            break;
        case InputT_Console:
            input->d.cl = (struct InputConsoleData
            ) {.cmdarr = NULL, .compls_valid = False};
            break;
        case InputT_Interact: break;
    }
}

// clang-format off
static void sel_circ_set_tool_selection(struct Ctx* ctx) { tc_set_tool(&CURR_TC(ctx), Tool_Selection); }
static void sel_circ_set_tool_pencil(struct Ctx* ctx) { tc_set_tool(&CURR_TC(ctx), Tool_Pencil); }
static void sel_circ_set_tool_fill(struct Ctx* ctx) { tc_set_tool(&CURR_TC(ctx), Tool_Fill); }
static void sel_circ_set_tool_picker(struct Ctx* ctx) { tc_set_tool(&CURR_TC(ctx), Tool_Picker); }
static void sel_circ_set_tool_brush(struct Ctx* ctx) { tc_set_tool(&CURR_TC(ctx), Tool_Brush); }
static void sel_circ_set_tool_figure(struct Ctx* ctx) { tc_set_tool(&CURR_TC(ctx), Tool_Figure); }
static void sel_circ_figure_toggle_fill(struct Ctx* ctx) { CURR_TC(ctx).d.fig.fill ^= 1; }
static void sel_circ_set_figure(struct Ctx* ctx, enum FigureType type) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    assert(tc->t == Tool_Figure);
    tc->d.fig.curr = type;
}
static void sel_circ_figure_set_circle(struct Ctx* ctx) { sel_circ_set_figure(ctx, Figure_Circle); }
static void sel_circ_figure_set_rectangle(struct Ctx* ctx) { sel_circ_set_figure(ctx, Figure_Rectangle); }
static void sel_circ_figure_set_triangle(struct Ctx* ctx) { sel_circ_set_figure(ctx, Figure_Triangle); }
// clang-format on

void sel_circ_init(struct Ctx* ctx, i32 x, i32 y) {
    if (CURR_TC(ctx).t == Tool_Figure) {
        static struct Item callbacks[] = {
            {.on_select = &sel_circ_figure_set_circle, .icon = I_Figure},
            {.on_select = &sel_circ_figure_set_rectangle, .icon = I_Figure},
            {.on_select = &sel_circ_figure_set_triangle, .icon = I_Figure},
            {.on_select = &sel_circ_figure_toggle_fill, .icon = I_Fill},
            {.on_select = &sel_circ_set_tool_pencil, .icon = I_Pencil},
        };
        ctx->sc.items = callbacks;
        ctx->sc.item_count = LENGTH(callbacks);
    } else {
        static struct Item callbacks[] = {
            {.on_select = &sel_circ_set_tool_selection, .icon = I_Select},
            {.on_select = &sel_circ_set_tool_pencil, .icon = I_Pencil},
            {.on_select = &sel_circ_set_tool_fill, .icon = I_Fill},
            {.on_select = &sel_circ_set_tool_picker, .icon = I_Picker},
            {.on_select = &sel_circ_set_tool_brush, .icon = I_Brush},
            {.on_select = &sel_circ_set_tool_figure, .icon = I_Figure},
        };
        ctx->sc.items = callbacks;
        ctx->sc.item_count = LENGTH(callbacks);
    }
    ctx->sc.x = x;
    ctx->sc.y = y;
    ctx->sc.is_active = True;
}

void sel_circ_free(struct SelectionCircle* sel_circ) {
    sel_circ->is_active = False;
}

i32 sel_circ_curr_item(struct SelectionCircle const* sc, i32 x, i32 y) {
    i32 const pointer_x_rel = x - sc->x;
    i32 const pointer_y_rel = y - sc->y;
    if (pointer_x_rel == 0 && pointer_y_rel == 0) {
        return NIL;  // prevent 0.0 / 0.0 division
    }
    double const segment_rad = PI * 2 / MAX(1, sc->item_count);
    double const segment_deg = segment_rad / PI * 180;
    double const pointer_r =
        sqrt(pointer_x_rel * pointer_x_rel + pointer_y_rel * pointer_y_rel);

    if (pointer_r > SELECTION_CIRCLE.outer_r_px
        || pointer_r < SELECTION_CIRCLE.inner_r_px) {
        return NIL;
    }
    // FIXME do it right
    double angle = -atan(pointer_y_rel * 1.0 / pointer_x_rel) / PI * 180;
    if (pointer_x_rel < 0) {
        angle += 180;
    } else if (pointer_y_rel >= 0) {
        angle += 360;
    }

    return (i32)(angle / segment_deg);
}

void tool_selection_on_press(
    struct Ctx* ctx,
    XButtonPressedEvent const* event
) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    struct DrawCtx* dc = &ctx->dc;
    assert(tc->t == Tool_Selection);
    if (event->button == XLeftMouseBtn) {
        struct SelectionData* sd = &tc->d.sel;
        Pair pointer = point_from_scr_to_cv_xy(dc, event->x, event->y);
        if (HAS_SELECTION(tc) && point_in_rect(pointer, sd->begin, sd->end)) {
            sd->drag_from = pointer;
            sd->drag_to = pointer;
        } else {
            sd->begin.x = CLAMP(pointer.x, 0, dc->cv.im->width);
            sd->begin.y = CLAMP(pointer.y, 0, dc->cv.im->height);
            sd->end = PNIL;
        }
    }
}

void tool_selection_on_release(
    struct Ctx* ctx,
    XButtonReleasedEvent const* event
) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    struct DrawCtx* dc = &ctx->dc;
    assert(tc->t == Tool_Selection);
    if (event->button != XLeftMouseBtn) {
        return;
    }
    struct SelectionData* sd = &tc->d.sel;

    if (SELECTION_DRAGGING(tc)) {
        // finish drag selection
        Pair pointer = point_from_scr_to_cv_xy(dc, event->x, event->y);
        Pair move_vec = {
            pointer.x - sd->drag_from.x,
            pointer.y - sd->drag_from.y
        };
        Pair area = {MIN(sd->begin.x, sd->end.x), MIN(sd->begin.y, sd->end.y)};
        canvas_copy_region(
            ctx,
            area,
            (Pair
            ) {MAX(sd->begin.x, sd->end.x) - area.x,
               MAX(sd->begin.y, sd->end.y) - area.y},
            (Pair) {area.x + move_vec.x, area.y + move_vec.y},
            !(event->state & ShiftMask)
        );
    } else if (ctx->input.is_dragging) {
        // select area
        XSetSelectionOwner(dc->dp, XA_PRIMARY, dc->window, CurrentTime);
        trace("clipboard owned");
        return;
    }
    // unselect area
    sd->begin = sd->end = sd->drag_from = sd->drag_to = PNIL;
    XSetSelectionOwner(dc->dp, XA_PRIMARY, None, CurrentTime);
    trace("clipboard released");
}

void tool_selection_on_drag(struct Ctx* ctx, XMotionEvent const* event) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    struct DrawCtx* dc = &ctx->dc;
    assert(tc->t == Tool_Selection);
    if (ctx->input.holding_button != XLeftMouseBtn) {
        return;
    }

    Pair pointer = point_from_scr_to_cv_xy(dc, event->x, event->y);
    if (SELECTION_DRAGGING(tc)) {
        tc->d.sel.drag_to = pointer;
    } else if (ctx->input.is_holding) {
        tc->d.sel.end.x = CLAMP(pointer.x, 0, dc->cv.im->width);
        tc->d.sel.end.y = CLAMP(pointer.y, 0, dc->cv.im->height);
    }
}

void tool_drawer_on_press(struct Ctx* ctx, XButtonPressedEvent const* event) {
    if (event->button != XLeftMouseBtn) {
        return;
    }

    if (!(event->state & ShiftMask)) {
        CURR_TC(ctx).sdata.anchor =
            point_from_scr_to_cv_xy(&ctx->dc, event->x, event->y);
    }
}

void tool_drawer_on_release(
    struct Ctx* ctx,
    XButtonReleasedEvent const* event
) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    struct DrawCtx* dc = &ctx->dc;
    if (event->button != XLeftMouseBtn || ctx->input.is_dragging) {
        return;
    }

    Pair pointer = point_from_scr_to_cv_xy(dc, event->x, event->y);
    if (event->state & ShiftMask) {
        canvas_line(ctx, tc->sdata.anchor, pointer, tc->d.drawer.fn);
    } else {
        tc->d.drawer.fn(ctx, pointer);
    }
    tc->sdata.anchor = point_from_scr_to_cv_xy(dc, event->x, event->y);
}

void tool_drawer_on_drag(struct Ctx* ctx, XMotionEvent const* event) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    struct DrawCtx* dc = &ctx->dc;
    if (ctx->input.holding_button != XLeftMouseBtn) {
        return;
    }

    Pair pointer = point_from_scr_to_cv_xy(dc, event->x, event->y);
    canvas_line(ctx, tc->sdata.anchor, pointer, tc->d.drawer.fn);
    tc->sdata.anchor = point_from_scr_to_cv_xy(dc, event->x, event->y);
}

void tool_figure_on_release(
    struct Ctx* ctx,
    XButtonReleasedEvent const* event
) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    struct DrawCtx* dc = &ctx->dc;
    if (event->button != XLeftMouseBtn || ctx->input.is_dragging) {
        return;
    }

    Pair pointer = point_from_scr_to_cv_xy(dc, event->x, event->y);
    canvas_figure(ctx, pointer, tc->sdata.anchor);
}

void tool_figure_on_drag(struct Ctx* ctx, XMotionEvent const* event) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    struct DrawCtx* dc = &ctx->dc;
    if (ctx->input.holding_button != XLeftMouseBtn) {
        return;
    }

    history_restore(ctx);
    Pair pointer = point_from_scr_to_cv_xy(dc, event->x, event->y);
    canvas_figure(ctx, pointer, tc->sdata.anchor);
}

static void flood_fill(XImage* im, argb targ_col, i32 x, i32 y) {
    assert(im);
    if (x < 0 || y < 0 || x >= im->width || y >= im->height) {
        return;
    }

    static i32 const d_rows[] = {1, 0, 0, -1};
    static i32 const d_cols[] = {0, 1, -1, 0};

    argb const area_col = XGetPixel(im, x, y);
    if (area_col == targ_col) {
        return;
    }

    Pair* queue_arr = NULL;
    Pair first = {x, y};
    arrpush(queue_arr, first);

    while (arrlen(queue_arr)) {
        Pair curr = arrpop(queue_arr);

        for (i32 dir = 0; dir < 4; ++dir) {
            Pair d_curr = {curr.x + d_rows[dir], curr.y + d_cols[dir]};

            if (d_curr.x < 0 || d_curr.y < 0 || d_curr.x >= im->width
                || d_curr.y >= im->height) {
                continue;
            }

            if (XGetPixel(im, d_curr.x, d_curr.y) == area_col) {
                XPutPixel(im, d_curr.x, d_curr.y, targ_col);
                arrpush(queue_arr, d_curr);
            }
        }
    }

    arrfree(queue_arr);
}

void tool_fill_on_release(struct Ctx* ctx, XButtonReleasedEvent const* event) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    struct DrawCtx* dc = &ctx->dc;
    if (ctx->input.holding_button != XLeftMouseBtn) {
        return;
    }
    Pair const pointer = point_from_scr_to_cv_xy(dc, event->x, event->y);

    flood_fill(dc->cv.im, *tc_curr_col(tc), pointer.x, pointer.y);
}

void tool_picker_on_release(
    struct Ctx* ctx,
    XButtonReleasedEvent const* event
) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    struct DrawCtx* dc = &ctx->dc;
    Pair const pointer = point_from_scr_to_cv_xy(dc, event->x, event->y);

    if (point_in_rect(
            pointer,
            (Pair) {0, 0},
            (Pair) {(i32)dc->cv.im->width, (i32)dc->cv.im->height}
        )) {
        *tc_curr_col(tc) = XGetPixel(dc->cv.im, pointer.x, pointer.y);
    }
}

Bool history_move(struct Ctx* ctx, Bool forward) {
    struct History** hist_pop =
        forward ? &ctx->hist_prevarr : &ctx->hist_nextarr;
    struct History** hist_save =
        forward ? &ctx->hist_nextarr : &ctx->hist_prevarr;

    if (!arrlenu(*hist_pop)) {
        return False;
    }

    struct History curr = arrpop(*hist_pop);
    history_push(hist_save, ctx);

    history_apply(ctx, &curr);

    return True;
}

void history_push(struct History** hist, struct Ctx* ctx) {
    trace("xpaint: history push");
    arrpush(*hist, history_clone(&(struct History) {.im = ctx->dc.cv.im}));
}

void history_forward(struct Ctx* ctx) {
    // next history invalidated after user action
    historyarr_clear(ctx->dc.dp, &ctx->hist_nextarr);
    history_push(&ctx->hist_prevarr, ctx);
}

void history_apply(struct Ctx* ctx, struct History* hist) {
    XDestroyImage(ctx->dc.cv.im);
    ctx->dc.cv.im = hist->im;
}

Bool history_restore(struct Ctx* ctx) {
    if (!arrlen(ctx->hist_prevarr)) {
        return False;
    }
    struct History hist = history_clone(&arrlast(ctx->hist_prevarr));
    history_apply(ctx, &hist);
    return True;
}

struct History history_clone(struct History const* hist) {
    struct History result;
    result.im = XSubImage(hist->im, 0, 0, hist->im->width, hist->im->height);

    return result;
}

void historyarr_clear(Display* dp, struct History** histarr) {
    for (u32 i = 0; i < arrlenu(*histarr); ++i) {
        struct History* h = &(*histarr)[i];
        XDestroyImage(h->im);
    }
    arrfree(*histarr);
}

Bool ximage_put_checked(XImage* im, u32 x, u32 y, argb col) {
    if (x >= im->width || y >= im->height) {
        return False;
    }

    XPutPixel(im, x, y, col);
    return True;
}

static u8 canvas_brush_get_a(struct Ctx* ctx, double r, Pair p) {
    double const curr_r = sqrt((p.x - r) * (p.x - r) + (p.y - r) * (p.y - r));
    return (u32)((1.0 - brush_ease(curr_r / r)) * 0xFF);
}

void canvas_draw_fn_brush(struct Ctx* ctx, Pair c) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    canvas_circle(
        ctx,
        c,
        tc->sdata.line_w,
        *tc_curr_col(tc),
        &canvas_brush_get_a
    );
}

void canvas_draw_fn_pencil(struct Ctx* ctx, Pair c) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    i32 const w = (i32)tc->sdata.line_w;
    canvas_fill_rect(
        ctx,
        (Pair) {c.x - w / 2, c.y - w / 2},
        (Pair) {w, w},
        *tc_curr_col(tc)
    );
}

static u8 canvas_figure_circle_get_a_fill(struct Ctx* ctx, double r, Pair p) {
    return 0xFF;
}

static u8 canvas_figure_circle_get_a(struct Ctx* ctx, double r, Pair p) {
    double const curr_r = sqrt((p.x - r) * (p.x - r) + (p.y - r) * (p.y - r));
    // FIXME fixed border width (same as line_w)
    return ((curr_r / r) > 0.9) * 0xFF;
}

void canvas_figure(struct Ctx* ctx, Pair p1, Pair p2) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    if (tc->t != Tool_Figure) {
        return;
    }
    struct FigureData const* fig = &tc->d.fig;
    argb const col = *tc_curr_col(tc);
    i32 const dx = p1.x - p2.x;
    i32 const dy = p1.y - p2.y;

    switch (fig->curr) {
        case Figure_Circle: {
            double const d = sqrt(dx * dx + dy * dy);
            canvas_circle(
                ctx,
                (Pair) {(p1.x + p2.x) / 2, (p1.y + p2.y) / 2},
                (u32)d,
                col,
                fig->fill ? &canvas_figure_circle_get_a_fill
                          : &canvas_figure_circle_get_a
            );
        } break;
        case Figure_Rectangle: {
            if (fig->fill) {
                canvas_fill_rect(ctx, p2, (Pair) {dx, dy}, col);
            } else {
                canvas_rect(ctx, p2, (Pair) {dx, dy}, col, tc->sdata.line_w);
            }
        } break;
        // FIXME combine with Figure_Rectangle? same signatures
        case Figure_Triangle: {
            if (fig->fill) {
                canvas_fill_triangle(ctx, p2, (Pair) {dx, dy}, col);
            } else {
                canvas_triangle(
                    ctx,
                    p2,
                    (Pair) {dx, dy},
                    col,
                    tc->sdata.line_w
                );
            }
        } break;
    }
}

void canvas_fill_rect(struct Ctx* ctx, Pair c, Pair dims, argb col) {
    struct DrawCtx* dc = &ctx->dc;
    Bool const nx = dims.x < 0;
    Bool const ny = dims.y < 0;
    for (i32 x = c.x + (nx ? dims.x : 0); x < c.x + (nx ? 0 : dims.x); ++x) {
        for (i32 y = c.y + (ny ? dims.y : 0); y < c.y + (ny ? 0 : dims.y);
             ++y) {
            ximage_put_checked(dc->cv.im, x, y, col);
        }
    }
}

void canvas_rect(struct Ctx* ctx, Pair c, Pair dims, argb col, u32 w) {
    // draw 4 sides and fill 2 corners (edge case on negative-negative input)
    Pair const cap = (Pair) {dims.x < 0 ? (i32)w : 0, dims.y < 0 ? (i32)w : 0};
    Pair const c1 = (Pair) {c.x - cap.x, c.y - cap.y};
    Pair const c2 = (Pair) {c.x + dims.x + cap.x, c.y + dims.y + cap.y};
    canvas_fill_rect(ctx, c1, (Pair) {dims.x + cap.x, (i32)w}, col);
    canvas_fill_rect(ctx, c1, (Pair) {(i32)w, dims.y + cap.y}, col);
    canvas_fill_rect(ctx, c2, (Pair) {-dims.x - cap.x, -(i32)w}, col);
    canvas_fill_rect(ctx, c2, (Pair) {-(i32)w, -dims.y - cap.y}, col);
    if (dims.x < 0 && dims.y < 0) {
        canvas_fill_rect(ctx, c1, (Pair) {(i32)w, (i32)w}, col);
        canvas_fill_rect(ctx, c2, (Pair) {-(i32)w, -(i32)w}, col);
    }
}

void canvas_fill_triangle(struct Ctx* ctx, Pair c, Pair dims, argb col) {
    struct DrawCtx* dc = &ctx->dc;
    for (i32 i = 0; i < abs(dims.x); ++i) {
        i32 const line_w = (i32)(abs(dims.y) * ((double)i / abs(dims.x)));
        for (i32 j = 0; j < line_w; ++j) {
            // FIXME fix shape
            ximage_put_checked(
                dc->cv.im,
                c.x + (dims.x > 0 ? i : -i),
                c.y + (dims.y > 0 ? j : -j),
                col
            );
        }
    }
}

void canvas_triangle(struct Ctx* ctx, Pair c, Pair dims, argb col, u32 w) {
    Pair const edges[3] = {
        {c.x + dims.x / 2, c.y},
        {c.x, c.y + dims.y},
        {c.x + dims.x, c.y + dims.y},
    };
    canvas_line(ctx, edges[0], edges[1], &canvas_draw_fn_pencil);
    canvas_line(ctx, edges[1], edges[2], &canvas_draw_fn_pencil);
    canvas_line(ctx, edges[0], edges[2], &canvas_draw_fn_pencil);
}

void canvas_line(struct Ctx* ctx, Pair from, Pair to, draw_fn draw) {
    struct DrawCtx* dc = &ctx->dc;
    assert(dc->cv.im);

    i32 dx = abs(to.x - from.x);
    i32 sx = from.x < to.x ? 1 : -1;
    i32 dy = -abs(to.y - from.y);
    i32 sy = from.y < to.y ? 1 : -1;
    i32 error = dx + dy;

    // FIXME don't work if start point out of canvas
    while (from.x >= 0 && from.y >= 0 && from.x < dc->cv.im->width
           && from.y < dc->cv.im->height) {
        draw(ctx, from);
        if (from.x == to.x && from.y == to.y) {
            break;
        }
        i32 e2 = 2 * error;
        if (e2 >= dy) {
            if (from.x == to.x) {
                break;
            }
            error += dy;
            from.x += sx;
        }
        if (e2 <= dx) {
            if (from.y == to.y) {
                break;
            }
            error += dx;
            from.y += sy;
        }
    }
}

void canvas_circle(
    struct Ctx* ctx,
    Pair c,
    u32 d,
    argb col,
    circle_get_alpha_fn get_a
) {
    struct DrawCtx* dc = &ctx->dc;
    if (d == 1) {
        ximage_put_checked(dc->cv.im, c.x, c.y, col);
        return;
    }
    double const r = d / 2.0;
    double const r_sq = r * r;
    u32 const l = c.x - (u32)r;
    u32 const t = c.y - (u32)r;
    for (i32 dx = 0; dx < d; ++dx) {
        for (i32 dy = 0; dy < d; ++dy) {
            double const dr = (dx - r) * (dx - r) + (dy - r) * (dy - r);
            u32 const x = l + dx;
            u32 const y = t + dy;
            if (!BETWEEN(x, 0, dc->cv.im->width - 1)
                || !BETWEEN(y, 0, dc->cv.im->height - 1) || dr > r_sq) {
                continue;
            }
            argb const bg = XGetPixel(dc->cv.im, x, y);
            argb const blended =
                blend_background(col, bg, get_a(ctx, r, (Pair) {dx, dy}));
            XPutPixel(dc->cv.im, x, y, blended);
        }
    }
}

void canvas_copy_region(
    struct Ctx* ctx,
    Pair from,
    Pair dims,
    Pair to,
    Bool clear_source
) {
    struct DrawCtx* dc = &ctx->dc;
    i32 const w = dc->cv.im->width;
    i32 const h = dc->cv.im->height;

    u32* region_dyn = (u32*)ecalloc(w * h, sizeof(u32));
    for (i32 get_or_set = 1; get_or_set >= 0; --get_or_set) {
        for (i32 y = 0; y < dims.y; ++y) {
            for (i32 x = 0; x < dims.x; ++x) {
                if (get_or_set) {
                    region_dyn[y * w + x] =
                        XGetPixel(dc->cv.im, from.x + x, from.y + y);
                    if (clear_source) {
                        ximage_put_checked(
                            dc->cv.im,
                            from.x + x,
                            from.y + y,
                            CANVAS.background_argb
                        );
                    }
                } else {
                    ximage_put_checked(
                        dc->cv.im,
                        to.x + x,
                        to.y + y,
                        region_dyn[y * w + x]
                    );
                }
            }
        }
    }
    free(region_dyn);
}

void canvas_fill(struct Ctx* ctx, argb col) {
    struct DrawCtx* dc = &ctx->dc;
    assert(dc && dc->cv.im);

    for (i32 i = 0; i < dc->cv.im->width; ++i) {
        for (i32 j = 0; j < dc->cv.im->height; ++j) {
            XPutPixel(dc->cv.im, i, j, col);
        }
    }
}

static void canvas_load(struct DrawCtx* dc, XImage* im, char const* file_path) {
    assert(im);
    canvas_free(dc->dp, &dc->cv);
    dc->cv.im = im;
    dc->cv.type = file_type(file_path);
}

void canvas_free(Display* dp, struct Canvas* cv) {
    if (cv->im) {
        XDestroyImage(cv->im);
        cv->im = NULL;
    }
}

void canvas_change_zoom(struct DrawCtx* dc, Pair cursor, i32 delta) {
    double old_zoom = ZOOM_C(dc);
    dc->cv.zoom = CLAMP(dc->cv.zoom + delta, CANVAS.min_zoom, CANVAS.max_zoom);
    // keep cursor at same position
    dc->cv.scroll.x +=
        (i32)((dc->cv.scroll.x - cursor.x) * (ZOOM_C(dc) / old_zoom - 1));
    dc->cv.scroll.y +=
        (i32)((dc->cv.scroll.y - cursor.y) * (ZOOM_C(dc) / old_zoom - 1));
}

void canvas_resize(struct Ctx* ctx, i32 new_width, i32 new_height) {
    if (new_width <= 0 || new_height <= 0) {
        trace("resize_canvas: invalid canvas size");
        return;
    }
    struct DrawCtx* dc = &ctx->dc;
    u32 const old_width = dc->cv.im->width;
    u32 const old_height = dc->cv.im->height;

    // FIXME can fill color be changed?
    XImage* new_cv_im = XSubImage(dc->cv.im, 0, 0, new_width, new_height);
    XDestroyImage(dc->cv.im);
    dc->cv.im = new_cv_im;

    // fill new area if needed
    if (old_width < new_width) {
        canvas_fill_rect(
            ctx,
            (Pair) {(i32)old_width, 0},
            (Pair) {(i32)(new_width - old_width), new_height},
            CANVAS.background_argb
        );
    }
    if (old_height < new_height) {
        canvas_fill_rect(
            ctx,
            (Pair) {0, (i32)old_height},
            (Pair) {new_width, (i32)(new_height - old_height)},
            CANVAS.background_argb
        );
    }
}

u32 get_statusline_height(struct DrawCtx const* dc) {
    return dc->fnt.xfont->ascent + STATUSLINE.padding_bottom;
}

void draw_string(
    struct DrawCtx* dc,
    char const* str,
    Pair c,
    enum Schm sc,
    Bool invert
) {
    XftDraw* d =
        XftDrawCreate(dc->dp, dc->back_buffer, dc->vinfo.visual, dc->colmap);
    XftDrawStringUtf8(
        d,
        invert ? &dc->schemes_dyn[sc].bg : &dc->schemes_dyn[sc].fg,
        dc->fnt.xfont,
        c.x,
        c.y,
        (XftChar8*)str,
        (i32)strlen(str)
    );
    XftDrawDestroy(d);
}

void draw_int(struct DrawCtx* dc, i32 i, Pair c, enum Schm sc, Bool invert) {
    char* msg = str_new("%d", i);
    draw_string(dc, msg, c, sc, invert);
    str_free(&msg);
}

// XXX always opaque
int fill_rect(struct DrawCtx* dc, Pair p, Pair dim, argb col) {
    XSetForeground(dc->dp, dc->screen_gc, col | 0xFF000000);
    return XFillRectangle(
        dc->dp,
        dc->back_buffer,
        dc->screen_gc,
        p.x,
        p.y,
        dim.x,
        dim.y
    );
}

int draw_rect(
    struct DrawCtx* dc,
    Pair p,
    Pair dim,
    argb col,
    u32 line_w,
    i32 line_st,
    i32 cap_st,
    i32 join_st
) {
    XSetForeground(dc->dp, dc->screen_gc, col);
    XSetLineAttributes(dc->dp, dc->screen_gc, line_w, line_st, cap_st, join_st);
    return XDrawRectangle(
        dc->dp,
        dc->back_buffer,
        dc->screen_gc,
        p.x,
        p.y,
        dim.x,
        dim.y
    );
}

int draw_line(
    struct DrawCtx* dc,
    Pair from,
    Pair to,
    enum Schm sc,
    Bool invert
) {
    XSetForeground(
        dc->dp,
        dc->screen_gc,
        invert ? COL_BG(dc, sc) : COL_FG(dc, sc)
    );
    return XDrawLine(
        dc->dp,
        dc->back_buffer,
        dc->screen_gc,
        from.x,
        from.y,
        to.x,
        to.y
    );
}

u32 get_string_width(struct DrawCtx const* dc, char const* str, u32 len) {
    XGlyphInfo ext;
    XftTextExtentsUtf8(dc->dp, dc->fnt.xfont, (XftChar8*)str, (i32)len, &ext);
    return ext.xOff;
}

u32 get_int_width(struct DrawCtx const* dc, char const* format, u32 i) {
    static u32 const MAX_BUF = 50;
    char buf[MAX_BUF];
    snprintf(buf, MAX_BUF, format, i);
    return get_string_width(dc, buf, strlen(buf));
}

void draw_selection_circle(
    struct DrawCtx* dc,
    struct SelectionCircle const* sc,
    i32 const pointer_x,
    i32 const pointer_y
) {
    if (!sc->is_active) {
        return;
    }

    i32 const outer_r = (i32)SELECTION_CIRCLE.outer_r_px;
    i32 const inner_r = (i32)SELECTION_CIRCLE.inner_r_px;

    XSetLineAttributes(
        dc->dp,
        dc->screen_gc,
        SELECTION_CIRCLE.line_w,
        SELECTION_CIRCLE.line_style,
        SELECTION_CIRCLE.cap_style,
        SELECTION_CIRCLE.join_style
    );

    XSetForeground(dc->dp, dc->screen_gc, COL_BG(dc, SchmNorm));
    XFillArc(
        dc->dp,
        dc->window,
        dc->screen_gc,
        sc->x - outer_r,
        sc->y - outer_r,
        outer_r * 2,
        outer_r * 2,
        0,
        360 * 64
    );

    {
        double const segment_rad = PI * 2 / MAX(1, sc->item_count);
        double const segment_deg = segment_rad / PI * 180;

        // item images
        for (u32 item = 0; item < sc->item_count; ++item) {
            XImage* image = images[sc->items[item].icon];
            assert(image != NULL);

            XPutImage(
                dc->dp,
                dc->window,
                dc->screen_gc,
                image,
                0,
                0,
                (i32)(sc->x
                      + cos(-segment_rad * (item + 0.5))
                          * ((outer_r + inner_r) * 0.5)
                      - image->width / 2.0),
                (i32)(sc->y
                      + sin(-segment_rad * (item + 0.5))
                          * ((outer_r + inner_r) * 0.5)
                      - image->height / 2.0),
                image->width,
                image->height
            );
        }

        // selected item fill
        i32 const current_item = sel_circ_curr_item(sc, pointer_x, pointer_y);
        if (current_item != NIL) {
            XSetForeground(dc->dp, dc->screen_gc, COL_BG(dc, SchmFocus));
            XFillArc(
                dc->dp,
                dc->window,
                dc->screen_gc,
                sc->x - outer_r,
                sc->y - outer_r,
                outer_r * 2,
                outer_r * 2,
                (i32)(current_item * segment_deg) * 64,
                (i32)segment_deg * 64
            );
            XSetForeground(dc->dp, dc->screen_gc, COL_BG(dc, SchmNorm));
            XFillArc(
                dc->dp,
                dc->window,
                dc->screen_gc,
                sc->x - inner_r,
                sc->y - inner_r,
                inner_r * 2,
                inner_r * 2,
                (i32)(current_item * segment_deg) * 64,
                (i32)segment_deg * 64
            );
        }

        if (sc->item_count >= 2) {  // segment lines
            XSetForeground(dc->dp, dc->screen_gc, COL_FG(dc, SchmNorm));
            for (u32 line_num = 0; line_num < sc->item_count; ++line_num) {
                XDrawLine(
                    dc->dp,
                    dc->window,
                    dc->screen_gc,
                    sc->x + (i32)(cos(segment_rad * line_num) * inner_r),
                    sc->y + (i32)(sin(segment_rad * line_num) * inner_r),
                    sc->x + (i32)(cos(segment_rad * line_num) * outer_r),
                    sc->y + (i32)(sin(segment_rad * line_num) * outer_r)
                );
            }
        }
    }

    /* lines */ {
        XSetForeground(dc->dp, dc->screen_gc, COL_FG(dc, SchmNorm));
        XDrawArc(
            dc->dp,
            dc->window,
            dc->screen_gc,
            sc->x - inner_r,
            sc->y - inner_r,
            inner_r * 2,
            inner_r * 2,
            0,
            360 * 64
        );

        XDrawArc(
            dc->dp,
            dc->window,
            dc->screen_gc,
            sc->x - outer_r,
            sc->y - outer_r,
            outer_r * 2,
            outer_r * 2,
            0,
            360 * 64
        );
    }
}

void update_screen(struct Ctx* ctx) {
    struct ToolCtx* tc = &CURR_TC(ctx);
    struct DrawCtx* dc = &ctx->dc;
    /* draw canvas */ {
        fill_rect(
            dc,
            (Pair) {0, 0},
            (Pair) {(i32)dc->width, (i32)dc->height},
            WINDOW.background_argb
        );
        /* put scaled image */ {
            //  https://stackoverflow.com/a/66896097
            if (dc->cache.pm == 0 || dc->cache.pm_w != dc->cv.im->width
                || dc->cache.pm_h != dc->cv.im->height) {
                if (dc->cache.pm != 0) {
                    XFreePixmap(dc->dp, dc->cache.pm);
                }
                dc->cache.pm = XCreatePixmap(
                    dc->dp,
                    dc->window,
                    dc->cv.im->width,
                    dc->cv.im->height,
                    dc->vinfo.depth
                );
                dc->cache.pm_w = dc->cv.im->width;
                dc->cache.pm_h = dc->cv.im->height;
            }
            // clang-format off
            XPutImage(
                dc->dp,
                dc->cache.pm,
                dc->screen_gc,
                dc->cv.im,
                0, 0,
                0, 0,
                dc->cv.im->width, dc->cv.im->height
            );
            // clang-format on

            Picture src_pict = XRenderCreatePicture(
                dc->dp,
                dc->cache.pm,
                dc->xrnd_pic_format,
                0,
                &(XRenderPictureAttributes) {.subwindow_mode = IncludeInferiors}
            );
            Picture dst_pict = XRenderCreatePicture(
                dc->dp,
                dc->back_buffer,
                dc->xrnd_pic_format,
                0,
                &(XRenderPictureAttributes) {.subwindow_mode = IncludeInferiors}
            );

            double const z = 1.0 / ZOOM_C(dc);
            XRenderSetPictureTransform(
                dc->dp,
                src_pict,
                &(XTransform) {{
                    {XDoubleToFixed(z), XDoubleToFixed(0), XDoubleToFixed(0)},
                    {XDoubleToFixed(0), XDoubleToFixed(z), XDoubleToFixed(0)},
                    {XDoubleToFixed(0), XDoubleToFixed(0), XDoubleToFixed(1)},
                }}
            );

            // clang-format off
            XRenderComposite(
                dc->dp, PictOpSrc,
                src_pict, 0,
                dst_pict,
                0, 0,
                0, 0,
                dc->cv.scroll.x, dc->cv.scroll.y,
                (u32)(dc->cv.im->width * ZOOM_C(dc)), (u32)(dc->cv.im->height * ZOOM_C(dc))
            );
            // clang-format on

            XRenderFreePicture(dc->dp, src_pict);
            XRenderFreePicture(dc->dp, dst_pict);
        }
    }
    /* current selection */ {
        if (HAS_SELECTION(tc)) {
            struct SelectionData sd = tc->d.sel;
            Pair p = {MIN(sd.begin.x, sd.end.x), MIN(sd.begin.y, sd.end.y)};
            Pair dim = {
                MAX(sd.begin.x, sd.end.x) - p.x,
                MAX(sd.begin.y, sd.end.y) - p.y
            };
            if (!SELECTION_DRAGGING(tc) || SELECTION_TOOL.draw_while_drag) {
                draw_rect(
                    dc,
                    point_from_cv_to_scr(dc, p),
                    point_from_cv_to_scr_no_move(dc, dim),
                    SELECTION_TOOL.rect_argb,
                    SELECTION_TOOL.line_w,
                    SELECTION_TOOL.line_style,
                    SELECTION_TOOL.cap_style,
                    SELECTION_TOOL.join_style
                );
            }
            if (SELECTION_DRAGGING(tc)) {
                i32 dx = sd.drag_to.x - sd.drag_from.x;
                i32 dy = sd.drag_to.y - sd.drag_from.y;
                draw_rect(
                    dc,
                    point_from_cv_to_scr_xy(dc, p.x + dx, p.y + dy),
                    point_from_cv_to_scr_no_move(dc, dim),
                    SELECTION_TOOL.drag_argb,
                    SELECTION_TOOL.line_w,
                    SELECTION_TOOL.line_style,
                    SELECTION_TOOL.cap_style,
                    SELECTION_TOOL.join_style
                );
            }
        }
    }
    if (WINDOW.anchor_size && tc->sdata.anchor.x != NIL
        && !ctx->input.is_dragging) {
        i32 const size = WINDOW.anchor_size;
        Pair center = point_from_cv_to_scr(dc, tc->sdata.anchor);
        Pair lt = (Pair) {center.x - size, center.y - size};
        Pair lb = (Pair) {center.x - size, center.y + size};
        Pair rt = (Pair) {center.x + size, center.y - size};
        Pair rb = (Pair) {center.x + size, center.y + size};
        draw_line(dc, lt, rb, SchmNorm, True);
        draw_line(dc, lb, rt, SchmNorm, True);
    }

    update_statusline(ctx);  // backbuffer swaped here
}

void update_statusline(struct Ctx* ctx) {
    struct DrawCtx* dc = &ctx->dc;
    struct ToolCtx* tc = &CURR_TC(ctx);
    u32 const statusline_h = get_statusline_height(dc);
    fill_rect(
        dc,
        (Pair) {0, (i32)(dc->height - statusline_h)},
        (Pair) {(i32)dc->width, (i32)statusline_h},
        COL_BG(&ctx->dc, SchmNorm)
    );
    if (ctx->input.t == InputT_Console) {
        char const* command = ctx->input.d.cl.cmdarr;
        usize const command_len = arrlen(command);
        // extra 2 for prompt and terminator
        char* cl_str_dyn = ecalloc(2 + command_len, sizeof(char));
        cl_str_dyn[0] = ':';
        cl_str_dyn[command_len + 1] = '\0';
        memcpy(cl_str_dyn + 1, command, command_len);
        i32 const user_cmd_w =
            (i32)get_string_width(&ctx->dc, cl_str_dyn, command_len + 1);
        i32 const cmd_y = (i32)(ctx->dc.height - STATUSLINE.padding_bottom);
        draw_string(&ctx->dc, cl_str_dyn, (Pair) {0, cmd_y}, SchmNorm, False);
        if (ctx->input.d.cl.compls_arr) {
            draw_string(
                &ctx->dc,
                ctx->input.d.cl.compls_arr[ctx->input.d.cl.compls_curr],
                (Pair) {user_cmd_w, cmd_y},
                SchmFocus,
                False
            );
        }
        str_free(&cl_str_dyn);
    } else {
        static u32 const gap = 5;
        static u32 const small_gap = gap / 2;
        // widths of captions
        // XXX uses 'F' as average character
        u32 const col_count_w = get_string_width(dc, "/", 1)
            + get_int_width(dc, "%d", MAX_COLORS) * 2 + gap;
        u32 tcs_w = 0;
        /* tcs_w */ {
            for (i32 tc_name = 1; tc_name <= TCS_NUM; ++tc_name) {
                tcs_w += get_int_width(dc, "%d", tc_name) + small_gap;
            }
            tcs_w += gap;
        }
        u32 const col_name_w = get_string_width(dc, "#FFFFFF", 7) + gap;
        // fixed length
        u32 const input_state_w = get_string_width(dc, "FFF", 3) + gap;
        u32 const tool_name_w = get_string_width(dc, "FFFFFFF", 7) + gap;
        // left **bottom** corners of captions
        Pair const tcs_c = {0, (i32)(dc->height - STATUSLINE.padding_bottom)};
        Pair const input_state_c = {(i32)(tcs_c.x + tcs_w), tcs_c.y};
        Pair const tool_name_c = {
            (i32)(input_state_c.x + input_state_w),
            input_state_c.y
        };
        Pair const line_w_c = {
            (i32)(tool_name_c.x + tool_name_w),
            tool_name_c.y
        };
        Pair const col_count_c = {(i32)(dc->width - col_count_w), line_w_c.y};
        Pair const col_c = {(i32)(col_count_c.x - col_name_w), col_count_c.y};
        // colored rectangle
        static u32 const col_rect_w = 30;
        static u32 const col_value_size = 1 + 6;

        XSetBackground(dc->dp, dc->screen_gc, COL_BG(&ctx->dc, SchmNorm));
        XSetForeground(dc->dp, dc->screen_gc, COL_FG(&ctx->dc, SchmNorm));
        /* tc */ {
            i32 x = tcs_c.x;
            for (i32 tc_name = 1; tc_name <= TCS_NUM; ++tc_name) {
                draw_int(
                    dc,
                    tc_name,
                    (Pair) {x, tcs_c.y},
                    ctx->curr_tc == (tc_name - 1) ? SchmFocus : SchmNorm,
                    False
                );
                x += (i32)(get_int_width(dc, "%d", tc_name) + small_gap);
            }
        }
        /* input state */ {
            draw_string(
                dc,
                ctx->input.t == InputT_Interact      ? "INT"
                    : ctx->input.t == InputT_Color   ? "COL"
                    : ctx->input.t == InputT_Console ? "CMD"
                                                     : "???",
                input_state_c,
                SchmNorm,
                False
            );
        }
        draw_string(dc, tc_get_tool_name(tc), tool_name_c, SchmNorm, False);
        draw_int(dc, (i32)tc->sdata.line_w, line_w_c, SchmNorm, False);
        /* color */ {
            char col_value[col_value_size + 1];
            sprintf(col_value, "#%06X", *tc_curr_col(tc) & 0xFFFFFF);
            draw_string(dc, col_value, col_c, SchmNorm, False);
            /* color count */ {
                // FIXME how it possible
                char col_count[digit_count(MAX_COLORS) * 2 + 1 + 1];
                sprintf(
                    col_count,
                    "%d/%td",
                    tc->sdata.curr_col + 1,
                    arrlen(tc->sdata.colarr)
                );
                draw_string(dc, col_count, col_count_c, SchmNorm, False);
            }
            if (ctx->input.t == InputT_Color) {
                static u32 const hash_w = 1;
                u32 const curr_dig = ctx->input.d.col.current_digit;
                char const col_digit_value[] =
                    {[0] = col_value[curr_dig + hash_w], [1] = '\0'};
                draw_string(
                    dc,
                    col_digit_value,
                    (Pair
                    ) {col_c.x
                           + (i32
                           )get_string_width(dc, col_value, curr_dig + hash_w),
                       col_c.y},
                    SchmFocus,
                    False
                );
            }
            fill_rect(
                dc,
                (Pair
                ) {(i32)(dc->width - col_name_w - col_rect_w - col_count_w),
                   (i32)(dc->height - statusline_h)},
                (Pair) {(i32)col_rect_w, (i32)statusline_h},
                *tc_curr_col(tc)
            );
        }
    }

    XdbeSwapBuffers(
        ctx->dc.dp,
        &(XdbeSwapInfo) {
            .swap_window = ctx->dc.window,
            .swap_action = 0,
        },
        1
    );
}

// FIXME DRY
void show_message(struct Ctx* ctx, char const* msg) {
    u32 const statusline_h =
        ctx->dc.fnt.xfont->ascent + STATUSLINE.padding_bottom;
    fill_rect(
        &ctx->dc,
        (Pair) {0, (i32)(ctx->dc.height - statusline_h)},
        (Pair) {(i32)ctx->dc.width, (i32)statusline_h},
        COL_BG(&ctx->dc, SchmNorm)
    );
    draw_string(
        &ctx->dc,
        msg,
        (Pair) {0, (i32)(ctx->dc.height - STATUSLINE.padding_bottom)},
        SchmNorm,
        False
    );
    XdbeSwapBuffers(
        ctx->dc.dp,
        &(XdbeSwapInfo) {
            .swap_window = ctx->dc.window,
            .swap_action = 0,
        },
        1
    );
}

void show_message_va(struct Ctx* ctx, char const* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    char* msg_dyn = str_new_va(fmt, ap);
    show_message(ctx, msg_dyn);
    str_free(&msg_dyn);
    va_end(ap);
}

struct Ctx ctx_init(Display* dp) {
    return (struct Ctx) {
        .dc =
            (struct DrawCtx) {
                .dp = dp,
                .width = CANVAS.default_width,
                .height = CANVAS.default_height,
                .cv =
                    (struct Canvas) {
                        .im = NULL,
                        .type = IMT_Png,  // save as png by default
                        .zoom = 0,
                        .scroll = {0, 0},
                    },
                .cache = (struct Cache) {.pm = 0},
                .png_compression_level = PNG_DEFAULT_COMPRESSION,
                .jpg_quality_level = JPG_DEFAULT_QUALITY,
            },
        .input = (struct Input) {.t = InputT_Interact},
        .sel_buf.im = NULL,
        .tcarr = NULL,
        .curr_tc = 0,
        .hist_nextarr = NULL,
        .hist_prevarr = NULL,
    };
}

void setup(Display* dp, struct Ctx* ctx) {
    assert(dp);
    assert(ctx);

    /* init arrays */ {
        for (i32 i = 0; i < TCS_NUM; ++i) {
            struct ToolCtx tc = {
                .sdata.colarr = NULL,
                .sdata.curr_col = 0,
                .sdata.prev_col = 0,
                .sdata.line_w = TOOLS.default_line_w,
            };
            arrpush(ctx->tcarr, tc);
            arrpush(ctx->tcarr[i].sdata.colarr, 0xFF000000);
            arrpush(ctx->tcarr[i].sdata.colarr, 0xFFFFFFFF);
        }
    }

    /* atoms */ {
        atoms[A_Clipboard] = XInternAtom(dp, "CLIPBOARD", False);
        atoms[A_Targets] = XInternAtom(dp, "TARGETS", False);
        atoms[A_Utf8string] = XInternAtom(dp, "UTF8_STRING", False);
        atoms[A_ImagePng] = XInternAtom(dp, "image/png", False);
    }

    /* xrender */ {
        ctx->dc.xrnd_pic_format =
            XRenderFindStandardFormat(ctx->dc.dp, PictStandardARGB32);
        assert(ctx->dc.xrnd_pic_format);
    }

    i32 screen = DefaultScreen(dp);
    Window root = DefaultRootWindow(dp);

    i32 result = XMatchVisualInfo(dp, screen, 32, TrueColor, &ctx->dc.vinfo);
    assert(result != 0);

    ctx->dc.colmap = XCreateColormap(dp, root, ctx->dc.vinfo.visual, AllocNone);

    /* create window */
    ctx->dc.window = XCreateWindow(
        dp,
        root,
        0,
        0,
        ctx->dc.width,
        ctx->dc.height,
        0,  // border width
        ctx->dc.vinfo.depth,
        InputOutput,
        ctx->dc.vinfo.visual,
        CWColormap | CWBorderPixel | CWBackPixel | CWEventMask,
        &(XSetWindowAttributes
        ) {.colormap = ctx->dc.colmap,
           .border_pixel = 0,
           .background_pixel = 0xFFFF00FF,
           .event_mask = ButtonPressMask | ButtonReleaseMask | KeyPressMask
               | ExposureMask | PointerMotionMask | StructureNotifyMask}
    );
    ctx->dc.screen_gc = XCreateGC(dp, ctx->dc.window, 0, 0);

    XSetWMName(
        dp,
        ctx->dc.window,
        &(XTextProperty
        ) {.value = (unsigned char*)title,
           .nitems = strlen(title),
           .format = 8,
           .encoding = atoms[A_Utf8string]}
    );

    /* X input method setup */ {
        // from https://gist.github.com/baines/5a49f1334281b2685af5dcae81a6fa8a
        XSetLocaleModifiers("");

        ctx->dc.xim = XOpenIM(dp, 0, 0, 0);
        if (!ctx->dc.xim) {
            // fallback to internal input method
            XSetLocaleModifiers("@im=none");
            ctx->dc.xim = XOpenIM(dp, 0, 0, 0);
        }

        ctx->dc.xic = XCreateIC(
            ctx->dc.xim,
            XNInputStyle,
            XIMPreeditNothing | XIMStatusNothing,
            XNClientWindow,
            ctx->dc.window,
            XNFocusWindow,
            ctx->dc.window,
            NULL
        );
        XSetICFocus(ctx->dc.xic);
    }

    ctx->dc.back_buffer = XdbeAllocateBackBufferName(dp, ctx->dc.window, 0);

    /* turn on protocol support */ {
        Atom wm_delete_window = XInternAtom(dp, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dp, ctx->dc.window, &wm_delete_window, 1);
    }

    if (!fnt_set(&ctx->dc, FONT_NAME)) {
        die("failed to load default font: %s", FONT_NAME);
    }

    /* schemes */ {
        ctx->dc.schemes_dyn = ecalloc(SchmLast, sizeof(ctx->dc.schemes_dyn[0]));
        for (i32 i = 0; i < SchmLast; ++i) {
            for (i32 j = 0; j < 2; ++j) {
                if (!XftColorAllocValue(
                        dp,
                        ctx->dc.vinfo.visual,
                        ctx->dc.colmap,
                        &SCHEMES[i][j],
                        j ? &ctx->dc.schemes_dyn[i].bg
                          : &ctx->dc.schemes_dyn[i].fg
                    )) {
                    die("can't alloc color");
                };
            }
        }
    }

    /* static images */ {
        for (i32 i = 0; i < I_Last; ++i) {
            struct IconData icon = get_icon_data(i);
            images[i] = read_file_from_memory(
                &ctx->dc,
                icon.data,
                icon.len,
                COL_BG(&ctx->dc, SchmNorm)
            );
        }
    }

    /* canvas */ {
        XGCValues canvas_gc_vals = {
            .line_style = LineSolid,
            .line_width = 5,
            .cap_style = CapButt,
            .fill_style = FillSolid
        };
        ctx->dc.gc = XCreateGC(
            dp,
            ctx->dc.window,
            GCForeground | GCBackground | GCFillStyle | GCLineStyle
                | GCLineWidth | GCCapStyle | GCJoinStyle,
            &canvas_gc_vals
        );
        // read canvas data from file or create empty
        if (ctx->finp.path_dyn) {
            XImage* im = read_file_from_path(&ctx->dc, ctx->finp.path_dyn, 0);
            if (im) {
                canvas_load(&ctx->dc, im, ctx->finp.path_dyn);
            } else {
                die("xpaint: failed to read input file");
            }
        } else {
            Pixmap data = XCreatePixmap(
                dp,
                ctx->dc.window,
                ctx->dc.width,
                ctx->dc.height,
                ctx->dc.vinfo.depth
            );
            ctx->dc.cv.im = XGetImage(
                dp,
                data,
                0,
                0,
                ctx->dc.width,
                ctx->dc.height,
                AllPlanes,
                ZPixmap
            );
            XFreePixmap(dp, data);
            // initial canvas color
            canvas_fill(ctx, CANVAS.background_argb);
        }

        ctx->dc.width = CLAMP(
            ctx->dc.cv.im->width,
            WINDOW.min_launch_size.x,
            WINDOW.max_launch_size.x
        );
        ctx->dc.height = CLAMP(
            ctx->dc.cv.im->height + get_statusline_height(&ctx->dc),
            WINDOW.min_launch_size.y,
            WINDOW.max_launch_size.y
        );
        XResizeWindow(dp, ctx->dc.window, ctx->dc.width, ctx->dc.height);
    }

    for (i32 i = 0; i < TCS_NUM; ++i) {
        tc_set_tool(&ctx->tcarr[i], Tool_Pencil);
    }
    history_push(&ctx->hist_prevarr, ctx);

    /* show up window */
    XMapRaised(dp, ctx->dc.window);
}

void run(struct Ctx* ctx) {
    static Bool (*const handlers[LASTEvent])(struct Ctx*, XEvent*) = {
        [KeyPress] = &key_press_hdlr,
        [ButtonPress] = &button_press_hdlr,
        [ButtonRelease] = &button_release_hdlr,
        [MotionNotify] = &motion_notify_hdlr,
        [Expose] = &expose_hdlr,
        [DestroyNotify] = &destroy_notify_hdlr,
        [ConfigureNotify] = &configure_notify_hdlr,
        [SelectionRequest] = &selection_request_hdlr,
        [SelectionNotify] = &selection_notify_hdlr,
        [ClientMessage] = &client_message_hdlr,
        [MappingNotify] = &mapping_notify_hdlr,
    };

    Bool running = True;
    XEvent event;

    XSync(ctx->dc.dp, False);
    while (running && !XNextEvent(ctx->dc.dp, &event)) {
        if (XFilterEvent(&event, ctx->dc.window)) {
            continue;
        }
        if (handlers[event.type]) {
            running = handlers[event.type](ctx, &event);
        }
    }
}

Bool button_press_hdlr(struct Ctx* ctx, XEvent* event) {
    XButtonPressedEvent* e = (XButtonPressedEvent*)event;
    if (e->button == XLeftMouseBtn) {
        history_forward(ctx);
    }
    if (CURR_TC(ctx).on_press) {
        CURR_TC(ctx).on_press(ctx, e);
        update_screen(ctx);
    }
    if (e->button == XRightMouseBtn) {
        sel_circ_init(ctx, e->x, e->y);
        draw_selection_circle(&ctx->dc, &ctx->sc, NIL, NIL);
    }

    ctx->input.holding_button = e->button;
    ctx->input.is_holding = True;

    return True;
}

Bool button_release_hdlr(struct Ctx* ctx, XEvent* event) {
    XButtonReleasedEvent* e = (XButtonReleasedEvent*)event;
    if (e->button == XRightMouseBtn) {
        i32 const selected_item = sel_circ_curr_item(&ctx->sc, e->x, e->y);
        if (selected_item != NIL && ctx->sc.items[selected_item].on_select) {
            ctx->sc.items[selected_item].on_select(ctx);
        }
        sel_circ_free(&ctx->sc);
        update_screen(ctx);
        ctx->input.is_holding = False;
        ctx->input.is_dragging = False;
        return True;  // something selected do nothing else
    }

    if (e->button == XMouseScrollDown || e->button == XMouseScrollUp) {
        i32 sign = e->button == XMouseScrollUp ? 1 : -1;
        if (e->state & ControlMask) {
            canvas_change_zoom(&ctx->dc, ctx->input.prev_c, sign);
        } else if (e->state & ShiftMask) {
            ctx->dc.cv.scroll.x -= sign * 10;
        } else {
            ctx->dc.cv.scroll.y += sign * 10;
        }
        update_screen(ctx);
    }

    if (CURR_TC(ctx).on_release) {
        CURR_TC(ctx).on_release(ctx, e);
        update_screen(ctx);
    }

    ctx->input.is_holding = False;
    ctx->input.is_dragging = False;

    return True;
}

Bool destroy_notify_hdlr(struct Ctx* ctx, XEvent* event) {
    return True;
}

Bool expose_hdlr(struct Ctx* ctx, XEvent* event) {
    update_screen(ctx);
    return True;
}

#define HANDLE_KEY_BEGIN(p_xkeypressedevent) \
    { \
        KeySym const handle_key_inp_key_sym = \
            XLookupKeysym(&(p_xkeypressedevent), 0); \
        XKeyPressedEvent const handle_key_inp_event = p_xkeypressedevent;
#define HANDLE_KEY_END() }
#define HANDLE_KEY_CASE_MASK(p_mask, p_key) \
    if (handle_key_inp_event.state & (p_mask) \
        && handle_key_inp_key_sym == (p_key))
#define HANDLE_KEY_CASE_MASK_NOT(p_mask, p_key) \
    if (!(handle_key_inp_event.state & (p_mask)) \
        && handle_key_inp_key_sym == (p_key))
#define HANDLE_KEY_CASE(p_key) if (handle_key_inp_key_sym == (p_key))

static void to_next_input_digit(struct Input* input, Bool is_increment) {
    assert(input->t == InputT_Color);

    if (input->d.col.current_digit == 0 && !is_increment) {
        input->d.col.current_digit = 5;
    } else if (input->d.col.current_digit == 5 && is_increment) {
        input->d.col.current_digit = 0;
    } else {
        input->d.col.current_digit += is_increment ? 1 : -1;
    }
}

Bool key_press_hdlr(struct Ctx* ctx, XEvent* event) {
    XKeyPressedEvent e = event->xkey;
    if (e.type == KeyRelease) {
        return True;
    }

    Status lookup_status;
    KeySym key_sym = NoSymbol;
    char lookup_buf[32] = {0};
    i32 const text_len = Xutf8LookupString(
        ctx->dc.xic,
        &e,
        lookup_buf,
        sizeof(lookup_buf) - 1,
        &key_sym,
        &lookup_status
    );

    if (lookup_status == XBufferOverflow) {
        trace("xpaint: input buffer overflow");
    }

    HANDLE_KEY_BEGIN(e)
    switch (ctx->input.t) {
        case InputT_Interact: {
            HANDLE_KEY_CASE_MASK(ControlMask, XK_z) {
                if (!history_move(ctx, !(e.state & ShiftMask))) {
                    trace("xpaint: can't undo/revert history");
                }
                update_screen(ctx);
            }
            HANDLE_KEY_CASE_MASK(ControlMask, XK_c) {
                if (HAS_SELECTION(&CURR_TC(ctx))) {
                    struct SelectionData* sd = &CURR_TC(ctx).d.sel;
                    XSetSelectionOwner(
                        ctx->dc.dp,
                        atoms[A_Clipboard],
                        ctx->dc.window,
                        CurrentTime
                    );
                    i32 x = MIN(sd->begin.x, sd->end.x);
                    i32 y = MIN(sd->begin.y, sd->end.y);
                    u32 width = MAX(sd->end.x, sd->begin.x) - x;
                    u32 height = MAX(sd->end.y, sd->begin.y) - y;
                    if (ctx->sel_buf.im != NULL) {
                        XDestroyImage(ctx->sel_buf.im);
                    }
                    ctx->sel_buf.im =
                        XSubImage(ctx->dc.cv.im, x, y, width, height);
                    assert(ctx->sel_buf.im != NULL);
                    assert(
                        ctx->sel_buf.im->width == width
                        && ctx->sel_buf.im->height == height
                    );
                    if (ctx->sel_buf.im->red_mask == 0
                        && ctx->sel_buf.im->green_mask == 0
                        && ctx->sel_buf.im->blue_mask == 0) {
                        puts("ximage: XGetImage returned empty masks");
                        ctx->sel_buf.im->red_mask = 0xFF0000;
                        ctx->sel_buf.im->green_mask = 0xFF00;
                        ctx->sel_buf.im->blue_mask = 0xFF;
                    }
                    if (is_verbose_output) {
                        u32 const image_size = ctx->sel_buf.im->bits_per_pixel
                            * ctx->sel_buf.im->height;
                        trace(
                            "\nsize: %d\nwidth: %d\nheight: %d\nbpp: %d\nbbo: %d\n"
                            "format: %d\nred: %lX\nblue: %lX\ngreen %lX\n",
                            image_size,
                            ctx->sel_buf.im->width,
                            ctx->sel_buf.im->height,
                            ctx->sel_buf.im->bits_per_pixel,
                            ctx->sel_buf.im->bitmap_bit_order,
                            ctx->sel_buf.im->format,
                            ctx->sel_buf.im->red_mask,
                            ctx->sel_buf.im->blue_mask,
                            ctx->sel_buf.im->green_mask
                        );
                    }
                } else {
                    trace("^c without selection");
                }
            }
            HANDLE_KEY_CASE_MASK_NOT(ControlMask, XK_c) {
                input_state_set(&ctx->input, InputT_Color);
                update_statusline(ctx);
            }
            HANDLE_KEY_CASE(XK_x) {
                tc_set_curr_col_num(&CURR_TC(ctx), CURR_TC(ctx).sdata.prev_col);
                update_statusline(ctx);
            }
            if (BETWEEN(key_sym, XK_1, XK_9)) {
                u32 val = key_sym - XK_1;
                if (val < TCS_NUM) {
                    ctx->curr_tc = val;
                    update_statusline(ctx);
                }
            }
            if (BETWEEN(key_sym, XK_Left, XK_Down) && e.state & ControlMask) {
                u32 const value = e.state & ShiftMask ? 25 : 5;
                canvas_resize(
                    ctx,
                    (i32)(ctx->dc.cv.im->width
                          + (key_sym == XK_Left        ? -value
                                 : key_sym == XK_Right ? value
                                                       : 0)),
                    (i32)(ctx->dc.cv.im->height
                          + (key_sym == XK_Down     ? -value
                                 : key_sym == XK_Up ? value
                                                    : 0))
                );
                update_screen(ctx);
            }
            HANDLE_KEY_CASE_MASK(ControlMask, XK_equal) {
                canvas_change_zoom(&ctx->dc, ctx->input.prev_c, 1);
                update_screen(ctx);
            }
            HANDLE_KEY_CASE_MASK(ControlMask, XK_minus) {
                canvas_change_zoom(&ctx->dc, ctx->input.prev_c, -1);
                update_screen(ctx);
            }
            // XK_colon not work for some reason
            HANDLE_KEY_CASE_MASK(ShiftMask, XK_semicolon) {
                input_state_set(&ctx->input, InputT_Console);
                update_statusline(ctx);
            }
        } break;

        case InputT_Color: {
            HANDLE_KEY_CASE_MASK(ControlMask, XK_Up) {
                u32 const len = arrlen(CURR_TC(ctx).sdata.colarr);
                if (len != MAX_COLORS) {
                    tc_set_curr_col_num(&CURR_TC(ctx), len);
                    arrpush(CURR_TC(ctx).sdata.colarr, 0xFF000000);
                    update_statusline(ctx);
                }
            }
            HANDLE_KEY_CASE(XK_Right) {
                to_next_input_digit(&ctx->input, True);
                update_statusline(ctx);
            }
            HANDLE_KEY_CASE(XK_Left) {
                to_next_input_digit(&ctx->input, False);
                update_statusline(ctx);
            }
            // change selected color digit with pressed key
            if (strlen(lookup_buf) == 1) {
                u32 val = lookup_buf[0]
                    - (BETWEEN(lookup_buf[0], '0', '9')       ? '0'
                           : BETWEEN(lookup_buf[0], 'a', 'f') ? ('a' - 10)
                           : BETWEEN(lookup_buf[0], 'A', 'F') ? ('A' - 10)
                                                              : 0);
                if (val != lookup_buf[0]) {  // if assigned
                    // selected digit counts from left to
                    // right except alpha (aarrggbb <=> --012345)
                    u32 shift = (5 - ctx->input.d.col.current_digit) * 4;
                    *tc_curr_col(&CURR_TC(ctx)) &= ~(0xF << shift);  // clear
                    *tc_curr_col(&CURR_TC(ctx)) |= val << shift;  // set
                    to_next_input_digit(&ctx->input, True);
                    update_statusline(ctx);
                }
            }
        } break;

        case InputT_Console: {
            struct InputConsoleData* cl = &ctx->input.d.cl;
            if (key_sym == XK_Return) {
                if (cl->compls_arr) {  // apply completion
                    char* compl = cl->compls_arr[cl->compls_curr];
                    while (*compl ) {
                        arrpush(cl->cmdarr, *compl );
                        ++compl ;
                    }
                    cl_compls_free(cl);
                    update_statusline(ctx);
                } else {  // apply command
                    char* cmd_dyn = cl_cmd_get_str_dyn(cl);
                    input_state_set(&ctx->input, InputT_Interact);
                    ClCPrsResult res = cl_cmd_parse(ctx, cmd_dyn);
                    str_free(&cmd_dyn);
                    Bool is_exit = False;
                    switch (res.t) {
                        case ClCPrs_Ok: {
                            struct ClCommand* cmd = &res.d.ok;
                            ClCPrcResult res = cl_cmd_process(ctx, cmd);
                            update_screen(ctx);
                            if (res.bit_status & ClCPrc_Msg) {
                                show_message(ctx, res.msg_dyn);
                                str_free(&res.msg_dyn);  // XXX member free
                            }
                            is_exit = (Bool)(res.bit_status & ClCPrc_Exit);
                        } break;
                        case ClCPrs_ENoArg: {
                            show_message(ctx, "no command");
                        } break;
                        case ClCPrs_EInvArg: {
                            show_message_va(
                                ctx,
                                "invalid arg '%s'",
                                res.d.invarg
                            );
                        } break;
                        case ClCPrs_ENoSubArg: {
                            show_message_va(
                                ctx,
                                "provide value to '%s' cmd",
                                res.d.nosubarg.arg_dyn
                            );
                        } break;
                        case ClCPrs_EInvSubArg: {
                            show_message_va(
                                ctx,
                                "invalid arg '%s' provided to '%s' cmd",
                                res.d.invsubarg.inv_val_dyn,
                                res.d.invsubarg.arg_dyn
                            );
                        } break;
                    }

                    cl_cmd_parse_res_free(&res);
                    if (is_exit) {
                        return False;
                    }
                }
            } else if (key_sym == XK_Tab) {
                if (!cl->compls_valid) {
                    cl_compls_update(cl);
                } else {
                    usize max = arrlen(cl->compls_arr);
                    if (max) {
                        cl->compls_curr = (cl->compls_curr + 1) % max;
                    }
                }
                update_statusline(ctx);
            } else if (key_sym == XK_BackSpace) {
                cl_pop(cl);
                update_statusline(ctx);
            } else if (key_sym != XK_Escape) {
                if ((lookup_status == XLookupBoth
                     || lookup_status == XLookupChars)
                    && !(iscntrl((u32)*lookup_buf))) {
                    for (i32 i = 0; i < text_len; ++i) {
                        cl_push(cl, (char)(lookup_buf[i] & 0xFF));
                    }
                    update_statusline(ctx);
                }
            }
        } break;

        default: assert(!"unknown input state");
    }
    // any state except console input
    if (ctx->input.t != InputT_Console) {
        HANDLE_KEY_CASE(XK_q) {
            return False;
        }
        if ((key_sym == XK_Up || key_sym == XK_Down)
            && !(e.state & ControlMask)) {
            u32 col_num = arrlen(CURR_TC(ctx).sdata.colarr);
            assert(col_num != 0);
            tc_set_curr_col_num(
                &CURR_TC(ctx),
                (CURR_TC(ctx).sdata.curr_col + (key_sym == XK_Up ? 1 : -1))
                    % col_num
            );
            update_statusline(ctx);
        }
        HANDLE_KEY_CASE_MASK(ControlMask, XK_s) {  // save to current file
            if (save_file(&ctx->dc, ctx->dc.cv.type, ctx->fout.path_dyn)) {
                trace("xpaint: file saved");
            } else {
                trace("xpaint: failed to save image");
            }
        }
    }
    // independent
    HANDLE_KEY_CASE(XK_Escape) {
        input_state_set(&ctx->input, InputT_Interact);
        update_statusline(ctx);
    }
    HANDLE_KEY_END()

    return True;
}

Bool mapping_notify_hdlr(struct Ctx* ctx, XEvent* event) {
    XRefreshKeyboardMapping(&event->xmapping);
    return True;
}

Bool motion_notify_hdlr(struct Ctx* ctx, XEvent* event) {
    XMotionEvent* e = (XMotionEvent*)event;

    if (ctx->input.is_holding) {
        if (!ctx->input.is_dragging) {
            ctx->input.is_dragging = True;
            ctx->input.drag_from =
                point_from_scr_to_cv_xy(&ctx->dc, e->x, e->y);
        }
        if (CURR_TC(ctx).on_drag) {
            struct timeval current_time;
            gettimeofday(&current_time, 0x0);
            if (current_time.tv_usec - ctx->input.last_proc_drag_ev_us
                >= DRAG_PERIOD_US) {
                CURR_TC(ctx).on_drag(ctx, e);
                ctx->input.last_proc_drag_ev_us = current_time.tv_usec;
                update_screen(ctx);
            }
        }
        if (ctx->input.holding_button == XMiddleMouseBtn) {
            ctx->dc.cv.scroll.x += e->x - ctx->input.prev_c.x;
            ctx->dc.cv.scroll.y += e->y - ctx->input.prev_c.y;
            update_screen(ctx);
        }
    } else {
        if (CURR_TC(ctx).on_move) {
            CURR_TC(ctx).on_move(ctx, e);
            ctx->input.last_proc_drag_ev_us = 0;
            update_screen(ctx);
        }
    }

    draw_selection_circle(&ctx->dc, &ctx->sc, e->x, e->y);

    ctx->input.prev_c.x = e->x;
    ctx->input.prev_c.y = e->y;

    return True;
}

Bool configure_notify_hdlr(struct Ctx* ctx, XEvent* event) {
    ctx->dc.width = event->xconfigure.width;
    ctx->dc.height = event->xconfigure.height;

    // backbuffer resizes automatically

    return True;
}

Bool selection_request_hdlr(struct Ctx* ctx, XEvent* event) {
    XSelectionRequestEvent request = event->xselectionrequest;

    if (XGetSelectionOwner(ctx->dc.dp, atoms[A_Clipboard]) == ctx->dc.window
        && request.selection == atoms[A_Clipboard]
        && request.property != None) {
        if (request.target == atoms[A_Targets]) {
            Atom avaliable_targets[] = {atoms[A_ImagePng]};
            XChangeProperty(
                request.display,
                request.requestor,
                request.property,
                XA_ATOM,
                32,
                PropModeReplace,
                (unsigned char const*)avaliable_targets,
                LENGTH(avaliable_targets)
            );
        } else if (request.target == atoms[A_ImagePng]) {
            trace("requested image/png");
            u8* rgb_dyn = ximage_to_rgb(ctx->sel_buf.im, False);
            i32 png_data_size = NIL;
            stbi_uc* png_imdyn = stbi_write_png_to_mem(
                rgb_dyn,
                0,
                ctx->sel_buf.im->width,
                ctx->sel_buf.im->height,
                3,
                &png_data_size
            );
            if (png_imdyn == NULL) {
                die("stbi: %s", stbi_failure_reason());
            }

            XChangeProperty(
                request.display,
                request.requestor,
                request.property,
                request.target,
                8,
                PropModeReplace,
                png_imdyn,
                png_data_size
            );

            free(rgb_dyn);
            stbi_image_free(png_imdyn);
        }
        XSelectionEvent sendEvent = {
            .type = SelectionNotify,
            .serial = request.serial,
            .send_event = request.send_event,
            .display = request.display,
            .requestor = request.requestor,
            .selection = request.selection,
            .target = request.target,
            .property = request.property,
            .time = request.time,
        };
        XSendEvent(ctx->dc.dp, request.requestor, 0, 0, (XEvent*)&sendEvent);
    } else {
        trace("xpaint: invalid selection request event received");
    }
    return True;
}

Bool selection_notify_hdlr(struct Ctx* ctx, XEvent* event) {
    static Atom target = None;
    trace("selection notify handler");

    XSelectionEvent selection = event->xselection;
    target = None;

    if (selection.property != None) {
        Atom actual_type = 0;
        i32 actual_format = 0;
        u64 bytes_after = 0;
        Atom* data_xdyn = NULL;
        u64 count = 0;
        XGetWindowProperty(
            ctx->dc.dp,
            ctx->dc.window,
            atoms[A_Clipboard],
            0,
            LONG_MAX,
            False,
            AnyPropertyType,
            &actual_type,
            &actual_format,
            &count,
            &bytes_after,
            (unsigned char**)&data_xdyn
        );

        if (selection.target == atoms[A_Targets]) {
            for (u32 i = 0; i < count; ++i) {
                Atom li = data_xdyn[i];
                // leak
                trace("Requested target: %s\n", XGetAtomName(ctx->dc.dp, li));
                if (li == atoms[A_Utf8string]) {
                    target = atoms[A_Utf8string];
                    break;
                }
            }
            if (target != None) {
                XConvertSelection(
                    ctx->dc.dp,
                    atoms[A_Clipboard],
                    target,
                    atoms[A_Clipboard],
                    ctx->dc.window,
                    CurrentTime
                );
            }
        } else if (selection.target == target) {
            // the data is in {data, count}
        }

        if (data_xdyn) {
            XFree(data_xdyn);
        }
    }
    return True;
}

Bool client_message_hdlr(struct Ctx* ctx, XEvent* event) {
    // close window on request
    return False;
}

void cleanup(struct Ctx* ctx) {
    /* global */ {
        for (u32 i = 0; i < I_Last; ++i) {
            if (images[i] != NULL) {
                XDestroyImage(images[i]);
            }
        }
    }
    /* file paths */ {
        file_ctx_free(&ctx->fout);
        file_ctx_free(&ctx->finp);
    }
    /* SelectionBuffer */ {
        if (ctx->sel_buf.im != NULL) {
            XDestroyImage(ctx->sel_buf.im);
        }
    }
    /* History */ {
        historyarr_clear(ctx->dc.dp, &ctx->hist_nextarr);
        historyarr_clear(ctx->dc.dp, &ctx->hist_prevarr);
    }
    /* ToolCtx */ {
        for (i32 i = 0; i < TCS_NUM; ++i) {
            tc_free(&ctx->tcarr[i]);
        }
        arrfree(ctx->tcarr);
    }
    /* Input */ {
        if (ctx->input.t == InputT_Console) {
            cl_free(&ctx->input.d.cl);
        }
    }
    /* DrawCtx */ {
        /* Cache */ {
            if (ctx->dc.cache.pm != 0) {
                XFreePixmap(ctx->dc.dp, ctx->dc.cache.pm);
            }
        }
        /* Scheme */ {  // depends on VisualInfo and Colormap
            for (i32 i = 0; i < SchmLast; ++i) {
                for (i32 j = 0; j < 2; ++j) {
                    XftColorFree(
                        ctx->dc.dp,
                        ctx->dc.vinfo.visual,
                        ctx->dc.colmap,
                        j ? &ctx->dc.schemes_dyn[i].bg
                          : &ctx->dc.schemes_dyn[i].fg
                    );
                }
            }
            free(ctx->dc.schemes_dyn);
        }
        fnt_free(ctx->dc.dp, &ctx->dc.fnt);
        canvas_free(ctx->dc.dp, &ctx->dc.cv);
        XdbeDeallocateBackBufferName(ctx->dc.dp, ctx->dc.back_buffer);
        XFreeGC(ctx->dc.dp, ctx->dc.gc);
        XFreeGC(ctx->dc.dp, ctx->dc.screen_gc);
        XFreeColormap(ctx->dc.dp, ctx->dc.colmap);
        XDestroyWindow(ctx->dc.dp, ctx->dc.window);
    }
}
