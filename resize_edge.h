/* Edge detection for resize operations */
#ifndef RESIZE_EDGE_H
#define RESIZE_EDGE_H

/* Border sensitivity for edge detection in pixels */
#define EDGE_THRESHOLD 10

/* Minimum window size to prevent crashes */
#define MIN_WINDOW_WIDTH 50
#define MIN_WINDOW_HEIGHT 50

/* Safety margin for resize operations */
#define RESIZE_SAFETY_MARGIN 2

/* Resize edge types */
enum {
    EDGE_NONE = 0,
    EDGE_LEFT = 1,
    EDGE_RIGHT = 2,
    EDGE_TOP = 4,
    EDGE_BOTTOM = 8
};

/* Define mid-point of the window for extended resize zones */
#define IS_LEFT_HALF(x, c) ((x) >= (c)->geom.x && (x) <= (c)->geom.x + (c)->geom.width / 2)
#define IS_RIGHT_HALF(x, c) ((x) > (c)->geom.x + (c)->geom.width / 2 && (x) <= (c)->geom.x + (c)->geom.width)

#endif /* RESIZE_EDGE_H */
