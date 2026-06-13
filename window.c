/*
 * window - scriptable X11 display server
 * usage:
 *   PIPE=$(window)          # spawn window, get pipe path
 *   window $PIPE <command>  # send command to window
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <stdarg.h>
#include <math.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/cursorfont.h>
#include <time.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define MAX_CMD    4096
#define MAX_TOKENS 64

/* ── global state ───────────────────────────────────────────── */

static Display *dpy;
static int      scr;
static Window   win;
static GC       gc;
static Pixmap   buf       = 0;
static Atom     wm_delete;
static char     pipe_path[256]   = "";
static char     events_path[256] = "";
static int      evfd = -1;   /* write end of events FIFO */

/* ── asset store ────────────────────────────────────────────── */

#define MAX_ASSETS 128

typedef enum { ASSET_IMAGE, ASSET_STRIP, ASSET_FONT } AssetType;

typedef struct {
    char        name[64];
    AssetType   type;
    Pixmap      pixmap;
    Picture     picture;   /* XRender picture for alpha compositing */
    int         w, h;
    /* strip only */
    int         src_x, src_y, frame_w, frame_h, frame_count;
    /* font only */
    XftFont    *xft_font;
} Asset;

static Asset assets[MAX_ASSETS];
static int   nassets    = 0;
static Asset *active_font = NULL;

static Asset *asset_find(const char *name)
{
    for (int i = 0; i < nassets; i++)
        if (!strcmp(assets[i].name, name)) return &assets[i];
    return NULL;
}

static Drawable tgt(void);  /* forward declaration */

static Picture make_picture(Pixmap pix, int has_alpha)
{
    XRenderPictFormat *fmt = has_alpha
        ? XRenderFindStandardFormat(dpy, PictStandardARGB32)
        : XRenderFindStandardFormat(dpy, PictStandardRGB24);
    XRenderPictureAttributes pa;
    pa.repeat = RepeatNone;
    return XRenderCreatePicture(dpy, pix, fmt, CPRepeat, &pa);
}

/* load RGBA pixel data into a Pixmap and XRender Picture */
static int load_image_data(unsigned char *pixels, int w, int h,
                            Pixmap *pix_out, Picture *pic_out)
{
    /* create pixmap */
    Pixmap pix = XCreatePixmap(dpy, win, w, h, 32);

    /* create XImage from RGBA data - convert to ARGB (X11 byte order) */
    char *xdata = malloc(w * h * 4);
    if (!xdata) return 0;
    for (int i = 0; i < w * h; i++) {
        unsigned char r = pixels[i*4+0];
        unsigned char g = pixels[i*4+1];
        unsigned char b = pixels[i*4+2];
        unsigned char a = pixels[i*4+3];
        /* store as BGRA in memory (little-endian ARGB) */
        xdata[i*4+0] = b;
        xdata[i*4+1] = g;
        xdata[i*4+2] = r;
        xdata[i*4+3] = a;
    }

    Visual *vis32 = NULL;
    XVisualInfo vinfo;
    if (XMatchVisualInfo(dpy, scr, 32, TrueColor, &vinfo))
        vis32 = vinfo.visual;

    XImage *img = XCreateImage(dpy, vis32 ? vis32 : DefaultVisual(dpy, scr),
                                32, ZPixmap, 0, xdata, w, h, 32, 0);
    if (!img) { free(xdata); return 0; }
    img->byte_order = LSBFirst;

    GC gc32 = XCreateGC(dpy, pix, 0, NULL);
    XPutImage(dpy, pix, gc32, img, 0, 0, 0, 0, w, h);
    XFreeGC(dpy, gc32);

    img->data = NULL; /* prevent XDestroyImage from freeing our data */
    XDestroyImage(img);
    free(xdata);

    *pix_out = pix;
    *pic_out = make_picture(pix, 1);
    return 1;
}

/* ── transform stack ────────────────────────────────────────── */
/* 2D affine matrix: [a c tx]   stored as m[6] = {a,b,c,d,tx,ty}
 *                  [b d ty]
 *                  [0 0  1] */

#define STACK_MAX 64
static double m[6]              = {1,0,0,1,0,0}; /* current matrix */
static double mstack[STACK_MAX][6];
static int    mstack_top        = 0;

static void mat_mul(double *out, const double *a, const double *b)
{
    /* out = a * b  (both 2x3 affine, treating as 3x3 with last row 0 0 1) */
    out[0] = a[0]*b[0] + a[2]*b[1];
    out[1] = a[1]*b[0] + a[3]*b[1];
    out[2] = a[0]*b[2] + a[2]*b[3];
    out[3] = a[1]*b[2] + a[3]*b[3];
    out[4] = a[0]*b[4] + a[2]*b[5] + a[4];
    out[5] = a[1]*b[4] + a[3]*b[5] + a[5];
}

static void tx(double xi, double yi, int *xo, int *yo)
{
    *xo = (int)(m[0]*xi + m[2]*yi + m[4]);
    *yo = (int)(m[1]*xi + m[3]*yi + m[5]);
}

static int transform_active(void)
{
    return !(m[0]==1 && m[1]==0 && m[2]==0 && m[3]==1 && m[4]==0 && m[5]==0);
}

/* ── cursor state ───────────────────────────────────────────── */
static Cursor  cur_handle   = None;
static Asset  *cur_strip    = NULL;   /* animated cursor strip */
static int     cur_hot_x    = 0;
static int     cur_hot_y    = 0;
static int     cur_fps      = 12;
static int     cur_frame    = 0;
static double  cur_last_t   = 0.0;

static double now_secs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static Cursor cursor_from_strip_frame(Asset *a, int frame, int hx, int hy)
{
    int sx = a->src_x + frame * a->frame_w;
    /* read pixels from the pixmap via XGetImage */
    XImage *img = XGetImage(dpy, a->pixmap, sx, a->src_y,
                            a->frame_w, a->frame_h, AllPlanes, ZPixmap);
    if (!img) return None;

    XcursorImage *ci = XcursorImageCreate(a->frame_w, a->frame_h);
    ci->xhot = hx;
    ci->yhot = hy;
    for (int y = 0; y < a->frame_h; y++)
        for (int x = 0; x < a->frame_w; x++)
            ci->pixels[y * a->frame_w + x] =
                (XcursorPixel)XGetPixel(img, x, y);
    XDestroyImage(img);
    Cursor c = XcursorImageLoadCursor(dpy, ci);
    XcursorImageDestroy(ci);
    return c;
}

static int win_w          = 800,  win_h    = 600;
static int buffered       = 1;
static int start_fullscreen = 0;

static int bg_r = 0,      bg_g = 0,      bg_b = 0;
static int fill_r = 255,  fill_g = 255,  fill_b = 255;
static int stroke_r = 0,  stroke_g = 0,  stroke_b = 0;
static int do_fill   = 1;
static int do_stroke = 1;
static int stroke_w  = 1;

/* ── color helpers ──────────────────────────────────────────── */

static unsigned long xcolor(int r, int g, int b)
{
    /* compute pixel directly from visual masks - works for all TrueColor displays */
    Visual *vis = DefaultVisual(dpy, scr);
    unsigned long rm = vis->red_mask;
    unsigned long gm = vis->green_mask;
    unsigned long bm = vis->blue_mask;

    /* find shift amounts from masks */
    int rs = 0, gs = 0, bs = 0;
    unsigned long t;
    for (t = rm; !(t & 1); t >>= 1) rs++;
    for (t = gm; !(t & 1); t >>= 1) gs++;
    for (t = bm; !(t & 1); t >>= 1) bs++;

    return ((unsigned long)r << rs & rm) |
           ((unsigned long)g << gs & gm) |
           ((unsigned long)b << bs & bm);
}

static void cmd_triangle(int x1, int y1, int x2, int y2, int x3, int y3)
{
    XPoint pts[3];
    int ax, ay;
    tx(x1, y1, &ax, &ay); pts[0].x = ax; pts[0].y = ay;
    tx(x2, y2, &ax, &ay); pts[1].x = ax; pts[1].y = ay;
    tx(x3, y3, &ax, &ay); pts[2].x = ax; pts[2].y = ay;

    if (do_fill) {
        XSetForeground(dpy, gc, xcolor(fill_r, fill_g, fill_b));
        XFillPolygon(dpy, tgt(), gc, pts, 3, Convex, CoordModeOrigin);
    }
    if (do_stroke) {
        XPoint closed[4] = { pts[0], pts[1], pts[2], pts[0] };
        XSetForeground(dpy, gc, xcolor(stroke_r, stroke_g, stroke_b));
        XSetLineAttributes(dpy, gc, stroke_w, LineSolid, CapRound, JoinRound);
        XDrawLines(dpy, tgt(), gc, closed, 4, CoordModeOrigin);
    }
    if (!buffered) XFlush(dpy);
}

/* ── bezier curve ───────────────────────────────────────────── */

#define BEZIER_STEPS 64

static void cmd_bezier(int x1, int y1, int cx1, int cy1,
                       int cx2, int cy2, int x2, int y2)
{
    XPoint pts[BEZIER_STEPS + 1];
    for (int i = 0; i <= BEZIER_STEPS; i++) {
        double t = (double)i / BEZIER_STEPS;
        double u = 1.0 - t;
        double bx = u*u*u*x1 + 3*u*u*t*cx1 + 3*u*t*t*cx2 + t*t*t*x2;
        double by = u*u*u*y1 + 3*u*u*t*cy1 + 3*u*t*t*cy2 + t*t*t*y2;
        int px, py;
        tx(bx, by, &px, &py);
        pts[i].x = (short)px;
        pts[i].y = (short)py;
    }
    XSetForeground(dpy, gc, xcolor(stroke_r, stroke_g, stroke_b));
    XSetLineAttributes(dpy, gc, stroke_w, LineSolid, CapRound, JoinRound);
    XDrawLines(dpy, tgt(), gc, pts, BEZIER_STEPS + 1, CoordModeOrigin);
    if (!buffered) XFlush(dpy);
}

/* ── polygon ────────────────────────────────────────────────── */

static void cmd_polygon(int npts, int *xs, int *ys)
{
    if (npts < 3) return;
    XPoint *pts = malloc(sizeof(XPoint) * (npts + 1));
    if (!pts) return;
    for (int i = 0; i < npts; i++) {
        int px, py;
        tx(xs[i], ys[i], &px, &py);
        pts[i].x = (short)px;
        pts[i].y = (short)py;
    }
    if (do_fill) {
        XSetForeground(dpy, gc, xcolor(fill_r, fill_g, fill_b));
        XFillPolygon(dpy, tgt(), gc, pts, npts, Complex, CoordModeOrigin);
    }
    if (do_stroke) {
        pts[npts] = pts[0];
        XSetForeground(dpy, gc, xcolor(stroke_r, stroke_g, stroke_b));
        XSetLineAttributes(dpy, gc, stroke_w, LineSolid, CapRound, JoinRound);
        XDrawLines(dpy, tgt(), gc, pts, npts + 1, CoordModeOrigin);
    }
    free(pts);
    if (!buffered) XFlush(dpy);
}

/* ── image / sprite draw ────────────────────────────────────── */

static Picture get_target_picture(void)
{
    /* XRender picture for the current draw target */
    Drawable d = tgt();
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, scr));
    XRenderPictureAttributes pa;
    pa.repeat = RepeatNone;
    return XRenderCreatePicture(dpy, d, fmt, CPRepeat, &pa);
}

static void cmd_draw(const char *name, int x, int y, int dw, int dh)
{
    Asset *a = asset_find(name);
    if (!a) { fprintf(stderr, "window: asset not found: %s\n", name); return; }

    Picture dst = get_target_picture();

    if (dw <= 0) dw = a->w;
    if (dh <= 0) dh = a->h;

    if (dw == a->w && dh == a->h) {
        XRenderComposite(dpy, PictOpOver,
                         a->picture, None, dst,
                         0, 0, 0, 0, x, y, dw, dh);
    } else {
        /* scaled draw using XRender transform */
        XTransform xform = {{
            { XDoubleToFixed((double)a->w / dw), 0, 0 },
            { 0, XDoubleToFixed((double)a->h / dh), 0 },
            { 0, 0, XDoubleToFixed(1.0) }
        }};
        XRenderSetPictureTransform(dpy, a->picture, &xform);
        XRenderComposite(dpy, PictOpOver,
                         a->picture, None, dst,
                         0, 0, 0, 0, x, y, dw, dh);
        XTransform identity = {{{XDoubleToFixed(1),0,0},{0,XDoubleToFixed(1),0},{0,0,XDoubleToFixed(1)}}};
        XRenderSetPictureTransform(dpy, a->picture, &identity);
    }

    XRenderFreePicture(dpy, dst);
    if (!buffered) XFlush(dpy);
}

static void cmd_drawframe(const char *name, int frame, int x, int y, int dw, int dh)
{
    Asset *a = asset_find(name);
    if (!a || a->type != ASSET_STRIP) {
        fprintf(stderr, "window: strip not found: %s\n", name); return;
    }
    if (frame < 0 || frame >= a->frame_count) return;

    int sx = a->src_x + frame * a->frame_w;
    int sy = a->src_y;
    int fw = a->frame_w;
    int fh = a->frame_h;

    if (dw <= 0) dw = fw;
    if (dh <= 0) dh = fh;

    Picture dst = get_target_picture();

    if (dw == fw && dh == fh) {
        XRenderComposite(dpy, PictOpOver,
                         a->picture, None, dst,
                         sx, sy, 0, 0, x, y, dw, dh);
    } else {
        XTransform xform = {{
            { XDoubleToFixed((double)fw / dw), 0, XDoubleToFixed(sx) },
            { 0, XDoubleToFixed((double)fh / dh), XDoubleToFixed(sy) },
            { 0, 0, XDoubleToFixed(1.0) }
        }};
        XRenderSetPictureTransform(dpy, a->picture, &xform);
        XRenderComposite(dpy, PictOpOver,
                         a->picture, None, dst,
                         0, 0, 0, 0, x, y, dw, dh);
        XTransform identity = {{{XDoubleToFixed(1),0,0},{0,XDoubleToFixed(1),0},{0,0,XDoubleToFixed(1)}}};
        XRenderSetPictureTransform(dpy, a->picture, &identity);
    }

    XRenderFreePicture(dpy, dst);
    if (!buffered) XFlush(dpy);
}

/* ── cursor ─────────────────────────────────────────────────── */

static void set_cursor(Cursor c)
{
    if (cur_handle != None) XFreeCursor(dpy, cur_handle);
    cur_handle = c;
    cur_strip  = NULL;
    if (c != None)
        XDefineCursor(dpy, win, c);
    else
        XUndefineCursor(dpy, win);
    XFlush(dpy);
}

static Cursor named_cursor(const char *name)
{
    unsigned int shape = XC_left_ptr;
    if      (!strcmp(name, "crosshair")) shape = XC_crosshair;
    else if (!strcmp(name, "hand"))      shape = XC_hand2;
    else if (!strcmp(name, "text"))      shape = XC_xterm;
    else if (!strcmp(name, "move"))      shape = XC_fleur;
    else if (!strcmp(name, "wait"))      shape = XC_watch;
    else if (!strcmp(name, "none")) {
        /* invisible cursor */
        Pixmap blank = XCreatePixmap(dpy, win, 1, 1, 1);
        XColor dummy = {0};
        Cursor c = XCreatePixmapCursor(dpy, blank, blank, &dummy, &dummy, 0, 0);
        XFreePixmap(dpy, blank);
        return c;
    }
    return XCreateFontCursor(dpy, shape);
}

static void cmd_cursor(int n, char **tok)
{
    const char *mode = n > 1 ? tok[1] : "default";

    if (!strcmp(mode, "image")) {
        /* cursor image assetname hx hy */
        Asset *a = asset_find(tok[2]);
        if (!a) { fprintf(stderr, "window: cursor asset not found: %s\n", tok[2]); return; }
        int hx = n > 3 ? atoi(tok[3]) : 0;
        int hy = n > 4 ? atoi(tok[4]) : 0;
        XcursorImage *ci = XcursorImageCreate(a->w, a->h);
        ci->xhot = hx; ci->yhot = hy;
        XImage *img = XGetImage(dpy, a->pixmap, 0, 0, a->w, a->h, AllPlanes, ZPixmap);
        if (img) {
            for (int y = 0; y < a->h; y++)
                for (int x = 0; x < a->w; x++)
                    ci->pixels[y * a->w + x] = (XcursorPixel)XGetPixel(img, x, y);
            XDestroyImage(img);
        }
        set_cursor(XcursorImageLoadCursor(dpy, ci));
        XcursorImageDestroy(ci);
        return;
    }

    if (!strcmp(mode, "anim")) {
        /* cursor anim stripname hx hy fps */
        Asset *a = asset_find(tok[2]);
        if (!a || a->type != ASSET_STRIP) {
            fprintf(stderr, "window: cursor strip not found: %s\n", tok[2]); return;
        }
        if (cur_handle != None) { XFreeCursor(dpy, cur_handle); cur_handle = None; }
        cur_strip  = a;
        cur_hot_x  = n > 3 ? atoi(tok[3]) : 0;
        cur_hot_y  = n > 4 ? atoi(tok[4]) : 0;
        cur_fps    = n > 5 ? atoi(tok[5]) : 12;
        cur_frame  = 0;
        cur_last_t = now_secs();
        Cursor c = cursor_from_strip_frame(a, 0, cur_hot_x, cur_hot_y);
        if (c != None) { XDefineCursor(dpy, win, c); XFlush(dpy); cur_handle = c; }
        return;
    }

    /* named cursor: default, crosshair, hand, text, move, wait, none */
    set_cursor(named_cursor(mode));
}

/* ── text draw ──────────────────────────────────────────────── */

static void cmd_text(const char *str, int x, int y)
{
    if (!active_font || !active_font->xft_font) {
        fprintf(stderr, "window: no font set\n"); return;
    }
    XftDraw *draw = XftDrawCreate(dpy, tgt(),
                                  DefaultVisual(dpy, scr),
                                  DefaultColormap(dpy, scr));
    XftColor xftc;
    XRenderColor rc = {
        (unsigned short)(fill_r << 8),
        (unsigned short)(fill_g << 8),
        (unsigned short)(fill_b << 8),
        0xffff
    };
    XftColorAllocValue(dpy, DefaultVisual(dpy, scr),
                       DefaultColormap(dpy, scr), &rc, &xftc);
    XftDrawStringUtf8(draw, &xftc, active_font->xft_font,
                      x, y, (FcChar8 *)str, strlen(str));
    XftColorFree(dpy, DefaultVisual(dpy, scr),
                 DefaultColormap(dpy, scr), &xftc);
    XftDrawDestroy(draw);
    if (!buffered) XFlush(dpy);
}

static void write_event(const char *fmt, ...); /* forward declaration */

/* ── grab / screen capture ──────────────────────────────────── */

static int grab_seq = 0;

static void cmd_grab(int x, int y, int w, int h, const char *filepath)
{
    /* clamp to buffer bounds */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w <= 0 || x + w > win_w) w = win_w - x;
    if (h <= 0 || y + h > win_h) h = win_h - y;
    if (w <= 0 || h <= 0) return;

    XImage *img = XGetImage(dpy, buf, x, y, w, h, AllPlanes, ZPixmap);
    if (!img) { fprintf(stderr, "window: grab XGetImage failed\n"); return; }

    /* convert to RGBA */
    unsigned char *pixels = malloc(w * h * 4);
    if (!pixels) { XDestroyImage(img); return; }

    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            unsigned long pixel = XGetPixel(img, px, py);
            int i = (py * w + px) * 4;
            pixels[i+0] = (pixel >> 16) & 0xff; /* R */
            pixels[i+1] = (pixel >>  8) & 0xff; /* G */
            pixels[i+2] =  pixel        & 0xff; /* B */
            pixels[i+3] = 0xff;                 /* A */
        }
    }
    XDestroyImage(img);

    /* determine output path */
    char tmppath[256];
    if (!filepath || !*filepath) {
        snprintf(tmppath, sizeof(tmppath),
                 "/tmp/window-%d-grab-%d.png", getpid(), ++grab_seq);
        filepath = tmppath;
    }

    if (!stbi_write_png(filepath, w, h, 4, pixels, w * 4)) {
        fprintf(stderr, "window: grab write failed: %s\n", filepath);
        free(pixels);
        return;
    }
    free(pixels);

    /* emit path over events pipe so client can retrieve it */
    write_event("grab %s", filepath);
}

/* ── event writer ───────────────────────────────────────────── */

static void write_event(const char *fmt, ...)
{
    if (evfd < 0) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[len++] = '\n';
    /* non-blocking write - drop event if buffer full */
    int r = write(evfd, buf, len); (void)r;
}

static const char *button_name(int b)
{
    if (b == 1) return "left";
    if (b == 2) return "middle";
    if (b == 3) return "right";
    if (b == 4) return "scrollup";
    if (b == 5) return "scrolldown";
    return "unknown";
}

/* returns 1 on success, sets r g b */
static int parse_hex(const char *s, int *r, int *g, int *b)
{
    if (!s || s[0] != '#') return 0;
    unsigned int hex;
    if (sscanf(s + 1, "%06x", &hex) != 1) return 0;
    *r = (hex >> 16) & 0xff;
    *g = (hex >>  8) & 0xff;
    *b =  hex        & 0xff;
    return 1;
}

/* ── fullscreen ─────────────────────────────────────────────── */

static void cmd_fullscreen(int on)
{
    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom fs       = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = win;
    ev.xclient.message_type = wm_state;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]   = on ? 1 : 0;  /* 1=add 0=remove */
    ev.xclient.data.l[1]   = (long)fs;
    XSendEvent(dpy, RootWindow(dpy, scr), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

/* ── draw target (window or backbuffer) ─────────────────────── */

static Drawable tgt(void) { return buffered ? (Drawable)buf : (Drawable)win; }

/* ── backbuffer ─────────────────────────────────────────────── */

static void recreate_buf(void)
{
    if (buf) XFreePixmap(dpy, buf);
    buf = XCreatePixmap(dpy, win, win_w, win_h, DefaultDepth(dpy, scr));
}

/* ── render commands ────────────────────────────────────────── */

static void cmd_clear(void)
{
    XSetForeground(dpy, gc, xcolor(bg_r, bg_g, bg_b));
    XFillRectangle(dpy, tgt(), gc, 0, 0, win_w, win_h);
    if (!buffered) XFlush(dpy);
}

static void cmd_flush(void)
{
    if (buffered)
        XCopyArea(dpy, buf, win, gc, 0, 0, win_w, win_h, 0, 0);
    XFlush(dpy);
}

static void cmd_point(int x, int y)
{
    int px, py;
    tx(x, y, &px, &py);
    XSetForeground(dpy, gc, xcolor(stroke_r, stroke_g, stroke_b));
    XDrawPoint(dpy, tgt(), gc, px, py);
    if (!buffered) XFlush(dpy);
}

static void cmd_line(int x1, int y1, int x2, int y2)
{
    int ax, ay, bx, by;
    tx(x1, y1, &ax, &ay);
    tx(x2, y2, &bx, &by);
    XSetForeground(dpy, gc, xcolor(stroke_r, stroke_g, stroke_b));
    XSetLineAttributes(dpy, gc, stroke_w, LineSolid, CapRound, JoinRound);
    XDrawLine(dpy, tgt(), gc, ax, ay, bx, by);
    if (!buffered) XFlush(dpy);
}

static void cmd_rect(int x, int y, int w, int h)
{
    if (transform_active()) {
        /* transform all 4 corners into a polygon */
        XPoint pts[4];
        int px, py;
        tx(x,   y,   &px, &py); pts[0].x=px; pts[0].y=py;
        tx(x+w, y,   &px, &py); pts[1].x=px; pts[1].y=py;
        tx(x+w, y+h, &px, &py); pts[2].x=px; pts[2].y=py;
        tx(x,   y+h, &px, &py); pts[3].x=px; pts[3].y=py;
        if (do_fill) {
            XSetForeground(dpy, gc, xcolor(fill_r, fill_g, fill_b));
            XFillPolygon(dpy, tgt(), gc, pts, 4, Convex, CoordModeOrigin);
        }
        if (do_stroke) {
            XSetForeground(dpy, gc, xcolor(stroke_r, stroke_g, stroke_b));
            XSetLineAttributes(dpy, gc, stroke_w, LineSolid, CapRound, JoinRound);
            XPoint closed[5];
            for (int i=0; i<4; i++) closed[i] = pts[i];
            closed[4] = pts[0];
            XDrawLines(dpy, tgt(), gc, closed, 5, CoordModeOrigin);
        }
    } else {
        if (do_fill) {
            XSetForeground(dpy, gc, xcolor(fill_r, fill_g, fill_b));
            XFillRectangle(dpy, tgt(), gc, x, y, w, h);
        }
        if (do_stroke) {
            XSetForeground(dpy, gc, xcolor(stroke_r, stroke_g, stroke_b));
            XSetLineAttributes(dpy, gc, stroke_w, LineSolid, CapRound, JoinRound);
            XDrawRectangle(dpy, tgt(), gc, x, y, w, h);
        }
    }
    if (!buffered) XFlush(dpy);
}

/* x,y = center, r = radius */
static void cmd_circle(int x, int y, int r)
{
    int cx, cy;
    tx(x, y, &cx, &cy);
    /* scale radius by average of x/y scale factors */
    double sx = sqrt(m[0]*m[0] + m[1]*m[1]);
    double sy = sqrt(m[2]*m[2] + m[3]*m[3]);
    int sr = (int)(r * (sx + sy) / 2.0);
    if (do_fill) {
        XSetForeground(dpy, gc, xcolor(fill_r, fill_g, fill_b));
        XFillArc(dpy, tgt(), gc, cx - sr, cy - sr, sr*2, sr*2, 0, 360*64);
    }
    if (do_stroke) {
        XSetForeground(dpy, gc, xcolor(stroke_r, stroke_g, stroke_b));
        XSetLineAttributes(dpy, gc, stroke_w, LineSolid, CapRound, JoinRound);
        XDrawArc(dpy, tgt(), gc, cx - sr, cy - sr, sr*2, sr*2, 0, 360*64);
    }
    if (!buffered) XFlush(dpy);
}

/* x,y = center, w,h = bounding box */
static void cmd_ellipse(int x, int y, int w, int h)
{
    int cx, cy;
    tx(x, y, &cx, &cy);
    double sx = sqrt(m[0]*m[0] + m[1]*m[1]);
    double sy = sqrt(m[2]*m[2] + m[3]*m[3]);
    int sw = (int)(w * sx), sh = (int)(h * sy);
    if (do_fill) {
        XSetForeground(dpy, gc, xcolor(fill_r, fill_g, fill_b));
        XFillArc(dpy, tgt(), gc, cx - sw/2, cy - sh/2, sw, sh, 0, 360*64);
    }
    if (do_stroke) {
        XSetForeground(dpy, gc, xcolor(stroke_r, stroke_g, stroke_b));
        XSetLineAttributes(dpy, gc, stroke_w, LineSolid, CapRound, JoinRound);
        XDrawArc(dpy, tgt(), gc, cx - sw/2, cy - sh/2, sw, sh, 0, 360*64);
    }
    if (!buffered) XFlush(dpy);
}

/* x,y = top-left of bounding box, start/end in degrees */
static void cmd_arc(int x, int y, int w, int h, int start, int end)
{
    int ax, ay;
    tx(x, y, &ax, &ay);
    XSetForeground(dpy, gc, xcolor(stroke_r, stroke_g, stroke_b));
    XSetLineAttributes(dpy, gc, stroke_w, LineSolid, CapRound, JoinRound);
    XDrawArc(dpy, tgt(), gc, ax, ay, w, h, start * 64, (end - start) * 64);
    if (!buffered) XFlush(dpy);
}

/* ── command dispatcher ─────────────────────────────────────── */

static void dispatch(char *line)
{
    /* skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;
    /* strip trailing newline */
    line[strcspn(line, "\r\n")] = 0;
    /* skip blank lines and comments */
    if (!*line || *line == '#') return;


    /* tokenize into tok[], handling "quoted strings" */
    static char tmp[MAX_CMD];
    strncpy(tmp, line, MAX_CMD - 1);
    tmp[MAX_CMD - 1] = 0;

    char *tok[MAX_TOKENS];
    int   n = 0;
    char *p = tmp;
    while (*p && n < MAX_TOKENS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            tok[n++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = 0;
        } else {
            tok[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
        }
    }
    if (!n) return;

#define T(i)  (n > (i) ? tok[i] : "")
#define I(i)  atoi(T(i))

    /* ── buffer mode ── */
    if (strncmp(tok[0], "buffer=", 7) == 0) {
        buffered = strcmp(tok[0] + 7, "true") == 0;
        return;
    }

    /* ── window properties ── */
    if (!strcmp(tok[0], "title")) {
        /* rejoin remaining tokens */
        char title[512] = "";
        for (int i = 1; i < n; i++) {
            if (i > 1) strncat(title, " ", sizeof(title) - strlen(title) - 1);
            strncat(title, tok[i], sizeof(title) - strlen(title) - 1);
        }
        XStoreName(dpy, win, title);
        return;
    }

    if (!strcmp(tok[0], "size")) {
        win_w = I(1); win_h = I(2);
        XResizeWindow(dpy, win, win_w, win_h);
        recreate_buf();
        cmd_clear();
        return;
    }

    if (!strcmp(tok[0], "position")) {
        XMoveWindow(dpy, win, I(1), I(2));
        return;
    }

    /* ── style ── */
    if (!strcmp(tok[0], "background")) {
        if (!parse_hex(T(1), &bg_r, &bg_g, &bg_b)) {
            bg_r = I(1); bg_g = I(2); bg_b = I(3);
        }
        return;
    }

    if (!strcmp(tok[0], "fill")) {
        if (!parse_hex(T(1), &fill_r, &fill_g, &fill_b)) {
            fill_r = I(1); fill_g = I(2); fill_b = I(3);
        }
        do_fill = 1;
        return;
    }

    if (!strcmp(tok[0], "nofill"))   { do_fill = 0; return; }

    if (!strcmp(tok[0], "stroke")) {
        if (!parse_hex(T(1), &stroke_r, &stroke_g, &stroke_b)) {
            stroke_r = I(1); stroke_g = I(2); stroke_b = I(3);
        }
        do_stroke = 1;
        return;
    }

    if (!strcmp(tok[0], "nostroke"))     { do_stroke = 0; return; }
    if (!strcmp(tok[0], "strokeweight")) { stroke_w = I(1); return; }

    /* ── clear / flush ── */
    if (!strcmp(tok[0], "clear")) { cmd_clear(); return; }
    if (!strcmp(tok[0], "flush")) { cmd_flush(); return; }

    /* ── shapes ── */
    if (!strcmp(tok[0], "point"))
        { cmd_point(I(1), I(2)); return; }

    if (!strcmp(tok[0], "line"))
        { cmd_line(I(1), I(2), I(3), I(4)); return; }

    if (!strcmp(tok[0], "rect"))
        { cmd_rect(I(1), I(2), I(3), I(4)); return; }

    if (!strcmp(tok[0], "circle"))
        { cmd_circle(I(1), I(2), I(3)); return; }

    if (!strcmp(tok[0], "ellipse"))
        { cmd_ellipse(I(1), I(2), I(3), I(4)); return; }

    if (!strcmp(tok[0], "arc"))
        { cmd_arc(I(1), I(2), I(3), I(4), I(5), I(6)); return; }

    if (!strcmp(tok[0], "triangle"))
        { cmd_triangle(I(1), I(2), I(3), I(4), I(5), I(6)); return; }

    if (!strcmp(tok[0], "bezier"))
        { cmd_bezier(I(1), I(2), I(3), I(4), I(5), I(6), I(7), I(8)); return; }

    if (!strcmp(tok[0], "polygon")) {
        int npts = (n - 1) / 2;
        if (npts >= 3) {
            int xs[MAX_TOKENS/2], ys[MAX_TOKENS/2];
            for (int i = 0; i < npts; i++) {
                xs[i] = atoi(tok[1 + i*2]);
                ys[i] = atoi(tok[2 + i*2]);
            }
            cmd_polygon(npts, xs, ys);
        }
        return;
    }

    /* ── grab ── */
    if (!strcmp(tok[0], "grab")) {
        /* grab x y w h [filepath] */
        cmd_grab(I(1), I(2), I(3), I(4), n > 5 ? T(5) : "");
        return;
    }

    /* ── cursor ── */
    if (!strcmp(tok[0], "cursor")) {
        cmd_cursor(n, tok);
        return;
    }

    /* ── transforms ── */
    if (!strcmp(tok[0], "push")) {
        if (mstack_top < STACK_MAX)
            memcpy(mstack[mstack_top++], m, sizeof(m));
        return;
    }

    if (!strcmp(tok[0], "pop")) {
        if (mstack_top > 0)
            memcpy(m, mstack[--mstack_top], sizeof(m));
        return;
    }

    if (!strcmp(tok[0], "translate")) {
        double tmp[6] = {1,0,0,1, atof(T(1)), atof(T(2))};
        double out[6];
        mat_mul(out, m, tmp);
        memcpy(m, out, sizeof(m));
        return;
    }

    if (!strcmp(tok[0], "scale")) {
        double sx = atof(T(1)), sy = atof(T(2));
        double tmp[6] = {sx, 0, 0, sy, 0, 0};
        double out[6];
        mat_mul(out, m, tmp);
        memcpy(m, out, sizeof(m));
        return;
    }

    if (!strcmp(tok[0], "rotate")) {
        double rad = atof(T(1)) * 3.14159265358979 / 180.0;
        double c = cos(rad), s = sin(rad);
        double tmp[6] = {c, s, -s, c, 0, 0};
        double out[6];
        mat_mul(out, m, tmp);
        memcpy(m, out, sizeof(m));
        return;
    }

    if (!strcmp(tok[0], "identity")) {
        double id[6] = {1,0,0,1,0,0};
        memcpy(m, id, sizeof(m));
        return;
    }

    /* ── font / text ── */
    if (!strcmp(tok[0], "font")) {
        Asset *a = asset_find(T(1));
        if (!a || a->type != ASSET_FONT)
            fprintf(stderr, "window: font not found: %s\n", T(1));
        else
            active_font = a;
        return;
    }

    if (!strcmp(tok[0], "text")) {
        /* text x y rest of line is the string (no quotes needed) */
        int x = I(1), y = I(2);
        char str[MAX_CMD] = "";
        for (int i = 3; i < n; i++) {
            if (i > 3) strncat(str, " ", sizeof(str) - strlen(str) - 1);
            strncat(str, tok[i], sizeof(str) - strlen(str) - 1);
        }
        cmd_text(str, x, y);
        return;
    }

    /* ── load asset ── */
    if (!strcmp(tok[0], "load")) {
        /* load name ./file.png
         * load name strip ./file.png src_x src_y frame_w frame_h count */
        if (n < 3) return;
        const char *aname = tok[1];
        if (nassets >= MAX_ASSETS) {
            fprintf(stderr, "window: too many assets\n"); return;
        }
        Asset *a = &assets[nassets];
        memset(a, 0, sizeof(*a));
        strncpy(a->name, aname, sizeof(a->name)-1);

        if (!strcmp(tok[2], "strip")) {
            /* load name strip file sx sy fw fh count */
            if (n < 9) { fprintf(stderr, "window: load strip needs 6 args\n"); return; }
            const char *path = tok[3];
            int sx = atoi(tok[4]), sy = atoi(tok[5]);
            int fw = atoi(tok[6]), fh = atoi(tok[7]);
            int cnt = atoi(tok[8]);
            int iw, ih, ch;
            unsigned char *px = stbi_load(path, &iw, &ih, &ch, 4);
            if (!px) { fprintf(stderr, "window: cannot load %s: %s\n", path, stbi_failure_reason()); return; }
            if (!load_image_data(px, iw, ih, &a->pixmap, &a->picture))
                { stbi_image_free(px); return; }
            stbi_image_free(px);
            a->type = ASSET_STRIP;
            a->w = iw; a->h = ih;
            a->src_x = sx; a->src_y = sy;
            a->frame_w = fw; a->frame_h = fh;
            a->frame_count = cnt;
            nassets++;
        } else {
            /* load name ./file.png */
            const char *path = tok[2];
            int iw, ih, ch;
            unsigned char *px = stbi_load(path, &iw, &ih, &ch, 4);
            if (!px) { fprintf(stderr, "window: cannot load %s: %s\n", path, stbi_failure_reason()); return; }
            if (!load_image_data(px, iw, ih, &a->pixmap, &a->picture))
                { stbi_image_free(px); return; }
            stbi_image_free(px);
            a->type = ASSET_IMAGE;
            a->w = iw; a->h = ih;
            nassets++;
        }
        return;
    }

    /* ── load font ── */
    if (!strcmp(tok[0], "loadfont")) {
        /* loadfont name size Family Name With Spaces
         * loadfont name size ./path/to/font.ttf */
        if (n < 4) return;
        if (nassets >= MAX_ASSETS) return;
        Asset *a = &assets[nassets];
        memset(a, 0, sizeof(*a));
        strncpy(a->name, tok[1], sizeof(a->name)-1);
        int size = atoi(tok[2]);
        /* rejoin remaining tokens as font name/path */
        char src[512] = "";
        for (int i = 3; i < n; i++) {
            if (i > 3) strncat(src, " ", sizeof(src) - strlen(src) - 1);
            strncat(src, tok[i], sizeof(src) - strlen(src) - 1);
        }
        /* if starts with . or / treat as file path, else font family name */
        if (src[0] == '.' || src[0] == '/') {
            a->xft_font = XftFontOpen(dpy, scr,
                XFT_FILE,   XftTypeString, src,
                XFT_SIZE,   XftTypeDouble, (double)size,
                NULL);
        } else {
            a->xft_font = XftFontOpen(dpy, scr,
                XFT_FAMILY, XftTypeString, src,
                XFT_SIZE,   XftTypeDouble, (double)size,
                NULL);
        }
        if (!a->xft_font) {
            fprintf(stderr, "window: cannot load font: %s\n", src); return;
        }
        a->type = ASSET_FONT;
        nassets++;
        if (!active_font) active_font = a;  /* auto-select first font */
        return;
    }

    /* ── draw image ── */
    if (!strcmp(tok[0], "draw")) {
        /* draw name x y [w h] */
        if (n < 4) return;
        cmd_draw(tok[1], I(2), I(3), n > 4 ? I(4) : 0, n > 5 ? I(5) : 0);
        return;
    }

    /* ── draw sprite frame ── */
    if (!strcmp(tok[0], "drawframe")) {
        /* drawframe name frame x y [w h] */
        if (n < 5) return;
        cmd_drawframe(tok[1], I(2), I(3), I(4), n > 5 ? I(5) : 0, n > 6 ? I(6) : 0);
        return;
    }

    /* ── fullscreen ── */
    if (!strcmp(tok[0], "fullscreen")) { cmd_fullscreen(1); return; }
    if (!strcmp(tok[0], "windowed"))   { cmd_fullscreen(0); return; }

    /* ── close ── */
    if (!strcmp(tok[0], "close")) {
        if (*pipe_path)   unlink(pipe_path);
        if (*events_path) unlink(events_path);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        exit(0);
    }

    fprintf(stderr, "window: unknown command: %s\n", tok[0]);

#undef T
#undef I
}

/* ── server mode ────────────────────────────────────────────── */

static void run_server(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "window: cannot open display\n"); exit(1); }
    scr = DefaultScreen(dpy);

    /* create window */
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr),
                              100, 100, win_w, win_h, 0,
                              BlackPixel(dpy, scr),
                              BlackPixel(dpy, scr));

    /* request WM_DELETE_WINDOW so we know when user closes it */
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    XSelectInput(dpy, win,
                 ExposureMask | StructureNotifyMask |
                 KeyPressMask | KeyReleaseMask |
                 ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask |
                 EnterWindowMask | LeaveWindowMask);

    XMapWindow(dpy, win);
    XSync(dpy, False);

    if (start_fullscreen)
        cmd_fullscreen(1);

    /* GC and backbuffer */
    gc = XCreateGC(dpy, win, 0, NULL);
    recreate_buf();
    cmd_clear();
    write_event("resize %d %d", win_w, win_h);

    /* create command pipe (client -> server) */
    snprintf(pipe_path, sizeof(pipe_path), "/tmp/window-%d.pipe", getpid());
    mkfifo(pipe_path, 0600);
    int rfd = open(pipe_path, O_RDONLY | O_NONBLOCK);
    if (rfd < 0) { perror("window: open pipe (read)"); exit(1); }
    int wfd = open(pipe_path, O_WRONLY | O_NONBLOCK);
    if (wfd < 0) { perror("window: open pipe (write)"); exit(1); }
    int pfd = rfd;

    /* create events pipe (server -> client) */
    snprintf(events_path, sizeof(events_path), "/tmp/window-%d.events", getpid());
    mkfifo(events_path, 0600);
    /* hold read end so write end never blocks when no client is listening */
    int ev_rfd = open(events_path, O_RDONLY | O_NONBLOCK);
    if (ev_rfd < 0) { perror("window: open events (read)"); exit(1); }
    evfd = open(events_path, O_WRONLY | O_NONBLOCK);
    if (evfd < 0) { perror("window: open events (write)"); exit(1); }
    (void)ev_rfd;


    char linebuf[MAX_CMD];
    int  linelen = 0;

    for (;;) {
        /* drain all pending X events */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (ev.type == Expose && ev.xexpose.count == 0)
                cmd_flush();

            if (ev.type == ConfigureNotify) {
                int nw = ev.xconfigure.width;
                int nh = ev.xconfigure.height;
                if (nw != win_w || nh != win_h) {
                    win_w = nw; win_h = nh;
                    recreate_buf();
                    cmd_clear();
                    write_event("resize %d %d", nw, nh);
                }
            }

            if (ev.type == ButtonPress) {
                int b = ev.xbutton.button;
                if (b == 4 || b == 5)
                    write_event("scroll %d %d %s",
                        ev.xbutton.x, ev.xbutton.y,
                        b == 4 ? "up" : "down");
                else
                    write_event("click %d %d %s",
                        ev.xbutton.x, ev.xbutton.y, button_name(b));
            }

            if (ev.type == ButtonRelease) {
                int b = ev.xbutton.button;
                if (b != 4 && b != 5)
                    write_event("release %d %d %s",
                        ev.xbutton.x, ev.xbutton.y, button_name(b));
            }

            if (ev.type == EnterNotify)
                write_event("enter %d %d", ev.xcrossing.x, ev.xcrossing.y);

            if (ev.type == LeaveNotify)
                write_event("leave %d %d", ev.xcrossing.x, ev.xcrossing.y);

            if (ev.type == MotionNotify) {
                unsigned int s = ev.xmotion.state;
                if (s & Button1Mask)
                    write_event("move %d %d left", ev.xmotion.x, ev.xmotion.y);
                else if (s & Button2Mask)
                    write_event("move %d %d middle", ev.xmotion.x, ev.xmotion.y);
                else if (s & Button3Mask)
                    write_event("move %d %d right", ev.xmotion.x, ev.xmotion.y);
                else
                    write_event("move %d %d", ev.xmotion.x, ev.xmotion.y);
            }

            if (ev.type == KeyPress || ev.type == KeyRelease) {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                const char *name = XKeysymToString(ks);
                if (name) {
                    unsigned int s = ev.xkey.state;
                    char mods[64] = "";
                    if (s & ShiftMask)   strncat(mods, "+shift",   sizeof(mods)-strlen(mods)-1);
                    if (s & ControlMask) strncat(mods, "+ctrl",    sizeof(mods)-strlen(mods)-1);
                    if (s & Mod1Mask)    strncat(mods, "+alt",     sizeof(mods)-strlen(mods)-1);
                    if (s & Mod4Mask)    strncat(mods, "+super",   sizeof(mods)-strlen(mods)-1);
                    if (*mods)
                        write_event("key %s %s %s",
                            ev.type == KeyPress ? "down" : "up", name, mods + 1);
                    else
                        write_event("key %s %s",
                            ev.type == KeyPress ? "down" : "up", name);
                }
            }

            if (ev.type == ClientMessage &&
                (Atom)ev.xclient.data.l[0] == wm_delete) {
                write_event("close");
                unlink(pipe_path);
                unlink(events_path);
                XCloseDisplay(dpy);
                exit(0);
            }
        }

        /* poll pipe non-blocking - avoids select() issues on WSL2 */
        char ch;
        while (read(pfd, &ch, 1) == 1) {
            if (ch == '\n') {
                linebuf[linelen] = 0;
                dispatch(linebuf);
                linelen = 0;
            } else if (linelen < MAX_CMD - 1) {
                linebuf[linelen++] = ch;
            }
        }

        /* animated cursor tick */
        if (cur_strip) {
            double t = now_secs();
            double interval = 1.0 / cur_fps;
            if (t - cur_last_t >= interval) {
                cur_last_t = t;
                cur_frame = (cur_frame + 1) % cur_strip->frame_count;
                Cursor c = cursor_from_strip_frame(cur_strip, cur_frame,
                                                   cur_hot_x, cur_hot_y);
                if (c != None) {
                    if (cur_handle != None) XFreeCursor(dpy, cur_handle);
                    cur_handle = c;
                    XDefineCursor(dpy, win, c);
                    XFlush(dpy);
                }
            }
        }

        usleep(5000); /* 5ms poll interval */
    }
}

/* ── client mode ────────────────────────────────────────────── */

static void run_client(const char *cpipe, int argc, char **argv)
{
    /* special case: grab - send command then read grab event response */
    if (argc >= 3 && strcmp(argv[2], "grab") == 0) {
        /* send grab command to server */
        char cmd[MAX_CMD] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
            strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
        }
        strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);
        int fd = open(cpipe, O_WRONLY);
        if (fd < 0) { perror("window: open pipe"); exit(1); }
        if (write(fd, cmd, strlen(cmd)) < 0) perror("window: write");
        close(fd);

        /* read events until we get the grab response */
        char evpath[256];
        strncpy(evpath, cpipe, sizeof(evpath) - 1);
        char *dot = strrchr(evpath, '.');
        if (dot) strcpy(dot, ".events");

        int efd = open(evpath, O_RDONLY);
        if (efd < 0) { perror("window: open events"); exit(1); }
        char buf[4096];
        int len = 0;
        char ch;
        /* read lines until we find one starting with "grab " */
        while (1) {
            if (read(efd, &ch, 1) != 1) break;
            if (ch == '\n') {
                buf[len] = 0;
                if (strncmp(buf, "grab ", 5) == 0) {
                    printf("%s\n", buf + 5);
                    break;
                }
                len = 0;
            } else if (len < (int)sizeof(buf) - 1) {
                buf[len++] = ch;
            }
        }
        close(efd);
        return;
    }

    /* special case: wait - block and read one event from events FIFO */
    if (argc >= 3 && strcmp(argv[2], "wait") == 0) {
        /* derive events path: /tmp/window-<pid>.events */
        char evpath[256];
        strncpy(evpath, cpipe, sizeof(evpath) - 1);
        char *dot = strrchr(evpath, '.');
        if (dot) strcpy(dot, ".events");

        int fd = open(evpath, O_RDONLY); /* blocks until an event is written */
        if (fd < 0) { perror("window: open events"); exit(1); }
        char buf[256];
        int  len = 0;
        char ch;
        while (read(fd, &ch, 1) == 1 && ch != '\n')
            if (len < (int)sizeof(buf) - 1) buf[len++] = ch;
        buf[len] = 0;
        close(fd);
        printf("%s\n", buf);
        return;
    }

    /* normal command: join argv[2..] and write to command pipe */
    char cmd[MAX_CMD] = "";
    for (int i = 2; i < argc; i++) {
        if (i > 2) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
    }
    strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);

    int fd = open(cpipe, O_WRONLY);
    if (fd < 0) { perror("window: open pipe"); exit(1); }
    if (write(fd, cmd, strlen(cmd)) < 0) perror("window: write");
    close(fd);
}

/* ── main ───────────────────────────────────────────────────── */

static void print_help(void)
{
    printf(
"window - scriptable X11 display server\n"
"\n"
"USAGE\n"
"  window [flags]              spawn a window, print pipe path to stdout\n"
"  window <pipe> <command>     send a command to a running window\n"
"  window <pipe> wait          block and read one event from the events pipe\n"
"\n"
"QUICK START\n"
"  ./window &\n"
"  PIPE=\"/tmp/window-$!.pipe\"\n"
"\n"
"  # option 1 - simple, one process per command (fine for scripts)\n"
"  w() { ./window \"$PIPE\" \"$@\"; }\n"
"\n"
"  # option 2 - fast, keep pipe open (required for animation loops)\n"
"  exec 4>\"$PIPE\"\n"
"  w() { echo \"$@\" >&4; }\n"
"\n"
"  w background 30 30 30\n"
"  w fill 255 0 0\n"
"  w rect 10 10 100 50\n"
"  w flush\n"
"\n"
"PIPES\n"
"  When spawned, window creates two named FIFOs:\n"
"    /tmp/window-<pid>.pipe    commands  (you -> window)\n"
"    /tmp/window-<pid>.events  events    (window -> you)\n"
"\n"
"  The fast way to write commands is to open the pipe once as a shell fd:\n"
"    exec 4>\"/tmp/window-$!.pipe\"\n"
"    w() { echo \"$@\" >&4; }\n"
"  This avoids forking a new process per command - essential for animation.\n"
"\n"
"  To read events:\n"
"    EVENT=$(./window $PIPE wait)   # blocks until one event arrives\n"
"  Or open the events pipe directly for a non-blocking game loop:\n"
"    exec 3<>\"/tmp/window-<pid>.events\"\n"
"    read -t 0.033 -u 3 event      # 33ms timeout, non-blocking\n"
"\n"
"SPAWN FLAGS\n"
"  --fullscreen    start in fullscreen mode\n"
"  --help, -h      show this help\n"
"\n"
"WINDOW\n"
"  title <text>             set window title\n"
"  size <w> <h>             resize window\n"
"  position <x> <y>         move window\n"
"  fullscreen               go fullscreen (EWMH)\n"
"  windowed                 return to windowed mode\n"
"  close                    destroy window and exit\n"
"\n"
"BUFFER\n"
"  buffer=true|false        enable/disable double buffering (default: true)\n"
"  clear                    fill backbuffer with background color\n"
"  flush                    copy backbuffer to screen\n"
"\n"
"STYLE\n"
"  background <r> <g> <b>   set clear color (0-255 each)\n"
"  fill <r> <g> <b>         set fill color and enable fill\n"
"  stroke <r> <g> <b>       set stroke color and enable stroke\n"
"  strokeweight <n>         set stroke width in pixels\n"
"  nofill                   disable fill\n"
"  nostroke                 disable stroke\n"
"  Colors also accept hex: fill #ff8800\n"
"\n"
"SHAPES\n"
"  rect <x> <y> <w> <h>              rectangle (top-left origin)\n"
"  circle <x> <y> <r>                circle (center + radius)\n"
"  ellipse <x> <y> <w> <h>           ellipse (center + bounding box)\n"
"  arc <x> <y> <w> <h> <start> <end> arc (top-left box, degrees)\n"
"  line <x1> <y1> <x2> <y2>          line segment\n"
"  point <x> <y>                     single pixel (uses stroke color)\n"
"  triangle <x1> <y1> <x2> <y2> <x3> <y3>  filled triangle\n"
"  bezier <x1> <y1> <cx1> <cy1> <cx2> <cy2> <x2> <y2>\n"
"                                     cubic bezier curve (stroke only)\n"
"  polygon <x1> <y1> <x2> <y2> ...   filled polygon, any number of points\n"
"\n"
"TRANSFORMS\n"
"  Transforms apply to all shape drawing commands.\n"
"  push                     save current transform matrix\n"
"  pop                      restore saved matrix\n"
"  translate <x> <y>        move origin\n"
"  rotate <degrees>         rotate around origin\n"
"  scale <sx> <sy>          scale axes\n"
"  identity                 reset transform to identity\n"
"\n"
"  Example - rotated rect:\n"
"    w push\n"
"    w translate 200 150\n"
"    w rotate 45\n"
"    w rect -50 -50 100 100\n"
"    w pop\n"
"\n"
"TEXT\n"
"  loadfont <name> <size> <Family Name>   load font by family name\n"
"  loadfont <name> <size> ./path/to.ttf  load font from file\n"
"  font <name>                            select active font\n"
"  text <x> <y> <message...>             draw text (no quotes needed)\n"
"\n"
"IMAGES\n"
"  load <name> <file.png>                     load PNG image\n"
"  draw <name> <x> <y> [<w> <h>]             draw image (scaled if w/h given)\n"
"  load <name> strip <file> <sx> <sy> <fw> <fh> <count>\n"
"                                             load sprite strip\n"
"  drawframe <name> <frame> <x> <y> [<w> <h>] draw one frame of a strip\n"
"  Images support alpha blending via XRender.\n"
"\n"
"CURSOR\n"
"  cursor default|crosshair|hand|text|move|wait|none\n"
"  cursor image <assetname> <hx> <hy>       custom cursor from loaded image\n"
"  cursor anim  <stripname> <hx> <hy> <fps> animated cursor from strip\n"
"\n"
"EVENTS\n"
"  Received via: EVENT=$(./window $PIPE wait)\n"
"  Or read directly from /tmp/window-<pid>.events\n"
"\n"
"  click <x> <y> left|middle|right    mouse button pressed\n"
"  release <x> <y> left|middle|right  mouse button released\n"
"  scroll <x> <y> up|down             mouse wheel\n"
"  move <x> <y> [left|middle|right]   mouse moved (button appended if held)\n"
"  enter <x> <y>                      cursor entered window\n"
"  leave <x> <y>                      cursor left window\n"
"  key down <name> [modifiers]        key pressed  (e.g. key down a shift)\n"
"  key up   <name> [modifiers]        key released (modifiers: shift ctrl alt super)\n"
"  resize <w> <h>                     window resized (also fires on startup)\n"
"  close                              window closed by user\n"
"\n"
"SCREEN CAPTURE\n"
"  grab <x> <y> <w> <h> [<file.png>]  save region of backbuffer to PNG\n"
"  After sending grab, read the events pipe - you will receive:\n"
"    grab /tmp/window-<pid>-grab-<n>.png\n"
"\n"
"  Example:\n"
"    w grab 0 0 800 600\n"
"    PATH=$(./window $PIPE wait)  # returns: grab /tmp/...\n"
"\n"
"EXAMPLE SCRIPT\n"
"  #!/bin/bash\n"
"  ./window &\n"
"  PIPE=\"/tmp/window-$!.pipe\"\n"
"  exec 4>\"$PIPE\"\n"
"  w() { echo \"$@\" >&4; }\n"
"  sleep 0.3\n"
"\n"
"  w size 400 300\n"
"  w title \"hello\"\n"
"  w background 20 20 40\n"
"  w loadfont mono 18 DejaVu Sans\n"
"  w font mono\n"
"\n"
"  while true; do\n"
"    w clear\n"
"    w fill 255 200 0\n"
"    w circle 200 150 80\n"
"    w fill 20 20 40\n"
"    w text 160 158 hello\n"
"    w flush\n"
"    sleep 0.016\n"
"  done\n"
    );
}

int main(int argc, char **argv)
{
    /* handle help flags before anything else */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
    }

    /* strip --fullscreen flag */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fullscreen") == 0) {
            start_fullscreen = 1;
            /* remove from argv */
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j+1];
            argc--;
            i--;
        }
    }

    if (argc == 1)
        run_server();
    else
        run_client(argv[1], argc, argv);

    return 0;
}
