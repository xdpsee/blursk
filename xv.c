/* xv.c -- derived from the video_out_xv.c file in Xine.  The Xine copyright
 * appears below
 */

/* 
 * Copyright (C) 2000 the xine project
 * 
 * This file is part of xine, a unix video player.
 * 
 * xine is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * xine is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 * $Id: xv.c,v 1.32 2002/06/21 00:12:45 steve Exp $
 * 
 * video_out_xv.c, X11 video extension interface for xine
 *
 * based on mpeg2dec code from
 * Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * Xv image support by Gerd Knorr <kraxel@goldbach.in-berlin.de>
 *
 * 15 & 16 bpp support added by Franck Sicard <Franck.Sicard@solsoft.fr>
 *
 * xine-specific code by Guenter Bartsch <bartscgr@studbox.uni-stuttgart.de>
 * 
 */

/* IDEA: Use the XVideo (XV) extension to implement full-screen support for
 * the blursk visualization plugin.  If HAVE_XV is defined, then this source
 * file defines the following functions:
 *
 * int xv_start()
 *	Open a new connection to the default X server, and create an XV window
 *	on it.	Return the file descriptor of the X server connection if totally
 *	successful, or -1 if anything at all went slightly wrong.  Note that
 *	calling this function starts full-screen mode.  If it was already in
 *	full-screen mode due to an earlier call, then it simply returns the
 *	same file descriptor again.
 *
 * void xv_event()
 *	This is called when Gnome/Gtk detects that there is something waiting
 *	int the XVideo connection to the X server.  This function reads any
 *	pending events, and processes them.
 *
 * void xv_end()
 *	Terminate full-screen mode.  This involves closing the XV window and
 *	closing shutting down the connection to the x server.
 *
 * int xv_putimg()
 *	Copy an image out to the XV window.  Returns 1 if in full-screen mode,
 *	or 0 (without doing anything) in windowed mode.
 *
 * void xv_palette(int i)
 *	Convert the palette color at colors[i] from RGB to whatever XV needs.
 */


/* Define this to make xmms/blursk dump core if there's a segfault. */
#undef DUMP_CORE_ON_SEGFAULT

/* Define this to average multiple pixels in low-resolution planes */
#define XV_INTERPOLATE

#include "config.h"
#ifdef HAVE_XV

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>

#ifdef DUMP_CORE_ON_SEGFAULT
#include <signal.h>
#endif


#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <errno.h>

#include <X11/extensions/XShm.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>
#include <gtk/gtk.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>

#include "blursk.h"

#include "btnwhite.xbm"
#include "btnblack.xbm"
#ifdef HAVE_DL
# include <dlfcn.h>
#endif


/* Some FOURCC codes for common video modes */
#define FOURCC_YUY2 0x32595559
#define FOURCC_UYUV 0x56555955
#define FOURCC_YV12 0x32315659
#define FOURCC_I420 0x30323449

#define EDGE_THICKNESS 5

typedef enum { xv_NOFORMAT, xv_RGB, xv_YUV} xvfmt_t;
typedef enum { xv_UNMAPPED, xv_BUSY, xv_READY } xvstate_t;

typedef struct xv_image_info_s
{
	XvImage		*mImage;
	XShmSegmentInfo	mShminfo;
} xvimg_t;


static Display		*xvDisplay;	/* window display */
static int		xvScreen;	/* window screen */
static Colormap		xvColormap;	/* window colormap */
static uint		xvDepth;	/* window depth (bits per pixel) */
static uint		xvWidth;	/* window width */
static uint		xvHeight;	/* window height */
static Window		xvWindow;	/* window */
static Window		xvButton;	/* revert-to-window button */
static GC		xvGC;		/* window graphic context */
static xvfmt_t		xvFormat;	/* format -- xv_RGB or xv_YUV */
static XvImageFormatValues xvFmtInfo;	/* details of format */
static XvPortID		xvPort;		/* XVideo port */
static xvimg_t		*xvImg;		/* XVideo image buffer */
static gint		xvInput;	/* Gdk handle for window input hook */
static Cursor		xvCursor;	/* an invisible cursor */
static xvstate_t	xvState;	/* state of window */
static int		xvComplete;	/* MIT-SHM completion event */
static int		xvBlockSS;	/* screen saver blocking counter */
static long		xvTransparent;	/* masking color for XVideo */
static Window		xvBottomEdge;	/* window for masking off bottom edge */
static Window		xvRightEdge;	/* window for masking off right edge */

/* These are options.  Most of them are copied from the `config' struct when
 * we enter XV fullscreen mode, and never changed until after we leave it.
 * The xvOptYUV709 and xvOptFilter options are rechecked for each frame, though.
 */
static int		xvOptDouble;	/* double-width image */
static int		xvOptShm;	/* use shared memory */
static int		xvOptYUV709;	/* use alternative YUV conversion */
static int		xvOptRoot;	/* use root window */
static int		xvOptEdges;	/* mask out edges */

static guint16 colory[256], coloru[256], colorv[256];
#ifdef XV_INTERPOLATE
static char mixu[256][256], mixv[256][256];
#endif

#ifdef HAVE_DL

static void 		*xvdlhandle;

typedef XvImage *(*dlXvCreateImage_t)(Display*, XvPortID, int, char*, int, int);
typedef void (*dlXvFreeAdaptorInfo_t)(XvAdaptorInfo *);
typedef int (*dlXvGrabPort_t)(Display *, XvPortID, Time);
typedef XvImageFormatValues *(*dlXvListImageFormats_t)(Display*, XvPortID, int*);
typedef int (*dlXvPutImage_t)(Display*, XvPortID, Drawable, GC, XvImage*, int, int, unsigned, unsigned, int, int, unsigned, unsigned);
typedef int (*dlXvQueryAdaptors_t)(Display*, Drawable, int*, XvAdaptorInfo**);
typedef int (*dlXvQueryExtension_t)(Display*, unsigned*, unsigned*, unsigned*, unsigned*, unsigned*);
typedef int (*dlXvGetPortAttribute_t)(Display*, XvPortID, Atom, int *);
typedef int (*dlXvSetPortAttribute_t)(Display*, XvPortID, Atom, int);
typedef XvImage *(*dlXvShmCreateImage_t)(Display*, XvPortID, int, char*, int, int, XShmSegmentInfo*);
typedef int (*dlXvShmPutImage_t)(Display*, XvPortID, Drawable, GC, XvImage*, int, int, unsigned, unsigned, int, int, unsigned, unsigned, Bool);

static dlXvCreateImage_t	dlXvCreateImage;
static dlXvFreeAdaptorInfo_t	dlXvFreeAdaptorInfo;
static dlXvGrabPort_t		dlXvGrabPort;
static dlXvListImageFormats_t	dlXvListImageFormats;
static dlXvPutImage_t		dlXvPutImage;
static dlXvQueryAdaptors_t	dlXvQueryAdaptors;
static dlXvQueryExtension_t	dlXvQueryExtension;
static dlXvGetPortAttribute_t	dlXvGetPortAttribute;
static dlXvSetPortAttribute_t	dlXvSetPortAttribute;
static dlXvShmCreateImage_t	dlXvShmCreateImage;
static dlXvShmPutImage_t	dlXvShmPutImage;

# define XvCreateImage		(*dlXvCreateImage)
# define XvFreeAdaptorInfo	(*dlXvFreeAdaptorInfo)
# define XvGrabPort		(*dlXvGrabPort)
# define XvListImageFormats	(*dlXvListImageFormats)
# define XvPutImage		(*dlXvPutImage)
# define XvQueryAdaptors	(*dlXvQueryAdaptors)
# define XvQueryExtension	(*dlXvQueryExtension)
# define XvGetPortAttribute	(*dlXvGetPortAttribute)
# define XvSetPortAttribute	(*dlXvSetPortAttribute)
# define XvShmCreateImage	(*dlXvShmCreateImage)
# define XvShmPutImage		(*dlXvShmPutImage)

#endif /* HAVE_DL */

/* Return an invisible cursor */
static Cursor create_cursor(void)
{
	Pixmap	bm;
	XColor	black, dummy;
	Cursor	none;
	static char no_cursor_data[] = { 0,0,0,0, 0,0,0,0 };
		
	bm = XCreateBitmapFromData(xvDisplay, xvWindow, no_cursor_data, 8, 8);
	XAllocNamedColor(xvDisplay, xvColormap, "black", &black, &dummy);
	none = XCreatePixmapCursor(xvDisplay, bm, bm, &black, &black, 0, 0);
	XFreePixmap(xvDisplay, bm);
	return none;
}


/* Free an XV image buffer */
static void free_image(void)
{
	/* if already freed, do nothing */
	if (!xvImg)
		return;

	if (xvOptShm)
	{
		/* Free the image info */
		XFree (xvImg->mImage);

		/* Detach the freed memory from the X server */
		XShmDetach (xvDisplay, &xvImg->mShminfo);

		/* Free the shared memory */
		shmdt (xvImg->mShminfo.shmaddr);
		shmctl (xvImg->mShminfo.shmid, IPC_RMID,NULL);
	}
	else
	{
		/* Free the image data */
		free(xvImg->mImage->data);

		/* Free the image info */
		XFree (xvImg->mImage);
	}

	/* Free the xvImg itself */
	free (xvImg);
	xvImg = NULL;
}

/* Allocate an XV image buffer */
static xvimg_t *alloc_image(int width, int height)
{

	xvimg_t	*img;

	/* Allocate the xvImg struct */
	img = (xvimg_t *)malloc(sizeof(xvimg_t));

	/* Round the dimensions upward to a multiple of 4 */
	width = ((width - 1) | 0x3) + 1;
	height = ((height - 1) | 0x3) + 1;

	/* shared memory is different from local memory */
	if (xvOptShm)
	{
		img->mImage = XvShmCreateImage(xvDisplay, xvPort, xvFmtInfo.id,
					NULL, width, height, &img->mShminfo);
		if (!img->mImage) 
		{
			about_error("Could not allocate shared memory image\n"
			"You probably won't be able to use XV with shared memory,\n"
			"but you might get it to work if you disable shared memory\n"
			"via Blursk's [Advanced] dialog.  Perhaps updating your\n"
			"X server or libraries would help.\n");
			free(img);
			return NULL;
		}
		if (img->mImage->width < width || img->mImage->height < height)
		{
			about_error("Tried to allocate %dx%d image, but got %dx%d\n"
			"XVideo usually has a limit on how large an image it can\n"
			"handle.  Sometimes the limit is smaller than the size\n"
			"reported by xvinfo.  Try reducing the size of your Blursk\n"
			"window.  If you're using \"XV doubled\", try switching to\n"
			"plain \"XV\".",
				width, height,
				img->mImage->width, img->mImage->height);
			XFree(img->mImage);
			free(img);
			return NULL;
		}

		img->mShminfo.shmid = shmget(IPC_PRIVATE, 
						 img->mImage->data_size, 
						 IPC_CREAT | 0777);
		if (img->mShminfo.shmid < 0 )
		{
			about_error("Shared memory error, errno=%d\n"
			"I have no idea how to fix this.  Sorry.\n", errno); 
			XFree(img->mImage);
			free(img);
			return NULL;
		}

		img->mShminfo.shmaddr = (char *)shmat(img->mShminfo.shmid, 0,0);
		if (!img->mShminfo.shmaddr)
		{
			about_error("Shared memory error (address NULL)\n"
			"I have no idea how to fix this.  Sorry.\n");
			shmctl(xvImg->mShminfo.shmid, IPC_RMID,NULL);
			XFree(img->mImage);
			free(img);
			return NULL;
		}

		if (img->mShminfo.shmaddr == ((char *) -1))
		{
			about_error("Shared memory error (address error)\n"
			"I have no idea how to fix this.  Sorry.\n");
			shmctl(xvImg->mShminfo.shmid, IPC_RMID,NULL);
			XFree(img->mImage);
			free(img);
			return NULL;
		}

		img->mShminfo.readOnly = False;
		img->mImage->data = img->mShminfo.shmaddr;

		XShmAttach(xvDisplay, &img->mShminfo);
		XSync(xvDisplay, False);

		shmctl(img->mShminfo.shmid, IPC_RMID, NULL);
	}
	else
	{
		/* allocate the XV image buffer */
		img->mImage = XvCreateImage(xvDisplay, xvPort, xvFmtInfo.id,
					NULL, width, height);
		if (!img->mImage) 
		{
			about_error("Could not allocate local image\n"
			"I have no idea how to fix this.  Sorry.\n");
			free(img);
			return NULL;
		}
		if (img->mImage->width < width || img->mImage->height < height)
		{
			about_error("Tried to allocate %dx%d image, but got %dx%d\n"
			"XVideo usually has a limit on how large an image it can\n"
			"handle.  Sometimes the limit is smaller than the size\n"
			"reported by xvinfo.  Try reducing the size of your Blursk\n"
			"window.  If you're using \"XV doubled\", try switching to\n"
			"plain \"XV\".",
				width, height,
				img->mImage->width, img->mImage->height);
			XFree(img->mImage);
			free(img);
			return NULL;
		}

		/* The XV image buffer needs data memory.  Allocate it */
		img->mImage->data = malloc(img->mImage->data_size);
		if (!img->mImage) 
		{
			about_error("Could not allocate local memory for image data\n"
			"I have no idea how to fix this.  Sorry.\n");
			free(img);
			return NULL;
		}
	}
	
	return img;
}

/*----------------------------------------------------------------------------*/

/* Return a Pixmap containing the button image */
static Pixmap mkbutton(void)
{
	Pixmap	whitebits;
	Pixmap	blackbits;
	Pixmap	combined;
	XColor	black;
	GC	gc;

	/* Use a dark version of the base color as "black" */
	black.red = (config.color >> 9) & 0x7f10;
	black.green = (config.color >> 1)& 0x7f10;
	black.blue = (config.color << 7) & 0x7f10;
	black.red += black.red / 2;
	black.green += black.green / 2;
	black.blue += black.blue / 2;
	black.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(xvDisplay, xvColormap, &black);

	/* Generate the black & white parts of the image */
	whitebits = XCreateBitmapFromData(xvDisplay, xvButton,
					btnwhite_bits,
					btnwhite_width, btnwhite_height);
	blackbits = XCreateBitmapFromData(xvDisplay, xvButton,
					btnblack_bits,
					btnblack_width, btnblack_height);

	/* Create a Pixmap to combine them */
	combined = XCreatePixmap(xvDisplay, xvButton,
				btnwhite_width, btnwhite_height, xvDepth);

	/* Create a graphic context for use while setting up the image */
	gc = XCreateGC(xvDisplay, combined, 0L, NULL);

	/* Background will be transparent */
	XSetForeground(xvDisplay, gc, xvTransparent);
	XFillRectangle(xvDisplay, combined, gc, 0, 0,
			btnwhite_width, btnwhite_height);

	/* Add the black parts */
	XSetForeground(xvDisplay, gc, black.pixel);
	XSetFillStyle(xvDisplay, gc, FillStippled);
	XSetStipple(xvDisplay, gc, blackbits);
	XFillRectangle(xvDisplay, combined, gc, 0, 0,
			btnwhite_width, btnwhite_height);

	/* Add the white parts */
	XSetForeground(xvDisplay, gc, WhitePixel(xvDisplay, xvScreen));
	XSetStipple(xvDisplay, gc, whitebits);
	XFillRectangle(xvDisplay, combined, gc, 0, 0,
			btnwhite_width, btnwhite_height);

	/* Clean up */
	XFreeGC(xvDisplay, gc);
	XFreePixmap(xvDisplay, whitebits);
	XFreePixmap(xvDisplay, blackbits);

	return combined;
}

/* Adjust the visibility of the edges.  This makes the xvBottomEdge and
 * xvRightEdge windows be mapped or unmapped, as appropriate.  It also ensures
 * that they're below any other window, which is important when you're running
 * it in the root window.
 */
static void drawedges(void)
{
	static int lower;

	if (config.fullscreen_edges && !xvOptEdges)
	{
		xvOptEdges = config.fullscreen_edges;
		XMapWindow(xvDisplay, xvBottomEdge);
		XMapWindow(xvDisplay, xvRightEdge);
		XLowerWindow(xvDisplay, xvBottomEdge);
		XLowerWindow(xvDisplay, xvRightEdge);
	}
	else if (!config.fullscreen_edges && xvOptEdges)
	{
		xvOptEdges = config.fullscreen_edges;
		XUnmapWindow(xvDisplay, xvBottomEdge);
		XUnmapWindow(xvDisplay, xvRightEdge);
	}
	else if (xvOptEdges && lower++ > 15)
	{
		lower = 0;
		XLowerWindow(xvDisplay, xvBottomEdge);
		XLowerWindow(xvDisplay, xvRightEdge);
	}
}


/* Open a new connection to the default X server, and create an XV window
 * on it.  Return 1 if successful, or 0 if anything went wrong.
 */
int xv_start(void)
{
	XWindowAttributes	attribs;
	unsigned int		ver,rel,req,ev,err;
	int			adaptors, formats;
	unsigned int		i;
	XvAdaptorInfo		*ai;
	XvImageFormatValues	*fo;
	XSetWindowAttributes	attr;
	unsigned long		valuemask;
	Atom			colorkey;
	int			x, y;

	/* If the display is already open for XV, then close it and fail.
	 * This way, calling xv_start() will alternately start & stop XV.
	 */
	if (xvDisplay)
	{
		xv_end();
		return 0;
	}

	/* Open a connection to the display */
	xvDisplay = XOpenDisplay(NULL);
	if (!xvDisplay)
	{
		if (!getenv("DISPLAY"))
			about_error("Could not connect to the X server.\n"
			"You need to set the DISPLAY environment variable.\n");
		else
			about_error("Could not connect to the X server.\n"
			"Currently, the DISPLAY environment variable is set to \"%s\".\n"
			"Perhaps you need to change that?\n", getenv("DISPLAY"));

		return 0;
	}
#ifdef DUMP_CORE_ON_SEGFAULT
	signal(SIGSEGV, SIG_DFL);
	XSetErrorHandler(abort);
#endif
#if 0
	/* This is sometimes handy during debugging, but it also has the
	 * side-effect of interfering with SHM completion events.  If you
	 * enable this code, you'll probably want to turn off the
	 * fullscreen_completion * option too (in the Advanced dialog).
	 */
	XSynchronize(xvDisplay, 1);
#endif
	xvScreen = DefaultScreen(xvDisplay);
	xvColormap = DefaultColormap(xvDisplay, xvScreen);

	/* Copy options into local variables */
	xvOptDouble = !strcmp(config.fullscreen_method, "Use XV doubled");
	xvOptShm = config.fullscreen_shm;
	xvOptYUV709 = config.fullscreen_yuv709;
	xvOptRoot = config.fullscreen_root;
	/* xvOptEdges will be set in drawedges() */

	/* Get default screen's attributes */
	XGetWindowAttributes(xvDisplay, DefaultRootWindow(xvDisplay), &attribs);
	xvDepth = attribs.depth;
	xvWidth = attribs.width;
	xvHeight = attribs.height;

	/* If the default depth is 8, then hopefully the screen also supports
	 * other depths.  (Some Sun workstations are like this.)  If this
	 * assumption is wrong then we'll discover that when we try to open
	 * the window, later.
	 */
	if (xvDepth == 8)
		xvDepth = 24;

#ifdef HAVE_DL
	xvdlhandle = dlopen("libXv.so", RTLD_NOW);
	if (!xvdlhandle)
	{
		about_error("Could not load the XVideo library, \"libXv.so\"\n"
		"This library is new in XFree86 4.x.  Blursk needs a\n"
		"dymamically linkable form of that library.  Either you're\n"
		"some other X server, or an old version of XFree86, or you\n"
		"only have a static version of that library, \"libXv.a\",\n"
		"which doesn't do Blursk any good.  You can't use XV until\n"
		"you upgrade\n");
		goto Fail;
	}

	if (!(dlXvCreateImage = (dlXvCreateImage_t)dlsym(xvdlhandle, "XvCreateImage"))
	 || !(dlXvFreeAdaptorInfo = (dlXvFreeAdaptorInfo_t)dlsym(xvdlhandle, "XvFreeAdaptorInfo"))
	 || !(dlXvGrabPort = (dlXvGrabPort_t)dlsym(xvdlhandle, "XvGrabPort"))
	 || !(dlXvListImageFormats = (dlXvListImageFormats_t)dlsym(xvdlhandle, "XvListImageFormats"))
	 || !(dlXvPutImage = (dlXvPutImage_t)dlsym(xvdlhandle, "XvPutImage"))
	 || !(dlXvQueryAdaptors = (dlXvQueryAdaptors_t)dlsym(xvdlhandle, "XvQueryAdaptors"))
	 || !(dlXvQueryExtension = (dlXvQueryExtension_t)dlsym(xvdlhandle, "XvQueryExtension"))
	 || !(dlXvGetPortAttribute = (dlXvGetPortAttribute_t)dlsym(xvdlhandle, "XvGetPortAttribute"))
	 || !(dlXvSetPortAttribute = (dlXvSetPortAttribute_t)dlsym(xvdlhandle, "XvSetPortAttribute"))
	 || !(dlXvShmCreateImage = (dlXvShmCreateImage_t)dlsym(xvdlhandle, "XvShmCreateImage"))
	 || !(dlXvShmPutImage = (dlXvShmPutImage_t)dlsym(xvdlhandle, "XvShmPutImage")))
	{
		about_error("Could not resolve all XVideo library function names\n"
		"Perhaps you have an old version of \"libXv.so\"?\n");
		goto Fail;
	}
#endif /* HAVE_DL */

	/* Check for the presence of the MIT-SHM extension. */
	if (xvOptShm && !XShmQueryExtension(xvDisplay))
	{
		about_error("Server doesn't support shared memory\n"
		"Shared memory is only available if xdpyinfo includes \"MIT-SHM\"\n"
		"in the list of extensions.  I'll try running without it.\n");
		xvOptShm = FALSE;
	}
	if (xvOptShm)
	{
		xvComplete = XShmGetEventBase(xvDisplay) + ShmCompletion;
	}

	/* Check for the presence of the XVideo extension */
	if (Success != XvQueryExtension(xvDisplay, &ver, &rel, &req, &ev, &err))
	{
		about_error("This server doesn't support XVideo\n"
		"XVideo is only available if xdpyinfo includes \"XVideo\"\n"
		"in the list of extensions.\n");
		goto Fail;
	}

	/* Choose an XVideo adaptor */
	xvPort = 0;
	if (Success != XvQueryAdaptors(xvDisplay, DefaultRootWindow(xvDisplay), 
				 &adaptors, &ai)
	 || adaptors == 0) 
	{
		about_error("This server has no XVideo adaptors\n"
		"Either your video card has no video scaling hardware,\n"
		"or XFree86 has no driver for it.\n");
		goto Fail;
	}
	for (i = 0; i < adaptors && (~ai[i].type & XvImageMask); i++) 
	{
	}
	if (i < adaptors)
		xvPort = ai[i].base_id;
	XvFreeAdaptorInfo(ai);
	if (i >= adaptors)
	{
		about_error("No XVideo adaptors support imaging\n"
		"Blursk's XV module can't work without imaging.  Perhaps\n"
		"a newer version of XFree86 would solve this.  The early\n"
		"versions of the XVideo extension didn't include imaging;\n"
		"it was added in XFree86 4.1.0\n");
		goto Fail;
	}

	/* Does it support any formats that we know & love? */
	fo = XvListImageFormats(xvDisplay, xvPort, &formats);
	for (xvFormat = xv_NOFORMAT, i = 0; i < formats; i++)
	{
#if 0
		printf("fo[%d]={'%.4s', %s, %s, bpp=%d, %s, planes=%d\n",
			i, (char*)&fo[i].id,
			fo[i].type==XvRGB?"XvRGB":"XvYUV",
			fo[i].byte_order==LSBFirst?"LSBFirst":"MSBFirst",
			fo[i].bits_per_pixel,
			fo[i].format==XvPacked ? "XvPacked" : "XvPlanar",
			fo[i].num_planes);
		if (fo[i].type==XvRGB)
			printf("depth=%d, masks={0x%x,0x%x,0x%x}\n",
				fo[i].depth,
				fo[i].red_mask,
				fo[i].green_mask,
				fo[i].blue_mask);
		else
			printf("bits=[%d,%d,%d], hperiod=[%d,%d,%d], vperiod=[%d,%d,%d], order=\"%s\", %s\n",
				fo[i].y_sample_bits,
				fo[i].u_sample_bits,
				fo[i].v_sample_bits,
				fo[i].horz_y_period,
				fo[i].horz_u_period,
				fo[i].horz_v_period,
				fo[i].vert_y_period,
				fo[i].vert_u_period,
				fo[i].vert_v_period,
				fo[i].component_order,
				fo[i].scanline_order==XvTopToBottom?"XvTopToBottom":"XvBottomToTop");
#endif

		if (fo[i].id != FOURCC_YV12 && fo[i].id != FOURCC_I420)
			continue;
		xvFormat = xv_YUV;
		xvFmtInfo = fo[i];
		break;
	}
	XFree(fo);
	if (xvFormat == xv_NOFORMAT)
	{
		about_error("This XVideo adaptor doesn't support Blursk\n"
		"Blursk only works with planar YV12 or I420 image formats.\n"
		"Although this XVideo adaptor does support some imaging\n"
		"formats, it doesn't support any that Blursk knows how to\n"
		"use.  Sorry.");
		goto Fail;
	}

	/* Start the video port */
	if(Success != XvGrabPort(xvDisplay, xvPort, CurrentTime))
	{
		about_error("Couldn't grab the XVideo port\n"
		"Is some other program already using it?\n");
		goto Fail;
	}

	/* Fetch the colorkey value. */
	colorkey = XInternAtom(xvDisplay, "XV_COLORKEY", True);
	if (colorkey == None
	 || Success != XvGetPortAttribute(xvDisplay, xvPort, colorkey, &x))
		xvTransparent = BlackPixel(xvDisplay, xvScreen);
	else
		xvTransparent = (long)x;

	if (xvOptRoot)
	{
		/* Use the root window */
		xvWindow = DefaultRootWindow(xvDisplay);
		xvState = xv_READY;

		/* We want to get Expose events for the root window */
		memset(&attr, 0, sizeof attr);
		attr.event_mask = ExposureMask;
		XChangeWindowAttributes(xvDisplay, xvWindow, CWEventMask, &attr);

		/* Choose a button position -- whichever corner of the Blursk
		 * window happens to be closest to the screen corner.
		 */
		x = config.x + config.width;
		if (config.x < xvWidth - x)
			x = config.x;
		else
			x -= btnwhite_width - 6;
		y = config.y + config.height;
		if (config.y < xvHeight - y)
			y = config.y;
		else
			y -= btnwhite_height - 6;

		/* Create a button for reverting to window mode.  Note that
		 * the button won't accept keypresses -- it can't get input
		 * focus, so those events would never come anyway.
		 */
		attr.background_pixel = BlackPixel(xvDisplay, xvScreen);
		attr.event_mask = ButtonPressMask | ExposureMask;
		attr.override_redirect = True;
		valuemask = CWBackPixel | CWEventMask | CWOverrideRedirect;
		xvButton = XCreateWindow(xvDisplay,
					 RootWindow(xvDisplay, xvScreen),
					 x, y,
					 btnwhite_width, btnwhite_height,
					 0,			 /* border */
					 xvDepth,		 /* depth */
					 InputOutput,		 /* class */
					 CopyFromParent,	 /* visual */
					 valuemask, &attr);	 /* attributes*/

		/* Create a button image & use it as the window's background */
		XSetWindowBackgroundPixmap(xvDisplay, xvButton, mkbutton());

		/* show the window */
		XMapRaised(xvDisplay, xvButton);

		/* create a "hand" cursor for the button */
		xvCursor = XCreateFontCursor(xvDisplay, XC_hand2);
		XDefineCursor(xvDisplay, xvButton, xvCursor);
	}
	else
	{
		/* Create a window that fills the screen */
		attr.background_pixel = BlackPixel(xvDisplay, xvScreen);
		attr.event_mask = KeyPressMask | ButtonPressMask | StructureNotifyMask | ExposureMask;
		attr.override_redirect = True;
		valuemask = CWBackPixel | CWEventMask | CWOverrideRedirect;
		xvWindow = XCreateWindow(xvDisplay,
					 RootWindow(xvDisplay, xvScreen),
					 0, 0, xvWidth, xvHeight,/* geometry */
					 0,			 /* border */
					 xvDepth,		 /* depth */
					 InputOutput,		 /* class */
					 CopyFromParent,	 /* visual */
					 valuemask, &attr);	 /* attributes*/
		xvState = xv_UNMAPPED;
		XMapRaised(xvDisplay, xvWindow);

		/* Give it input focus, so keypresses can be detected */
		XSetInputFocus(xvDisplay, xvWindow, RevertToNone, CurrentTime);

		/* There is no button for reverting */
		xvButton = None;
	}

	/* Create some skinny windows to mask off the right & bottom edges. */
	attr.background_pixel = BlackPixel(xvDisplay, xvScreen);
	attr.override_redirect = True;
	valuemask = CWBackPixel | CWOverrideRedirect;
	xvRightEdge = XCreateWindow(xvDisplay,
				 xvWindow,
				 xvWidth - EDGE_THICKNESS, 0,
				 EDGE_THICKNESS, xvHeight - EDGE_THICKNESS,
				 0,			 /* border */
				 xvDepth,		 /* depth */
				 InputOutput,		 /* class */
				 CopyFromParent,	 /* visual */
				 valuemask, &attr);	 /* attributes*/
	xvBottomEdge = XCreateWindow(xvDisplay,
				 xvWindow,
				 0, xvHeight - EDGE_THICKNESS,
				 xvWidth, EDGE_THICKNESS,
				 0,			 /* border */
				 xvDepth,		 /* depth */
				 InputOutput,		 /* class */
				 CopyFromParent,	 /* visual */
				 valuemask, &attr);	 /* attributes*/

	/* Create a graphic context.  We don't do anything fancy with it, but
	 * XvShmPutImage() requires one anyway.
	 */
	xvGC = XCreateGC(xvDisplay, xvWindow, 0L, NULL);

	/* Make the mouse pointer be invisible */
	if (!xvOptRoot)
	{
		xvCursor = create_cursor();
		XDefineCursor(xvDisplay, xvWindow, xvCursor);
	}

	/* Convert the whole palette */
	color_genmap(FALSE);

	/* Make the problematic areas of the screen go away */
	xvOptEdges = False;
	drawedges();

	/* Arrange for events to be handled */
	xvInput = gdk_input_add_full(XConnectionNumber(xvDisplay),
				GDK_INPUT_READ,
				(GdkInputFunction)xv_event,
				NULL,
				(GdkDestroyNotify)NULL);

	/* There could already be events waiting.  Better check... */
	XFlush(xvDisplay);
	xv_event();
	if (!xvDisplay)
		goto Fail;

Succeed:
	return 1;

Fail:
	XCloseDisplay(xvDisplay);
	xvDisplay = NULL;
#ifdef HAVE_DL
	if (xvdlhandle)
	{
		dlclose(xvdlhandle);
		xvdlhandle = NULL;
	}
#endif
	return 0;
}

/* This is called when Gnome/Gtk detects that there is something waiting
 * in the XVideo connection to the X server.  This function reads any
 * pending events, and processes them.
 *
 * NOTE: This function can end the XV fullscreen mode.  Immediately after
 * calling this function, you should check whether xvDisplay has become NULL.
 */
void xv_event(void)
{
	XEvent	xev;
	char	text[10];
	KeySym	mykey;
	gint	volume;
	static XComposeStatus compose;

	while (xvDisplay && XPending(xvDisplay))
	{
		XNextEvent(xvDisplay, &xev);
		if (xev.type == xvComplete)
		{
			if (xvState == xv_BUSY)
			{
				xvState = xv_READY;
			}
		}
		else
		{
			switch (xev.type)
			{
			  case MapNotify:
				/* can only happen for xvWindow */
				if (xvState == xv_UNMAPPED)
				{
					xvState = xv_READY;
				}
				XSetInputFocus(xvDisplay,
						xvWindow,
						RevertToPointerRoot,
						CurrentTime);
				break;

			  case KeyPress:
				/* can only happen for xvWindow */
				if (1 == XLookupString(&xev.xkey, text, sizeof text, &mykey, &compose))
				{
					switch (*text)
					{
					  case '\r':
					  case '\n':
					  case 'f':
					  case 'F':
						config.fullscreen_desired = FALSE;
						xv_end();
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
				else if (mykey == XK_Up)
				{
					volume = xmms_remote_get_main_volume(0);
					volume += 2;
					if (volume > 100)
						volume = 100;
					xmms_remote_set_main_volume(0, volume);
				}
				else if (mykey == XK_Down)
				{
					volume = xmms_remote_get_main_volume(0);
					volume -= 2;
					if (volume < 0)
						volume = 0;
					xmms_remote_set_main_volume(0, volume);
				}
				break;

			  case ButtonPress:
				/* can happen for either xvWindow or xvButton */
				switch (xev.xbutton.button)
				{
				  case Button1:
					config.fullscreen_desired = FALSE;
					xv_end();
					break;

				  case Button2:
					gtk_selection_convert(blursk_window,
						GDK_SELECTION_PRIMARY,
						GDK_SELECTION_TYPE_STRING,
						GDK_CURRENT_TIME);
					break;

				  case Button3:
					/* This should only work on root mode's
					 * button.  If we pop up the config
					 * dialog in full-screen mode, it'll
					 * grab input focus and  there's no
					 * easy way for the full-screen window
					 * to grab it back later, so <Alt-Enter>
					 * won't work after that.  It's best to
					 * avoid that problem.
					 */
					if (xvOptRoot)
						config_dialog();
					break;

				  case Button4:
					volume = xmms_remote_get_main_volume(0);
					volume += 8;
					if (volume > 100)
						volume = 100;
					xmms_remote_set_main_volume(0, volume);
					break;

				  case Button5:
					volume = xmms_remote_get_main_volume(0);
					volume -= 8;
					if (volume < 0)
						volume = 0;
					xmms_remote_set_main_volume(0, volume);
					break;
				}
				break;
			} /* switch xev.type */
		} /* if xev.type != xvComplete */
	} /* while events pending */
}

/* Terminate full-screen mode.  This involves closing the XV window and
 * shutting down the connection to the x server.
 */
void xv_end()
{
	/* If the full-screen display isn't open then do nothing */
	if (!xvDisplay)
		return;

	/* Free the image buffer, if there is one */
	if (xvImg)
	{
		free_image();
	}

	/* Tell Gtk to stop sending us events */
	gdk_input_remove(xvInput);

	/* If we were using the root window, then erase it so the wallpaper
	 * is redrawn.
	 */
	if (xvOptRoot)
	{
		XClearWindow(xvDisplay, xvWindow);
	}
	XFlush(xvDisplay);

	/* Close the connection.  This frees all XV resources */
	XCloseDisplay(xvDisplay);
	xvDisplay = NULL;

	/* Tell the main Blursk module to resume window updates */
	blursk_fullscreen(TRUE);

#ifdef HAVE_DL
	/* close the XVideo library */
	if (xvdlhandle)
	{
		dlclose(xvdlhandle);
		xvdlhandle = NULL;
	}
#endif
}

/* Copy an image out to the XV window.  Returns 1 if in full-screen mode,
 * or 0 (without doing anything) in windowed mode.
 */
int xv_putimg(guchar *img, int width, int height, int bpl)
{
	int	p, x, y;/* plane number, and a position within it */
	int	i, j;
	guchar	*src;
	char	*dst8;
	guint16	*dst16;
	guint16	*color;
	int	hp, vp;	/* horizontal & vertical period, within a plane */
	int	mvp, mhp;/* scaling factors for width & height, if "Doubled" */
#ifdef XV_INTERPOLATE
	guchar	*src2;
	char	(*mix)[256];
#endif

	/* If not in full-screen mode, then just return 0 */
	if (!xvDisplay)
		return 0;

	/* if not ready for a new image, skip it */
	if (xvState != xv_READY)
	{
		/* check for events, and then retry the state test */
		xv_event();
		if (!xvDisplay)
			return 0;
		if (xvState != xv_READY)
			return 1;
	}

	/* if the yuv709 option has changed, then reconvert the whole palette */
	if (xvOptYUV709 != config.fullscreen_yuv709)
	{
		xvOptYUV709 = config.fullscreen_yuv709;
		for (x = 0; x < 256; x++)
			xv_palette(x, colors[x]);
	}

	/* If the edges option has changed, then map/unmap the edge windows.
	 * Even if the option hasn't changed, we still want to lower those
	 * windows periodically.
	 */
	drawedges();

	/* In double mode, increase the size of the source image.  This may be
	 * desirable because the color information is split among groups of
	 * pixels, which can look bad when adjacent pixels have wildly
	 * different colors.  By increasing the image size and writing each
	 * source pixel as multiple XV pixels, we can avoid that weirdness.
	 */
	if (xvOptDouble)
	{
		width *= mhp = xvFmtInfo.horz_u_period;
		height *= mvp = xvFmtInfo.vert_u_period;
	}

	/* If size changed, then free old image.  Note that the XV image buffer
	 * is often going to be a fixed-size image that is larger than we asked
	 * for, so we only bother to reallocate if it is smaller.
	 */
	if (xvImg && (xvImg->mImage->width < width
	           || xvImg->mImage->height < height))
	{
		free_image();
	}

	/* If first call, or if size changed, then allocate image. */
	if (!xvImg)
	{
		xvImg = alloc_image(width, height);
		if (!xvImg)
		{
			/* We couldn't allocate an image buffer.  The reason
			 * for this was already output, so we just need to
			 * exit fullscreen mode.
			 */
			xv_end();
			config.fullscreen_desired = FALSE;
			return 0;
		}
	}

	/* Convert the indexed-color image to RGB or YUV */
	/* can only be planar YV12 or I420, so for each plane... */
	for (p = 0; p < xvFmtInfo.num_planes; p++)
	{
		/* choose the periods & YUV plane */
#if 0 /* Why does XVideo claim the planes are in YVU order?  They're YUV! */
		switch (xvFmtInfo.component_order[p])
#else
		switch ("YUV"[p])
#endif
		{
		  case 'Y':
			hp = xvFmtInfo.horz_y_period;
			vp = xvFmtInfo.vert_y_period;
			color = colory;
#ifdef XV_INTERPOLATE
			mix = NULL;
#endif
			break;

		  case 'U':
			hp = xvFmtInfo.horz_u_period;
			vp = xvFmtInfo.vert_u_period;
			color = coloru;
#ifdef XV_INTERPOLATE
			mix = mixu;
#endif
			break;

		  case 'V':
			hp = xvFmtInfo.horz_v_period;
			vp = xvFmtInfo.vert_v_period;
			color = colorv;
#ifdef XV_INTERPOLATE
			mix = mixv;
#endif
			break;
		}

		if (xvOptDouble)
		{
			/* Each source pixel will be converted into multiple
			 * destination pixels, for some planes.
			 */

			/* for each destination row... */
			for (y = 0; y < height / vp; y++)
			{
				/* initialize the source & dest pointers */
				src = img + (y * vp / mvp) * bpl;
				dst8 = xvImg->mImage->data
					+ xvImg->mImage->offsets[p]
					+ y * xvImg->mImage->pitches[p];

				/* typically, each source pixel becomes 1 or 2
				 * destination pixels.  Handle those cases
				 * separately, for the sake of speed.
				 */
				switch (mhp / hp)
				{
				  case 1:
					/* copy pixel color info one-to-one */
					for (x = width / hp; x > 0; x--)
						*dst8++ = (guchar)color[*src++];
					break;

				  case 2:
					/* copy pixel color info one-to-two */
					dst16 = (guint16 *)dst8;
					for (x = width / hp / 2; x > 0; x--)
						*dst16++ = color[*src++];
					break;

				  default:
					/* other width, do it the hard way */
					for (x = 0; x < width; x += hp)
					{
						j = color[*src++];
						for (i = mhp; i > 0; i -= hp)
							*dst8++ = j;
					}
					break;
				}
			}
		}
		else
		{
			/* For some planes, we'll reduce the resolution */

			/* for each destination row... */
			for (y = 0; y < height; y += vp)
			{
				/* initialize the source & dest pointers */
				src = img + y * bpl;
				dst8 = xvImg->mImage->data
					+ xvImg->mImage->offsets[p]
					+ y / vp * xvImg->mImage->pitches[p];

#ifdef XV_INTERPOLATE
				if (mix && vp >= 2 && hp >= 2)
				{
					/* figure out where the second source
					 * point is, relative to the first.
					 */
					src2 = src + bpl + 1;

					/* for each destination pixel... */
					for (x = 0; x < width; x += hp)
					{
						/* convert & copy */
						*dst8++ = mix[*src][*src2];
						src += hp;
						src2 += hp;
					}
				}
				else
#endif
				if (hp == 1)
				{
					/* for each destination pixel... */
					for (x = width; --x >= 0; )
					{
						/* convert & copy */
						*dst8++ = color[*src++];
					}
				}
				else
				{
					/* for each destination pixel... */
					for (x = 0; x < width; x += hp)
					{
						/* convert & copy */
						*dst8++ = color[*src];
						src += hp;
					}
				}
			}
		}

	}

	/* Send the image to the XVideo port */
	if (!xvOptShm)
	{
		XvPutImage(xvDisplay, xvPort, xvWindow, xvGC, xvImg->mImage, 
				0, 0, width, height,
				0, 0, xvWidth, xvHeight);
	}
	else
	{
		XvShmPutImage(xvDisplay, xvPort, xvWindow, xvGC, xvImg->mImage, 
				0, 0, width, height,
				0, 0, xvWidth, xvHeight,
				True);
		xvState = xv_BUSY;
	}

	/* Normally (when we're not running in the root window), we want to
	 * disable the screen saver.  XResetScreenSaver() should tickle the
	 * screen saver, to make it believe the user is still present & active.
	 * Sadly, some screen savers don't play by the rules, so we also
	 * wiggle the mouse cursor.
	 */
	if (!xvOptRoot && ++xvBlockSS >= 100)
	{
		XResetScreenSaver(xvDisplay);
		XWarpPointer(xvDisplay, None, xvWindow, 0, 0, 0, 0,
			config.x + rand_0_to(config.width), config.y);
		xvBlockSS = 0;
	}

	/* Since we never call XNextEvent() for this connection, the requests
	 * won't be flushed automatically.  We need to flush them explicitly.
	 */
	XFlush(xvDisplay);

	return 1;
}

/* Convert a color from RGB to whatever the XVideo port uses */
void xv_palette(int i, guint32 rgb)
{
	double r, g, b;
	double y, u, v;
#ifdef XV_INTERPOLATE
	int	j, iy, jy;
#endif

	/* If not in full-screen mode, then do nothing */
	if (!xvDisplay)
		return;

	/* If RGB then no conversion is necessary */
	if (xvFmtInfo.type == XvRGB)
		return;

	/* Break the color down into separate R/G/B components */
	r = (double)(rgb & 0xff);
	g = (double)((rgb >> 8) & 0xff);
	b = (double)((rgb >> 16) & 0xff);

	/* !!! Should we clamp RGB values to 16...240 range here? */

	/* Compute YUV values from RGB */
	if (xvOptYUV709)
	{
		/* Alternative YCbCr 709 conversion formula */
		y =  0.183 * r + 0.614 * g + 0.062 * b +  16.0;
		u = -0.101 * r - 0.338 * g + 0.439 * b + 128.0;
		v =  0.439 * r - 0.399 * g - 0.040 * b + 128.0;
	}
	else
	{
		/* Normal YCbCr 601 conversion formula */
		y =  0.257 * r + 0.504 * g + 0.098 * b +  16.0;
		u = -0.148 * r - 0.291 * g + 0.493 * b + 128.0;
		v =  0.439 * r - 0.368 * g - 0.071 * b + 128.0;
	}

	colory[i] = (guint16)y * 0x0101;
	coloru[i] = (guint16)u * 0x0101;
	colorv[i] = (guint16)v * 0x0101;

#ifdef XV_INTERPOLATE
	/* To support interpolation, we precompute versions of this color
	 * mixed with every other color.  The u and v planes' values are
	 * weighted by their y values.
	 */
	iy = (guchar)colory[i];
	for (j = 0; j < 256; j++)
	{
		if (colory[i] == 0)
		{
			mixu[i][j] = mixu[j][i] = coloru[j];
			mixv[i][j] = mixv[j][i] = colorv[j];
		}
		else
		{
			jy = (guchar)colory[j];
			mixu[i][j] = mixu[j][i] = ((guchar)coloru[i] * iy + 
					(guchar)coloru[j] * jy) / (iy + jy);
			mixv[i][j] = mixv[j][i] = ((guchar)colorv[i] * iy + 
					(guchar)colorv[j] * jy) / (iy + jy);
		}
	}
#endif
}

#endif /* HAVE_XV */
