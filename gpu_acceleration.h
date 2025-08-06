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

/* Environment Variables for GPU Acceleration */
static const char* gpu_acceleration_env_vars[][2] = {
    {"WLR_RENDERER", "gles2"},                    /* Prefer hardware-accelerated GLES2 */
    {"WLR_DRM_NO_ATOMIC", "0"},                   /* Enable atomic modesetting */
    {"WLR_DRM_NO_MODIFIERS", "0"},                /* Enable DRM modifiers */
    {"WLR_SCENE_DISABLE_VISIBILITY", "0"},        /* Enable scene visibility optimizations */
    {"WLR_NO_HARDWARE_CURSORS", "0"},             /* Enable hardware cursors */
    {"MESA_GL_VERSION_OVERRIDE", "3.3"},          /* Force OpenGL 3.3 for better performance */
    {"MESA_GLSL_VERSION_OVERRIDE", "330"},        /* Force GLSL 3.30 */
    {"__GL_SYNC_TO_VBLANK", "1"},                 /* Enable VSync for NVIDIA */
    {"vblank_mode", "1"},                         /* Enable VSync for AMD/Intel */
    {NULL, NULL}                                  /* Terminator */
};

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
