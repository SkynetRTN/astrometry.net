/*
   This file is part of the Astrometry.net suite.
   Copyright 2007 Keir Mierle and Dustin Lang.
   Copyright 2009 Dustin Lang.

   The Astrometry.net suite is free software; you can redistribute
   it and/or modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation, version 2.

   The Astrometry.net suite is distributed in the hope that it will be
   useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with the Astrometry.net suite ; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
*/
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/param.h>
#include <assert.h>
#include <stdarg.h>

#include <zlib.h>
#include <cairo.h>
#include <cairo-pdf.h>

#include "an-bool.h"
#include "tilerender.h"
#include "starutil.h"
#include "cairoutils.h"
#include "ioutils.h"
#include "fitsioutils.h"
#include "bl.h"
#include "log.h"
#include "errors.h"

#include "render_tycho.h"
#include "render_gridlines.h"
#include "render_usnob.h"
#include "render_rdls.h"
#include "render_boundary.h"
#include "render_constellation.h"
#include "render_messier.h"
#include "render_solid.h"
#include "render_images.h"
#include "render_cairo.h"
#include "render_skdt.h"
#include "render_quads.h"
#include "render_match.h"
#include "render_healpixes.h"

// Ugh, zlib before 1.2.0 didn't include compressBound()...
// And ZLIB_VERNUM wasn't defined until 1.2.0.2
#if !defined(ZLIB_VERNUM) || (ZLIB_VERNUM < 0x1200)
// This was copy-n-pasted directly from the zlib source code version 1.2.3:
/* compress.c -- compress a memory buffer
 * Copyright (C) 1995-2003 Jean-loup Gailly.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */
uLong compressBound (uLong sourceLen) {
  return sourceLen + (sourceLen >> 12) + (sourceLen >> 14) + 11;
}
#endif

/**
  This program gets called by "tile.php"/django in response to a
  client requesting a Google maps tile.  The coordinates of the tile
  are specified as a range of RA and DEC values.

  The RA coordinates are passed as    -x <lower-RA> -X <upper-RA>
  The DEC coordinates are             -y <lower-DEC> -Y <upper-DEC>
  The width and height in pixels are  -w <width> -h <height>
  */

static void print_help(char* prog) {
	printf("Usage: %s\n"
		   "\n"
		   "     These four flags are required: they specify the rectangle in RA,Dec space\n"
		   "     that this tile will cover.\n"
		   "  -x <lower-RA>\n"
		   "  -X <upper-RA>\n"
		   "  -y <lower-Dec>\n"
		   "  -Y <upper-Dec>\n"
		   "\n"
		   "     If you use this flag, the coordinates are instead interpreted as Mercator\n"
		   "     coordinates in the unit square.\n"
		   "  [-M]: in Mercator coords\n"
		   "\n"
		   "     These two flags are required: they specify the output image size.\n"
		   "  -w <image-width>\n"
		   "  -h <image-height>\n"
		   "\n"
		   "     Output options: the default is to write a PNG image to standard out.\n"
		   "  [-J]: write jpeg\n"
		   "  [-j]: write PDF\n"
		   "  [-R]: write a raw floating-point image\n"
		   "\n"
		   "\n"
		   "     Argument files: in order to pass a large number of filenames or other arguments,\n"
		   "     you can pass a file containing:\n"
		   "          <keyword> <arguments>\n"
		   "          <keyword> <arguments>\n"
		   "          ...\n"
		   "  [-A <argument-file>]\n"
		   "\n"
		   "\n"
		   "     Layers: the output image is rendered one layer at a time, in the specified order.\n"
		   "             Each rendering layer takes its own arguments, listed below.\n"
		   "  -l <layer-name> [-l <layer-name> ...]\n"
		   "\n"
		   "  -l solid   -- Renders a solid, opaque, black background.\n"
		   "                By default the background is black but transparent.\n"
		   "\n"
		   "  -l tycho   -- Renders Tycho-2 stars.\n"
		   "    -T <tycho-mkdt-file>: path to the Tycho-2 Mercator-kdtree data file.\n"
		   "                          This file can be created with \"make-merctree\".\n"
		   "    [-c]: apply Hogg's color-correction\n"
		   "    [-s]: apply arcsinh brightness mapping\n"
		   "    [-q]: apply sqrt brightness mapping\n"
		   "    [-e <scale>]: apply nonlinearities at the given scale\n"
		   "    [-g <gain>]: apply this gain factor to brightnesses (makes the stars brighter)\n"
		   "\n"
		   "  -l images  -- Renders images.\n"
		   "    [-n]: plot pixel density, not the pixels themselves.\n"
		   "    [-s, -e, -g]: as above.\n"
		   "                Reads from argument file (-A):\n"
		   "     wcsfn <wcs-file>\n"
		   "     jpegfn <jpeg-file>\n"
		   "\n"
		   "  -l quads   -- Renders Astrometry.net index qudas\n"
		   "                Reads from argument file (-A):\n"
		   "     index <index-filename>\n"
		   "\n"
		   "  -l cairo   -- Renders cairo commands.\n"
		   "                Reads from argument file (-A):\n"
		   "     cairo color <r> <g> <b> [<a>]\n"
		   "     cairo moveto <ra> <dec>\n"
		   "     cairo lineto <ra> <dec>\n"
		   "     cairo stroke\n"
		   "\n"
		   "  -l healpix -- Renders healpix boundaries.\n"
		   "     [-f <nside>]: default 1\n"
		   "\n"
		   "\n", prog);
}

const char* OPTIONS = "ab:c:de:f:g:h:i:jk:l:npqr:svw:x:y:zA:B:C:D:F:I:JK:L:MN:PRS:T:V:W:X:Y:";

struct renderer {
	char* name;
	render_func_t imgrender;
	render_cairo_func_t cairorender;
	// don't change the order of these fields!
};
typedef struct renderer renderer_t;

/* All render layers must go in here */
static renderer_t renderers[] = {
	{ "tycho",     render_tycho,        NULL },
	{ "grid",      NULL,                render_gridlines },
	{ "healpix",   NULL,                render_healpixes },
	{ "usnob",     render_usnob,        NULL },
	{ "rdls",      NULL,                render_rdls },
	{ "constellation", render_constellation, NULL },
	{ "messier",   render_messier,      NULL },
	{ "clean",     render_usnob,        NULL },
	{ "dirty",     render_usnob,        NULL },
	{ "solid",     NULL,                render_solid },
	{ "images",    render_images,       NULL },
	{ "userimage", render_images,       NULL },
	{ "boundaries",NULL,                render_boundary },
	{ "userboundary", NULL,             render_boundary },
	{ "userdot",   NULL,                render_boundary },
	{ "cairo",     NULL,                render_cairo },
	{ "skdt",      NULL,                render_skdt },
	{ "quads",     NULL,                render_quads },
	{ "match",     NULL,                render_match },
};

static void default_rdls_args(render_args_t* args) {
	// Set the other RDLS-related args if they haven't been set already.
	if (sl_size(args->rdlscolors) < sl_size(args->rdlsfns))
		sl_append(args->rdlscolors, NULL);
	if (il_size(args->Nstars) < sl_size(args->rdlsfns))
		il_append(args->Nstars, 0);
	if (il_size(args->fieldnums) < sl_size(args->rdlsfns))
		il_append(args->fieldnums, 0);
}

void get_string_args_of_types(render_args_t* args, const char* prefixes[], int Nprefixes, sl* lst, sl* matched_prefixes) {
    int i, j;
    if (!args->arglist)
        return;
    for (i=0; i<sl_size(args->arglist); i++) {
        char* str = sl_get(args->arglist, i);
		for (j=0; j<Nprefixes; j++)
			if (starts_with(str, prefixes[j])) {
				sl_append(lst, str + strlen(prefixes[j]));
				if (matched_prefixes)
					sl_append(matched_prefixes, prefixes[j]);
			}
    }
}

void get_string_args_of_type(render_args_t* args, const char* prefix, sl* lst) {
    int i;
    int skip = strlen(prefix);
    if (!args->arglist)
        return;
    for (i=0; i<sl_size(args->arglist); i++) {
        char* str = sl_get(args->arglist, i);
        if (starts_with(str, prefix)) {
            sl_append(lst, str + skip);
        }
    }
}

int parse_rgba_arg(const char* argstr, double* rgba) {
	dl* dlst;
	dlst = dl_new(4);
	get_double_args(argstr, dlst);
	if (dl_size(dlst) != 4) {
		logmsg("Argument: \"%s\": expected 4 numbers, got %i.\n", argstr, dl_size(dlst));
		return -1;
	}
	dl_copy(dlst, 0, 4, rgba);
	dl_free(dlst);
	return 0;
}

int get_first_rgba_arg_of_type(render_args_t* args, const char* prefix, double* rgba) {
	const char* argstr;
	argstr = get_first_arg_of_type(args, prefix);
	if (!argstr)
		return -1;
	return parse_rgba_arg(argstr, rgba);
}

const char* get_first_arg_of_type(render_args_t* args, const char* prefix) {
    int i;
    if (!args->arglist)
        return NULL;
    for (i=0; i<sl_size(args->arglist); i++) {
        char* str = sl_get(args->arglist, i);
        if (starts_with(str, prefix))
			return str;
    }
	return NULL;
}

int get_int_arg(const char* arg, int def) {
	char* c = index(arg, ' ');
	char* endp;
	int val;
	if (!c) return def;
	val = strtol(c+1, &endp, 0);
	if (endp == c) return def;
	return val;
}

double get_first_double_arg_of_type(render_args_t* args, const char* prefix, double def) {
	const char* arg = get_first_arg_of_type(args, prefix);
	if (!arg)
		return def;
	return get_double_arg(arg, def);
}

double get_double_arg(const char* arg, double def) {
	char* c = index(arg, ' ');
	char* endp;
	double val;
	if (!c) return def;
	c++;
	val = strtod(c, &endp);
	if (endp == c) return def;
	return val;
}

void get_double_args(const char* arg, dl* lst) {
	char* c = index(arg, ' ');
	char* endp;
	double val;
	if (!c) return;
	while (c && *c) {
		c++;
		val = strtod(c, &endp);
		if (endp == c) break;
		dl_append(lst, val);
		c = endp;
	}
}

void get_double_args_of_type(render_args_t* args, const char* prefix, dl* lst) {
    int i;
    int skip = strlen(prefix);
    if (!args->arglist)
        return;
    for (i=0; i<sl_size(args->arglist); i++) {
        double d;
        char* str = sl_get(args->arglist, i);
        if (!starts_with(str, prefix))
            continue;
        d = atof(str + skip);
        dl_append(lst, d);
    }
}

double get_double_arg_of_type(render_args_t* args, const char* name, double def) {
	double rtn;
	dl* lst = dl_new(4);
	get_double_args_of_type(args, name, lst);
	if (dl_size(lst) == 0)
		rtn = def;
	else
		rtn = dl_get(lst, 0);
	dl_free(lst);
	return rtn;
}

static cairo_status_t write_func_for_cairo(void *closure,
										   const unsigned char *data,
										   unsigned int length) {
	FILE* fid = closure;
	if (fwrite(data, 1, length, fid) != length) {
		SYSERROR("Failed to write cairo data");
		return CAIRO_STATUS_WRITE_ERROR;
	}
	return CAIRO_STATUS_SUCCESS;
}

extern char *optarg;
extern int optind, opterr, optopt;

int main(int argc, char *argv[]) {
	int argchar;
	int gotx, goty, gotX, gotY, gotw, goth;
	double xzoom;
	unsigned char* img = NULL;
	render_args_t args;
	sl* layers;
	int i;
	bool inmerc = 0;
    bool writejpeg = FALSE;
	int loglvl = LOG_MSG;
	bool writepdf = FALSE;
	cairo_t* cairo;
	cairo_surface_t* target;

	if (argc == 1) {
		print_help(argv[0]);
		exit(0);
	}

	memset(&args, 0, sizeof(render_args_t));

	// default args:
	args.colorcor = 1.44;
	args.linewidth = 2.0;

	args.rdlsfns = sl_new(4);
	args.rdlscolors = sl_new(4);
	args.Nstars = il_new(4);
	args.fieldnums = il_new(4);
	args.imagefns = sl_new(4);
	args.imwcsfns = sl_new(4);
	args.argfilenames = sl_new(4);

	layers = sl_new(16);
	gotx = goty = gotX = gotY = gotw = goth = FALSE;

	while ((argchar = getopt (argc, argv, OPTIONS)) != -1)
		switch (argchar) {
		case 'f':
			args.nside = atoi(optarg);
			break;
		case 'T':
			args.tycho_mkdt = optarg;
			break;
		case 'v':
			loglvl++;
			break;
        case 'A':
            sl_append(args.argfilenames, optarg);
            break;
        case 'K':
            args.colorlist = optarg;
            break;
		case 'b':
			args.ubstyle = optarg;
			break;
        case 'J':
            writejpeg = TRUE;
            break;
		case 'j':
			writepdf = TRUE;
			break;
        case 'S':
				args.filelist = strdup(optarg);
				break;
			case 'n':
				args.density = TRUE;
				break;
			case 'D':
				args.cachedir = strdup(optarg);
				break;
			case 'V':
				args.version = optarg;
				break;
			case 'z':
				args.zoomright = TRUE;
				break;
			case 'd':
				args.zoomdown = TRUE;
				break;
			case 'p':
				args.nopre = TRUE;
				break;
			case 'C':
				args.cmap = strdup(optarg);
				break;
			case 'M':
				inmerc = TRUE;
				break;
			case 'R':
				args.makerawfloatimg = 1;
				break;
			case 'B':
				args.dashbox = atof(optarg);
				break;
			case 'F':
				il_append(args.fieldnums, atoi(optarg));
				break;
			case 'N':
				il_append(args.Nstars, atoi(optarg));
				break;
			case 'r':
				default_rdls_args(&args);
				sl_append(args.rdlsfns, optarg);
				break;
			case 'k':
				sl_append(args.rdlscolors, optarg);
				break;
			case 'i':
				sl_append(args.imagefns, optarg);
				break;
			case 'W':
				args.wcsfn = strdup(optarg);
				break;
			case 'I':
				sl_append(args.imwcsfns, optarg);
				break;
			case 'c':
				args.colorcor = atof(optarg);
				break;
			case 's':
				args.arc = TRUE;
				break;
		case 'q':
			args.sqrt = TRUE;
			break;
		case 'e':
			args.nlscale = atof(optarg);
			break;
			case 'a':
				args.arith = TRUE;
				break;
			case 'g':
				args.gain = atof(optarg);
				break;
			case 'l':
				sl_append(layers, optarg);
				break;
			case 'L':
				args.linewidth = atof(optarg);
				break;
			case 'x':
				args.ramin = atof(optarg);
				gotx = TRUE;
				break;
			case 'X':
				args.ramax = atof(optarg);
				goty = TRUE;
				break;
			case 'y':
				args.decmin = atof(optarg);
				gotX = TRUE;
				break;
			case 'Y':
				args.decmax = atof(optarg);
				gotY = TRUE;
				break;
			case 'w':
				args.W = atoi(optarg);
				gotw = TRUE;
				break;
			case 'h':
				args.H = atoi(optarg);
				goth = TRUE;
				break;
		}
	log_init(loglvl);
	log_to(stderr);

	if (!(gotx && goty && gotX && gotY && gotw && goth)) {
		logmsg("tilecache: Invalid inputs: need ");
		if (!gotx) logmsg("-x ");
		if (!gotX) logmsg("-X ");
		if (!goty) logmsg("-y ");
		if (!gotY) logmsg("-Y ");
		if (!gotw) logmsg("-w ");
		if (!goth) logmsg("-h ");
		logmsg("\n");
		exit(-1);
	}

	default_rdls_args(&args);

    if (args.W > 4096 || args.H > 4096) {
        logmsg("tilecache: Width or height too large (limit 4096)\n");
        exit(-1);
    }

	if (writejpeg && writepdf) {
		logmsg("Can't write both JPEG and PDF.\n");
		exit(-1);
	}

	logmsg("tilecache: BEGIN TILECACHE\n");

    fits_use_error_system();

	if (inmerc) {
		// -x -X -y -Y were given in Mercator coordinates - convert to deg.
		// this is for cases where it's more convenient to specify the coords
		// in Merc coords (eg prerendering)
		args.ramin  = merc2radeg(args.ramin);
		args.ramax  = merc2radeg(args.ramax);
		args.decmin = merc2decdeg(args.decmin);
		args.decmax = merc2decdeg(args.decmax);
	}

	// min ra -> max merc.
	args.xmercmax =  radeg2merc(args.ramin);
	args.xmercmin =  radeg2merc(args.ramax);
	args.ymercmin = decdeg2merc(args.decmin);
	args.ymercmax = decdeg2merc(args.decmax);

	logverb("RA range: [%g, %g] deg\n", args.ramin, args.ramax);
	logverb("Dec range: [%g, %g] deg\n", args.decmin, args.decmax);
	logverb("RA merc: [%g, %g]\n", args.xmercmin, args.xmercmax);
	logverb("Dec merc: [%g, %g]\n", args.ymercmin, args.ymercmax);

	// The y mercator position can end up *near* but not exactly
	// equal to the boundary conditions... clamp.
	args.ymercmin = MAX(0.0, args.ymercmin);
	args.ymercmax = MIN(1.0, args.ymercmax);
	logverb("After clamping: Dec merc: [%g, %g]\n", args.ymercmin, args.ymercmax);

	args.xpixelpermerc = (double)args.W / (args.xmercmax - args.xmercmin);
	args.ypixelpermerc = (double)args.H / (args.ymercmax - args.ymercmin);
	args.xmercperpixel = 1.0 / args.xpixelpermerc;
	args.ymercperpixel = 1.0 / args.ypixelpermerc;

	xzoom = args.xpixelpermerc / 256.0;
	args.zoomlevel = (int)rint(log(fabs(xzoom)) / log(2.0));
	logmsg("tilecache: zoomlevel: %d\n", args.zoomlevel);

	// Rescue boneheads.
	if (!sl_size(layers)) {
		logmsg("tilecache: Do you maybe want to try rendering some layers?\n");
	}

	for (i=0; i<sl_size(args.argfilenames); i++) {
		sl* lines;
		char* fn = sl_get(args.argfilenames, i);
        lines = file_get_lines(fn, FALSE);
		if (!lines) {
			ERROR("Failed to read args file: \"%s\"", fn);
			return -1;
		}
		if (!args.arglist)
			args.arglist = lines;
		else {
			sl_merge_lists(args.arglist, lines);
			sl_free2(lines);
		}
	}

	if (writepdf) {
		cairo_write_func_t wfunc = write_func_for_cairo;
		target = cairo_pdf_surface_create_for_stream(wfunc, stdout, args.W, args.H);
		if (!target) {
			ERROR("Failed to create cairo surface for PDF");
			exit(-1);
		}
		logmsg("Image size: %ix%i pixels\n",
			   cairo_image_surface_get_width(target),
			   cairo_image_surface_get_height(target));
	} else {
		// Allocate a black image.
		img = calloc(4 * args.W * args.H, 1);
		target = cairo_image_surface_create_for_data(img, CAIRO_FORMAT_ARGB32, args.W, args.H, args.W*4);
	}
	cairo = cairo_create(target);

	cairoutils_surface_status_errors(target);
	cairoutils_cairo_status_errors(cairo);


	for (i=0; i<sl_size(layers); i++) {
		int j;
		int NR = sizeof(renderers) / sizeof(renderer_t);
		char* layer = sl_get(layers, i);
		bool gotit = FALSE;

		for (j=0; j<NR; j++) {
			renderer_t* r = renderers + j;
			int res = -1;
			if (!streq(layer, r->name))
				continue;
			args.currentlayer = r->name;
			if (r->cairorender) {
				res = r->cairorender(cairo, &args);
			} else if (r->imgrender) {
				cairo_surface_t* thissurf;
				cairo_pattern_t* pat;
				uchar* thisimg = calloc(4 * args.W * args.H, 1);
				res = r->imgrender(thisimg, &args);
				thissurf = cairo_image_surface_create_for_data(thisimg, CAIRO_FORMAT_ARGB32, args.W, args.H, args.W*4);
				pat = cairo_pattern_create_for_surface(thissurf);
				cairo_set_source(cairo, pat);
				cairo_paint(cairo);
				cairo_pattern_destroy(pat);
				cairo_surface_destroy(thissurf);
				free(thisimg);

				/*
				 Sweet, you can control how images are resized with:

				 void                cairo_pattern_set_filter            (cairo_pattern_t *pattern,
				 cairo_filter_t filter);

				 Check out also:

				 void                cairo_pattern_set_matrix            (cairo_pattern_t *pattern,
                                                         const cairo_matrix_t *matrix);
				 */
			} else {
				logmsg("tilecache: neither 'imgrender' nor 'cairorender' is defined for renderer \"%s\"\n", r->name);
				continue;
			}
			if (res) {
				logmsg("tilecache: Renderer \"%s\" failed.\n", r->name);
			} else {
				logmsg("tilecache: Renderer \"%s\" succeeded.\n", r->name);
			}
			gotit = TRUE;
			break;
		}
		// Save a different kind of bonehead.
		if (!gotit) {
			logmsg("tilecache: No renderer found for layer \"%s\".\n", layer);
		}

		cairoutils_surface_status_errors(target);
		cairoutils_cairo_status_errors(cairo);
	}

	if (writepdf) {
		cairo_surface_flush(target);
		cairo_surface_finish(target);
	} else {
		if (args.makerawfloatimg) {
			fwrite(args.rawfloatimg, sizeof(float), args.W * args.H * 3, stdout);
			free(args.rawfloatimg);
		} else {
			cairoutils_argb32_to_rgba(img, args.W, args.H);
			if (writejpeg)
				cairoutils_stream_jpeg(stdout, img, args.W, args.H);
			else
				cairoutils_stream_png(stdout, img, args.W, args.H);
		}
	}

	free(img);

	sl_free2(args.arglist);
	sl_free2(args.rdlsfns);
	sl_free2(args.rdlscolors);
	sl_free2(args.imagefns);
	sl_free2(args.imwcsfns);
	sl_free2(layers);

	il_free(args.Nstars);
	il_free(args.fieldnums);

	free(args.wcsfn);
	free(args.cmap);

	logmsg("tilecache: END TILECACHE\n");

	cairoutils_surface_status_errors(target);
	cairoutils_cairo_status_errors(cairo);

	cairo_surface_destroy(target);
	cairo_destroy(cairo);

	return 0;
}

int parse_color(char c, double* p_r, double* p_g, double* p_b) {
	double r, g, b;
	switch (c) {
	case 'r': // red
		r = 1.0;
		g = b = 0.0;
		break;
	case 'b': // blue
		r = g = 0.0;
		b = 1.0;
		break;
	case 'm': // magenta
		r = b = 1.0;
		g = 0.0;
		break;
	case 'y': // yellow
		r = g = 1.0;
		b = 0.0;
		break;
	case 'g': // green
		r = b = 0.0;
		g = 1.0;
		break;
	case 'c': // cyan
		r = 0.0;
		g = b = 1.0;
		break;
	case 'w': // white
		r = g = b = 1.0;
		break;
	case 'k': // black
		r = g = b = 0.0;
		break;
	default:
		return -1;
	}
	if (p_r) *p_r = r;
	if (p_g) *p_g = g;
	if (p_b) *p_b = b;
	return 0;
}

/*
 We need to flip RA somewhere...

 We convert Longitude to RA to Mercator to Pixels.

 We choose to insert the flip in the conversion from RA to Mercator.
*/

double xpixel2mercf(double pix, render_args_t* args) {
	return args->xmercmin + pix * args->xmercperpixel;
}

double ypixel2mercf(double pix, render_args_t* args) {
	return args->ymercmax - pix * args->ymercperpixel;
}

double xmerc2pixelf(double x, render_args_t* args) {
	return (x - args->xmercmin) * args->xpixelpermerc;
}

double ymerc2pixelf(double y, render_args_t* args) {
	return (args->ymercmax - y) * args->ypixelpermerc;
}

////// The following are just composed of simpler conversions.

// RA in degrees
int ra2pixel(double ra, render_args_t* args) {
	return xmerc2pixel(radeg2merc(ra), args);
}

// DEC in degrees
int dec2pixel(double dec, render_args_t* args) {
	return ymerc2pixel(decdeg2merc(dec), args);
}

// RA in degrees
double ra2pixelf(double ra, render_args_t* args) {
	return xmerc2pixelf(radeg2merc(ra), args);
}

// DEC in degrees
double dec2pixelf(double dec, render_args_t* args) {
	return ymerc2pixelf(decdeg2merc(dec), args);
}

// to RA in degrees
double pixel2ra(double pix, render_args_t* args) {
	return merc2radeg(xpixel2mercf(pix, args));
}

// to DEC in degrees
double pixel2dec(double pix, render_args_t* args) {
	return merc2decdeg(ypixel2mercf(pix, args));
}

int xmerc2pixel(double x, render_args_t* args) {
	return (int)floor(xmerc2pixelf(x, args));
}

int ymerc2pixel(double y, render_args_t* args) {
	return (int)floor(ymerc2pixelf(y, args));
}

int in_image(int x, int y, render_args_t* args) {
	return (x >= 0 && x < args->W && y >=0 && y < args->H);
}

int in_image_margin(int x, int y, int margin, render_args_t* args) {
	return (x >= -margin && x < (args->W + margin) && y >= -margin && y < (args->H + margin));
}

uchar* pixel(int x, int y, uchar* img, render_args_t* args) {
	return img + 4*(y*args->W + x);
}

// draw a line in Mercator space, handling wrap-around if necessary.
void draw_line_merc(double mx1, double my1, double mx2, double my2,
		cairo_t* cairo, render_args_t* args) {
	cairo_move_to(cairo, xmerc2pixel(mx1, args), ymerc2pixel(my1, args));
	cairo_line_to(cairo, xmerc2pixel(mx2, args), ymerc2pixel(my2, args));
	if (MIN(mx1,mx2) < 0) {
		cairo_move_to(cairo, xmerc2pixel(mx1+1, args), ymerc2pixel(my1, args));
		cairo_line_to(cairo, xmerc2pixel(mx2+1, args), ymerc2pixel(my2, args));
	}
	if (MAX(mx1,mx2) > 1) {
		cairo_move_to(cairo, xmerc2pixel(mx1-1, args), ymerc2pixel(my1, args));
		cairo_line_to(cairo, xmerc2pixel(mx2-1, args), ymerc2pixel(my2, args));
	}
}

// ra,dec in degrees.
void draw_segmented_line(double ra1, double dec1,
		double ra2, double dec2,
		int SEGS,
		cairo_t* cairo, render_args_t* args) {
	int i, s, k;
	double xyz1[3], xyz2[3];
	bool wrap;

	radecdeg2xyzarr(ra1, dec1, xyz1);
	radecdeg2xyzarr(ra2, dec2, xyz2);

	wrap = (fabs(ra1 - ra2) >= 180.0) ||
		(args->ramin < 0) || (args->ramax > 360.0);

	// Draw segmented line.
	for (i=0; i<(1 + (wrap?1:0)); i++) {
		for (s=0; s<SEGS; s++) {
			double xyz[3], frac;
			double ra, dec;
			double mx;
			double px, py;
			frac = (double)s / (double)(SEGS-1);
			for (k=0; k<3; k++)
				xyz[k] = xyz1[k]*(1.0-frac) + xyz2[k]*frac;
			normalize_3(xyz);
			xyzarr2radecdeg(xyz, &ra, &dec);
			mx = radeg2merc(ra);

			if (wrap) {
				// in the first pass we draw the left side (mx>0.5)
				if ((i==0) && (mx < 0.5)) mx += 1.0;
				// in the second pass we draw the right side (wx<0.5)
				if ((i==1) && (mx > 0.5)) mx -= 1.0;
			}
			px = xmerc2pixelf(mx, args);
			py = dec2pixelf(dec, args);

			if (s==0)
				cairo_move_to(cairo, px, py);
			else
				cairo_line_to(cairo, px, py);
		}
	}
}

static int cache_get_filename(render_args_t* args,
		const char* cachedomain, const char* key,
		char* fn, int fnlen) {
	if (snprintf(fn, fnlen, "%s/%s/%s", args->cachedir, cachedomain, key) > fnlen) {
		logmsg("Filename truncated in cache_load/cache_save.\n");
		return -1;
	}
	return 0;
}

void* cache_load(render_args_t* args,
		const char* cachedomain, const char* key, int* length) {
	char fn[1024];
	unsigned char* buf;
	size_t len;
	uint32_t typeid;
	unsigned char* orig;
	int rtn;
	uLong origlen;
	uint32_t* ubuf;

	if (!args->cachedir)
		return NULL;
	if (cache_get_filename(args, cachedomain, key, fn, sizeof(fn))) {
		return NULL;
	}
	if (!file_exists(fn))
		return NULL;
	buf = file_get_contents(fn, &len, FALSE);
	if (!buf) {
		logmsg("Failed to read file contents in cache_load.\n");
		return NULL;
	}
	if (len < 2*sizeof(uint32_t)) {
		logmsg("Cache file too small: \"%s\n", fn);
		free(buf);
		return NULL;
	}

    // Pull the two header values off the front...
    ubuf = (uint32_t*)buf;
	// Grab typeid.
	typeid = ubuf[0];
	// Grab original (uncompressed) length.
	origlen = ubuf[1];

	if (typeid != 1) {
		logmsg("File \"%s\" does not have typeid 1.\n", fn);
		free(buf);
		return NULL;
	}
	orig = malloc(origlen);
	if (!orig) {
		logmsg("Failed to allocate %i bytes for uncompressed cache file \"%s\".\n", (int)origlen, fn);
		free(buf);
		return NULL;
	}
	if (length)
		*length = origlen;
	//logmsg("Origlen as described by the cache file: %d\n", ulen);
	//logmsg("File size as determined by file_get_contents() = %d\n", len);
    rtn = uncompress(orig, &origlen, buf + 2*sizeof(uint32_t), len - 2*sizeof(uint32_t));
	free(buf);
	if (rtn != Z_OK) {
		logmsg("Failed to uncompress() file \"%s\": %s\n", fn, zError(rtn));
		free(orig);
		return NULL;
	}
	return orig;
}

int cache_save(render_args_t* args,
		const char* cachedomain, const char* key,
		const void* data, int length) {
	char fn[1024];
	FILE* fid;
	uint32_t typeid;
	unsigned char* compressed = NULL;
	uLong complen;
	uint32_t ulen;
	int rtn;

	if (!args->cachedir)
		return -1;
	if (cache_get_filename(args, cachedomain, key, fn, sizeof(fn))) {
		return -1;
	}
	fid = fopen(fn, "wb");
	if (!fid) {
		logmsg("Failed to open cache file \"%s\": %s\n", fn, strerror(errno));
		goto cleanup;
	}

	complen = compressBound(length);
	compressed = malloc(complen + 2*sizeof(uint32_t));
	if (!compressed) {
		logmsg("Failed to allocate compressed cache buffer\n");
		goto cleanup;
	}

	// first four bytes: type id
	typeid = 1;
	if (fwrite(&typeid, sizeof(uint32_t), 1, fid) != 1) {
		logmsg("Failed to write cache file \"%s\": %s\n", fn, strerror(errno));
		goto cleanup;
	}
	ulen = length;
	if (fwrite(&ulen, sizeof(uint32_t), 1, fid) != 1) {
		logmsg("Failed to write cache file \"%s\": %s\n", fn, strerror(errno));
		goto cleanup;
	}
	rtn = compress(compressed, &complen, data, length);
	if (rtn != Z_OK) {
		logmsg("compress() error: %s\n", zError(rtn));
		goto cleanup;
	}
	if (fwrite(compressed, 1, complen, fid) != complen) {
		logmsg("Failed to write cache file \"%s\": %s\n", fn, strerror(errno));
		goto cleanup;
	}
	if (fclose(fid)) {
		logmsg("Failed to close cache file \"%s\": %s\n", fn, strerror(errno));
		goto cleanup;
	}

	free(compressed);
	return 0;

cleanup:
	free(compressed);
	if (fid)
		fclose(fid);
	unlink(fn);
	return -1;
}


