#ifndef PCH_H
#define PCH_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define ImTextureID uint64_t
#include <imgui.h>
#include <reshade.hpp>
#include <psapi.h>

using namespace reshade::api;

#endif //PCH_H
