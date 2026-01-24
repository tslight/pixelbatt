/* -*- tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t -*-
 *
 * Complete refactor of xbattbar, inspired by the simpler code of pixelclock.
 *
 * Copyright (c) 2025 Toby Slight <tslight@pm.me>
 *
 */

#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>

#define DEFSIZE 2
#define DEFPOS 'l'
#define DEFPOLL 10
#define DEFWARN 10
#define DEFFONT "monospace:bold:size=18"

static volatile sig_atomic_t terminate = 0;
int ac_line, time_remaining;
unsigned int battery_life;
static char* progname;
// Whether or not to stay on top of all other windows
static int above = 0;
static char* font = DEFFONT;

static struct xinfo {
  Display* dpy;
  int width, height;
  int screen;
  Window bar;
  Window popup;
  unsigned int size;
  char position;
  GC gc;
  Colormap colormap;
  unsigned long black, green, magenta, yellow, red, blue, olive;
} x;

static const struct option longopts[] = {
  { "help",	no_argument,		NULL, 'h' },
  { "above",	no_argument,		NULL, 'a' },
  { "font",	required_argument,	NULL, 'f' },
  { "size",	required_argument,	NULL, 's' },
  { "poll",	required_argument,	NULL, 'p' },
  { "warn",	required_argument,	NULL, 'w' },
  { "display",	required_argument,	NULL, 'd' },
  { "left",	no_argument,		NULL, 'l' },
  { "right",	no_argument,		NULL, 'r' },
  { "top",	no_argument,		NULL, 't' },
  { "bottom",	no_argument,		NULL, 'b' },
  { NULL,	0,			NULL, 0   }
};

static void usage(void) {
  errx(1, "usage:\n"
	  "[-help]                     Show this message and exit.\n"
	  "[-above]                    Keep above all other windows.\n"
	  "[-font <xftfont>]           What font to use for the popup.\n"
	  "[-size <pixels>]            How big should the bar be.\n"
	  "[-poll <seconds>]           How often to check on the battery.\n"
	  "[-warn <percent>]           When should we start alerting.\n"
	  "[-display host:dpy]         Which display do we want to be on.\n"
	  "[-left|-right|-top|-bottom] Which side of the screen to use. \n");
}

static void kill_popup(void) {
  if ( x.popup != 0 ) {
    XDestroyWindow(x.dpy, x.popup);
    XFlush(x.dpy);
    x.popup = 0;
  }
}

/* Logic stolen and adapted from xbattbar... */
static void show_popup(void) {
  XSetWindowAttributes att;
  char msg[64];
  XftDraw *xftdraw = NULL;
  XftFont *xftfont;
  XftColor xftcolor;
  XGlyphInfo extents;
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

  xftfont = XftFontOpenName(x.dpy, x.screen, font);
  if (!xftfont) err(1, "XftFontOpenName failed for %s", font);
  // Get width and height of message
  XftTextExtentsUtf8(x.dpy, xftfont, (FcChar8 *)msg, (int)strlen(msg),
		     &extents);

  int boxw = extents.xOff + 2 * padw; // offset better than width for some reason!
  int boxh = (xftfont->ascent + xftfont->descent) + 2 * padh; // reliable line height
  /* clamp to screen size to avoid (unsigned) wrapping and BadValue */
  if (boxw > x.width) boxw = x.width - 2;
  if (boxh > x.height) boxh = x.height - 2;
  int left = (x.width - boxw) / 2;
  if (left < 0) left = 0;
  int top  = (x.height - boxh) / 2;
  if (top < 0) top = 0;

  if(x.popup != 0) kill_popup();
  x.popup = XCreateSimpleWindow(x.dpy, DefaultRootWindow(x.dpy),
				left, top,
				(unsigned int)boxw, (unsigned int)boxh,
				1, x.magenta, x.black);

  att.override_redirect = True;
  XChangeWindowAttributes(x.dpy, x.popup, CWOverrideRedirect, &att);
  XMapWindow(x.dpy, x.popup);

  xftdraw = XftDrawCreate(x.dpy,
			  x.popup,
			  DefaultVisual(x.dpy, x.screen),
			  x.colormap);
  if (!xftdraw) err(1, "XftDrawCreate");

  XRenderColor render_color = { 0x0000,   // red
				0xffff,   // green
				0x0000,   // blue
				0xffff }; // opacity
  if (!XftColorAllocValue(x.dpy, DefaultVisual(x.dpy, x.screen),
			  x.colormap, &render_color, &xftcolor))
    err(1, "XftColorAllocValue");
  XftDrawStringUtf8(xftdraw, &xftcolor, xftfont, padw, padh + xftfont->ascent,
		    (FcChar8 *)msg, (int)strlen(msg));

  // Free Xft resources
  XftDrawDestroy(xftdraw);
  XftFontClose(x.dpy, xftfont);
  XftColorFree(x.dpy, DefaultVisual(x.dpy, x.screen),
	       x.colormap, &xftcolor);
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
  int a, t;
  unsigned int l;
  size_t a_size, l_size, t_size;

  a_size = sizeof(a);
  if (sysctlbyname("hw.acpi.acline", &a, &a_size, NULL, 0) == -1) {
    err(1, "failed to get AC-line status.\n");
  }

  l_size = sizeof(l);
  if (sysctlbyname("hw.acpi.battery.life", &l, &l_size, NULL, 0) == -1) {
    err(1, "failed to get battery life status.\n");
  }

  t_size = sizeof(t);
  if (sysctlbyname("hw.acpi.battery.time", &t, &t_size, NULL, 0) == -1) {
    err(1, "failed to get battery time status.\n");
  }

  ac_line = a;
  battery_life = l;
  time_remaining = t;
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

  x.screen	= DefaultScreen(x.dpy);
  x.width	= DisplayWidth(x.dpy, x.screen);
  x.height	= DisplayHeight(x.dpy, x.screen);
  x.popup	= None;
  x.colormap	= DefaultColormap(x.dpy, DefaultScreen(x.dpy));
  x.black	= getcolor("black");
  x.magenta	= getcolor("magenta");
  x.green	= getcolor("green");
  x.yellow	= getcolor("yellow");
  x.red         = getcolor("red");
  x.blue        = getcolor("blue");
  x.olive       = getcolor("olive drab");

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
  char *end; errno = 0;
  unsigned long l = strtoul(a, &end, 10);
  if (end == a || *end != '\0') errx(1, "invalid integer: %s", a);
  if (a[0] == '-') errx(1, "unsigned only: %s", a);
  if (errno == ERANGE || l > UINT_MAX) err(1, "out of range: %s", optarg);
  *ui = (unsigned int)l;
}

int main(int argc, char* argv[]) {
  progname = argv[0];
  char *display = NULL;
  struct sigaction sa;
  struct timeval tv;
  unsigned int p;
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
    case 'a':
      above = 1;
      break;
    case 'd':
      display = optarg;
      break;
    case 'f':
      if (!optarg || optarg[0] == '\0') errx(1, "empty font name");
      if (strnlen(optarg, 1024) >= 1024) errx(1, "font name too long");
      font = optarg;
      break;
    case 'b':
    case 't':
    case 'l':
    case 'r':
      x.position = (char)c;
      break;
    case 'p':
      safe_atoui(optarg, &p);
      if (p > 600) p = DEFPOLL;
    case 's':
      safe_atoui(optarg, &x.size);
      if (x.size > 1000) x.size = DEFSIZE;
      break;
    case 'h':
    default:
      usage();
    }
  }

  if (!x.position) x.position = DEFPOS;

  init_x(display);
  battery_status();

  for (;;) {
    fd_set fds; // Set of file descriptors for select()
    int xfd = ConnectionNumber(x.dpy);
    FD_ZERO(&fds);     // Clear the set of file descriptors
    FD_SET(xfd, &fds); // Add the X server connection to the set

    tv.tv_sec  = p;
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

  XCloseDisplay(x.dpy);
  exit(0);
}
