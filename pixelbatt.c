/* -*- tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t -*-
 *
 * Complete refactor of xbattbar, inspired by the simpler code of pixelclock.
 *
 * Copyright (c) 2025 Toby Slight <tslight@pm.me>
 *
 */

#include <err.h>
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
#define FONT "monospace:bold:size=18"
#define POLLTIME 10
#define RAISE 1
#define WARNLEVEL 10

int ac_line, battery_life, time_remaining = -1;

char* progname = "pixelbatt";

struct xinfo {
  Display* dpy;
  int width, height;
  int screen;
  Window bar;
  Window popup;
  int size;
  char position;
  GC gc;
  Colormap colormap;
  unsigned long black, green, magenta, yellow, red, blue, olive;
} x;

const struct option longopts[] = {
  { "display",	required_argument,	NULL,	'd' },
  { "size",	required_argument,	NULL,	's' },
  { "left",	no_argument,		NULL,	'l' },
  { "right",	no_argument,		NULL,	'r' },
  { "top",	no_argument,		NULL,	't' },
  { "bottom",	no_argument,		NULL,	'b' },
  { "help",	no_argument,		NULL,	'h' },
  { NULL,		0,			NULL,	0   }
};

extern char *__progname;

static void usage(void) {
  fprintf(stderr, "usage: %s %s\n", __progname,
	  "[-display host:dpy] "
	  "[-help] "
	  "[-left|-right|-top|-bottom] "
	  "[-size <pixels>] ");
  exit(1);
}

static void kill_popup(void) {
  if ( x.popup != -1 ) {
    XDestroyWindow(x.dpy, x.popup);
    x.popup = -1;
  }
}

/* Logic stolen and adapted from xbattbar... */
static void show_popup(void) {
  XSetWindowAttributes att;
  int boxw, boxh;
  char diagmsg[64];
  XftDraw *xftdraw = NULL;
  XftFont *xftfont;
  XftColor xftcolor;
  XGlyphInfo extents;

  if (time_remaining > 0) {
    sprintf(diagmsg, "%s: %d%% - %d minutes",
	    ac_line ? "Charging" : "Discharging",
	    battery_life, time_remaining);
  } else {
    sprintf(diagmsg, "%s: %d%%",
	    ac_line ? "Charging" : "Discharging",
	    battery_life);
  }
  xftfont = XftFontOpenName(x.dpy, x.screen, FONT);
  if (!xftfont)
    errx(1, "XftFontOpenName failed for %s", FONT);
  // Get width and height of message
  XftTextExtentsUtf8(x.dpy, xftfont, (FcChar8 *)diagmsg, strlen(diagmsg),
		     &extents);

  boxw = extents.width + 20;
  boxh = extents.height + 20;

  if(x.popup != -1) kill_popup();
  x.popup = XCreateSimpleWindow(x.dpy, DefaultRootWindow(x.dpy),
				(x.width-boxw)/2,
				(x.height-boxh)/2,
				boxw, boxh,
				1,         // width
				x.magenta, //border
				x.black);  // background

  att.override_redirect = True;
  XChangeWindowAttributes(x.dpy, x.popup, CWOverrideRedirect, &att);
  XMapWindow(x.dpy, x.popup);

  xftdraw = XftDrawCreate(x.dpy,
			  x.popup,
			  DefaultVisual(x.dpy, x.screen),
			  x.colormap);
  XRenderColor render_color = { 0x0000,   // red
				0xffff,   // green
				0x0000,   // blue
				0xffff }; // opacity
  XftColorAllocValue(x.dpy, DefaultVisual(x.dpy, x.screen),
		     x.colormap, &render_color, &xftcolor);
  XftDrawStringUtf8(xftdraw, &xftcolor, xftfont, 10, 30, (FcChar8 *)diagmsg,
		    strlen(diagmsg));

  // Free Xft resources
  XftDrawDestroy(xftdraw);
  XftFontClose(x.dpy, xftfont);
  XftColorFree(x.dpy, DefaultVisual(x.dpy, x.screen),
	       x.colormap, &xftcolor);
}

static void draw_discharging(int left) {
  if (x.position == 'b' || x.position == 't') { // horizontal
    int p = x.width * left / 100;
    XSetForeground(x.dpy, x.gc, x.magenta);
    XFillRectangle(x.dpy, x.bar, x.gc, 0, 0, p, x.size);
    XSetForeground(x.dpy, x.gc, (battery_life < 25 ? x.red : x.yellow));
    XFillRectangle(x.dpy, x.bar, x.gc, p, 0, x.width, x.size);
  } else {
    int p = x.height * left / 100;
    XSetForeground(x.dpy, x.gc, x.magenta);
    XFillRectangle(x.dpy, x.bar, x.gc, 0, x.height-p, x.size, x.height);
    XSetForeground(x.dpy, x.gc, (battery_life < 25 ? x.red : x.yellow));
    XFillRectangle(x.dpy, x.bar, x.gc, 0, 0, x.size, x.height-p);
  }
  XFlush(x.dpy);
}

static void draw_charging(int left) {
  if (x.position == 'b' || x.position == 't') {
    int p = x.width * left / 100;
    XSetForeground(x.dpy, x.gc, x.green);
    XFillRectangle(x.dpy, x.bar, x.gc, 0, 0, p, x.size);
    XSetForeground(x.dpy, x.gc, (battery_life < 75 ? x.yellow : x.olive));
    XFillRectangle(x.dpy, x.bar, x.gc, p+1, 0, x.width, x.size);
  } else {
    int p = x.height * left / 100;
    XSetForeground(x.dpy, x.gc, x.green);
    XFillRectangle(x.dpy, x.bar, x.gc, 0, x.height - p, x.size, x.height);
    XSetForeground(x.dpy, x.gc, (battery_life < 75 ? x.yellow : x.olive));
    XFillRectangle(x.dpy, x.bar, x.gc, 0, 0, x.size, x.height - p);
  }
  XFlush(x.dpy);
}

static void redraw(void) {
  if (ac_line) {
    draw_charging(battery_life);
  } else {
    draw_discharging(battery_life);
    if (battery_life < WARNLEVEL) show_popup();
  }
}

static void battery_status(void) {
  int a, l, t;
  size_t a_size, l_size, t_size;

  a_size = sizeof(a);
  if (sysctlbyname("hw.acpi.acline", &a, &a_size, NULL, 0) == -1) {
    errx(1, "%s failed to get AC-line status.\n", progname);
  }

  l_size = sizeof(l);
  if (sysctlbyname("hw.acpi.battery.life", &l, &l_size, NULL, 0) == -1) {
    errx(1, "%s failed to get battery life status.\n", progname);
  }

  t_size = sizeof(t);
  if (sysctlbyname("hw.acpi.battery.time", &t, &t_size, NULL, 0) == -1) {
    errx(1, "%s failed to get battery time status.\n", progname);
  }

  ac_line = a;
  battery_life = l;
  time_remaining = t;
  redraw();
}

static long getcolor(const char *color) {
  int rc;
  XColor tcolor;

  if (!(rc = XAllocNamedColor(x.dpy, x.colormap, color, &tcolor,
			      &tcolor)))
    errx(1, "can't allocate %s", color);

  return tcolor.pixel;
}

static void handler(int sig) {
  XCloseDisplay(x.dpy);
  exit(0);
}

static void init_x(const char *display) {
  int rc;
  int left = 0, top = 0, width = 0, height = 0;
  XGCValues values;
  XSetWindowAttributes attributes;
  XTextProperty progname_prop;

  if (!(x.dpy = XOpenDisplay(display)))
    errx(1, "unable to open display %s", XDisplayName(display));

  x.screen	= DefaultScreen(x.dpy);
  x.width	= DisplayWidth(x.dpy, x.screen);
  x.height	= DisplayHeight(x.dpy, x.screen);
  x.popup	= -1;
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
    height = x.size;
    top = x.height - height;
    width = x.width;
    break;
  case 't':
    left = 0;
    top = 0;
    height = x.size;
    width = x.width;
    break;
  case 'l':
    left = 0;
    top = 0;
    height = x.height;
    width = x.size;
    break;
  case 'r':
    width = x.size;
    left = x.width - width;
    top = 0;
    height = x.height;
    break;
  }

  x.bar = XCreateSimpleWindow(x.dpy, RootWindow(x.dpy, x.screen),
			      left, top, width, height,
			      0, x.black, x.black);

  if (!(rc = XStringListToTextProperty(&progname, 1, &progname_prop)))
    errx(1, "XStringListToTextProperty");

  XSetWMName(x.dpy, x.bar, &progname_prop);

  attributes.override_redirect = True; // brute force position/size and decoration
  XChangeWindowAttributes(x.dpy, x.bar, CWOverrideRedirect, &attributes);

  if (!(x.gc = XCreateGC(x.dpy, x.bar, 0, &values)))
    errx(1, "XCreateGC");

  XMapWindow(x.dpy, x.bar);

  XSelectInput(x.dpy, x.bar, ExposureMask|
			     EnterWindowMask|
			     LeaveWindowMask|
			     VisibilityChangeMask);

  XFlush(x.dpy);
  XSync(x.dpy, False);
}

int main(int argc, char* argv[]) {
  char *display = NULL, *p;
  struct timeval tv;
  XEvent event;
  int c;

  bzero(&x, sizeof(struct xinfo));
  x.size = DEFSIZE;
  x.position = 0;

  while ((c = getopt_long_only(argc, argv, "", longopts, NULL)) != -1) {
    switch (c) {
    case 'd':
      display = optarg;
      break;
    case 'b':
    case 't':
    case 'l':
    case 'r':
      if (x.position)
	errx(1, "only one of -top, -bottom, -left, "
		"-right allowed");
      x.position = c;
      break;
    case 's':
      x.size = strtol(optarg, &p, 10);
      if (*p || x.size < 1)
	errx(1, "illegal value -- %s", optarg);
      break;
    case 'h':
      usage();
    default:
      usage();
    }
  }

  if (!x.position) x.position = DEFPOS;

  init_x(display);
  battery_status();

  signal(SIGINT, handler);
  signal(SIGTERM, handler);

  for (;;) {
    fd_set fds; // Set of file descriptors for select()
    int xfd = ConnectionNumber(x.dpy);
    FD_ZERO(&fds);     // Clear the set of file descriptors
    FD_SET(xfd, &fds); // Add the X server connection to the set

    tv.tv_sec  = POLLTIME;
    tv.tv_usec = 0; // No microseconds

    // Wait for either an X event or timeout
    int ret = select(xfd + 1, &fds, NULL, NULL, &tv);

    if (ret == 0) { // timeout: poll battery
      battery_status();
    }

    if (ret > 0) { // At least one X event
      while (XPending(x.dpy)) {
	XNextEvent(x.dpy, &event);
	if (event.type == EnterNotify) {
	  show_popup();
	} else if (event.type == LeaveNotify) {
	  kill_popup();
	} else if (event.type == VisibilityNotify) {
	  if (RAISE) XRaiseWindow(x.dpy, x.bar);
	} else if (event.type == Expose) {
	  redraw();
	}
      }
    }
  }

  exit(1);
}
