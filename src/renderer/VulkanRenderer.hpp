#pragma once
#include "Renderer.hpp"

class VulkanRenderer : public Renderer {
public:
    void initialize() override;
    void renderFrame() override;

private:
    void initVulkan(); // intern funksjon
};
