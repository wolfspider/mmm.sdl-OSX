/*
 * 2014 (c) Øyvind Kolås
Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "mmm.c"
#include <mmm.h>
#include <mmm-pset.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <SDL/SDL.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include "host.h"
#include <pthread.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

#define WIDTH              512
#define HEIGHT             384
#define BPP                4

#define UFB_PID             0x18
#define UFB_TITLE           0xb0
#define UFB_WIDTH           0x3b0
#define UFB_HEIGHT          0x3b4
#define UFB_STRIDE          0x3b8
#define UFB_FB_OFFSET       0x3bc
#define UFB_FLIP_STATE      0x3c0
//#define UFB_SIZE          131124  // too low and it will conflict with pcm-data

#define UFB_SIZE 0x4ffff
#define UFB_FLIP_INIT       0
#define UFB_FLIP_NEUTRAL    1
#define UFB_FLIP_DRAWING    2
#define UFB_FLIP_WAIT_FLIP  3
#define UFB_FLIP_FLIPPING   4

#define PEEK(addr)     (*(int32_t*)(&ram_base[addr]))
#define POKE(addr,val) do{(*(int32_t*)(&ram_base[addr]))=(val); }while(0)

static int W = 400;
static int H = 300;

int tx = 0;
int ty = 0;

static inline int fastrand () {
	static int g_seed = 23;
	g_seed = (214013*g_seed+2531011);
	return (g_seed>>16)&0x7FFF;
}


typedef int (*Fragment)(int x, int y, int foo);

static inline int frag_constant (int x, int y, int foo) { return foo; }

static inline int frag_ripple (int x, int y, int foo)
{
	int u, v;
	u = x - W/2;
	v = y - H/2;
	return (((u * u + v * v)+foo) % (40000) < 20000) * 255;
}

static inline int frag_mandel (int hx, int hy, int foo)
{
	float cx,cy,xsq,ysq;
	unsigned int iter;
	int iterations = 64;
	float magnify = 1.4;
	float initx = (((float)hx)/((float)W)-0.5)/magnify*4.0-0.7;
	float inity = (((float)hy)/((float)H)-0.5)/magnify*3.0;
	
	cx=initx+initx*initx - inity*inity;
	cy=inity+initx*inity+initx*inity;
	
	for (iter=0;iter<iterations && (ysq=cy*cy)+(xsq=cx*cx)<4;iter++,cy=inity+cx*cy+cx*cy,cx=initx-ysq+xsq) ;
	return (iter&1) * 255;
}

static inline int frag_random (int x, int y, int foo)
{
	return fastrand () % 255;
}

static inline int frag_ripple_interference (int x, int y,int foo)
{
	return frag_ripple(x-(foo-512),y,0)^ frag_ripple(x+(foo-512),y,0);
}

static inline int frag_ripple_interference2 (int x, int y,int foo)
{
	return frag_ripple(x-(foo-512),y,0) + frag_ripple(x+(foo-512),y,0);
}

static inline int frag_hack2 (int x, int y,int foo)
{
	x+=foo * 5;
	x/=4;
	y/=4;
	return (x ^ y);
}

static inline int frag_hack (int x, int y,int foo)
{
	x/=4;
	y/=4;
	return (x ^ y) + foo;
}

void fill_random (Mmm *fb, int n)
{
	int x, y;
	unsigned char *buffer;
	int width, height, stride;
	int bpp;
	
	buffer = mmm_get_buffer_write (fb, &width, &height, &stride, NULL);
	
	bpp = mmm_get_bytes_per_pixel (fb);
	
	for (y = 0; y < height; y++)
  {
  unsigned char *pix = mmm_get_pix (fb, buffer, 0, y);
  for (x = 0; x < width; x++)
	  {
	  int val = fastrand()&0xff;
	  pix = mmm_pix_pset (fb, pix, bpp, x, y, val, val, val, 255);
	  }
  }
	
	mmm_write_done (fb, 0, 0, -1, -1);
}

void blank_it (Mmm *fb, int n)
{
	int x, y;
	unsigned char *buffer;
	int width, height, stride;
	int bpp;
	
	buffer = mmm_get_buffer_write (fb, &width, &height, &stride, NULL);
	
	bpp = mmm_get_bytes_per_pixel (fb);
	
	for (y = 0; y < height; y++)
  {
  unsigned char *pix = mmm_get_pix (fb, buffer, 0, y);
  for (x = 0; x < width; x++)
	  {
	  int val = 255;
	  pix = mmm_pix_pset (fb, pix, bpp, x, y, val, val, val, 255);
	  }
  }
	
	mmm_write_done (fb, 0, 0, -1, -1);
}


void fill_render (Mmm *fb, Fragment fragment, int foo)
{
	int x, y;
	unsigned char *buffer;
	int width, height, stride;
	int bpp;
	mmm_client_check_size (fb, &width, &height);
	
	buffer = mmm_get_buffer_write (fb, &width, &height, &stride, NULL);
	
	bpp = mmm_get_bytes_per_pixel (fb);
	
	for (y = 0; y < height; y++)
  {
  unsigned char *pix = mmm_get_pix (fb, buffer, 0, y);
  int u, v;
  u = 0 - (tx-width/2); v = y - (ty-height/2);
  for (x = 0; x < width; x++, u++)
	  {
	  int val = fragment (u, v, foo);
	  pix = mmm_pix_pset (fb, pix, bpp, x, y, val, val, val, 255);
	  }
  }
	
	mmm_write_done (fb, 0, 0, -1, -1);
}

void event_handling (Mmm *fb)
{
	while (mmm_has_event (fb))
		{
		const char *event = mmm_get_event (fb);
		float x = 0, y = 0;
		const char *p;
		p = strchr (event, ' ');
		if (p)
			{
			x = atof (p+1);
			p = strchr (p+1, ' ');
			if (p)
				y = atof (p+1);
			}
		tx = x;
		ty = y;
		}
}

void ghostbuster (Mmm *fb)
{
	int i;
	for (i = 0; i < 3; i++)
		{
		fill_render (fb, frag_random, i);
		usleep (140000);
		}
}

void* fragment()
{
	Mmm *fb = mmm_new (W, H, 0, NULL);
	int j;
	if (!fb)
		{
		fprintf (stderr, "failed to open buffer\n");
		return 0;
		}
	
	W = mmm_get_width (fb);
	H = mmm_get_height (fb);
	
	tx = W/2;
	ty = H/2;
	
	for (j = 0; j < 2; j ++)
  {
  int i;
  
  event_handling (fb);
  for (i = 0; i < 64; i+=2)
	  {
	  fill_render (fb, frag_ripple_interference, i);
	  event_handling (fb);
	  }
  for (i = 64; i < 96; i+=2)
	  {
	  fill_render (fb, frag_ripple_interference2, i);
	  event_handling (fb);
	  }
	  {
	  
	  //mmm_set_size (fb, 64, 64);
	  W = mmm_get_width (fb);
	  H = mmm_get_height (fb);
	  
	  tx = W/2;
	  ty = H/2;
	  }
  
  for (i = 96; i < 1280; i+=2)
	  {
	  fill_render (fb, frag_ripple_interference, i);
	  event_handling (fb);
	  }
  
  ghostbuster (fb);
  fill_render (fb, frag_mandel, i);
  sleep (1);
  ghostbuster (fb);
  
  mmm_add_message (fb, "foo");
  
  for (i = 0; i < 32; i+=1)
	  {
	  fill_render (fb, frag_hack2, 64-i);
	  event_handling (fb);
	  }
  mmm_add_message (fb, "bar");
  
  
  
  for (i = 0; i < 32; i+=1)
	  {
	  fill_render (fb, frag_hack, i);
	  event_handling (fb);
	  }
  
  ghostbuster (fb);
  
  for (i = 0; i < 74; i+=1)
	  {
	  fill_render (fb, frag_ripple, i * 1000);
	  event_handling (fb);
	  }
  for (i = 74; i >0; i-=2)
	  {
	  fill_render (fb, frag_ripple, i * 1000);
	  event_handling (fb);
	  }
  ghostbuster (fb);
  
  event_handling (fb);
  }
	
	mmm_destroy (fb);
	fprintf (stderr, "nano-test done!\n");
	return 0;
}

static uint8_t *ram_base  = NULL;

static uint8_t *pico_fb (int width, int height)
{
	char path[512];
	int fd;
	int size = UFB_SIZE + width * height * BPP;
	const char *ufb_path = getenv ("MMM_PATH");
	if (!ufb_path)
		return NULL;
	sprintf (path, "%s/fb.XXXXXX", ufb_path);
	fd = mkstemp (path);
	pwrite (fd, "", 1, size);
	fsync (fd);
	chmod (path, 511);
	ram_base = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	memset (ram_base, 0, size);
	strcpy ((void*)ram_base + UFB_TITLE, "foo");
	
	POKE (UFB_FLIP_STATE,  UFB_FLIP_INIT);
	POKE (UFB_PID,         (int)getpid());
	POKE (UFB_WIDTH,       width);
	POKE (UFB_HEIGHT,      height);
	POKE (UFB_STRIDE,      width * BPP);
	/* format? */
	POKE (UFB_FB_OFFSET,   UFB_SIZE);
	POKE (UFB_FLIP_STATE,  UFB_FLIP_NEUTRAL);
	
#if 0
	poke to start pcm
	
	poke to start events
#endif
	
	
	
	return (uint8_t*) & PEEK(UFB_SIZE);
}

static void    pico_exit (void)
{
	if (ram_base)
		munmap (ram_base, UFB_SIZE * PEEK (UFB_WIDTH) * PEEK (UFB_HEIGHT) * BPP);
	ram_base = NULL;
}

static void wait_sync (void)
{
	/* this client will block if there is no compositor */
	while (PEEK(UFB_FLIP_STATE) != UFB_FLIP_NEUTRAL)
		usleep (5000);
	POKE (UFB_FLIP_STATE, UFB_FLIP_DRAWING);
}

static void flip_buffer (void)
{
	POKE (UFB_FLIP_STATE, UFB_FLIP_WAIT_FLIP);
}

void* picoFB ()
{
	uint8_t *pixels = pico_fb (512, 384);
	
	if (!pixels)
		return 0;
	
	int frame;
	int val = 0;
	int dir = 1;
	
	for (frame = 0; frame < 100000; frame ++)
  {
  int x, y;
  int i;
  
  wait_sync ();
  
  i = 0;
  for (y = 0 ; y < HEIGHT; y ++)
	  for (x = 0; x < WIDTH; x ++)
		  {
		  float d = (x - WIDTH/2) * (x - WIDTH / 2) +
		  (y - HEIGHT/2) * (y - HEIGHT / 2);
		  pixels[i + 0] = (x & y) + frame;
		  pixels[i + 1] = val;
		  pixels[i + 2] = (x - WIDTH/2) * frame / ((y-HEIGHT/2)+0.5);
		  pixels[i + 3] = d * 800 / frame;
		  i += 4;
		  }
  
  flip_buffer ();
  
  val += dir;
  if (val >= 255 || val < 0)
	  {
	  dir *= -1;
	  val += dir;
	  }
  }
	
	pico_exit ();
	return 0;
}

typedef struct _HostSDL   HostSDL;

/////////////////////////////////////////////////////////////////////

struct _HostSDL
{
  Host         host;
  SDL_Surface *screen;
};

static int baseflags = SDL_SWSURFACE;

static void render_client (Host *host, Client *client, float ptr_x, float ptr_y)
{
  HostSDL *host_sdl = (void*)host;
  SDL_Surface *screen = host_sdl->screen;
#if MRG_CAIRO
  cairo_t *cr = mrg_cr (mrg);
  cairo_surface_t *surface;
#endif
  int width, height, rowstride;

  //if (client->pid == getpid ())
    //return;

  int cwidth, cheight;

  const unsigned char *pixels = mmm_get_buffer_read (client->mmm,
      &width, &height, &rowstride);
  int x, y;

  x = mmm_get_x (client->mmm);
  y = mmm_get_y (client->mmm);

  if (ptr_x >= x && ptr_x < x + width &&
      ptr_y >= y && ptr_y < y + height)
    {
      host->focused = client;
    }

  if (pixels && width && height)
  {
    int front_offset = y * host->stride + x * host->bpp;
    uint8_t *dst = screen->pixels + front_offset;
    const uint8_t *src = pixels;
    int copy_count;

    int start_scan;
    int end_scan;
    int scan;

    start_scan = y;
    end_scan = y + height;

    if (end_scan < host->dirty_ymin ||
        x + width < host->dirty_xmin)
    {
      mmm_read_done (client->mmm);
      return;
    }

    if (start_scan < host->dirty_ymin)
      start_scan = host->dirty_ymin;
    if (end_scan > host->dirty_ymax)
      end_scan = host->dirty_ymax;
    start_scan -= y;
    end_scan -= y;
    
    copy_count = host->width - x;
    if (copy_count > width)
    {
      copy_count = width;
    }

    if (host->dirty_xmin > x)
    {
      dst += 4 * (host->dirty_xmin - x);
      src += host->bpp * (host->dirty_xmin - x);
      copy_count -= (host->dirty_xmin - x);
    }

    if (host->dirty_xmin + copy_count > host->dirty_xmax)
    {
      copy_count = (host->dirty_xmax - host->dirty_xmin);
    }

    dst += (host->stride) * start_scan;
    src += (rowstride) * start_scan;

    for (scan = start_scan; scan < end_scan; scan ++)
    {
      if (dst >= (uint8_t*)screen->pixels &&
          dst < ((uint8_t*)screen->pixels) + (host->stride * host->height) - copy_count * 4)
        memcpy (dst, src, copy_count * 4);
      dst += host->stride;
      src += rowstride;
    }

    /* XXX: pass a copy to video-encoder thread, or directly encode */

    mmm_read_done (client->mmm);
  }

  mmm_host_get_size (client->mmm, &cwidth, &cheight);

  if (host->single_app)
  {
    static int old_baseflags = 0;

    if (mmm_get_value (client->mmm, "borderless"))
      baseflags |= SDL_NOFRAME;
    else
    {
      if (baseflags & SDL_NOFRAME)
        baseflags -= SDL_NOFRAME;
    }
    
    if ( (cwidth  && cwidth  != host->width) ||
         (cheight && cheight != host->height) ||
         (old_baseflags != baseflags))
    {
      host->width = cwidth;
      host->height = cheight;
      host->stride = host->width * host->bpp;
      host_sdl->screen = SDL_SetVideoMode (host->width, host->height, 32,
                                           baseflags | SDL_RESIZABLE);
      old_baseflags = baseflags;
    }
  }
}

void host_destroy (Host *host)
{
  free (host);
}

static void mmfb_sdl_fullscreen (Host *host, int fullscreen)
{
  HostSDL *host_sdl = (void*)host;
  SDL_Surface *screen = host_sdl->screen;
  int width = 1024, height = 768;

  if (fullscreen)
  {
    SDL_Rect **modes;
    modes = SDL_ListModes(NULL, SDL_HWSURFACE|SDL_FULLSCREEN);
    if (modes == (SDL_Rect**)0) {
        fprintf(stderr, "No modes available!\n");
        return;
    }

    width = modes[0]->w;
    height = modes[0]->h;

    screen = SDL_SetVideoMode(width, height, 32,
                              baseflags | SDL_FULLSCREEN );
    host_sdl->screen = screen;
  }
  else
  {
    screen = SDL_SetVideoMode(width, height, 32,
                              baseflags | SDL_RESIZABLE );
    host_sdl->screen = screen;
  }
  host->width = width;
  host->bpp = 4;
  host->stride = host->width * host->bpp;
  host->height = height;
  host->fullscreen = fullscreen;
}

Host *host_sdl_new (const char *path, int width, int height)
{
  Host *host = calloc (sizeof (HostSDL), 1);
  HostSDL *host_sdl = (void*)host;

  if (SDL_Init(SDL_INIT_VIDEO) < 0)
  {
    fprintf (stderr, "Unable to initialize SDL: %s\n", SDL_GetError());
    return NULL;
  }


  if (width < 0)
  {
    SDL_Rect **modes;
    modes = SDL_ListModes(NULL, SDL_HWSURFACE|SDL_FULLSCREEN);
    host->fullscreen = 1;
    if (modes == (SDL_Rect**)0) {
      width=host_width = 400;
      height=host_height = 300;
    }
    else
    {
      width=host_width = modes[0]->w - 128;
      height=host_height = modes[0]->h - 128;
    }

  }

  host->width = width;
  host->bpp = 4;
  host->stride = host->width * host->bpp;
  host->height = height;

  host_sdl->screen =  SDL_SetVideoMode (host->width, host->height, 32, baseflags | SDL_RESIZABLE);

  if (host->fullscreen)
    mmfb_sdl_fullscreen (host, 1);

  if (!host_sdl->screen)
  {
    fprintf (stderr, "Unable to create display surface: %s\n", SDL_GetError());
    return NULL;
  }

  SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
  SDL_EnableUNICODE(1);

  host_clear_dirt (host);
  host->fbdir = strdup (path);
  return host;
}

static int
mmfb_unichar_to_utf8 (unsigned int  ch,
                      unsigned char*dest)
{
/* http://www.cprogramming.com/tutorial/utf8.c  */
/*  Basic UTF-8 manipulation routines
  by Jeff Bezanson
  placed in the public domain Fall 2005 ... */
    if (ch < 0x80) {
        dest[0] = (char)ch;
        return 1;
    }
    if (ch < 0x800) {
        dest[0] = (ch>>6) | 0xC0;
        dest[1] = (ch & 0x3F) | 0x80;
        return 2;
    }
    if (ch < 0x10000) {
        dest[0] = (ch>>12) | 0xE0;
        dest[1] = ((ch>>6) & 0x3F) | 0x80;
        dest[2] = (ch & 0x3F) | 0x80;
        return 3;
    }
    if (ch < 0x110000) {
        dest[0] = (ch>>18) | 0xF0;
        dest[1] = ((ch>>12) & 0x3F) | 0x80;
        dest[2] = ((ch>>6) & 0x3F) | 0x80;
        dest[3] = (ch & 0x3F) | 0x80;
        return 4;
    }
    return 0;
}

static int sdl_check_events (Host *host)
{
  HostSDL *host_sdl = (void*)host;
  SDL_Event event;
  int got_event = 0;
  char buf[64];
  while (SDL_PollEvent (&event))
  {
    switch (event.type)
    {
      case SDL_MOUSEMOTION:
        {
          if (host->pointer_down[0])
            sprintf (buf, "mouse-drag %.0f %.0f",
                 (float)event.motion.x,
                 (float)event.motion.y);
          else
            sprintf (buf, "mouse-motion %.0f %.0f",
                 (float)event.motion.x,
                 (float)event.motion.y);

          if (host->focused)
            mmm_add_event (host->focused->mmm, buf);
        }
        break;
      case SDL_MOUSEBUTTONDOWN:
        {
          sprintf (buf, "mouse-press %.0f %.0f",
               (float)event.button.x,
               (float)event.button.y);
          if (host->focused)
            mmm_add_event (host->focused->mmm, buf);
          host->pointer_down[0] = 1;
        }
        break;
      case SDL_MOUSEBUTTONUP:
        {
          sprintf (buf, "mouse-release %.0f %.0f",
               (float)event.button.x,
               (float)event.button.y);

          if (host->focused)
            mmm_add_event (host->focused->mmm, buf);
          host->pointer_down[0] = 0;
        }
        break;
      case SDL_KEYDOWN:
        {
          char buf[64] = "";
          char *name = buf;

          buf[mmfb_unichar_to_utf8 (event.key.keysym.unicode, (void*)buf)]=0;
          if (event.key.keysym.mod & (KMOD_CTRL))
            {
            buf[0] = event.key.keysym.unicode + 96;
            buf[1] = 0;
            }



          switch (event.key.keysym.sym)
          {
            case SDLK_F1:        name = "F1";       break;
            case SDLK_F2:        name = "F2";       break;
            case SDLK_F3:        name = "F3";       break;
            case SDLK_F4:        name = "F4";       break;
            case SDLK_F5:        name = "F5";       break;
            case SDLK_F6:        name = "F6";       break;
            case SDLK_F7:        name = "F7";       break;
            case SDLK_F8:        name = "F8";       break;
            case SDLK_F9:        name = "F9";       break;
            case SDLK_F10:       name = "F10";      break;
            case SDLK_F11:       name = "F11";      break;
            case SDLK_F12:       name = "F12";      break;
            case SDLK_ESCAPE:    name = "escape";   break;
            case SDLK_DOWN:      name = "down";     break;
            case SDLK_LEFT:      name = "left";     break;
            case SDLK_UP:        name = "up";       break;
            case SDLK_RIGHT:     name = "right";    break;
            case SDLK_BACKSPACE: name = "backspace";break;
            case SDLK_SPACE:     name = "space";    break;
            case SDLK_TAB:       name = "tab";      break;
            case SDLK_DELETE:    name = "delete";   break;
            case SDLK_INSERT:    name = "insert";   break;
            case SDLK_RETURN:    name = "return";   break;
            case SDLK_HOME:      name = "home";     break;
            case SDLK_END:       name = "end";      break;
            case SDLK_PAGEDOWN:  name = "page-down";break;
            case SDLK_PAGEUP:    name = "page-up";  break;

            default:;
          }

          if (event.key.keysym.mod & (KMOD_CTRL) ||
              event.key.keysym.mod & (KMOD_ALT) ||
              strlen (name) >= 3)
          {

          if (event.key.keysym.mod & (KMOD_CTRL))
          {
            static char buf[64] = "";
            sprintf (buf, "control-%s", name);
            name = buf;
          }
          if (event.key.keysym.mod & (KMOD_ALT))
          {
            static char buf[64] = "";
            sprintf (buf, "alt-%s", name);
            name = buf;
          }
          if (event.key.keysym.mod & (KMOD_SHIFT))
          {
            static char buf[64] = "";
            sprintf (buf, "shift-%s", name);
            name = buf;
          }
          }
          if (name)
            if (host->focused)
              mmm_add_event (host->focused->mmm, name);
        }
        break;
      case SDL_VIDEORESIZE:
        host_sdl->screen = SDL_SetVideoMode (event.resize.w,
                                             event.resize.h,32,
                                             baseflags | SDL_RESIZABLE);
        host->width = event.resize.w;
        host->height = event.resize.h;
        host->stride = host->width * host->bpp;

        fprintf (stderr, "%ix%i\n", event.resize.w, event.resize.h);
#if 1
    //    if (host->single_app && host->focused)
          mmm_host_set_size (host->focused->mmm,
                             host->width, host->height);
#endif
        break;
    }
    got_event = 1;
  }
  if (!got_event)
  {
    usleep (16000);
  }
  return got_event;
}

static int main_sdl (const char *path, int single)
{
  Host *host;
  host     = host_sdl_new (path, 1024, 768);
  HostSDL *host_sdl = (void*)host;
  host_sdl = (void*) host;
  atexit (SDL_Quit);
  host->single_app = single;

  while (!host_has_quit)
  {
    int got_event;

    got_event = sdl_check_events (host);
    host_idle_check (host);
    host_monitor_dir (host);

    if (host_is_dirty (host)) {
      int x, y;
      SDL_GetMouseState(&x, &y);

      if (!host->single_app)
        host->focused = NULL;

      MmmList *l;
      for (l = host->clients; l; l = l->next)
      {
        render_client (host, l->data, x, y);
      }
      SDL_UpdateRect(host_sdl->screen, 0,0,0,0);
		SDL_Flip(host_sdl->screen);
      host_clear_dirt (host);
    }
    else
    {
      if (host->single_app && !host->focused)
        host->focused = host->clients?host->clients->data:NULL;
      if (host->single_app && host->focused)
      {
        static char *title = NULL;
        if (!title || strcmp (title, mmm_get_title (host->focused->mmm)))
        {
          if (title) free (title);
          title = strdup (mmm_get_title (host->focused->mmm));
          SDL_WM_SetCaption (title, "mmm");
        }
      }
      usleep (5000);
    }
  }

  if (host->single_app)
  {
    rmdir (host->fbdir);
  }

  return 0;
}

int checkAddress ()
{
	MmmShm test;
#define GET_ADDR(a) \
fprintf (stderr, "%s: %p\n", #a, (void*)((uint8_t*)& (test.a) - (uint8_t*)&test));
	
	GET_ADDR(header.pid)
	GET_ADDR(fb.title)
	GET_ADDR(fb.width)
	GET_ADDR(fb.height)
	GET_ADDR(fb.stride)
	GET_ADDR(fb.fb_offset)
	GET_ADDR(fb.flip_state)
	
	GET_ADDR(fb.x)
	GET_ADDR(fb.y)
	GET_ADDR(fb.damage_x)
	GET_ADDR(fb.damage_y)
	GET_ADDR(fb.damage_width)
	GET_ADDR(fb.damage_height)
	
	GET_ADDR(pcm.format)
	GET_ADDR(pcm.sample_rate)
	GET_ADDR(pcm.write)
	GET_ADDR(pcm.read)
	GET_ADDR(pcm.buffer)
	
	fprintf (stderr, "%p\n", (void*)sizeof (MmmShm));
	
	return 0;
}

int main (int argc, char **argv)
{
  char path[256];

  if (argv[1] == NULL)
  {
    if (!getenv ("MMM_PATH"))
    {
      sprintf (path, "/tmp/mmm-%i", getpid());
      setenv ("MMM_PATH", path, 1);
      mkdir (path, 0777);
    }
    return main_sdl (path, 0);
  }

  if (argv[1][0] == '-' &&
      argv[1][1] == 'p' &&
      argv[2])
  {
    return main_sdl (argv[2], 1);
  }

  if (!getenv ("MMM_PATH"))
  {
    sprintf (path, "/tmp/mmm-%i", getpid());
    setenv ("MMM_PATH", path, 1);
    mkdir (path, 0777);
  }
	
	checkAddress();
	
	pthread_t thread1;
	
	pthread_create(&thread1, NULL, fragment, "Foo");
	
	//picoFB();
	main_sdl (path, 1);
	
	pthread_join(thread1, NULL);
	
	return 0;
/*
  if (argv[1])
    switch (fork())
    {
      case 0:
		return main_sdl (path, 1);
		
      case -1:
        fprintf (stderr, "fork failed\n");
        return 0;
    }
execvp (argv[1], argv+1);
return 0;*/
}
