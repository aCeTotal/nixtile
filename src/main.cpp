#include </nix/store/.../include/wlroots-0.18/wlr/render/wlr_renderer.h>
#include </nix/store/.../include/wlroots-0.18/wlr/types/wlr_output.h>
#include </nix/store/.../include/wlroots-0.18/wlr/util/log.h>
#include </nix/store/.../include/wlroots-0.18/wlr/types/wlr_compositor.h>
#include </nix/store/.../include/wlroots-0.18/wlr/types/wlr_xdg_shell.h>
#include </nix/store/.../include/wlroots-0.18/wlr/wayland-server.h>

#include "renderer/RendererFactory.hpp"
#include <memory>
#include <iostream>

static void on_new_output(struct wl_listener *listener, void *data) {
    struct wlr_output *output = static_cast<wlr_output *>(data);
    std::cout << "New output: " << output->name << std::endl;

    if (!wl_list_empty(&output->modes)) {
        auto *mode = wl_container_of(output->modes.next, mode, link);
        wlr_output_set_mode(output, mode);
    }

    wlr_output_commit(output);
}

int main(int argc, char** argv) {
    wlr_log_init(WLR_DEBUG, nullptr);

    struct wl_display *display = wl_display_create();
    if (!display) {
        std::cerr << "Failed to create Wayland display.\n";
        return EXIT_FAILURE;
    }

    struct wlr_backend *backend = wlr_backend_autocreate(display, nullptr);
    if (!backend) {
        std::cerr << "Failed to create backend.\n";
        return EXIT_FAILURE;
    }

    struct wlr_renderer *wlrRenderer = wlr_backend_get_renderer(backend);
    wlr_renderer_init_wl_display(wlrRenderer, display);

    struct wlr_compositor *compositor = wlr_compositor_create(display, wlrRenderer);
    struct wlr_xdg_shell *xdg_shell = wlr_xdg_shell_create(display);

    // Bruk vår C++-renderer
    std::string rendererType = "vulkan"; // evt les fra argv
    std::unique_ptr<Renderer> renderer;
    try {
        renderer = RendererFactory::createRenderer(rendererType);
        renderer->initialize();
    } catch (const std::exception& e) {
        std::cerr << "Renderer init failed: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    struct wl_listener new_output_listener;
    new_output_listener.notify = on_new_output;
    wl_signal_add(&backend->events.new_output, &new_output_listener);

    if (!wlr_backend_start(backend)) {
        std::cerr << "Failed to start backend.\n";
        wlr_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    std::cout << "Nixtile compositor running.\n";

    while (true) {
        // TODO: input + window mgmt
        renderer->renderFrame();
        wl_display_flush_clients(display);
        usleep(16000); // ~60 fps
    }

    wl_display_destroy(display);
    return EXIT_SUCCESS;
}

