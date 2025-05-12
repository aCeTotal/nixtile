#include "RendererFactory.hpp"
#include "VulkanRenderer.hpp"
#include "MyCustomRenderer.hpp"

#include <stdexcept>

std::unique_ptr<Renderer> RendererFactory::createRenderer(const std::string& name) {
    if (name == "vulkan") {
        return std::make_unique<VulkanRenderer>();
    } else if (name == "custom") {
        return std::make_unique<MyCustomRenderer>();
    } else {
        throw std::invalid_argument("Unknown renderer: " + name);
    }
}
