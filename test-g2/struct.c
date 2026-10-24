#include "test.h"

struct Point { int x, y; };
struct Rect { struct Point min, max; int tag[2]; };

struct Point origin;
struct Rect r = {{1, 2}, {3, 4}, {5, 6}};

int width(struct Rect *p) { return p->max.x - p->min.x; }
void move(struct Point *p, int dx, int dy) { p->x += dx; p->y += dy; }

void start() {
  ASSERT(0, origin.x);
  ASSERT(2, r.min.y);
  ASSERT(4, r.max.y);
  ASSERT(6, r.tag[1]);
  ASSERT(2, width(&r));
  move(&r.max, 10, 20);
  ASSERT(13, r.max.x);
  ASSERT(24, r.max.y);
  ASSERT(12, width(&r));

  struct Point p = {7, 8};
  move(&p, -7, 1);
  ASSERT(0, p.x);
  ASSERT(9, p.y);
  struct Point *pp = &p;
  pp->y = 3;
  ASSERT(3, p.y);
  ASSERT(2, sizeof(struct Point));
  ASSERT(6, sizeof(struct Rect));

  struct Point pts[3] = {{1, 1}, {2, 2}, {3, 3}};
  int i = 2;
  ASSERT(3, pts[i].y);
  pts[i].x++;
  ASSERT(4, pts[2].x);
}
