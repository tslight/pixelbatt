/*
 * A different way of looking at power.
 *
 * FreeBSD specific refactor of xbattbar, inspired by pixelclock.
 *
 * Copyright (c) 2025 Toby Slight <tslight@pm.me>
 *
 */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFFONT	 "monospace:bold:size=18"
#define DEFHIDE	 98
#define DEFPOS	 'l'
#define DEFPOLL	 10
#define DEFRAISE 1
#define DEFSIZE	 2
#define DEFWARN	 10

static volatile sig_atomic_t terminate = 0;
static char *progname;
static unsigned int above = DEFRAISE; // always on top by default
static unsigned int hidepct = DEFHIDE;
static char *font = DEFFONT;
static int ac_line, time_remaining;
static unsigned int battery_life;

static struct xinfo {
	Display *dpy;
	int width, height;
	int screen;
	Window bar;
	Window popup;
	unsigned int size;
	char position;
	GC gc;
	Colormap colormap;
	unsigned long black, green, magenta, yellow, red, blue, olive;
	XftFont *font;
	XftColor fontcolor;
} x;

static const struct option longopts[] = { { "hide", required_argument, NULL,
					      'h' },
	{ "font", required_argument, NULL, 'f' },
	{ "size", required_argument, NULL, 's' },
	{ "poll", required_argument, NULL, 'p' },
	{ "warn", required_argument, NULL, 'w' },
	{ "display", required_argument, NULL, 'd' },
	{ "unraise", no_argument, NULL, 'u' },
	{ "left", no_argument, NULL, 'l' }, { "right", no_argument, NULL, 'r' },
	{ "top", no_argument, NULL, 't' }, { "bottom", no_argument, NULL, 'b' },
	{ NULL, 0, NULL, 0 } };
