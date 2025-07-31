/* Edge detection for resize operations */
#ifndef RESIZE_EDGE_H
#define RESIZE_EDGE_H

/* Border sensitivity for edge detection in pixels */
#define EDGE_THRESHOLD 10

/* Resize edge types */
enum {
    EDGE_NONE = 0,
    EDGE_LEFT = 1,
    EDGE_RIGHT = 2,
    EDGE_TOP = 4,
    EDGE_BOTTOM = 8
};

#endif /* RESIZE_EDGE_H */
