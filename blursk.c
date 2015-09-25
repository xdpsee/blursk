/* blursk.c */

/*  Blursk - visualization plugin for XMMS
 *  Copyright (C) 1999  Steve Kirkendall
 *
 *  Portions of this file are derived from the XMMS "Blur Scope" plugin.
 *  XMMS is Copyright (C) 1998-1999  Peter Alm, Mikael Alm, Olle Hallnas, Thomas Nilsson and 4Front Technologies
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include "config.h"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xmms/plugin.h>
#include <xmms/util.h>
#include <xmms/fullscreen.h>
#include <xmms/xmmsctrl.h>
#include "logo.xpm"
#include "blursk.h"

#define DUMPCORE
#ifdef DUMPCORE
# include <signal.h>
#endif

#ifdef HAVE_DL
# include <dlfcn.h>

typedef gboolean (*dlxmms_fullscreen_init_t)(GtkWidget *win);
typedef gboolean (*dlxmms_fullscreen_enter_t)(GtkWidget *win, gint *w, gint *h);
typedef void (*dlxmms_fullscreen_leave_t)(GtkWidget *win);
typedef gboolean (*dlxmms_fullscreen_in_t)(GtkWidget *win);
typedef void (*dlxmms_fullscreen_cleanup_t)(GtkWidget *win);

static dlxmms_fullscreen_init_t		dlxmms_fullscreen_init;
static dlxmms_fullscreen_enter_t	dlxmms_fullscreen_enter;
static dlxmms_fullscreen_leave_t	dlxmms_fullscreen_leave;
static dlxmms_fullscreen_in_t		dlxmms_fullscreen_in;
static dlxmms_fullscreen_cleanup_t	dlxmms_fullscreen_cleanup;

# define xmms_fullscreen_init		blurskfsinit
# define xmms_fullscreen_enter		(*dlxmms_fullscreen_enter)
# define xmms_fullscreen_leave		(*dlxmms_fullscreen_leave)
# define xmms_fullscreen_in		(*dlxmms_fullscreen_in)
# define xmms_fullscreen_cleanup	(*dlxmms_fullscreen_cleanup)

/* This function, and all of the above typedefs, variables, and macros, try
 * to solve a problem: At runtime, we want to link with the xmms_fullscreen
 * functions only if doing so won't result in undefined references to the
 * Xxf86dga and Xxf86vm functions.
 *
 * Early versions of XMMS had the xmms_fullscreen functions in the libxmms
 * library.  This is a problem because they depend on dynamic versions of
 * the Xxf86dga and Xxf86vm libraries, but standard distributions of XFree86
 * only have static versions of those libraries.  So if the xmms_fullscreen
 * functions are in the library, they won't always load correctly which could
 * prevent the Blursk plugin from loading.
 *
 * More recent versions of XMMS move the xmms_fullscreen functions into the
 * main XMMS executable, where they can be linked with the static versions
 * of the Xxf86dga and Xxf86vm libraries.  This solves the problem.
 *
 * So -- xmms_fullscreen in library = BAD; xmms_fullscreen in XMMS = GOOD.
 * We use dlsym() to test whether XMMS defines the xmms_fullscreen functions.
 * If not, then we can't use xmms_fullscreen... but at least we can still
 * run in a window.
 */
gboolean blurskfsinit(GtkWidget *win)
{
	/* look for the xmms_fullscreen functions in the XMMS executable */
	dlxmms_fullscreen_init = (dlxmms_fullscreen_init_t)dlsym(NULL, "xmms_fullscreen_init");
	dlxmms_fullscreen_enter = (dlxmms_fullscreen_enter_t)dlsym(NULL, "xmms_fullscreen_enter");
	dlxmms_fullscreen_leave = (dlxmms_fullscreen_leave_t)dlsym(NULL, "xmms_fullscreen_leave");
	dlxmms_fullscreen_in = (dlxmms_fullscreen_in_t)dlsym(NULL, "xmms_fullscreen_in");
	dlxmms_fullscreen_cleanup = (dlxmms_fullscreen_cleanup_t)dlsym(NULL, "xmms_fullscreen_cleanup");

	/* if any of the functions are missing, don't use xmms_fullscreen */
	if (!dlxmms_fullscreen_init
	 || !dlxmms_fullscreen_enter
	 || !dlxmms_fullscreen_leave
	 || !dlxmms_fullscreen_in
	 || !dlxmms_fullscreen_cleanup)
	{
		return FALSE;
	}

	/* okay, the functions exist.  Do the real xmms_fullscreen_init()
	 * thing now.
	 */
	return (*dlxmms_fullscreen_init)(win);
}
#endif

/* When looking for <Alt-Enter>, we want to treat any modifier as <Alt> */
#define ALT_MASK (GDK_MOD1_MASK | GDK_MOD2_MASK | GDK_MOD3_MASK | \
		  GDK_MOD4_MASK | GDK_MOD5_MASK)

/* There is no way to tell when the user has completed resizing the window.
 * We want to save the size when the user is done, but not for every
 * incremental change -- that would be slow, and possibly dangerous to the
 * XMMS config file.  So instead we wait until a few seconds have passed
 * since the last resize event.
 */
#define RESIZE_SAVE_DELAY 3 /* seconds */

/* To detect rhythms, blursk tracks the loudness of the signal across multiple
 * frames.  This is the maximum number of frames, and should correspond to the
 * slowest rhythm.
 */
#define BEAT_MAX	200

/* These define the resolution and tonal scale of the spectrum analyzer */
#define NUM_BANDS	32
static gint xscale[] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
	11, 12, 14, 16, 19, 22, 25, 28, 36, 42,
	49, 57, 66, 75, 85, 96, 107, 119, 132, 156,
	171, 187, 204, 220, 237, 255};	/* There are some extra bands here */
	
/* Windows and such */
GtkWidget *blursk_window = NULL;
static GtkWidget *area;
static GdkPixmap *bg_pixmap = NULL;

static void blursk_init(void);
static void blursk_cleanup(void);
static void blursk_playback_stop(void);
static void blursk_render_pcm(gint16 data[2][512]);
static void blursk_render_freq(gint16 data[2][256]);

/* This is used for slow motion */
static int oddeven;

/* This is used in conjunction with RESIZE_SAVE_DELAY to save size changes */
static time_t savewhen;

/* This indicates whether fullscreen operation is supported */
static int can_fullscreen;

/* This indicates whether the user has pressed <i> recently to show info */
int blurskinfo = FALSE;

/* This describes the plugin. */
VisPlugin blursk_vp =
{
	NULL,
	NULL,
	0, /* XMMS Session ID, filled in by XMMS */
	PACKAGE " " VERSION, /* description */
	1, /* Number of PCM channels wanted */
	0, /* Number of freq channels wanted */
	blursk_init, /* init */
	blursk_cleanup, /* cleanup */
	about, /* about */
	config_dialog, /* configure */
	NULL, /* disable_plugin (filled in by XMMS) */
	NULL, /* playback_start */
	blursk_playback_stop, /* playback_stop */
	blursk_render_pcm, /* render_pcm */
	blursk_render_freq  /* render_freq */
};
int nspectrums;

/* This is the main entry point.  XMMS calls this function to find anything
 * else that it needs to know about this plugin.
 */
VisPlugin *get_vplugin_info(void)
{
	/* Depends on whether we're plotting pcm data or freq data */
	config_read(NULL, NULL);
	blursk_genrender();

	/* return the info */
	return &blursk_vp;
}


/* This adjusts the blursk_vp variable to reflect the current signal style */
void blursk_genrender(void)
{
	/* Figure out the number of channels needed */
	switch (*config.signal_style)
	{
	  case 'O': /* Oscilloscope */
	  case 'P': /* Phase shift */
	  case 'F': /* Flower */
		blursk_vp.num_pcm_chs_wanted = 1;
		blursk_vp.num_freq_chs_wanted = 0;
		break;

	  case 'S': /* Stereo spectrum */
	  case 'R': /* Radial spectrum */
	  case 'H': /* High/low spectrum */
		blursk_vp.num_pcm_chs_wanted = 0;
		blursk_vp.num_freq_chs_wanted = 2;
		break;

	  case 'M': /* Mono spectrum */
		blursk_vp.num_pcm_chs_wanted = 0;
		blursk_vp.num_freq_chs_wanted = 1;
		break;
	}
	nspectrums = blursk_vp.num_freq_chs_wanted;

	/* Note: config.signal_style is also checked in the render.c file,
	 * each time there is data to be plotted.  It doesn't have any
	 * initialization that must be done when the signal_style changes,
	 * though, so we don't call any render_XXX() function here.
	 */
}



/* These store info about the mouse */
static gint	mouse_x, mouse_y;
static guint	mouse_state;

/* These store the name of the active fullscreen mode, or NULL if not in
 * fullscreen mode.
 */
static char	*fullscreen_method;

/* This function is called two ways.  When you press <Alt-Enter>, it is called
 * with a FALSE argument; this should toggle fullscreen mode on or off.  When
 * a fullscreen mode decided to end itself, this function is called with a
 * TRUE argument.
 */
void blursk_fullscreen(gint ending)
{
	char	*method;

	/* if method has changed while in fullscreen mode, then use the old
	 * mode name one last time, so we call the right shutdown code.
	 */
	method = fullscreen_method ? fullscreen_method : config.fullscreen_method;

#if HAVE_XV
	if (!strcmp(method, "Use XV") || !strcmp(method, "Use XV doubled"))
	{
		if (ending)
		{
			fullscreen_method = NULL;
			gtk_widget_show(blursk_window);
		}
		else if (fullscreen_method)
		{
			config.fullscreen_desired = FALSE;
			xv_end();
			fullscreen_method = NULL;
		}
		else if (xv_start())
		{
			config.fullscreen_desired = TRUE;
			fullscreen_method = method;
			gtk_widget_hide(blursk_window);
		}
		else
			goto Fail;
	}
#endif
	if (!strcmp(method, "Use XMMS"))
	{
		int	width, height;
		/* if fullscreen isn't supported, then do nothing */
		if (!can_fullscreen)
		{
			about_error("XMMS fullscreen isn't supported here.\n"
			"This is usually because you're running an X server other\n"
			"than XFree86.  On older versions of XMMS, it may also\n"
			"occur if you don't have dynamically-linked versions of\n"
			"the Xxf86fga and Xxf86vm libraries; newer versions of\n"
			"XMMS can avoid that problem.");
			goto Fail;
		}

		/* are we currently in fullscreen mode? */
		if (xmms_fullscreen_in(blursk_window))
		{
			/* Yes, so toggle fullscreen off */
			config.fullscreen_desired = FALSE;
			xmms_fullscreen_leave(blursk_window);
			fullscreen_method = NULL;
		}
		else
		{
			/* No, so toggle fullscreen on */
			config.fullscreen_desired = TRUE;
			width = img_width;
			height = img_height;
			xmms_fullscreen_enter(blursk_window, &width, &height);
			fullscreen_method = method;

			/* make sure the Blursk window still has input focus */
			gtk_widget_grab_focus(GTK_WIDGET(blursk_window));
		}
	}
	if (!strcmp(method, "Disabled"))
	{
		about_error("Full-screen mode is disabled.\n"
			"Before you can use Blursk in full-screen mode, you\n"
			"must configure the full-screen options in the [Advanced]\n"
			"dialog.  In particular, you should change \"Disabled\"\n"
			"to one of the \"Use xxxx\" methods.");
		goto Fail;
	}

	/* Write the configuration -- especially the fullscreen_desired flag */
	config_write(FALSE, NULL, NULL);

	return;

Fail:
	config.fullscreen_desired = FALSE;
}

static gint blursk_destroy_cb(GtkWidget *w,gpointer data)
{
	blursk_vp.disable_plugin(&blursk_vp);
	blursk_window = NULL;
	return TRUE;
}

static void selection_cb(GtkWidget *widget, GtkSelectionData *selection, gpointer data)
{
	/* If retrieval failed, then do nothing */
	if (selection->length < 0)
		return;

	/* Make sure data is a string */
	if (selection->type != GDK_SELECTION_TYPE_STRING)
		return;

	/* Parse the string, and load the configuration found in it */
	paste(selection->data);
}

/* Called for keypresses */
static gint key_cb(GtkWidget *widget, GdkEventKey *event)
{
	int	volume;

	/* Is this an <Alt-Enter> or <f> keystroke? */
	if (event->type == GDK_KEY_PRESS
	 && event->length == 1)
	{
		switch (*event->string)
		{
		  case '\r':
		  case '\n':
		  case 'f':
		  case 'F':
			blursk_fullscreen(FALSE);
			break;

		  case 'z':
		  case 'Z':
		  case 'y':
		  case 'Y':
			xmms_remote_playlist_prev(0);
			break;

		  case 'x':
		  case 'X':
			xmms_remote_play(0);
			break;

		  case 'c':
		  case 'C':
			xmms_remote_pause(0);
			break;

		  case 'v':
		  case 'V':
			xmms_remote_stop(0);
			break;

		  case 'b':
		  case 'B':
			xmms_remote_playlist_next(0);
			break;

		  case 'i':
		  case 'I':
			blurskinfo = TRUE;
			break;
		}
	}
	else if (event->keyval == GDK_Up)
	{
		volume = xmms_remote_get_main_volume(0);
		volume += 2;
		if (volume > 100)
			volume = 100;
		xmms_remote_set_main_volume(0, volume);
	}
	else if (event->keyval == GDK_Down)
	{
		volume = xmms_remote_get_main_volume(0);
		volume -= 2;
		if (volume < 0)
			volume = 0;
		xmms_remote_set_main_volume(0, volume);
	}

	return TRUE;
}

static gint mousebutton_cb(GtkWidget *widget, GdkEventButton *event)
{
	guchar	*str;
	gint	len, format;
	GdkAtom	type;
	int	volume;

	/* Save the state & position */
	mouse_x = event->x;
	mouse_y = event->y;
	mouse_state = event->state;

	/* The new button isn't reflected in the state, so we need to add it.
	 * Also, pressing button 3 causes the Configure dialog to appear.
	 */
	switch (event->type)
	{
	  case GDK_BUTTON_PRESS:
		switch (event->button)
		{
		  case 1:
			mouse_state |= GDK_BUTTON1_MASK;
			break;

		  case 2:
			gtk_selection_convert(widget, GDK_SELECTION_PRIMARY,
				GDK_SELECTION_TYPE_STRING, GDK_CURRENT_TIME);
			break;

		  case 3:
			config_dialog();
			break;

		  case 4:
			volume = xmms_remote_get_main_volume(0);
			volume += 8;
			if (volume > 100)
				volume = 100;
			xmms_remote_set_main_volume(0, volume);
			break;

		  case 5:
			volume = xmms_remote_get_main_volume(0);
			volume -=8;
			if (volume < 0)
				volume = 0;
			xmms_remote_set_main_volume(0, volume);
		}
		return TRUE;

	  case GDK_BUTTON_RELEASE:
		switch (event->button)
		{
		  case 1:
			/* Finished moving window. */
			mouse_state &= ~GDK_BUTTON1_MASK;
			if (fullscreen_method)
			{
				blursk_fullscreen(FALSE);
			}
			else
			{
				config_write(FALSE, NULL, NULL);
			}
			break;

		  /* buttons 2 through 5 do nothing */
		}
		return TRUE;

	  default:
		/* other events aren't handled here at all */
		return FALSE;
	}
	/*NOTREACHED*/
}

static gint mousemove_cb(GtkWidget *widget, GdkEventMotion *event)
{
	gint rx, ry;

	/* if button 1, then move window */
	if (!fullscreen_method && (mouse_state & GDK_BUTTON1_MASK))
	{
		/* Compute the new position */
		config.x = (gint)event->x_root - mouse_x;
		config.y = (gint)event->y_root - mouse_y;

		/* Move it */
		gtk_window_reposition(GTK_WINDOW(blursk_window), config.x, config.y);
		/* When remembering the position, compensate for the thickness
		 * of the window manager's decorations around the window.
		 */
		gdk_window_get_root_origin(blursk_window->window, &rx, &ry);
		config.x = rx;
		config.y = ry;

		/* Disable resize saves.  We'll save the size change at the
		 * end of this position change.
		 */
		savewhen = 0;
	}
	else
	{
		/* Save the state & position */
		mouse_x = event->x;
		mouse_y = event->y;
		mouse_state = event->state;
	}

	return TRUE;
}

static gint resize_cb(GtkWidget *widget, GdkEventConfigure *event)
{
	gint	width, height;

	/* detect change in window size */
	width = event->width;
	height = event->height;
	if ((width != img_physwidth || height != img_physheight)
					&& width >= 64 && height >= 64)
	{
		/* Change the image to match the window */
		gtk_drawing_area_size(GTK_DRAWING_AREA(area), width, height);
		img_resize(width, height);

		/* Store the new height & width.  Might as well grab the
		 * position too.
		 */
		config.width = width;
		config.height = height;
		config.x = event->x;
		config.y = event->y;

		/* Remember to save this change in .xmms/config eventually */
		savewhen = time(NULL) + RESIZE_SAVE_DELAY;
	}
	return TRUE;
}

static void blursk_init(void)
{
	if (blursk_window)
		return;

#ifdef DUMPCORE
	signal(SIGSEGV, SIG_DFL);
#endif
	/* Get the configuration and create the image buffers */
	config_read(NULL, NULL);
	preset_read();
	img_resize(config.width, config.height);

	/* Create the window */
	blursk_window = gtk_window_new(config.window_title ? GTK_WINDOW_TOPLEVEL
							   : GTK_WINDOW_DIALOG);
	gtk_window_set_title(GTK_WINDOW(blursk_window), PACKAGE);
	gtk_window_set_policy(GTK_WINDOW(blursk_window), TRUE, TRUE, TRUE);
	gtk_signal_connect(GTK_OBJECT(blursk_window), "destroy",
		GTK_SIGNAL_FUNC(blursk_destroy_cb), NULL);
	gtk_signal_connect(GTK_OBJECT(blursk_window), "destroy", 
		GTK_SIGNAL_FUNC(gtk_widget_destroyed), &blursk_window);
	gtk_signal_connect(GTK_OBJECT(blursk_window), "configure_event", 
		GTK_SIGNAL_FUNC(resize_cb), NULL);

	/* Put a drawing area in the window */
	area = gtk_drawing_area_new();
	gtk_drawing_area_size(GTK_DRAWING_AREA(area), img_physwidth, img_physheight);
	gtk_container_add(GTK_CONTAINER(blursk_window),area);
	gtk_widget_show(area);

	/* Arrange for key & mouse events to be detected */
	gtk_signal_connect(GTK_OBJECT(blursk_window), "selection_received",
		GTK_SIGNAL_FUNC(selection_cb), NULL);
	gtk_signal_connect(GTK_OBJECT(blursk_window), "key_press_event",
		GTK_SIGNAL_FUNC(key_cb), NULL);
	gtk_signal_connect(GTK_OBJECT(blursk_window), "button_press_event",
		GTK_SIGNAL_FUNC(mousebutton_cb), NULL);
	gtk_signal_connect(GTK_OBJECT(blursk_window), "button_release_event",
		GTK_SIGNAL_FUNC(mousebutton_cb), NULL);
	gtk_signal_connect(GTK_OBJECT(blursk_window), "motion_notify_event",
		GTK_SIGNAL_FUNC(mousemove_cb), NULL);
	gtk_widget_set_events(blursk_window, GDK_KEY_PRESS_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK);

	/* Initialize the drawing area */
	gtk_widget_realize(area);
	bg_pixmap = gdk_pixmap_create_from_xpm_d(area->window,
		NULL, NULL, blursk_xmms_logo_xpm);
	gdk_window_set_back_pixmap(area->window, bg_pixmap, 0);
	color_genmap(TRUE);

	/* Move the window to its usual place, if any.  If portions are beyond
	 * the edge of the screen, then move it to make everything visible.
	 */
	if (config.x != -1 || config.y != -1)
	{
		if (config.x < 0)
			config.x = 0;
		else if (config.x + img_physwidth >= gdk_screen_width())
			config.x = gdk_screen_width() - img_physwidth;
		if (config.y < 0)
			config.y = 0;
		else if (config.y + img_physheight >= gdk_screen_height())
			config.y = gdk_screen_height() - img_physheight;
		gtk_widget_realize(blursk_window);
		gtk_window_reposition(GTK_WINDOW(blursk_window), config.x, config.y);
	}

	/* Show it! */
	gtk_widget_show(blursk_window);

	/* Determine whether fullscreen operation is supported. */
	can_fullscreen = xmms_fullscreen_init(blursk_window);
}

static void blursk_cleanup(void)
{
	if (blursk_window)
	{
#if HAVE_XV
		xv_end();
#endif
		if (can_fullscreen)
			xmms_fullscreen_cleanup(blursk_window);
		gtk_widget_destroy(blursk_window);
		blursk_window = NULL;
	}
	if(bg_pixmap)
	{
		gdk_pixmap_unref(bg_pixmap);
		bg_pixmap = NULL;
	}
	color_cleanup();
}

static void blursk_playback_stop(void)
{
	if (config.fullscreen_revert)
	{
#if HAVE_XV
		xv_end();
#endif
		if (can_fullscreen)
			xmms_fullscreen_cleanup(blursk_window);
		fullscreen_method = NULL;
	}

	if(GTK_WIDGET_REALIZED(area))
		gdk_window_clear(area->window);
}


static gint32	beathistory[BEAT_MAX];
static int	beatbase;
static int	beatquiet;	/* force "quiet" situation? */

/* Detect beats.  This involves more than just comparing the loudness to a
 * preset trigger level -- it also compensates for overall loudness of the
 * music, and even tries to detect the rhythm.
 *
 * It returns TRUE for beats, FALSE otherwise.  It also sets *thickref to a
 * thickness value from 0 to 3, and it detects the start of silence for
 * the "Random quiet" setting.
 */
static int detect_beat(gint32 loudness, gint *thickref, gint *quietref)
{
	static gint32	aged;		/* smoothed out loudness */
	static gint32	lowest;		/* quietest point in current beat */
	static int	elapsed;	/* frames since last beat */
	static int	isquiet;	/* was previous frame quiet */
	static int	prevbeat;	/* period of previous beat */
	int		beat, i, j;
	gint32		total;
	int		sensitivity;

	/* Incorporate the current loudness into history */
	aged = (aged * 7 + loudness) >> 3;
	elapsed++;

	/* If silent, then clobber the beat */
	if (aged < 2000 || elapsed > BEAT_MAX)
	{
		elapsed = 0;
		lowest = aged;
		memset(beathistory, 0, sizeof beathistory);
	}
	else if (aged < lowest)
		lowest = aged;

	/* Beats are detected by looking for a sudden loudness after a lull.
	 * They are also limited to occur no more than once every 15 frames,
	 * so the beat flashes don't get too annoying.
	 */
	j = (beatbase + elapsed) % BEAT_MAX;
	beathistory[j] = loudness - aged;
	beat = FALSE;
	if (elapsed > 15 && aged > 2000 && loudness * 4 > aged * 5)
	{
		/* Compute the average loudness change, assuming this is beat */
		for (i = BEAT_MAX / elapsed, total = 0;
		     --i > 0;
		     j = (j + BEAT_MAX - elapsed) % BEAT_MAX)
		{
			total += beathistory[j];
		}
		total = total * elapsed / BEAT_MAX;

		/* Tweak the sensitivity to emphasize a consistent rhythm */
		sensitivity = config.beat_sensitivity;
		i = 3 - abs(elapsed - prevbeat)/2;
		if (i > 0)
			sensitivity += i;

		/* If average change is significantly positive, this is a beat.
		 */
		if (total * sensitivity > aged)
		{
			prevbeat = elapsed;
			beatbase = (beatbase + elapsed) % BEAT_MAX;
			lowest = aged;
			elapsed = 0;
			beat = TRUE;
		}
	}

	/* Thickness is computed from the difference between the instantaneous
	 * loudness and the aged loudness.  Thus, a sudden increase in volume
	 * will produce a thick line, regardless of rhythm.
	 */
	if (aged < 1500)
		*thickref = 0;
	else if (!config.thick_on_beats)
		*thickref = 1;
	else
	{
		*thickref = loudness * 2 / aged;
		if (*thickref > 3)
			*thickref = 3;
	}

	/* Silence is computed from the aged loudness.  The quietref value is
	 * set to TRUE only at the start of silence, not throughout the silent
	 * period.  Also, there is some hysteresis so that silence followed
	 * by a slight noise and more silence won't count as two silent
	 * periods -- that sort of thing happens during many fade edits, so
	 * we have to account for it.
	 */
	if (beatquiet || aged < (isquiet ? 1500 : 500))
	{
		/* Quiet now -- is this the start of quiet? */
		*quietref = !isquiet;
		isquiet = TRUE;
		beatquiet = FALSE;
	}
	else
	{
		*quietref = FALSE;
		isquiet = FALSE;
	}

	/* return the result */
	return beat;
}

static void drawfloaters(int beat)
{
	static int prevfloaters;
	static struct {int x, y, age; char color;} floater[10];
	static int oddeven;
	int	nfloaters;
	int	i, j, delta, dx, dy;

	/* choose the number of floaters */
	switch (*config.floaters)
	{
	  case 'N': /* No floaters */
		nfloaters = 0;
		break;

	  case 'D': /* Dots */
	  	nfloaters = 1;
	  	break;

	  case 'S': /* Slow */
		oddeven++;
		/* fall through... */

	  default: /* Slow/Fast/Retro floaters */
		nfloaters = 1 + img_width * img_height / 20000;
		if (nfloaters > 10)
			nfloaters = 10;
	}

	/* for each floater... */
	for (i = 0; i < nfloaters; i++)
	{
		/* if Dots, new, old, beat, or off-screen... */
		if (*config.floaters == 'D'
		 || i >= prevfloaters
		 || floater[i].age++ > 80 + i * 13
		 || beat
		 || floater[i].x < 0 || floater[i].x >= img_width
		 || floater[i].y < 0 || floater[i].y >= img_height)
		{
			/* Pretend motion is 0.  This will cause blursk to
			 * choose a new position, later in this function.
			 */
			delta = 0;
		}
		else
		{
			/* find the real motion */
			j = floater[i].y * img_bpl + floater[i].x;
			delta = &img_buf[j] - img_source[j];
		}

		/* if motion isn't 0, then move the floater */
		if (delta != 0)
		{
			/* decompose the delta into dx & dy.  Watch signs! */
			dx = (j + delta) % img_bpl - floater[i].x;
			dy = (j + delta) / img_bpl - floater[i].y;

			/* move the floater */
			switch (*config.floaters)
			{
			  case 'S':	/* Slow floaters */
				if ((oddeven ^ i) & 0x1)
					dx = dy = 0;
				break;

			  case 'F':	/* Fast floaters */
				dx *= 2;
				dy *= 2;
				break;

			  case 'R':	/* Retro floaters */
			  	dx = -dx;
			  	dy = -dy;
			  	break;
			}
			floater[i].x += dx;
			floater[i].y += dy;
		}

		/* if no motion, or motion carries it off the screen, then
		 * choose a new random position & contrasting color.
		 */
		if (delta == 0 
		 || floater[i].x < 0 || floater[i].x >= img_width
		 || floater[i].y < 0 || floater[i].y >= img_height)
		{
			/* choose a new random position */
			floater[i].x = rand_0_to(img_width - 9) + 2;
			floater[i].y = rand_0_to(img_height - 9) + 2;
			if (IMG_PIXEL(floater[i].x, floater[i].y) > 0x80)
				floater[i].color = 0;
			else
				floater[i].color = 0xfe;
			floater[i].age = 0;
		}

		/* draw the floater */
		render_dot(floater[i].x, floater[i].y, floater[i].color);
	}
	prevfloaters = nfloaters;
}


/* This detects changes in the title, and also draws the title when appropriate.
 * It should be called once for each frame.
 */
static guchar *show_info(guchar *img, gint height, gint bpl)
{
	gint pos, length;
	gchar *title;
	time_t now;
	static int prevpos;
	static gchar *prevtitle;
	static char buf[200];
	static time_t start, then;
	static gboolean persistent = FALSE;
	char	showinfo;

	/* Once per second, check for any changes in the title.  Note that we
	 * do this even for the "Never show title" setting, because a change
	 * in the title should trigger "quiet" actions even if the title isn't
	 * shown.
	 */
	time(&now);
	if (now != then)
	{
		then = now;
		pos = xmms_remote_get_playlist_pos(0);
		title = xmms_remote_get_playlist_title(0, pos);
		if (!title)
			title = "Unknown";
		if (pos != prevpos || !prevtitle || strcmp(title, prevtitle))
		{
			/* Yes, it changed.  Regenerate the info string */
			prevpos = pos;
			if (prevtitle)
				free(prevtitle);
			prevtitle = strdup(title);
			sprintf(buf, "{%d} %s", pos + 1, title);
			start = now;

			/* Trigger "quiet" acctions. */
			beatquiet = TRUE;
		}
	}

	/* If the user pressed 'i' recently, then show info for 4 seconds */
	showinfo = *config.show_info;
	if (blurskinfo || persistent)
	{
		if (showinfo == 'A')
		{
			config.show_info = config_default_show_info;
			showinfo = 'N';
		}
		else
		{
			showinfo = '4';
			if (blurskinfo)
			{
				start = now;
				persistent = TRUE;
			}
		}
		blurskinfo = FALSE;
	}

	/* If not supposed to show text, then we're done */
	switch (showinfo)
	{
	  case 'N': /* Never show info */
		return img;

	  case '4': /* 4 second info */
		if (now - start > 4)
		{
			persistent = FALSE;
			return img;
		}

	  case 'A': /* Always show info */
		break;
	}

	/* We don't want to draw onto the main image, because then the text
	 * would leave blur trails.  Most combinations of cpu_speed and
	 * overall_effect copy the image data into a temporary buffer, but
	 * the specific combination of cpu_speed=Fast and overall_effect=Normal
	 * (which is very common!) normally leaves the image in the main buffer.
	 * We need to detect this, and copy the image before we draw the text.
	 */
	if (img != img_tmp)
	{
		memcpy(img_tmp, img, img_chunks * 8);
		img = img_tmp;
	}

	/* draw the text */
	textdraw(img, height, bpl, "Center", buf);
	return img;
}

/* This is a generic rendering function.  It works for all signal styles.
 * The input always looks like one big PCM sample; if the input is really
 * a spectrum, then it will have been transformed by blurk_render_pcm()
 * into 256 PCM samples, with 20000 meaning "no sound" and smaller/negative
 * values representing a lot of sound.  This particular transformation was
 * chosen simply because the normal PCM plotter can then produce a nice-looking
 * spectrum graph.
 *
 * This function supports a variety of ways to plot the [pseudo-]PCM samples.
 * In addition to the usual line graph, it can also mirror the line graph or
 * produce a "bead" graph by passing the data off to render_bead().
 * The decision of how to plot is based on the value of "variation".
 */
static void update_image(gint32 loudness, gint ndata, gint16 *data)
{
	gint	i, y, thick, quiet, center;
	gint	beat;
	guchar	*img;
	gint	width, height, bpl;

#if 1
	/* If events are pending, then skip this frame */
	if (gdk_events_pending())
		return;
#endif

	/* If we completed a resize operation a few seconds ago, then save the
	 * new size now.
	 */
	if (savewhen != 0L && time(NULL) >= savewhen)
	{
		config_write(FALSE, NULL, NULL);
		savewhen = 0L;
	}

	/* If we're supposed to be in fullscreen mode, and aren't, then
	 * toggle fullscreen mode now.
	 */
	if (config.fullscreen_desired && !fullscreen_method)
		blursk_fullscreen(FALSE);

	/* Detect whether this is a beat, and choose a line thickness */
	beat = detect_beat(loudness, &thick, &quiet);

	/* If quiet, then maybe choose a new preset */
	if (quiet)
		preset_quiet();

	/* Perform the blurring.  This also affects whether the center of the
	 * signal will be moved lower in the window.
	 */
	center = img_height/2 + blur(beat, quiet);

	/* Perform the fade or solid flash */
	if (beat && !strcmp(config.flash_style, "Full flash"))
		i = 60;
	else
	{
		switch (config.fade_speed[0])
		{
		  case 'S':	i = -1;	break;	/* Slow */
		  case 'M':	i = -3;	break;	/* Medium */
		  case 'F':	i = -9;	break;	/* Fast */
		  default:	i = 0;		/* None */
		}
	}
	if (i != 0)
		loopfade(i);

	/* special processing for "Invert" & bitmap logo flashes */
	if (beat)
	{
		if (!strcmp(config.flash_style, "Invert flash"))
			img_invert();
		else if ((i = bitmap_index(config.flash_style)) >= 0)
			bitmap_flash(i);
	}

	/* Maybe change hue on beats */
	if (beat)
		color_beat();

	/* Add the signal data to the image */
	render(thick, center, ndata, data);

	/* Add floaters */
	drawfloaters(beat);

	/* shift the "ripple effect" from one frame to another */
	img_rippleshift += 3; /* cyclic, since img_rippleshift is a guchar */

	/* Apply the overall effect, if any */
	if (!strcmp(config.overall_effect, "Bump effect"))
	{
		img = img_bump(&width, &height, &bpl);
	}
	else if (!strcmp(config.overall_effect, "Anti-fade effect"))
	{
		img = img_travel(&width, &height, &bpl);
	}
	else if (!strcmp(config.overall_effect, "Ripple effect"))
	{
		img = img_ripple(&width, &height, &bpl);
	}
	else /* "Normal effect" */
	{
		img = img_expand(&width, &height, &bpl);
	}

	/* show info about the track */
	img = show_info(img, height, bpl);

	/* Allow the background color to change */
	color_bg(ndata, data);

	/* Copy the image into the window.  This also converts from
	 * 8-bits to 16/24/32 if necessary.
	 */
	GDK_THREADS_ENTER();
#if HAVE_XV
	if (!xv_putimg(img, width, height, bpl))
#endif
		gdk_draw_indexed_image(area->window,
			area->style->white_gc,
			0, 0, width, height, GDK_RGB_DITHER_NONE,
			img, bpl, color_map);
	GDK_THREADS_LEAVE();
}


/* This is the entry point for the pcm view.  Normally it just calls the
 * renderer with the input data.
 */
static void blursk_render_pcm(gint16 data[2][512])
{
	gint	i, imin, imax, start;
	gint32	loudness, delta_sum;

	/* if window isn't created yet, then do nothing */
	if (!blursk_window)
		return;

	/* If slow motion, then ignore odd-numbered frames */
	oddeven = !oddeven;
	if (config.slow_motion && oddeven)
		return;

	/* if not supposed to do PCM data, then do nothing */
	if (blursk_vp.num_pcm_chs_wanted == 0)
		return;

	/* Find the maximum and minimum, with the restriction that
	 * the minimum must occur after the maximum.
	 */
	for (i = 1, imin = imax = 0, delta_sum = 0; i < 127 / 2; i++)
	{
		if (data[0][i] < data[0][imin])
			imin = i;
		if (data[0][i] > data[0][imax])
			imin = imax = i;
		delta_sum += abs(data[0][i] - data[0][i - i]);
	}

	/* Triggered sweeps start halfway between min & max */
	start = (imax + imin) / 2;

	/* Compute the loudness.  We don't want to do a full spectrum analysis
	 * to do this, but we can guess the low-frequency sound is proportional
	 * to the maximum difference found (because loud low frequencies need
	 * big signal changes), and that high-frequency sound is proportional
	 * to the differences between adjacent samples.  We want to be sensitive
	 * to both of those, while ignoring the mid-range sound.
	 *
	 * Because we have only one low-frequency difference, but hundreds of
	 * high-frequency differences, we need to give more weight to the
	 * low-frequency difference (even though each high-frequency difference
	 * is small).
	 */
	loudness = (((gint32)data[0][imax] - (gint32)data[0][imin]) * 60 + delta_sum) / 75;

	/* Draw it */
	update_image(loudness, 256, &data[0][start]);
}


/* This is the entry point for the spectrum view.  It works by creating a
 * dummy pcm data table, and then calling the pcm renderer.
 */
static void blursk_render_freq(gint16 spectrum[2][256])
{
	gint	i,c;
	gint	y0, y1;
	gint16	data[NUM_BANDS * 2];
	gint32	loudness;

	/* If window isn't created yet, then do nothing */
	if (!blursk_window)
		return;

	/* If slow motion, then ignore odd-numbered frames */
	oddeven = !oddeven;
	if (config.slow_motion && oddeven)
		return;

	/* If not supposed to do spectrum plot, then do nothing */
	if (blursk_vp.num_freq_chs_wanted == 0)
		return;

	/* Compute the bar heights at each sample point, for both the left
	 * and right channels.  Also compute the overall loudness.
	 */
	for(i = 0, loudness = 0; i < NUM_BANDS; i++)
	{
		/* find the average within each bar */
		for(c = xscale[i], y0 = y1 = 0; c < xscale[i + 1]; c++)
		{
			y0 += spectrum[0][c];
			y1 += spectrum[1][c];
		}
		y0 /= xscale[i+1] - xscale[i] + 1;
		y1 /= xscale[i+1] - xscale[i] + 1;

		/* Add this value to the loudness, scaling it so we are less
		 * sensitive to midrange sounds.  Also, the spectrum data is
		 * naturally less sensitive to high frequencies, so we need to
		 * boost those.
		 */
		loudness += y0 * (abs(i - NUM_BANDS/2) + NUM_BANDS/2) * (4 + i);

		/* convert to values suitable for pcm-style graphing */
		y0 = 20000 - y0 * (4 + i);
		y1 = 20000 - y1 * (4 + i);

		/* store the values into the fake pcm sample table */
		if (blursk_vp.num_freq_chs_wanted == 2)
		{
			/* Two channels, joined in the center */
			data[NUM_BANDS - 1 - i] = y0;
			data[NUM_BANDS + i] = y1;
		}
		else
		{
			/* One band stretched across the window */
			data[i] = y0;
		}
	}

	/* Scale loudness to be in about the same range as PCM loudness */
	loudness /= (NUM_BANDS * 4);

	/* Plot it */
	update_image(loudness, NUM_BANDS * blursk_vp.num_freq_chs_wanted, data);
}

char *blursk_name(int i)
{
	static char *names[] =
	{
		"Oscilloscope", "Phase shift", "Flower", "Radial spectrum",
		"High/Low plot", "Stereo spectrum", "Mono spectrum",
		NULL
	};
	return names[i];
}

char *blursk_floater_name(int i)
{
	static char *names[] =
	{
		"No floaters", "Dots", "Slow floaters", "Fast floaters",
		"Retro floaters",
		NULL
	};
	return names[i];
}
