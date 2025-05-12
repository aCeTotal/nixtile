#pragma once
#include <memory>
#include <string>
#include "Renderer.hpp"

class RendererFactory {
public:
    static std::unique_ptr<Renderer> createRenderer(const std::string& name);
};
