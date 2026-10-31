// Embedded GUI wallet files (generated into the build directory by cmake/embed.cmake from src/gui/)
#pragma once
#include <cstddef>

namespace plk {

struct GuiAsset {
    const char* route;   // "/", "/app.js", "/app.css"
    const char* type;    // Content-Type
    const unsigned char* data;
    size_t size;
};
extern const GuiAsset GUI_ASSETS[];
extern const size_t GUI_ASSET_COUNT;

}  // namespace plk
