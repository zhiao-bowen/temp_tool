/* 编译: gcc -O2 -Wall -o temp temp.c $(pkg-config --cflags --libs xft xrender x11) */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#define W  360
#define H  36
#define MX 14
#define MY 24

static volatile int run = 1;
static void sig(int s) { (void)s; run = 0; }

static int cpu_temp(void)
{
    const char *p[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone1/temp",
        NULL
    };
    for (int i = 0; p[i]; i++) {
        FILE *f = fopen(p[i], "r");
        if (!f) continue;
        int t = 0;
        int ok = (fscanf(f, "%d", &t) == 1 && t > 0);
        fclose(f);
        if (ok) return t / 1000;
    }
    return -1;
}

static int gpu_temp(void)
{
    FILE *p = popen(
        "nvidia-smi --query-gpu=temperature.gpu "
        "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (!p) return -1;
    char b[32];
    int t = -1;
    if (fgets(b, sizeof b, p)) t = atoi(b);
    pclose(p);
    return t > 0 ? t : -1;
}

static XftColor mkc(Display *d, Visual *v, Colormap cm,
                    int r, int g, int b)
{
    XRenderColor rc = {r * 257, g * 257, b * 257, 0xFFFF};
    XftColor xc;
    XftColorAllocValue(d, v, cm, &rc, &xc);
    return xc;
}

static const XftColor *pick(int t,
                            const XftColor *n,
                            const XftColor *w,
                            const XftColor *c)
{
    if (t >= 85) return c;
    if (t >= 70) return w;
    return n;
}

int main(void)
{
    signal(SIGINT, sig);
    signal(SIGTERM, sig);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);

    /* 查找带 alpha 的 visual，记录真实 depth */
    XVisualInfo vt = {.class = TrueColor};
    int nvi;
    XVisualInfo *vl = XGetVisualInfo(dpy, VisualClassMask, &vt, &nvi);
    Visual *vis = NULL;
    int argb_depth = 0;
    for (int i = 0; vl && i < nvi; i++) {
        XRenderPictFormat *f = XRenderFindVisualFormat(dpy, vl[i].visual);
        if (f && f->type == PictTypeDirect && f->direct.alphaMask) {
            vis = vl[i].visual;
            argb_depth = vl[i].depth;
            break;
        }
    }
    if (vl) XFree(vl);
    if (!vis || argb_depth == 0) return 1;

    Colormap cmap = XCreateColormap(dpy, root, vis, AllocNone);
    XSetWindowAttributes swa = {
        .colormap         = cmap,
        .background_pixel = 0,
        .border_pixel     = 0,
        .override_redirect = True
    };
    Window win = XCreateWindow(dpy, root,
                               DisplayWidth(dpy, scr) - W - 735, -2,
                               W, H, 0, argb_depth, InputOutput, vis,
                               CWColormap | CWBackPixel |
                               CWBorderPixel | CWOverrideRedirect,
                               &swa);

    Atom a1 = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", 0);
    Atom a2 = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", 0);
    XChangeProperty(dpy, win, a1, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&a2, 1);
    a1 = XInternAtom(dpy, "_NET_WM_STATE", 0);
    a2 = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", 0);
    XChangeProperty(dpy, win, a1, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&a2, 1);

    XSelectInput(dpy, win,
                 ExposureMask | ButtonPressMask |
                 ButtonReleaseMask | PointerMotionMask);
    XMapRaised(dpy, win);

    XftDraw *xd = XftDrawCreate(dpy, win, vis, cmap);
    //XftFont   *ft = XftFontOpenName(dpy, scr, FONT);
    XftFont *ft = XftFontOpen(dpy, scr,XFT_FAMILY, FcTypeString, "Noto Sans Mono CJK SC",
        XFT_PIXEL_SIZE, FcTypeDouble, 14.0,NULL);
    if (!xd || !ft) return 1;

    XftColor cN = mkc(dpy, vis, cmap, 0xE0, 0xE0, 0xE0);
    XftColor cW = mkc(dpy, vis, cmap, 0xE0, 0xAF, 0x68);
    XftColor cR = mkc(dpy, vis, cmap, 0xF7, 0x76, 0x8E);

    int drag = 0, ox = 0, oy = 0;
    char cs[32] = "CPU ...";
    char gs[32] = "GPU ...";
    const XftColor *cc = &cN;
    const XftColor *gc = &cN;
    time_t last = 0;

    while (run) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose) goto draw;
            if (e.type == ButtonPress && e.xbutton.button == 1) {
                drag = 1; ox = e.xbutton.x; oy = e.xbutton.y;
            }
            if (e.type == ButtonPress && e.xbutton.button == 3)
                run = 0;
            if (e.type == ButtonRelease && e.xbutton.button == 1)
                drag = 0;
            if (e.type == MotionNotify && drag) {
                int wx, wy; Window ch;
                XTranslateCoordinates(dpy, win, root,
                                      e.xmotion.x - ox,
                                      e.xmotion.y - oy,
                                      &wx, &wy, &ch);
                XMoveWindow(dpy, win, wx, wy);
            }
        }

        time_t now = time(NULL);
        if (now - last >= 2) {
            last = now;
            int ct = cpu_temp();
            int gt = gpu_temp();

            cc = pick(ct, &cN, &cW, &cR);
            gc = pick(gt, &cN, &cW, &cR);

            snprintf(cs, sizeof cs,
                     ct >= 0 ? "CPU %d°C" : "CPU N/A", ct);
            snprintf(gs, sizeof gs,
                     gt >= 0 ? "GPU %d°C" : "GPU N/A", gt);
        }

draw:
        {
            Picture pic = XRenderCreatePicture(
                dpy, win,
                XRenderFindStandardFormat(dpy, PictStandardARGB32),
                0, NULL);
            XRenderColor tr = {0, 0, 0, 0};
            XRenderFillRectangle(dpy, PictOpSrc, pic, &tr,
                                 0, 0, W, H);
            XRenderFreePicture(dpy, pic);

            XftDrawStringUtf8(xd, cc, ft, MX, MY,
                              (FcChar8 *)cs, strlen(cs));

            XGlyphInfo ext;
            XftTextExtentsUtf8(dpy, ft,
                               (FcChar8 *)cs, strlen(cs), &ext);
            char tail[64];
            snprintf(tail, sizeof tail, "                   %s", gs);
            XftDrawStringUtf8(xd, gc, ft,
                              MX + ext.xOff, MY,
                              (FcChar8 *)tail, strlen(tail));
        }

        usleep(16000);
    }

    XftColorFree(dpy, vis, cmap, &cN);
    XftColorFree(dpy, vis, cmap, &cW);
    XftColorFree(dpy, vis, cmap, &cR);
    XftFontClose(dpy, ft);
    XftDrawDestroy(xd);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}