#include "internal.h"

void notcurses_debug(notcurses* nc, FILE* debugfp){
  const ncplane* n = nc->top;
  const ncplane* prev = NULL;
  int planeidx = 0;
  fprintf(debugfp, "*************************** notcurses debug state *****************************\n");
  while(n){
    fprintf(debugfp, "%04d off y: %3d x: %3d geom y: %3d x: %3d curs y: %3d x: %3d %s %p\n",
            planeidx, n->absy, n->absx, n->leny, n->lenx, n->y, n->x,
            n == notcurses_stdplane_const(nc) ? "std" : "   ", n);
    if(n->above != prev){
      fprintf(stderr, " WARNING: expected ->above %p, got %p\n", prev, n->above);
    }
    prev = n;
    n = n->below;
    ++planeidx;
  }
  if(nc->bottom != prev){
    fprintf(stderr, " WARNING: expected ->bottom %p, got %p\n", prev, nc->bottom);
  }
  fprintf(debugfp, "*******************************************************************************\n");
}
