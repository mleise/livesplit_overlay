#ifndef PCH_H
#define PCH_H

#define WIN32_LEAN_AND_MEAN
#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define ImTextureID ImU64

#include <imgui.h>
#include <reshade.hpp>
#include <psapi.h>
#include <string>
#include <vector>
#include "version.h"

using namespace reshade::api;

#endif //PCH_H
