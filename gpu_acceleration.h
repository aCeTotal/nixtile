/*
 * GPU Acceleration Configuration for Nixtile Window Manager
 * Ultra-smooth tile movements with hardware acceleration
 */

#ifndef GPU_ACCELERATION_H
#define GPU_ACCELERATION_H

#include <stdbool.h>
#include <stddef.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>

/* GPU Acceleration Settings */
#define GPU_ACCELERATION_ENABLED 1
#define ADAPTIVE_SYNC_PREFERRED 1
#define HARDWARE_CURSOR_PREFERRED 1
#define TEXTURE_CACHING_ENABLED 1
#define VSYNC_ENABLED 1

/* Performance Tuning */
#define HIGH_REFRESH_RATE_THRESHOLD 120  /* Hz */
#define SMOOTH_ANIMATION_FPS 60
#define GPU_BUFFER_OPTIMIZATION 1

/* GPU Feature Detection */
typedef struct {
    bool hardware_acceleration;
    bool adaptive_sync;
    bool hardware_cursors;
    bool texture_compression;
    bool timeline_sync;
    int max_refresh_rate;
    const char* renderer_name;
} gpu_capabilities_t;

/* Function Prototypes */
void init_gpu_acceleration(void);
void configure_gpu_environment(void);
/* Note: GPU capability functions are implemented as static functions in nixtile.c */

#endif /* GPU_ACCELERATION_H */
