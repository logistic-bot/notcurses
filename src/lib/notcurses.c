#include "version.h"
#include "egcpool.h"
#include "internal.h"
#include <ncurses.h> // needed for some definitions, see terminfo(3ncurses)
#include <time.h>
#include <term.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <locale.h>
#include <langinfo.h>
#include <sys/poll.h>
#include <stdatomic.h>
#include <sys/ioctl.h>

#define ESC "\x1b"

// only one notcurses object can be the target of signal handlers, due to their
// process-wide nature.
static notcurses* _Atomic signal_nc = ATOMIC_VAR_INIT(NULL); // ugh
static void (*signal_sa_handler)(int); // stashed signal handler we replaced

static int
drop_signals(notcurses* nc){
  notcurses* old = nc;
  if(!atomic_compare_exchange_strong(&signal_nc, &old, NULL)){
    fprintf(stderr, "Couldn't drop signals: %p != %p\n", old, nc);
    return -1;
  }
  return 0;
}

// Do the minimum necessary stuff to restore the terminal, then return. This is
// the end of the line for fatal signal handlers. notcurses_stop() will go on
// to tear down and account for internal structures.
static int
notcurses_stop_minimal(notcurses* nc){
  int ret = 0;
  drop_signals(nc);
  if(nc->tcache.rmcup && term_emit("rmcup", nc->tcache.rmcup, nc->ttyfp, true)){
    ret = -1;
  }
  if(nc->tcache.cnorm && term_emit("cnorm", nc->tcache.cnorm, nc->ttyfp, true)){
    ret = -1;
  }
  if(nc->tcache.op && term_emit("op", nc->tcache.op, nc->ttyfp, true)){
    ret = -1;
  }
  if(nc->tcache.sgr0 && term_emit("sgr0", nc->tcache.sgr0, nc->ttyfp, true)){
    ret = -1;
  }
  if(nc->tcache.oc && term_emit("oc", nc->tcache.oc, nc->ttyfp, true)){
    ret = -1;
  }
  ret |= notcurses_mouse_disable(nc);
  ret |= tcsetattr(nc->ttyfd, TCSANOW, &nc->tpreserved);
  return ret;
}

// this wildly unsafe handler will attempt to restore the screen upon
// reception of SIG{INT, SEGV, ABRT, QUIT}. godspeed you, black emperor!
static void
fatal_handler(int signo){
  notcurses* nc = atomic_load(&signal_nc);
  if(nc){
    notcurses_stop_minimal(nc);
    if(signal_sa_handler){
      signal_sa_handler(signo);
    }
    raise(signo);
  }
}

static int
setup_signals(notcurses* nc, bool no_quit_sigs, bool no_winch_sig){
  notcurses* expected = NULL;
  struct sigaction oldact;
  struct sigaction sa;

  if(!atomic_compare_exchange_strong(&signal_nc, &expected, nc)){
    fprintf(stderr, "%p is already registered for signals\n", expected);
    return -1;
  }
  if(!no_winch_sig){
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigwinch_handler;
    if(sigaction(SIGWINCH, &sa, NULL)){
      atomic_store(&signal_nc, NULL);
      fprintf(stderr, "Error installing SIGWINCH handler (%s)\n",
              strerror(errno));
      return -1;
    }
  }
  if(!no_quit_sigs){
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fatal_handler;
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGQUIT);
    sigaddset(&sa.sa_mask, SIGSEGV);
    sigaddset(&sa.sa_mask, SIGABRT);
    sa.sa_flags = SA_RESETHAND; // don't try twice
    int ret = 0;
    ret |= sigaction(SIGINT, &sa, &oldact);
    ret |= sigaction(SIGQUIT, &sa, &oldact);
    ret |= sigaction(SIGSEGV, &sa, &oldact);
    ret |= sigaction(SIGABRT, &sa, &oldact);
    if(ret){
      atomic_store(&signal_nc, NULL);
      fprintf(stderr, "Error installing fatal signal handlers (%s)\n",
              strerror(errno));
      return -1;
    }
    signal_sa_handler = oldact.sa_handler;
  }
  return 0;
}

// make a heap-allocated wchar_t expansion of the multibyte string at s
wchar_t* wchar_from_utf8(const char* s){
  mbstate_t ps;
  memset(&ps, 0, sizeof(ps));
  // worst case is a wchar_t for every byte of input (all-ASCII case)
  const size_t maxw = strlen(s) + 1;
  wchar_t* dst = malloc(sizeof(*dst) * maxw);
  size_t wcount = mbsrtowcs(dst, &s, maxw, &ps);
  if(wcount == (size_t)-1){
    free(dst);
    return NULL;
  }
  if(s){
    free(dst);
    return NULL;
  }
  return dst;
}

int ncplane_putstr_aligned(ncplane* n, int y, ncalign_e align, const char* s){
  wchar_t* w = wchar_from_utf8(s);
  if(w == NULL){
    return -1;
  }
  int r = ncplane_putwstr_aligned(n, y, align, w);
  free(w);
  return r;
}

static const char NOTCURSES_VERSION[] =
 notcurses_VERSION_MAJOR "."
 notcurses_VERSION_MINOR "."
 notcurses_VERSION_PATCH;

const char* notcurses_version(void){
  return NOTCURSES_VERSION;
}

void* ncplane_set_userptr(ncplane* n, void* opaque){
  void* ret = n->userptr;
  n->userptr = opaque;
  return ret;
}

void* ncplane_userptr(ncplane* n){
  return n->userptr;
}

const void* ncplane_userptr_const(const ncplane* n){
  return n->userptr;
}

// is the cursor in an invalid position? it never should be, but it's probably
// better to make sure (it's cheap) than to read from/write to random crap.
static bool
cursor_invalid_p(const ncplane* n){
  if(n->y >= n->leny || n->x >= n->lenx){
    return true;
  }
  if(n->y < 0 || n->x < 0){
    return true;
  }
  return false;
}

char* ncplane_at_cursor(ncplane* n, uint32_t* attrword, uint64_t* channels){
  if(cursor_invalid_p(n)){
    return NULL;
  }
  return cell_extract(n, &n->fb[nfbcellidx(n, n->y, n->x)], attrword, channels);
}

char* ncplane_at_yx(const ncplane* n, int y, int x, uint32_t* attrword, uint64_t* channels){
  char* ret = NULL;
  if(y < n->leny && x < n->lenx){
    if(y >= 0 && x >= 0){
      ret = cell_extract(n, &n->fb[nfbcellidx(n, y, x)], attrword, channels);
    }
  }
  return ret;
}

cell* ncplane_cell_ref_yx(ncplane* n, int y, int x){
  return &n->fb[nfbcellidx(n, y, x)];
}

void ncplane_dim_yx(const ncplane* n, int* rows, int* cols){
  if(rows){
    *rows = n->leny;
  }
  if(cols){
    *cols = n->lenx;
  }
}

// anyone calling this needs ensure the ncplane's framebuffer is updated
// to reflect changes in geometry.
int update_term_dimensions(int fd, int* rows, int* cols){
  struct winsize ws;
  int i = ioctl(fd, TIOCGWINSZ, &ws);
  if(i < 0){
    fprintf(stderr, "TIOCGWINSZ failed on %d (%s)\n", fd, strerror(errno));
    return -1;
  }
  if(ws.ws_row <= 0 || ws.ws_col <= 0){
    fprintf(stderr, "Bogus return from TIOCGWINSZ on %d (%d/%d)\n",
            fd, ws.ws_row, ws.ws_col);
    return -1;
  }
  if(rows){
    *rows = ws.ws_row;
  }
  if(cols){
    *cols = ws.ws_col;
  }
  return 0;
}

static void
free_plane(ncplane* p){
  if(p){
    --p->nc->stats.planes;
    p->nc->stats.fbbytes -= sizeof(*p->fb) * p->leny * p->lenx;
    egcpool_dump(&p->pool);
    free(p->fb);
    free(p);
  }
}

// create a new ncplane at the specified location (relative to the true screen,
// having origin at 0,0), having the specified size, and put it at the top of
// the planestack. its cursor starts at its origin; its style starts as null.
// a plane may exceed the boundaries of the screen, but must have positive
// size in both dimensions. bind the plane to 'n', which may be NULL. if bound
// to a plane, this plane moves when that plane moves, and move targets are
// relative to that plane.
static ncplane*
ncplane_create(notcurses* nc, ncplane* n, int rows, int cols,
               int yoff, int xoff, void* opaque){
  if(rows <= 0 || cols <= 0){
    return NULL;
  }
  ncplane* p = malloc(sizeof(*p));
  size_t fbsize = sizeof(*p->fb) * (rows * cols);
  if((p->fb = malloc(fbsize)) == NULL){
    free(p);
    return NULL;
  }
  memset(p->fb, 0, fbsize);
  p->scrolling = false;
  p->userptr = NULL;
  p->leny = rows;
  p->lenx = cols;
  p->x = p->y = 0;
  p->logrow = 0;
  if( (p->bound = n) ){
    p->absx = xoff + n->absx;
    p->absy = yoff + n->absy;
    p->bnext = n->blist;
    n->blist = p;
  }else{
    p->absx = xoff + nc->margin_l;
    p->absy = yoff + nc->margin_t;
    p->bnext = NULL;
  }
  p->attrword = 0;
  p->channels = 0;
  egcpool_init(&p->pool);
  cell_init(&p->basecell);
  p->blist = NULL;
  p->userptr = opaque;
  p->above = NULL;
  if( (p->below = nc->top) ){ // always happens save initial plane
    nc->top->above = p;
  }else{
    nc->bottom = p;
  }
  nc->top = p;
  p->nc = nc;
  nc->stats.fbbytes += fbsize;
  ++nc->stats.planes;
  return p;
}

// create an ncplane of the specified dimensions, but do not yet place it in
// the z-buffer. clear out all cells. this is for a wholly new context.
static ncplane*
create_initial_ncplane(notcurses* nc, int dimy, int dimx){
  nc->stdscr = ncplane_create(nc, NULL, dimy - (nc->margin_t + nc->margin_b),
                              dimx - (nc->margin_l + nc->margin_r), 0, 0, NULL);
  return nc->stdscr;
}

ncplane* notcurses_stdplane(notcurses* nc){
  return nc->stdscr;
}

const ncplane* notcurses_stdplane_const(const notcurses* nc){
  return nc->stdscr;
}

ncplane* ncplane_new(notcurses* nc, int rows, int cols, int yoff, int xoff, void* opaque){
  return ncplane_create(nc, NULL, rows, cols, yoff, xoff, opaque);
}

ncplane* ncplane_bound(ncplane* n, int rows, int cols, int yoff, int xoff, void* opaque){
  return ncplane_create(n->nc, n, rows, cols, yoff, xoff, opaque);
}

ncplane* ncplane_aligned(ncplane* n, int rows, int cols, int yoff,
                         ncalign_e align, void* opaque){
  return ncplane_create(n->nc, NULL, rows, cols, yoff, ncplane_align(n, align, cols), opaque);
}

inline int ncplane_cursor_move_yx(ncplane* n, int y, int x){
  if(x >= n->lenx){
    return -1;
  }else if(x < 0){
    if(x < -1){
      return -1;
    }
  }else{
    n->x = x;
  }
  if(y >= n->leny){
    return -1;
  }else if(y < 0){
    if(y < -1){
      return -1;
    }
  }else{
    n->y = y;
  }
  if(cursor_invalid_p(n)){
    return -1;
  }
  return 0;
}

ncplane* ncplane_dup(const ncplane* n, void* opaque){
  int dimy = n->leny;
  int dimx = n->lenx;
  uint32_t attr = ncplane_attr(n);
  uint64_t chan = ncplane_channels(n);
  ncplane* newn = ncplane_create(n->nc, n->bound, dimy, dimx, n->absy, n->absx, opaque);
  if(newn){
    if(egcpool_dup(&newn->pool, &n->pool)){
      ncplane_destroy(newn);
      return NULL;
    }else{
      ncplane_cursor_move_yx(newn, n->y, n->x);
      newn->attrword = attr;
      newn->channels = chan;
      memmove(newn->fb, n->fb, sizeof(*n->fb) * dimx * dimy);
      // we dupd the egcpool, so just dup the goffset
      newn->basecell = n->basecell;
    }
  }
  return newn;
}

// can be used on stdscr, unlike ncplane_resize() which prohibits it.
int ncplane_resize_internal(ncplane* n, int keepy, int keepx, int keepleny,
                            int keeplenx, int yoff, int xoff, int ylen, int xlen){
  if(keepleny < 0 || keeplenx < 0){ // can't retain negative size
//fprintf(stderr, "Can't retain negative size %dx%d\n", keepleny, keeplenx);
    return -1;
  }
  if(ylen <= 0 || xlen <= 0){ // can't resize to trivial or negative size
//fprintf(stderr, "Can't achieve negative size %dx%d\n", ylen, xlen);
    return -1;
  }
  if((!keepleny && keeplenx) || (keepleny && !keeplenx)){ // both must be 0
//fprintf(stderr, "Can't keep zero dimensions %dx%d\n", keepleny, keeplenx);
    return -1;
  }
  if(ylen < keepleny || xlen < keeplenx){ // can't be smaller than our keep
//fprintf(stderr, "Can't violate space %dx%d vs %dx%d\n", keepleny, keeplenx, ylen, xlen);
    return -1;
  }
  int rows, cols;
  ncplane_dim_yx(n, &rows, &cols);
//fprintf(stderr, "NCPLANE(RESIZING) to %dx%d at %d/%d (keeping %dx%d from %d/%d)\n",
//        ylen, xlen, yoff, xoff, keepleny, keeplenx, keepy, keepx);
  // we're good to resize. we'll need alloc up a new framebuffer, and copy in
  // those elements we're retaining, zeroing out the rest. alternatively, if
  // we've shrunk, we will be filling the new structure.
  int keptarea = keepleny * keeplenx;
  int newarea = ylen * xlen;
  size_t fbsize = sizeof(cell) * newarea;
  cell* fb = malloc(fbsize);
  if(fb == NULL){
    return -1;
  }
  // update the cursor, if it would otherwise be off-plane
  if(n->y >= ylen){
    n->y = ylen - 1;
  }
  if(n->x >= xlen){
    n->x = xlen - 1;
  }
  cell* preserved = n->fb;
  n->nc->stats.fbbytes -= sizeof(*preserved) * (rows * cols);
  n->nc->stats.fbbytes += fbsize;
  n->fb = fb;
  n->absy = n->absy + keepy - yoff;
  n->absx = n->absx + keepx - xoff;
  // if we're keeping nothing, dump the old egcspool. otherwise, we go ahead
  // and keep it. perhaps we ought compact it?
  if(keptarea == 0){ // keep nothing, resize/move only
    memset(fb, 0, sizeof(*fb) * newarea);
    egcpool_dump(&n->pool);
    n->lenx = xlen;
    n->leny = ylen;
    free(preserved);
    return 0;
  }
  // we currently have maxy rows of maxx cells each. we will be keeping rows
  // keepy..keepy + keepleny - 1 and columns keepx..keepx + keeplenx - 1. they
  // will end up at keepy + yoff..keepy + keepleny - 1 + yoff and
  // keepx + xoff..keepx + keeplenx - 1 + xoff. everything else is zerod out.
  int itery;
  // we'll prepare each cell in our new framebuffer with either zeroes or a copy
  // from the old one.
  int sourceline = keepy;
  for(itery = 0 ; itery < ylen ; ++itery){
    int copyoff = itery * xlen; // our target at any given time
    // FIXME in memset()s here of existing text, don't we need cell_release()?
    // if we have nothing copied to this line, zero it out in one go
    if(itery < keepy || itery > keepy + keepleny - 1){
      memset(fb + copyoff, 0, sizeof(*fb) * xlen);
      continue;
    }
    // we do have something to copy, and zero, one, or two regions to zero out
    int copied = 0;
    if(xoff < 0){
      memset(fb + copyoff, 0, sizeof(*fb) * -xoff);
      copyoff += -xoff;
      copied += -xoff;
    }
    const int sourceidx = nfbcellidx(n, sourceline, keepx);
    memcpy(fb + copyoff, preserved + sourceidx, sizeof(*fb) * keeplenx);
    copyoff += keeplenx;
    copied += keeplenx;
    if(xlen > copied){
      memset(fb + copyoff, 0, sizeof(*fb) * (xlen - copied));
    }
    ++sourceline;
  }
  n->lenx = xlen;
  n->leny = ylen;
  free(preserved);
  return 0;
}

int ncplane_resize(ncplane* n, int keepy, int keepx, int keepleny,
                   int keeplenx, int yoff, int xoff, int ylen, int xlen){
  if(n == n->nc->stdscr){
fprintf(stderr, "Can't resize standard plane\n");
    return -1;
  }
  return ncplane_resize_internal(n, keepy, keepx, keepleny, keeplenx,
                                 yoff, xoff, ylen, xlen);
}

int ncplane_destroy(ncplane* ncp){
  if(ncp == NULL){
    return 0;
  }
  if(ncp->nc->stdscr == ncp){
    return -1;
  }
  if(ncp->above){
    ncp->above->below = ncp->below;
  }else{
    ncp->nc->top = ncp->below;
  }
  if(ncp->below){
    ncp->below->above = ncp->above;
  }else{
    ncp->nc->bottom = ncp->above;
  }
  free_plane(ncp);
  return 0;
}

static int
make_nonblocking(FILE* fp){
  int fd = fileno(fp);
  if(fd < 0){
    return -1;
  }
  int flags = fcntl(fd, F_GETFL, 0);
  if(flags < 0){
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void
reset_stats(ncstats* stats){
  uint64_t fbbytes = stats->fbbytes;
  memset(stats, 0, sizeof(*stats));
  stats->render_min_ns = 1ull << 62u;
  stats->render_min_bytes = 1ull << 62u;
  stats->fbbytes = fbbytes;
}

// add the current stats to the cumulative stashed stats, and reset them
static void
stash_stats(notcurses* nc){
  nc->stashstats.renders += nc->stats.renders;
  nc->stashstats.render_ns += nc->stats.render_ns;
  nc->stashstats.failed_renders += nc->stats.failed_renders;
  nc->stashstats.render_bytes += nc->stats.render_bytes;
  if(nc->stashstats.render_max_bytes < nc->stats.render_max_bytes){
    nc->stashstats.render_max_bytes = nc->stats.render_max_bytes;
  }
  if(nc->stashstats.render_max_ns < nc->stats.render_max_ns){
    nc->stashstats.render_max_ns = nc->stats.render_max_ns;
  }
  if(nc->stashstats.render_min_bytes > nc->stats.render_min_bytes){
    nc->stashstats.render_min_bytes = nc->stats.render_min_bytes;
  }
  if(nc->stashstats.render_min_ns > nc->stats.render_min_ns){
    nc->stashstats.render_min_ns = nc->stats.render_min_ns;
  }
  nc->stashstats.cellelisions += nc->stats.cellelisions;
  nc->stashstats.cellemissions += nc->stats.cellemissions;
  nc->stashstats.fgelisions += nc->stats.fgelisions;
  nc->stashstats.fgemissions += nc->stats.fgemissions;
  nc->stashstats.bgelisions += nc->stats.bgelisions;
  nc->stashstats.bgemissions += nc->stats.bgemissions;
  nc->stashstats.defaultelisions += nc->stats.defaultelisions;
  nc->stashstats.defaultemissions += nc->stats.defaultemissions;
  // fbbytes aren't stashed
  reset_stats(&nc->stats);
}

void notcurses_stats(const notcurses* nc, ncstats* stats){
  memcpy(stats, &nc->stats, sizeof(*stats));
}

void notcurses_reset_stats(notcurses* nc, ncstats* stats){
  if(stats){
    memcpy(stats, &nc->stats, sizeof(*stats));
  }
  stash_stats(nc);
}

// Convert a notcurses log level to its ffmpeg equivalent.
static int
ffmpeg_log_level(ncloglevel_e level){
#ifdef USE_FFMPEG
  switch(level){
    case NCLOGLEVEL_SILENT: return AV_LOG_QUIET;
    case NCLOGLEVEL_PANIC: return AV_LOG_PANIC;
    case NCLOGLEVEL_FATAL: return AV_LOG_FATAL;
    case NCLOGLEVEL_ERROR: return AV_LOG_ERROR;
    case NCLOGLEVEL_WARNING: return AV_LOG_WARNING;
    case NCLOGLEVEL_INFO: return AV_LOG_INFO;
    case NCLOGLEVEL_VERBOSE: return AV_LOG_VERBOSE;
    case NCLOGLEVEL_DEBUG: return AV_LOG_DEBUG;
    case NCLOGLEVEL_TRACE: return AV_LOG_TRACE;
    default: break;
  }
  fprintf(stderr, "Invalid log level: %d\n", level);
  return AV_LOG_TRACE;
#else
  return level;
#endif
}

ncdirect* ncdirect_init(const char* termtype, FILE* outfp){
  if(outfp == NULL){
    outfp = stdout;
  }
  ncdirect* ret = malloc(sizeof(*ret));
  if(ret == NULL){
    return ret;
  }
  ret->ttyfp = outfp;
  memset(&ret->palette, 0, sizeof(ret->palette));
  int ttyfd = fileno(ret->ttyfp);
  if(ttyfd < 0){
    fprintf(stderr, "No file descriptor was available in outfp %p\n", outfp);
    free(ret);
    return NULL;
  }
  int termerr;
  if(setupterm(termtype, ttyfd, &termerr) != OK){
    fprintf(stderr, "Terminfo error %d (see terminfo(3ncurses))\n", termerr);
    free(ret);
    return NULL;
  }
  if(interrogate_terminfo(&ret->tcache)){
    free(ret);
    return NULL;
  }
  ret->fgdefault = ret->bgdefault = true;
  ret->fgrgb = ret->bgrgb = 0;
  ncdirect_styles_set(ret, 0);
  return ret;
}

// unless the suppress_banner flag was set, print some version information and
// (if applicable) warnings to stdout. we are not yet on the alternate screen.
static void
init_banner(const notcurses* nc){
  if(!nc->suppress_banner){
    char prefixbuf[BPREFIXSTRLEN + 1];
    term_fg_palindex(nc, stdout, nc->tcache.colors <= 256 ? 50 % nc->tcache.colors : 0x20e080);
    printf("\n notcurses %s by nick black et al", notcurses_version());
    term_fg_palindex(nc, stdout, nc->tcache.colors <= 256 ? 12 % nc->tcache.colors : 0x2080e0);
    printf("\n  %d rows, %d columns (%sB), %d colors (%s)\n"
           "  compiled with gcc-%s\n"
           "  terminfo from %s\n",
           nc->stdscr->leny, nc->stdscr->lenx,
           bprefix(nc->stats.fbbytes, 1, prefixbuf, 0),
           nc->tcache.colors, nc->tcache.RGBflag ? "direct" : "palette",
           __VERSION__, curses_version());
#ifdef USE_FFMPEG
    printf("  avformat %u.%u.%u\n  avutil %u.%u.%u\n  swscale %u.%u.%u\n",
          LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR, LIBAVFORMAT_VERSION_MICRO,
          LIBAVUTIL_VERSION_MAJOR, LIBAVUTIL_VERSION_MINOR, LIBAVUTIL_VERSION_MICRO,
          LIBSWSCALE_VERSION_MAJOR, LIBSWSCALE_VERSION_MINOR, LIBSWSCALE_VERSION_MICRO);
#else
#ifdef USE_OIIO
    printf("  openimageio %s\n", oiio_version());
#else
    term_fg_palindex(nc, stderr, nc->tcache.colors <= 88 ? 1 % nc->tcache.colors : 0xcb);
    fprintf(stderr, "\n Warning! Notcurses was built without multimedia support.\n");
#endif
#endif
    fflush(stdout);
    term_fg_palindex(nc, stderr, nc->tcache.colors <= 88 ? 1 % nc->tcache.colors : 0xcb);
    if(!nc->tcache.RGBflag){ // FIXME
      fprintf(stderr, "\n Warning! Colors subject to https://github.com/dankamongmen/notcurses/issues/4");
      fprintf(stderr, "\n  Specify a (correct) TrueColor TERM, or COLORTERM=24bit.\n");
    }else{
      if(!nc->tcache.CCCflag){
        fprintf(stderr, "\n Warning! Advertised TrueColor but no 'ccc' flag\n");
      }
    }
    if(!notcurses_canutf8(nc)){
      fprintf(stderr, "\n Warning! Encoding is not UTF-8.\n");
    }
  }
}

// it's critical that we're in a UTF-8 locale if at all possible. since the
// client might not have called setlocale(2) (if they weren't reading the
// directions...), go ahead and try calling it ourselves *iff* we're in the
// default "C" or "POSIX" locale. this still requires the user to have a proper
// LANG configured. either way, they're going to get a diagnostic (unless the
// user has explicitly configured a LANG of "C" or "POSIX"). recommended
// practice is for the client code to have called setlocale() themselves, and
// set the NCOPTION_INHIBIT_SETLOCALE flag. if that flag is set, we take the
// locale as we get it.
static void
init_lang(const notcurses_options* opts){
  if(!(opts->flags & NCOPTION_INHIBIT_SETLOCALE)){
    const char* locale = setlocale(LC_ALL, NULL);
    if(locale && (!strcmp(locale, "C") || !strcmp(locale, "POSIX"))){
      const char* lang = getenv("LANG");
      if(lang){
        // if LANG was explicitly set to C/POSIX, roll with it
        if(strcmp(locale, "C") && strcmp(locale, "POSIX")){
          if(!locale){ // otherwise, generate diagnostic
            fprintf(stderr, "Couldn't set locale based off LANG %s\n", lang);
          }else{
            fprintf(stderr, "Set %s locale from LANG; client should call setlocale(2)!\n",
                    lang ? lang : "???");
          }
        }
      }else{
        fprintf(stderr, "No LANG environment variable was set, ick :/\n");
      }
    }
  }
}

notcurses* notcurses_init(const notcurses_options* opts, FILE* outfp){
  notcurses_options defaultopts;
  memset(&defaultopts, 0, sizeof(defaultopts));
  if(!opts){
    opts = &defaultopts;
  }
  if(opts->margin_t < 0 || opts->margin_b < 0 || opts->margin_l < 0 || opts->margin_r < 0){
    fprintf(stderr, "Provided an illegal negative margin, refusing to start\n");
    return NULL;
  }
  notcurses* ret = malloc(sizeof(*ret));
  if(ret == NULL){
    return ret;
  }
  init_lang(opts);
  const char* encoding = nl_langinfo(CODESET);
  if(encoding && strcmp(encoding, "UTF-8") == 0){
    ret->utf8 = true;
  }else if(encoding && strcmp(encoding, "ANSI_X3.4-1968") == 0){
    ret->utf8 = false;
  }else{
    fprintf(stderr, "Encoding (\"%s\") was neither ANSI_X3.4-1968 nor UTF-8, refusing to start\n Did you call setlocale()?\n",
            encoding ? encoding : "none found");
    free(ret);
    return NULL;
  }
  bool own_outfp = false;
  if(outfp == NULL){
    if((outfp = fopen("/dev/tty", "wbe")) == NULL){
      free(ret);
      return NULL;
    }
    own_outfp = true;
  }
  ret->margin_t = opts->margin_t;
  ret->margin_b = opts->margin_b;
  ret->margin_l = opts->margin_l;
  ret->margin_r = opts->margin_r;
  ret->stats.fbbytes = 0;
  ret->stashstats.fbbytes = 0;
  reset_stats(&ret->stats);
  reset_stats(&ret->stashstats);
  ret->ttyfp = outfp;
  ret->ownttyfp = own_outfp;
  ret->renderfp = opts->renderfp;
  ret->inputescapes = NULL;
  ret->ttyinfp = stdin; // FIXME
  memset(&ret->rstate, 0, sizeof(ret->rstate));
  memset(&ret->palette_damage, 0, sizeof(ret->palette_damage));
  memset(&ret->palette, 0, sizeof(ret->palette));
  ret->lastframe = NULL;
  ret->lfdimy = 0;
  ret->lfdimx = 0;
  egcpool_init(&ret->pool);
  if(make_nonblocking(ret->ttyinfp)){
    free(ret);
    return NULL;
  }
  ret->inputbuf_occupied = 0;
  ret->inputbuf_valid_starts = 0;
  ret->inputbuf_write_at = 0;
  ret->input_events = 0;
  if((ret->ttyfd = fileno(ret->ttyfp)) < 0){
    fprintf(stderr, "No file descriptor was available in outfp %p\n", outfp);
    free(ret);
    return NULL;
  }
  notcurses_mouse_disable(ret);
  if(tcgetattr(ret->ttyfd, &ret->tpreserved)){
    fprintf(stderr, "Couldn't preserve terminal state for %d (%s)\n",
            ret->ttyfd, strerror(errno));
    free(ret);
    return NULL;
  }
  struct termios modtermios;
  memcpy(&modtermios, &ret->tpreserved, sizeof(modtermios));
  // see termios(3). disabling ECHO and ICANON means input will not be echoed
  // to the screen, input is made available without enter-based buffering, and
  // line editing is disabled. since we have not gone into raw mode, ctrl+c
  // etc. still have their typical effects. ICRNL maps return to 13 (Ctrl+M)
  // instead of 10 (Ctrl+J).
  modtermios.c_lflag &= (~ECHO & ~ICANON);
  modtermios.c_iflag &= (~ICRNL);
  if(tcsetattr(ret->ttyfd, TCSANOW, &modtermios)){
    fprintf(stderr, "Error disabling echo / canonical on %d (%s)\n",
            ret->ttyfd, strerror(errno));
    free(ret);
    return NULL;
  }
  if(setup_signals(ret, opts->no_quit_sighandlers, opts->no_winch_sighandler)){
    goto err;
  }
  int termerr;
  if(setupterm(opts->termtype, ret->ttyfd, &termerr) != OK){
    fprintf(stderr, "Terminfo error %d (see terminfo(3ncurses))\n", termerr);
    goto err;
  }
  int dimy, dimx;
  if(update_term_dimensions(ret->ttyfd, &dimy, &dimx)){
    goto err;
  }
  char* shortname_term = termname();
  char* longname_term = longname();
  if(!opts->suppress_banner){
    fprintf(stderr, "Term: %dx%d %s (%s)\n", dimy, dimx,
            shortname_term ? shortname_term : "?",
            longname_term ? longname_term : "?");
  }
  ret->truecols = dimx;
  if(interrogate_terminfo(&ret->tcache)){
    goto err;
  }
  if(prep_special_keys(ret)){
    goto err;
  }
  // Neither of these is supported on e.g. the "linux" virtual console.
  if(!opts->inhibit_alternate_screen){
    term_verify_seq(&ret->tcache.smcup, "smcup");
    term_verify_seq(&ret->tcache.rmcup, "rmcup");
  }
  ret->bottom = ret->top = ret->stdscr = NULL;
  if(ncvisual_init(ffmpeg_log_level(opts->loglevel))){
    goto err;
  }
  if((ret->stdscr = create_initial_ncplane(ret, dimy, dimx)) == NULL){
    goto err;
  }
  if(!opts->retain_cursor){
    if(ret->tcache.civis && term_emit("civis", ret->tcache.civis, ret->ttyfp, false)){
      free_plane(ret->top);
      goto err;
    }
  }
  if(ret->tcache.smkx && term_emit("smkx", ret->tcache.smkx, ret->ttyfp, false)){
    free_plane(ret->top);
    goto err;
  }
  if((ret->rstate.mstreamfp = open_memstream(&ret->rstate.mstream, &ret->rstate.mstrsize)) == NULL){
    free_plane(ret->top);
    goto err;
  }
  ret->suppress_banner = opts->suppress_banner;
  init_banner(ret);
  // flush on the switch to alternate screen, lest initial output be swept away
  if(ret->tcache.smcup && term_emit("smcup", ret->tcache.smcup, ret->ttyfp, true)){
    free_plane(ret->top);
    goto err;
  }
  return ret;

err:
  // FIXME looks like we have some memory leaks on this error path?
  tcsetattr(ret->ttyfd, TCSANOW, &ret->tpreserved);
  drop_signals(ret);
  free(ret);
  return NULL;
}

void notcurses_drop_planes(notcurses* nc){
  ncplane* p = nc->top;
  while(p){
    ncplane* tmp = p->below;
    if(nc->stdscr != p){
      free_plane(p);
    }
    p = tmp;
  }
}

int notcurses_stop(notcurses* nc){
  int ret = 0;
  if(nc){
    ret |= notcurses_stop_minimal(nc);
    while(nc->top){
      ncplane* p = nc->top->below;
      free_plane(nc->top);
      nc->top = p;
    }
    if(nc->rstate.mstreamfp){
      fclose(nc->rstate.mstreamfp);
    }
    egcpool_dump(&nc->pool);
    free(nc->lastframe);
    free(nc->rstate.mstream);
    input_free_esctrie(&nc->inputescapes);
    stash_stats(nc);
    if(nc->ownttyfp){
      ret |= fclose(nc->ttyfp);
    }
    if(!nc->suppress_banner){
      if(nc->stashstats.renders){
        char totalbuf[BPREFIXSTRLEN + 1];
        char minbuf[BPREFIXSTRLEN + 1];
        char maxbuf[BPREFIXSTRLEN + 1];
        char avgbuf[BPREFIXSTRLEN + 1];
        qprefix(nc->stashstats.render_ns, NANOSECS_IN_SEC, totalbuf, 0);
        qprefix(nc->stashstats.render_min_ns, NANOSECS_IN_SEC, minbuf, 0);
        qprefix(nc->stashstats.render_max_ns, NANOSECS_IN_SEC, maxbuf, 0);
        qprefix(nc->stashstats.render_ns / nc->stashstats.renders, NANOSECS_IN_SEC, avgbuf, 0);
        fprintf(stderr, "\n%ju render%s, %ss total (%ss min, %ss max, %ss avg)\n",
                nc->stashstats.renders, nc->stashstats.renders == 1 ? "" : "s",
                totalbuf, minbuf, maxbuf, avgbuf);
        bprefix(nc->stashstats.render_bytes, 1, totalbuf, 0),
        bprefix(nc->stashstats.render_min_bytes, 1, minbuf, 0),
        bprefix(nc->stashstats.render_max_bytes, 1, maxbuf, 0),
        fprintf(stderr, "%sB total (%sB min, %sB max, %.03gKiB avg)\n",
                totalbuf, minbuf, maxbuf,
                nc->stashstats.render_bytes / (double)nc->stashstats.renders / 1024);
      }
      if(nc->stashstats.renders || nc->stashstats.failed_renders){
        fprintf(stderr, "%.1f theoretical FPS, %ju failed render%s\n",
                nc->stashstats.renders ?
                  NANOSECS_IN_SEC * (double)nc->stashstats.renders / nc->stashstats.render_ns : 0.0,
                nc->stashstats.failed_renders,
                nc->stashstats.failed_renders == 1 ? "" : "s");
        fprintf(stderr, "RGB emits:elides: def %ju:%ju fg %ju:%ju bg %ju:%ju\n",
                nc->stashstats.defaultemissions,
                nc->stashstats.defaultelisions,
                nc->stashstats.fgemissions,
                nc->stashstats.fgelisions,
                nc->stashstats.bgemissions,
                nc->stashstats.bgelisions);
        fprintf(stderr, " Elide rates: %.2f%% %.2f%% %.2f%%\n",
                (nc->stashstats.defaultemissions + nc->stashstats.defaultelisions) == 0 ? 0 :
                (nc->stashstats.defaultelisions * 100.0) / (nc->stashstats.defaultemissions + nc->stashstats.defaultelisions),
                (nc->stashstats.fgemissions + nc->stashstats.fgelisions) == 0 ? 0 :
                (nc->stashstats.fgelisions * 100.0) / (nc->stashstats.fgemissions + nc->stashstats.fgelisions),
                (nc->stashstats.bgemissions + nc->stashstats.bgelisions) == 0 ? 0 :
                (nc->stashstats.bgelisions * 100.0) / (nc->stashstats.bgemissions + nc->stashstats.bgelisions));
        fprintf(stderr, "Cell emits:elides: %ju/%ju (%.2f%%)\n",
                nc->stashstats.cellemissions, nc->stashstats.cellelisions,
                (nc->stashstats.cellemissions + nc->stashstats.cellelisions) == 0 ? 0 :
                (nc->stashstats.cellelisions * 100.0) / (nc->stashstats.cellemissions + nc->stashstats.cellelisions));
      }
    }
    del_curterm(cur_term);
    free(nc);
  }
  return ret;
}

uint64_t ncplane_channels(const ncplane* n){
  return n->channels;
}

uint32_t ncplane_attr(const ncplane* n){
  return n->attrword;
}

void ncplane_set_channels(ncplane* n, uint64_t channels){
  n->channels = channels;
}

void ncplane_set_attr(ncplane* n, uint32_t attrword){
  n->attrword = attrword;
}

void ncplane_set_fg_default(ncplane* n){
  channels_set_fg_default(&n->channels);
}

void ncplane_set_bg_default(ncplane* n){
  channels_set_bg_default(&n->channels);
}

void ncplane_set_bg_rgb_clipped(ncplane* n, int r, int g, int b){
  channels_set_bg_rgb_clipped(&n->channels, r, g, b);
}

int ncplane_set_bg_rgb(ncplane* n, int r, int g, int b){
  return channels_set_bg_rgb(&n->channels, r, g, b);
}

void ncplane_set_fg_rgb_clipped(ncplane* n, int r, int g, int b){
  channels_set_fg_rgb_clipped(&n->channels, r, g, b);
}

int ncplane_set_fg_rgb(ncplane* n, int r, int g, int b){
  return channels_set_fg_rgb(&n->channels, r, g, b);
}

int ncplane_set_fg(ncplane* n, unsigned channel){
  return channels_set_fg(&n->channels, channel);
}

int ncplane_set_bg(ncplane* n, unsigned channel){
  return channels_set_bg(&n->channels, channel);
}

int ncplane_set_fg_alpha(ncplane* n, int alpha){
  return channels_set_fg_alpha(&n->channels, alpha);
}

int ncplane_set_bg_alpha(ncplane *n, int alpha){
  return channels_set_bg_alpha(&n->channels, alpha);
}

int ncplane_set_fg_palindex(ncplane* n, int idx){
  if(idx < 0 || idx >= NCPALETTESIZE){
    return -1;
  }
  n->channels |= CELL_FGDEFAULT_MASK;
  n->channels |= CELL_FG_PALETTE;
  n->channels &= ~(CELL_ALPHA_MASK << 32u);
  n->attrword &= 0xffff00ff;
  n->attrword |= (idx << 8u);
  return 0;
}

int ncplane_set_bg_palindex(ncplane* n, int idx){
  if(idx < 0 || idx >= NCPALETTESIZE){
    return -1;
  }
  n->channels |= CELL_BGDEFAULT_MASK;
  n->channels |= CELL_BG_PALETTE;
  n->channels &= ~CELL_ALPHA_MASK;
  n->attrword &= 0xffffff00;
  n->attrword |= idx;
  return 0;
}

int ncplane_set_base_cell(ncplane* ncp, const cell* c){
  return cell_duplicate(ncp, &ncp->basecell, c);
}

int ncplane_set_base(ncplane* ncp, const char* egc, uint32_t attrword, uint64_t channels){
  return cell_prime(ncp, &ncp->basecell, egc, attrword, channels);
}

int ncplane_base(ncplane* ncp, cell* c){
  return cell_duplicate(ncp, c, &ncp->basecell);
}

const char* cell_extended_gcluster(const ncplane* n, const cell* c){
  return extended_gcluster(n, c);
}

// 'n' ends up above 'above'
int ncplane_move_above(ncplane* restrict n, ncplane* restrict above){
  if(n == above){
    return -1;
  }
  if(n->below != above){
    // splice out 'n'
    if(n->below){
      n->below->above = n->above;
    }else{
      n->nc->bottom = n->above;
    }
    if(n->above){
      n->above->below = n->below;
    }else{
      n->nc->top = n->below;
    }
    if(above->above){
      above->above->below = n;
    }else{
      n->nc->top = n;
    }
    above->above = n;
    n->below = above;
  }
  return 0;
}

// 'n' ends up below 'below'
int ncplane_move_below(ncplane* restrict n, ncplane* restrict below){
  if(n == below){
    return -1;
  }
  if(n->above != below){
    if(n->below){
      n->below->above = n->above;
    }else{
      n->nc->bottom = n->above;
    }
    if(n->above){
      n->above->below = n->below;
    }else{
      n->nc->top = n->below;
    }
    if(below->below){
      below->below->above = n;
    }else{
      n->nc->bottom = n;
    }
    below->below = n;
    n->above = below;
  }
  return 0;
}

void ncplane_move_top(ncplane* n){
  if(n->above){
    if( (n->above->below = n->below) ){
      n->below->above = n->above;
    }else{
      n->nc->bottom = n->above;
    }
    n->above = NULL;
    if( (n->below = n->nc->top) ){
      n->below->above = n;
    }
    n->nc->top = n;
  }
}

void ncplane_move_bottom(ncplane* n){
  if(n->below){
    if( (n->below->above = n->above) ){
      n->above->below = n->below;
    }else{
      n->nc->top = n->below;
    }
    n->below = NULL;
    if( (n->above = n->nc->bottom) ){
      n->above->below = n;
    }
    n->nc->bottom = n;
  }
}

void ncplane_cursor_yx(const ncplane* n, int* y, int* x){
  if(y){
    *y = n->y;
  }
  if(x){
    *x = n->x;
  }
}

static inline void
cell_obliterate(ncplane* n, cell* c){
  cell_release(n, c);
  cell_init(c);
}

// increment y by 1 and rotate the framebuffer up one line. x moves to 0.
static inline void
scroll_down(ncplane* n){
  n->x = 0;
  if(n->y == n->leny - 1){
    n->logrow = (n->logrow + 1) % n->leny;
    cell* row = n->fb + nfbcellidx(n, n->y, 0);
    for(int clearx = 0 ; clearx < n->lenx ; ++clearx){
      cell_release(n, &row[clearx]);
    }
    memset(row, 0, sizeof(*row) * n->lenx);
  }else{
    ++n->y;
  }
}

int ncplane_putc_yx(ncplane* n, int y, int x, const cell* c){
  // if scrolling is enabled, check *before ncplane_cursor_move_yx()* whether
  // we're past the end of the line, and move to the next line if so.
  bool wide = cell_double_wide_p(c);
  if(x == -1 && y == -1 && n->x + wide >= n->lenx){
    if(!n->scrolling){
      return -1;
    }
    scroll_down(n);
  }
  if(ncplane_cursor_move_yx(n, y, x)){
    return -1;
  }
  if(c->gcluster == '\n'){
    if(n->scrolling){
      scroll_down(n);
      return 0;
    }
  }
  // A wide character obliterates anything to its immediate right (and marks
  // that cell as wide). Any character placed atop one half of a wide character
  // obliterates the other half. Note that a wide char can thus obliterate two
  // wide chars, totalling four columns.
  cell* targ = ncplane_cell_ref_yx(n, n->y, n->x);
  if(n->x > 0){
    if(cell_double_wide_p(targ)){ // replaced cell is half of a wide char
      if(targ->gcluster == 0){ // we're the right half
        cell_obliterate(n, &n->fb[nfbcellidx(n, n->y, n->x - 1)]);
      }else{
        cell_obliterate(n, &n->fb[nfbcellidx(n, n->y, n->x + 1)]);
      }
    }
  }
  if(cell_duplicate(n, targ, c) < 0){
    return -1;
  }
  int cols = 1;
  if(wide){ // must set our right wide, and check for further damage
    ++cols;
    if(n->x < n->lenx - 1){ // check to our right
      cell* candidate = &n->fb[nfbcellidx(n, n->y, n->x + 1)];
      if(n->x < n->lenx - 2){
        if(cell_wide_left_p(candidate)){
          cell_obliterate(n, &n->fb[nfbcellidx(n, n->y, n->x + 2)]);
        }
      }
      cell_obliterate(n, candidate);
      cell_set_wide(candidate);
      candidate->channels = c->channels;
    }
  }
  n->x += cols;
  return cols;
}

int ncplane_putegc_yx(ncplane* n, int y, int x, const char* gclust, int* sbytes){
  cell c = CELL_TRIVIAL_INITIALIZER;
  int primed = cell_prime(n, &c, gclust, n->attrword, n->channels);
  if(sbytes){
    *sbytes = primed;
  }
  if(primed < 0){
    return -1;
  }
  int ret = ncplane_putc_yx(n, y, x, &c);
  cell_release(n, &c);
  return ret;
}

int ncplane_putsimple_stainable(ncplane* n, char c){
  uint64_t channels = n->channels;
  uint32_t attrword = n->attrword;
  const cell* targ = &n->fb[nfbcellidx(n, n->y, n->x)];
  n->channels = targ->channels;
  n->attrword = targ->attrword;
  int ret = ncplane_putsimple(n, c);
  n->channels = channels;
  n->attrword = attrword;
  return ret;
}

int ncplane_putwegc_stainable(ncplane* n, const wchar_t* gclust, int* sbytes){
  uint64_t channels = n->channels;
  uint32_t attrword = n->attrword;
  const cell* targ = &n->fb[nfbcellidx(n, n->y, n->x)];
  n->channels = targ->channels;
  n->attrword = targ->attrword;
  int ret = ncplane_putwegc(n, gclust, sbytes);
  n->channels = channels;
  n->attrword = attrword;
  return ret;
}

int ncplane_putegc_stainable(ncplane* n, const char* gclust, int* sbytes){
  uint64_t channels = n->channels;
  uint32_t attrword = n->attrword;
  const cell* targ = &n->fb[nfbcellidx(n, n->y, n->x)];
  n->channels = targ->channels;
  n->attrword = targ->attrword;
  int ret = ncplane_putegc(n, gclust, sbytes);
  n->channels = channels;
  n->attrword = attrword;
  return ret;
}

int ncplane_cursor_at(const ncplane* n, cell* c, char** gclust){
  if(n->y == n->leny && n->x == n->lenx){
    return -1;
  }
  const cell* src = &n->fb[nfbcellidx(n, n->y, n->x)];
  memcpy(c, src, sizeof(*src));
  *gclust = NULL;
  if(!cell_simple_p(src)){
    *gclust = strdup(extended_gcluster(n, src));
    if(*gclust == NULL){
      return -1;
    }
  }
  return 0;
}

int cell_load(ncplane* n, cell* c, const char* gcluster){
  int bytes;
  int cols;
  if((bytes = utf8_egc_len(gcluster, &cols)) >= 0 && bytes <= 1){
    cell_release(n, c);
    c->channels &= ~CELL_WIDEASIAN_MASK;
    c->gcluster = *gcluster;
    return !!c->gcluster;
  }
  if(cols > 1){
    c->channels |= CELL_WIDEASIAN_MASK;
  }else{
    c->channels &= ~CELL_WIDEASIAN_MASK;
  }
  if(!cell_simple_p(c)){
    if(strcmp(gcluster, cell_extended_gcluster(n, c)) == 0){
      return bytes; // reduce, reuse, recycle
    }else{
      cell_release(n, c);
    }
  }
  int eoffset = egcpool_stash(&n->pool, gcluster, bytes);
  if(eoffset < 0){
    return -1;
  }
  c->gcluster = eoffset + 0x80;
  return bytes;
}

unsigned notcurses_supported_styles(const notcurses* nc){
  unsigned styles = 0;
  styles |= nc->tcache.standout ? NCSTYLE_STANDOUT : 0;
  styles |= nc->tcache.uline ? NCSTYLE_UNDERLINE : 0;
  styles |= nc->tcache.reverse ? NCSTYLE_REVERSE : 0;
  styles |= nc->tcache.blink ? NCSTYLE_BLINK : 0;
  styles |= nc->tcache.dim ? NCSTYLE_DIM : 0;
  styles |= nc->tcache.bold ? NCSTYLE_BOLD : 0;
  styles |= nc->tcache.italics ? NCSTYLE_ITALIC : 0;
  return styles;
}

int notcurses_palette_size(const notcurses* nc){
  return nc->tcache.colors;
}

// turn on any specified stylebits
void ncplane_styles_on(ncplane* n, unsigned stylebits){
  n->attrword |= (stylebits & NCSTYLE_MASK);
}

// turn off any specified stylebits
void ncplane_styles_off(ncplane* n, unsigned stylebits){
  n->attrword &= ~(stylebits & NCSTYLE_MASK);
}

// set the current stylebits to exactly those provided
void ncplane_styles_set(ncplane* n, unsigned stylebits){
  n->attrword = (n->attrword & ~NCSTYLE_MASK) | ((stylebits & NCSTYLE_MASK));
}

unsigned ncplane_styles(const ncplane* n){
  return (n->attrword & NCSTYLE_MASK);
}

// i hate the big allocation and two copies here, but eh what you gonna do?
// well, for one, we don't need the huge allocation FIXME
static char*
ncplane_vprintf_prep(ncplane* n, const char* format, va_list ap){
  const size_t size = n->lenx + 1; // healthy estimate, can embiggen below
  char* buf = malloc(size);
  if(buf == NULL){
    return NULL;
  }
  va_list vacopy;
  va_copy(vacopy, ap);
  int ret = vsnprintf(buf, size, format, ap);
  if(ret < 0){
    free(buf);
    va_end(vacopy);
    return NULL;
  }
  if((size_t)ret >= size){
    char* tmp = realloc(buf, ret + 1);
    if(tmp == NULL){
      free(buf);
      va_end(vacopy);
      return NULL;
    }
    buf = tmp;
    vsprintf(buf, format, vacopy);
  }
  va_end(vacopy);
  return buf;
}

int ncplane_vprintf_yx(ncplane* n, int y, int x, const char* format, va_list ap){
  char* r = ncplane_vprintf_prep(n, format, ap);
  if(r == NULL){
    return -1;
  }
  int ret = ncplane_putstr_yx(n, y, x, r);
  free(r);
  return ret;
}

int ncplane_vprintf_aligned(ncplane* n, int y, ncalign_e align,
                            const char* format, va_list ap){
  char* r = ncplane_vprintf_prep(n, format, ap);
  if(r == NULL){
    return -1;
  }
  int ret = ncplane_putstr_aligned(n, y, align, r);
  free(r);
  return ret;
}

int ncplane_hline_interp(ncplane* n, const cell* c, int len,
                         uint64_t c1, uint64_t c2){
  unsigned ur, ug, ub;
  int r1, g1, b1, r2, g2, b2;
  int br1, bg1, bb1, br2, bg2, bb2;
  channels_fg_rgb(c1, &ur, &ug, &ub);
  r1 = ur; g1 = ug; b1 = ub;
  channels_fg_rgb(c2, &ur, &ug, &ub);
  r2 = ur; g2 = ug; b2 = ub;
  channels_bg_rgb(c1, &ur, &ug, &ub);
  br1 = ur; bg1 = ug; bb1 = ub;
  channels_bg_rgb(c2, &ur, &ug, &ub);
  br2 = ur; bg2 = ug; bb2 = ub;
  int deltr = r2 - r1;
  int deltg = g2 - g1;
  int deltb = b2 - b1;
  int deltbr = br2 - br1;
  int deltbg = bg2 - bg1;
  int deltbb = bb2 - bb1;
  int ret;
  cell dupc = CELL_TRIVIAL_INITIALIZER;
  if(cell_duplicate(n, &dupc, c) < 0){
    return -1;
  }
  bool fgdef = false, bgdef = false;
  if(channels_fg_default_p(c1) && channels_fg_default_p(c2)){
    fgdef = true;
  }
  if(channels_bg_default_p(c1) && channels_bg_default_p(c2)){
    bgdef = true;
  }
  for(ret = 0 ; ret < len ; ++ret){
    int r = (deltr * ret) / len + r1;
    int g = (deltg * ret) / len + g1;
    int b = (deltb * ret) / len + b1;
    int br = (deltbr * ret) / len + br1;
    int bg = (deltbg * ret) / len + bg1;
    int bb = (deltbb * ret) / len + bb1;
    if(!fgdef){
      cell_set_fg_rgb(&dupc, r, g, b);
    }
    if(!bgdef){
      cell_set_bg_rgb(&dupc, br, bg, bb);
    }
    if(ncplane_putc(n, &dupc) <= 0){
      break;
    }
  }
  cell_release(n, &dupc);
  return ret;
}

int ncplane_vline_interp(ncplane* n, const cell* c, int len,
                         uint64_t c1, uint64_t c2){
  unsigned ur, ug, ub;
  int r1, g1, b1, r2, g2, b2;
  int br1, bg1, bb1, br2, bg2, bb2;
  channels_fg_rgb(c1, &ur, &ug, &ub);
  r1 = ur; g1 = ug; b1 = ub;
  channels_fg_rgb(c2, &ur, &ug, &ub);
  r2 = ur; g2 = ug; b2 = ub;
  channels_bg_rgb(c1, &ur, &ug, &ub);
  br1 = ur; bg1 = ug; bb1 = ub;
  channels_bg_rgb(c2, &ur, &ug, &ub);
  br2 = ur; bg2 = ug; bb2 = ub;
  int deltr = (r2 - r1) / (len + 1);
  int deltg = (g2 - g1) / (len + 1);
  int deltb = (b2 - b1) / (len + 1);
  int deltbr = (br2 - br1) / (len + 1);
  int deltbg = (bg2 - bg1) / (len + 1);
  int deltbb = (bb2 - bb1) / (len + 1);
  int ret, ypos, xpos;
  ncplane_cursor_yx(n, &ypos, &xpos);
  cell dupc = CELL_TRIVIAL_INITIALIZER;
  if(cell_duplicate(n, &dupc, c) < 0){
    return -1;
  }
  bool fgdef = false, bgdef = false;
  if(channels_fg_default_p(c1) && channels_fg_default_p(c2)){
    fgdef = true;
  }
  if(channels_bg_default_p(c1) && channels_bg_default_p(c2)){
    bgdef = true;
  }
  for(ret = 0 ; ret < len ; ++ret){
    if(ncplane_cursor_move_yx(n, ypos + ret, xpos)){
      return -1;
    }
    r1 += deltr;
    g1 += deltg;
    b1 += deltb;
    br1 += deltbr;
    bg1 += deltbg;
    bb1 += deltbb;
    if(!fgdef){
      cell_set_fg_rgb(&dupc, r1, g1, b1);
    }
    if(!bgdef){
      cell_set_bg_rgb(&dupc, br1, bg1, bb1);
    }
    if(ncplane_putc(n, &dupc) <= 0){
      break;
    }
  }
  cell_release(n, &dupc);
  return ret;
}

// how many edges need touch a corner for it to be printed?
static inline unsigned
box_corner_needs(unsigned ctlword){
  return (ctlword & NCBOXCORNER_MASK) >> NCBOXCORNER_SHIFT;
}

int ncplane_box(ncplane* n, const cell* ul, const cell* ur,
                const cell* ll, const cell* lr, const cell* hl,
                const cell* vl, int ystop, int xstop,
                unsigned ctlword){
  int yoff, xoff, ymax, xmax;
  ncplane_cursor_yx(n, &yoff, &xoff);
  // must be at least 2x2, with its upper-left corner at the current cursor
  if(ystop < yoff + 1){
    return -1;
  }
  if(xstop < xoff + 1){
    return -1;
  }
  ncplane_dim_yx(n, &ymax, &xmax);
  // must be within the ncplane
  if(xstop >= xmax || ystop >= ymax){
    return -1;
  }
  unsigned edges;
  edges = !(ctlword & NCBOXMASK_TOP) + !(ctlword & NCBOXMASK_LEFT);
  if(edges >= box_corner_needs(ctlword)){
    if(ncplane_putc(n, ul) < 0){
      return -1;
    }
  }
  if(!(ctlword & NCBOXMASK_TOP)){ // draw top border, if called for
    if(xstop - xoff >= 2){
      if(ncplane_cursor_move_yx(n, yoff, xoff + 1)){
        return -1;
      }
      if(!(ctlword & NCBOXGRAD_TOP)){ // cell styling, hl
        if(ncplane_hline(n, hl, xstop - xoff - 1) < 0){
          return -1;
        }
      }else{ // gradient, ul -> ur
        if(ncplane_hline_interp(n, hl, xstop - xoff - 1, ul->channels, ur->channels) < 0){
          return -1;
        }
      }
    }
  }
  edges = !(ctlword & NCBOXMASK_TOP) + !(ctlword & NCBOXMASK_RIGHT);
  if(edges >= box_corner_needs(ctlword)){
    if(ncplane_cursor_move_yx(n, yoff, xstop)){
      return -1;
    }
    if(ncplane_putc(n, ur) < 0){
      return -1;
    }
  }
  ++yoff;
  // middle rows (vertical lines)
  if(yoff < ystop){
    if(!(ctlword & NCBOXMASK_LEFT)){
      if(ncplane_cursor_move_yx(n, yoff, xoff)){
        return -1;
      }
      if((ctlword & NCBOXGRAD_LEFT)){ // grad styling, ul->ll
        if(ncplane_vline_interp(n, vl, ystop - yoff, ul->channels, ll->channels) < 0){
          return -1;
        }
      }else{
        if(ncplane_vline(n, vl, ystop - yoff) < 0){
          return -1;
        }
      }
    }
    if(!(ctlword & NCBOXMASK_RIGHT)){
      if(ncplane_cursor_move_yx(n, yoff, xstop)){
        return -1;
      }
      if((ctlword & NCBOXGRAD_RIGHT)){ // grad styling, ur->lr
        if(ncplane_vline_interp(n, vl, ystop - yoff, ur->channels, lr->channels) < 0){
          return -1;
        }
      }else{
        if(ncplane_vline(n, vl, ystop - yoff) < 0){
          return -1;
        }
      }
    }
  }
  // bottom line
  yoff = ystop;
  edges = !(ctlword & NCBOXMASK_BOTTOM) + !(ctlword & NCBOXMASK_LEFT);
  if(edges >= box_corner_needs(ctlword)){
    if(ncplane_cursor_move_yx(n, yoff, xoff)){
      return -1;
    }
    if(ncplane_putc(n, ll) < 0){
      return -1;
    }
  }
  if(!(ctlword & NCBOXMASK_BOTTOM)){
    if(xstop - xoff >= 2){
      if(ncplane_cursor_move_yx(n, yoff, xoff + 1)){
        return -1;
      }
      if(!(ctlword & NCBOXGRAD_BOTTOM)){ // cell styling, hl
        if(ncplane_hline(n, hl, xstop - xoff - 1) < 0){
          return -1;
        }
      }else{
        if(ncplane_hline_interp(n, hl, xstop - xoff - 1, ll->channels, lr->channels) < 0){
          return -1;
        }
      }
    }
  }
  edges = !(ctlword & NCBOXMASK_BOTTOM) + !(ctlword & NCBOXMASK_RIGHT);
  if(edges >= box_corner_needs(ctlword)){
    if(ncplane_cursor_move_yx(n, yoff, xstop)){
      return -1;
    }
    if(ncplane_putc(n, lr) < 0){
      return -1;
    }
  }
  return 0;
}

// takes the head of a list of bound planes. performs a DFS on all planes bound
// to 'n', and all planes down-list from 'n', moving all *by* 'dy' and 'dx'.
static void
move_bound_planes(ncplane* n, int dy, int dx){
  while(n){
    n->absy += dy;
    n->absx += dx;
    move_bound_planes(n->blist, dy, dx);
    n = n->bnext;
  }
}

int ncplane_move_yx(ncplane* n, int y, int x){
  if(n == n->nc->stdscr){
    return -1;
  }
  int dy, dx; // amount moved
  if(n->bound){
    dy = (n->bound->absy + y) - n->absy;
    dx = (n->bound->absx + x) - n->absx;
  }else{
    dy = (n->nc->stdscr->absy + y) - n->absy;
    dx = (n->nc->stdscr->absx + x) - n->absx;
  }
  n->absx += dx;
  n->absy += dy;
  move_bound_planes(n->blist, dy, dx);
  return 0;
}

void ncplane_yx(const ncplane* n, int* y, int* x){
  if(y){
    *y = n->absy - n->nc->stdscr->absy;
  }
  if(x){
    *x = n->absx - n->nc->stdscr->absx;
  }
}

void ncplane_erase(ncplane* n){
  // we must preserve the background, but a pure cell_duplicate() would be
  // wiped out by the egcpool_dump(). do a duplication (to get the attrword
  // and channels), and then reload.
  char* egc = cell_egc_copy(n, &n->basecell);
  memset(n->fb, 0, sizeof(*n->fb) * n->lenx * n->leny);
  egcpool_dump(&n->pool);
  egcpool_init(&n->pool);
  // we need to zero out the EGC before handing this off to cell_load, but
  // we don't want to lose the channels/attributes, so explicit gcluster load.
  n->basecell.gcluster = 0;
  cell_load(n, &n->basecell, egc);
  free(egc);
  n->y = n->x = 0;
}

void notcurses_cursor_enable(notcurses* nc){
  if(nc->tcache.cnorm){
    term_emit("cnorm", nc->tcache.cnorm, nc->ttyfp, false);
  }
}

void notcurses_cursor_disable(notcurses* nc){
  if(nc->tcache.civis){
    term_emit("civis", nc->tcache.civis, nc->ttyfp, false);
  }
}

ncplane* notcurses_top(notcurses* n){
  return n->top;
}

ncplane* ncplane_below(ncplane* n){
  return n->below;
}

// FIXME this clears the screen for some reason! what's up?
#define SET_BTN_EVENT_MOUSE   "1002"
#define SET_FOCUS_EVENT_MOUSE "1004"
#define SET_SGR_MODE_MOUSE    "1006"
int notcurses_mouse_enable(notcurses* n){
  return term_emit("mouse", ESC "[?" SET_BTN_EVENT_MOUSE ";"
                   /*SET_FOCUS_EVENT_MOUSE ";" */SET_SGR_MODE_MOUSE "h",
                   n->ttyfp, true);
}

// this seems to work (note difference in suffix, 'l' vs 'h'), but what about
// the sequences 1000 etc?
int notcurses_mouse_disable(notcurses* n){
  return term_emit("mouse", ESC "[?" SET_BTN_EVENT_MOUSE ";"
                   /*SET_FOCUS_EVENT_MOUSE ";" */SET_SGR_MODE_MOUSE "l",
                   n->ttyfp, true);
}

bool notcurses_canutf8(const notcurses* nc){
  return nc->utf8;
}

bool notcurses_canfade(const notcurses* nc){
  return nc->tcache.CCCflag || nc->tcache.RGBflag;
}

bool notcurses_canchangecolor(const notcurses* nc){
  if(!nc->tcache.CCCflag){
    return false;
  }
  palette256* p;
  if((unsigned)nc->tcache.colors < sizeof(p->chans) / sizeof(*p->chans)){
    return false;
  }
  return true;
}

palette256* palette256_new(notcurses* nc){
  palette256* p = malloc(sizeof(*p));
  if(p){
    memcpy(p, &nc->palette, sizeof(*p));
  }
  return p;
}

int palette256_use(notcurses* nc, const palette256* p){
  int ret = -1;
  if(!notcurses_canchangecolor(nc)){
    return -1;
  }
  for(size_t z = 0 ; z < sizeof(p->chans) / sizeof(*p->chans) ; ++z){
    if(nc->palette.chans[z] != p->chans[z]){
      nc->palette.chans[z] = p->chans[z];
      nc->palette_damage[z] = true;
    }
  }
  ret = 0;
  return ret;
}

void palette256_free(palette256* p){
  free(p);
}

bool ncplane_translate_abs(const ncplane* n, int* restrict y, int* restrict x){
  ncplane_translate(ncplane_stdplane_const(n), n, y, x);
  if(y){
    if(*y < 0){
      return false;
    }
    if(*y >= n->leny){
      return false;
    }
  }
  if(x){
    if(*x < 0){
      return false;
    }
    if(*x >= n->lenx){
      return false;
    }
  }
  return true;
}

void ncplane_translate(const ncplane* src, const ncplane* dst,
                       int* restrict y, int* restrict x){
  if(dst == NULL){
    dst = ncplane_stdplane_const(src);
  }
  if(y){
    *y = src->absy - dst->absy + *y;
  }
  if(x){
    *x = src->absx - dst->absx + *x;
  }
}

notcurses* ncplane_notcurses(ncplane* n){
  return n->nc;
}

ncplane* ncplane_reparent(ncplane* n, ncplane* newparent){
  if(n == n->nc->stdscr){
    return NULL; // can't reparent standard plane
  }
  if(n->bound){ // detach it, and extract it from list
    for(ncplane** prev = &n->bound->blist ; *prev ; prev = &(*prev)->bnext){
      if(*prev == n){
        *prev = n->bnext;
        break;
      }
    }
    n->bnext = NULL;
  }
  if( (n->bound = newparent) ){
    n->bnext = newparent->blist;
    newparent->blist = n;
  }
  return n;
}

bool ncplane_set_scrolling(ncplane* n, bool scrollp){
  bool old = n->scrolling;
  n->scrolling = scrollp;
  return old;
}

// extract an integer, which must be non-negative, and followed by either a
// comma or a NUL terminator.
static int
lex_long(const char* op, int* i, char** endptr){
  errno = 0;
  long l = strtol(op, endptr, 10);
  if(l < 0 || (l == LONG_MAX && errno == ERANGE) || (l > INT_MAX)){
    fprintf(stderr, "Invalid margin: %s\n", op);
    return -1;
  }
  if((**endptr != ',' && **endptr) || *endptr == op){
    fprintf(stderr, "Invalid margin: %s\n", op);
    return -1;
  }
  *i = l;
  return 0;
}

int notcurses_lex_margins(const char* op, notcurses_options* opts){
  char* eptr;
  if(lex_long(op, &opts->margin_t, &eptr)){
    return -1;
  }
  if(!*eptr){ // allow a single value to be specified for all four margins
    opts->margin_r = opts->margin_l = opts->margin_b = opts->margin_t;
    return 0;
  }
  op = ++eptr; // once here, we require four values
  if(lex_long(op, &opts->margin_r, &eptr) || !*eptr){
    return -1;
  }
  op = ++eptr;
  if(lex_long(op, &opts->margin_b, &eptr) || !*eptr){
    return -1;
  }
  op = ++eptr;
  if(lex_long(op, &opts->margin_l, &eptr) || *eptr){ // must end in NUL
    return -1;
  }
  return 0;
}

int notcurses_inputready_fd(notcurses* n){
  return fileno(n->ttyinfp);
}

uint32_t* ncplane_rgba(const ncplane* nc, int begy, int begx, int leny, int lenx){
  if(begy < 0 || begx < 0){
    return NULL;
  }
  if(begx >= nc->lenx || begy >= nc->leny){
    return NULL;
  }
  if(lenx == -1){ // -1 means "to the end"; use all space available
    lenx = nc->lenx - begx;
  }
  if(leny == -1){
    leny = nc->leny - begy;
  }
  if(lenx < 0 || leny < 0){ // no need to draw zero-size object, exit
    return NULL;
  }
  if(begx + lenx > nc->lenx || begy + leny > nc->leny){
    return NULL;
  }
  uint32_t* ret = malloc(sizeof(*ret) * lenx * leny * 2);
  if(ret){
    for(int y = begy, targy = 0 ; y < begy + leny ; ++y, targy += 2){
      for(int x = begx, targx = 0 ; x < begx + lenx ; ++x, ++targx){
        // FIXME what if there's a wide glyph to the left of the selection?
        uint32_t attrword;
        uint64_t channels;
        char* c = ncplane_at_yx(nc, y, x, &attrword, &channels);
        if(c == NULL){
          free(ret);
          return NULL;
        }
        uint32_t* top = &ret[targy * lenx + targx];
        uint32_t* bot = &ret[(targy + 1) * lenx + targx];
        unsigned fr, fg, fb, br, bg, bb;
        channels_fg_rgb(channels, &fr, &fb, &fg);
        channels_bg_rgb(channels, &br, &bb, &bg);
        // FIXME how do we deal with transparency?
        uint32_t frgba = (fr) + (fg << 8u) + (fb << 16u) + 0xff000000;
        uint32_t brgba = (br) + (bg << 8u) + (bb << 16u) + 0xff000000;
        if((strcmp(c, " ") == 0) || (strcmp(c, "") == 0)){
          *top = *bot = brgba;
        }else if(strcmp(c, "▄") == 0){
          *top = frgba;
          *bot = brgba;
        }else if(strcmp(c, "▀") == 0){
          *top = brgba;
          *bot = frgba;
        }else if(strcmp(c, "█") == 0){
          *top = *bot = frgba;
        }else{
          free(c);
          free(ret);
          return NULL;
        }
        free(c);
      }
    }
  }
  return ret;
}

char* ncplane_contents(const ncplane* nc, int begy, int begx, int leny, int lenx){
  if(begy < 0 || begx < 0){
    return NULL;
  }
  if(begx >= nc->lenx || begy >= nc->leny){
    return NULL;
  }
  if(lenx == -1){ // -1 means "to the end"; use all space available
    lenx = nc->lenx - begx;
  }
  if(leny == -1){
    leny = nc->leny - begy;
  }
  if(lenx < 0 || leny < 0){ // no need to draw zero-size object, exit
    return NULL;
  }
  if(begx + lenx > nc->lenx || begy + leny > nc->leny){
    return NULL;
  }
  size_t retlen = 1;
  char* ret = malloc(retlen);
  if(ret){
    for(int y = begy, targy = 0 ; y < begy + leny ; ++y, targy += 2){
      for(int x = begx, targx = 0 ; x < begx + lenx ; ++x, ++targx){
        uint32_t attrword;
        uint64_t channels;
        char* c = ncplane_at_yx(nc, y, x, &attrword, &channels);
        if(!c){
          free(ret);
          return NULL;
        }
        size_t clen = strlen(c);
        if(clen){
          char* tmp = realloc(ret, retlen + clen);
          if(!tmp){
            free(c);
            free(ret);
            return NULL;
          }
          ret = tmp;
          memcpy(ret + retlen - 1, c, clen);
          retlen += clen;
        }
        free(c);
      }
    }
    ret[retlen - 1] = '\0';
  }
  return ret;
}

int cells_ascii_box(struct ncplane* n, uint32_t attr, uint64_t channels,
                    cell* ul, cell* ur, cell* ll, cell* lr, cell* hl, cell* vl){
  return cells_load_box(n, attr, channels, ul, ur, ll, lr, hl, vl, "/\\\\/-|");
}

int cells_double_box(struct ncplane* n, uint32_t attr, uint64_t channels,
                     cell* ul, cell* ur, cell* ll, cell* lr, cell* hl, cell* vl){
  if(notcurses_canutf8(n->nc)){
    return cells_load_box(n, attr, channels, ul, ur, ll, lr, hl, vl, "╔╗╚╝═║");
  }
  return cells_ascii_box(n, attr, channels, ul, ur, ll, lr, hl, vl);
}

int cells_rounded_box(struct ncplane* n, uint32_t attr, uint64_t channels,
                      cell* ul, cell* ur, cell* ll, cell* lr, cell* hl, cell* vl){
  if(notcurses_canutf8(n->nc)){
    return cells_load_box(n, attr, channels, ul, ur, ll, lr, hl, vl, "╭╮╰╯─│");
  }
  return cells_ascii_box(n, attr, channels, ul, ur, ll, lr, hl, vl);
}

// find the center coordinate of a plane, preferring the top/left in the
// case of an even number of rows/columns (in such a case, there will be one
// more cell to the bottom/right of the center than the top/left). the
// center is then modified relative to the plane's origin.
void ncplane_center_abs(const ncplane* n, int* RESTRICT y, int* RESTRICT x){
  ncplane_center(n, y, x);
  if(y){
    *y += n->absy;
  }
  if(x){
    *x += n->absx;
  }
}
