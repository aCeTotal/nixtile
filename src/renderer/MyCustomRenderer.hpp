// MyCustomRenderer.hpp
#pragma once
#include "Renderer.hpp"

class MyCustomRenderer : public Renderer {
public:
    void initialize() override;
    void renderFrame() override;
};
