/* -*- tab-width: 2; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * vim: set ts=2 sw=2 expandtab:
 *
 * A different way of looking at power.
 *
 * FreeBSD specific refactor of xbattbar, inspired by pixelclock.
 *
 * Copyright (c) 2025 Toby Slight <tslight@pm.me>
 *
 */
#include "pixelbatt.h"

static void usage(void) {
  errx(1,
       "usage:\n"
       "[-size <pixels>]            Width of bar in pixels.\n"
       "[-hide <percent>]           Defaults to 98%%. 0 means never hide.\n"
       "[-font <xftfont>]           Defaults to 'monospace:bold:size=18'.\n"
       "[-poll <seconds>]           Defaults to checking every 10 seconds.\n"
       "[-warn <percent>]           Keep showing popup when this percent is reached.\n"
       "[-display <host:dpy>]       Specify a display to use.\n"
       "[-unraise]                  Prevents bar from always being on top.\n"
       "[-left|-right|-top|-bottom] Specify screen edge.");
}

static void kill_popup(void) {
  if ( x.popup != None ) {
    XUnmapWindow(x.dpy, x.popup); // keep the window for reuse
    XFlush(x.dpy);
  }
}

/* Logic stolen and adapted from xbattbar... */
static void show_popup(void) {
  char msg[64];
  XftDraw *xftdraw = NULL;
  XGlyphInfo extents;
  XSetWindowAttributes att;
  const int padw = 2, padh = 2;

  if (time_remaining > 0) {
    snprintf(msg, sizeof(msg), "%s: %d%% - %d minutes",
             ac_line ? "Charging" : "Discharging",
             battery_life, time_remaining);
  } else {
    snprintf(msg, sizeof(msg), "%s: %d%%",
             ac_line ? "Charging" : "Discharging",
             battery_life);
  }

  if (!x.font) { // cache to avoid calling on every popup
    x.font = XftFontOpenName(x.dpy, x.screen, font);
    if (!x.font) err(1, "XftFontOpenName failed for %s", font);
  }

  // Get width and height of message
  XftTextExtentsUtf8(x.dpy, x.font, (FcChar8 *)msg, (int)strlen(msg), &extents);

  int boxw = extents.xOff + 2 * padw; // offset better than width for some reason!
  int boxh = (x.font->ascent + x.font->descent) + 2 * padh; // reliable line height
  /* clamp to screen size to avoid (unsigned) wrapping and BadValue */
  if (boxw > x.width) boxw = x.width - 2;
  if (boxh > x.height) boxh = x.height - 2;
  int left = (x.width - boxw) / 2;
  if (left < 0) left = 0;
  int top  = (x.height - boxh) / 2;
  if (top < 0) top = 0;

  if (x.popup == None) {
    /* create once; resize/move on subsequent shows */
    x.popup = XCreateSimpleWindow(x.dpy, DefaultRootWindow(x.dpy),
                                  left, top,
                                  (unsigned int)boxw, (unsigned int)boxh,
                                  1, x.magenta, x.black);
    att.override_redirect = True;
    XChangeWindowAttributes(x.dpy, x.popup, CWOverrideRedirect, &att);
  } else {
    XMoveResizeWindow(x.dpy, x.popup, left, top, (unsigned int)boxw, (unsigned int)boxh);
  }

  XMapRaised(x.dpy, x.popup);

  xftdraw = XftDrawCreate(x.dpy,
                          x.popup,
                          DefaultVisual(x.dpy, x.screen),
                          x.colormap);
  if (!xftdraw) err(1, "XftDrawCreate");

  static XRenderColor font_green = { 0x0000, 0xffff, 0x0000, 0xffff };

  if (!XftColorAllocValue(x.dpy, DefaultVisual(x.dpy, x.screen),
                          x.colormap, &font_green, &x.fontcolor))
    err(1, "XftColorAllocValue");
  XftDrawStringUtf8(xftdraw, &x.fontcolor, x.font, padw, padh + x.font->ascent,
                    (FcChar8 *)msg, (int)strlen(msg));

  XftDrawDestroy(xftdraw); // Free Xft resources
  XftColorFree(x.dpy, DefaultVisual(x.dpy, x.screen),
               x.colormap, &x.fontcolor);
  XFlush(x.dpy);
}

static inline int pct_to_pixels(int total, unsigned int pct) {
  // do signed arithmetic to avoid overflow. sanity check & clamp pixels first.
  if (total > INT_MAX / 100) return (total / 100) * (int)pct;
  // promote to long long if needed as we live in an era of 6k displays!
  long long r = (long long)total * (long long)pct;
  return (int)(r/100);
}

static void draw_discharging(unsigned int left) {
  if (x.position == 'b' || x.position == 't') { // horizontal
    int p = pct_to_pixels(x.width, left);
    XSetForeground(x.dpy, x.gc, x.magenta);
    XFillRectangle(x.dpy, x.bar, x.gc, 0, 0, (unsigned int)p, x.size);
    XSetForeground(x.dpy, x.gc, (battery_life < 25 ? x.red : x.yellow));
    XFillRectangle(x.dpy, x.bar, x.gc, p, 0, (unsigned int)(x.width-p), x.size);
  } else {
    int p = pct_to_pixels(x.height, left);
    XSetForeground(x.dpy, x.gc, x.magenta);
    XFillRectangle(x.dpy, x.bar, x.gc, 0, x.height-p, x.size, (unsigned int)p);
    XSetForeground(x.dpy, x.gc, (battery_life < 25 ? x.red : x.yellow));
    XFillRectangle(x.dpy, x.bar, x.gc, 0, 0, x.size, (unsigned int)(x.height-p));
  }
  XFlush(x.dpy);
}

static void draw_charging(unsigned int left) {
  if (x.position == 'b' || x.position == 't') {
    int p = pct_to_pixels(x.width, left);
    XSetForeground(x.dpy, x.gc, x.green);
    XFillRectangle(x.dpy, x.bar, x.gc, 0, 0, (unsigned int)p, x.size);
    XSetForeground(x.dpy, x.gc, (battery_life < 75 ? x.yellow : x.olive));
    XFillRectangle(x.dpy, x.bar, x.gc, p, 0, (unsigned int)(x.width-p), x.size);
  } else {
    int p = pct_to_pixels(x.height, left);
    XSetForeground(x.dpy, x.gc, x.green);
    XFillRectangle(x.dpy, x.bar, x.gc, 0, x.height-p, x.size, (unsigned int)p);
    XSetForeground(x.dpy, x.gc, (battery_life < 75 ? x.yellow : x.olive));
    XFillRectangle(x.dpy, x.bar, x.gc, 0, 0, x.size, (unsigned int)(x.height-p));
  }
  XFlush(x.dpy);
}

static void redraw(void) {
  if (ac_line) {
    draw_charging(battery_life);
  } else {
    draw_discharging(battery_life);
    if (battery_life < DEFWARN) show_popup();
  }
}

static void battery_status(void) {
  size_t a_size, b_size, t_size;

  a_size = sizeof(ac_line);
  if (sysctlbyname("hw.acpi.acline", &ac_line, &a_size, NULL, 0) == -1) {
    err(1, "failed to get AC-line status.\n");
  }

  b_size = sizeof(battery_life);
  if (sysctlbyname("hw.acpi.battery.life", &battery_life, &b_size, NULL, 0) == -1) {
    err(1, "failed to get battery life status.\n");
  }

  t_size = sizeof(time_remaining);
  if (sysctlbyname("hw.acpi.battery.time", &time_remaining, &t_size, NULL, 0) == -1) {
    err(1, "failed to get battery time status.\n");
  }

  if (hidepct > 0) {
    if (ac_line && battery_life > hidepct) {
      XUnmapWindow(x.dpy, x.bar);
    } else {
      (above ? XMapRaised(x.dpy, x.bar) : XMapWindow(x.dpy, x.bar));
    }
  }

  redraw();
}

static unsigned long getcolor(const char *color) {
  int rc;
  XColor tcolor;

  if (!(rc = XAllocNamedColor(x.dpy, x.colormap, color, &tcolor,
                              &tcolor)))
    err(1, "can't allocate %s", color);

  return tcolor.pixel;
}

static void init_x(const char *display) {
  int rc;
  int left = 0, top = 0, width = 0, height = 0;
  XGCValues values;
  XSetWindowAttributes attributes;
  XTextProperty progname_prop;

  if (!(x.dpy = XOpenDisplay(display)))
    err(1, "unable to open display %s", XDisplayName(display));

  if (ConnectionNumber(x.dpy) >= FD_SETSIZE)
    errx(1, "X connection fd >= FD_SETSIZE; cannot use select() safely");

  x.screen   = DefaultScreen(x.dpy);
  x.width    = DisplayWidth(x.dpy, x.screen);
  x.height   = DisplayHeight(x.dpy, x.screen);
  x.popup    = None;
  x.colormap = DefaultColormap(x.dpy, DefaultScreen(x.dpy));
  x.black    = getcolor("black");
  x.magenta  = getcolor("magenta");
  x.green    = getcolor("green");
  x.yellow   = getcolor("yellow");
  x.red      = getcolor("red");
  x.blue     = getcolor("blue");
  x.olive    = getcolor("olive drab");

  switch (x.position) {
  case 'b':
    left = 0;
    height = (int)x.size;
    top = x.height - height;
    width = x.width;
    break;
  case 't':
    left = 0;
    top = 0;
    height = (int)x.size;
    width = x.width;
    break;
  case 'l':
    left = 0;
    top = 0;
    height = x.height;
    width = (int)x.size;
    break;
  case 'r':
    width = (int)x.size;
    left = x.width - width;
    top = 0;
    height = x.height;
    break;
  }

  x.bar = XCreateSimpleWindow(x.dpy, RootWindow(x.dpy, x.screen),
                              left, top, (unsigned int)width, (unsigned int)height,
                              0, x.black, x.black);

  if (!(rc = XStringListToTextProperty(&progname, 1, &progname_prop)))
    err(1, "XStringListToTextProperty");

  XSetWMName(x.dpy, x.bar, &progname_prop);
  if (progname_prop.value) XFree(progname_prop.value);

  attributes.override_redirect = True; // brute force position/size and decoration
  XChangeWindowAttributes(x.dpy, x.bar, CWOverrideRedirect, &attributes);

  if (!(x.gc = XCreateGC(x.dpy, x.bar, 0, &values)))
    err(1, "XCreateGC");

  XMapWindow(x.dpy, x.bar);

  XSelectInput(x.dpy, x.bar, ExposureMask|
                             EnterWindowMask|
                             LeaveWindowMask|
                             VisibilityChangeMask);

  XFlush(x.dpy);
  XSync(x.dpy, False);
}

static void handler(int sig) { (void)sig; terminate = 1; }

static void safe_atoui(const char *a, unsigned *ui) {
  if (!a || !ui) errx(1, "nothing passed to safe_atoui");
  char *end; errno = 0;
  unsigned long l = strtoul(a, &end, 10);
  if (end == a || *end != '\0') errx(1, "invalid integer: %s", a);
  if (a[0] == '-') errx(1, "unsigned only: %s", a);
  if (errno == ERANGE || l > UINT_MAX) err(1, "out of range: %s", a);
  *ui = (unsigned int)l;
}

int main(int argc, char* argv[]) {
  progname = argv[0];
  char *display = NULL;
  struct sigaction sa;
  struct timeval tv;
  unsigned int poll = DEFPOLL;
  XEvent event;
  int c;

  memset(&x, 0, sizeof(struct xinfo));
  x.size = DEFSIZE;
  x.position = 0;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
  sa.sa_flags = 0; // ensure syscalls like select can be interrupted
  if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1)
    err(1, "sigaction");

  while ((c = getopt_long_only(argc, argv, "", longopts, NULL)) != -1) {
    switch (c) {
    case 'd':
      display = optarg;
      break;
    case 'f':
      if (!optarg || optarg[0] == '\0') errx(1, "empty font name");
      if (strnlen(optarg, 1024) >= 1024) errx(1, "font name too long");
      font = optarg;
      break;
    case 'h':
      safe_atoui(optarg, &hidepct);
      break;
    case 'b':
    case 't':
    case 'l':
    case 'r':
      x.position = (char)c;
      break;
    case 'p':
      safe_atoui(optarg, &poll);
      if (poll > 3600) {
        warnx("Anything can happen in %d mins! "
              "Falling back to %d sec poll interval", poll/60, DEFPOLL);
        poll = DEFPOLL;
      }
      break;
    case 's':
      safe_atoui(optarg, &x.size);
      break;
    case 'u':
      above = 0;
      break;
    default:
      usage();
    }
  }

  if (!x.position) x.position = DEFPOS;

  init_x(display);

  if (x.size > (uint)x.width - 1) {
    warnx("%d is bigger than the display! Falling back to %d pixels.", x.size, x.width - 1);
    x.size = (uint)x.width - 1;
  }

  battery_status();

  for (;;) {
    fd_set fds; // Set of file descriptors for select()
    int xfd = ConnectionNumber(x.dpy);
    FD_ZERO(&fds);     // Clear the set of file descriptors
    FD_SET(xfd, &fds); // Add the X server connection to the set

    tv.tv_sec  = poll;
    tv.tv_usec = 0; // No microseconds

    // Wait for either an X event or timeout
    int ret = select(xfd + 1, &fds, NULL, NULL, &tv);

    if (terminate) break; // check signal handler

    if (ret < 0) {
      if (errno == EINTR) continue;
      err(1, "select");
    } else if (ret == 0) { // timeout: poll battery
      battery_status();
    } else if (ret > 0) { // At least one X event
      while (XPending(x.dpy)) {
        XNextEvent(x.dpy, &event);
        if (event.type == EnterNotify) {
          show_popup();
        } else if (event.type == LeaveNotify) {
          kill_popup();
        } else if (event.type == VisibilityNotify) {
          if (above) XRaiseWindow(x.dpy, x.bar);
        } else if (event.type == Expose) {
          redraw();
        }
      }
    }
  }

  if (x.popup != None) XDestroyWindow(x.dpy, x.popup);
  if (x.font) XftFontClose(x.dpy, x.font);
  XCloseDisplay(x.dpy);
  exit(0);
}
