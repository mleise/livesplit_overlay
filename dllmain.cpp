#include "pch.h"

const char* const SPINNER_CHARS = "|\\-/";
const char* const INI_SECTION = "LIVESPLIT";
const char* const INI_SHOW = "Show";
bool g_show_overlay = true;
bool g_osd_registered = false;
device* g_device = nullptr;
resource_desc g_texture_descriptor = resource_desc(0, 0, 1, 1, format::b8g8r8a8_unorm, 1, memory_heap::cpu_to_gpu, resource_usage::shader_resource, resource_flags::dynamic);
const resource_view_desc g_texture_view_descriptor = resource_view_desc(format::b8g8r8a8_unorm);
resource g_texture = {};
resource_view g_texture_view = {};
ULONGLONG g_last_window_search_tick = 0;
HWND g_livesplit_window_handle = NULL;
BITMAPINFOHEADER g_bitmap_info_header = { sizeof(BITMAPINFOHEADER), 0, 0, 1, 32, BI_RGB };
uint32_t g_widest_texture = 0;

static void release_texture()
{
    if (g_device != nullptr)
    {
        if (g_texture_view.handle != 0)
        {
            g_device->destroy_resource_view(g_texture_view);
            g_texture_view = {};
        }
        if (g_texture.handle != 0)
        {
            g_device->destroy_resource(g_texture);
            g_texture = {};
        }
    }
}

static void draw_osd_livesplit(effect_runtime* runtime)
{
    if (runtime->get_device() != g_device)
        return;

    // Try to find the LiveSplit window
    if (g_livesplit_window_handle == NULL)
    {
        ULONGLONG now = GetTickCount64();
        if (now - g_last_window_search_tick >= 1000)
        {
            g_livesplit_window_handle = FindWindow(NULL, L"LiveSplit");
            g_last_window_search_tick = now;
        }
    }

    if (g_livesplit_window_handle != NULL)
    {
        // Get device context handle from the LiveSplit window, to get at its buffered image.
        HDC device_context_handle = GetWindowDC(g_livesplit_window_handle);
        if (device_context_handle == NULL)
        {
            // The LiveSplit window probably closed, let's search for it again next time.
            g_livesplit_window_handle = NULL;
        }
        else
        {
            // Acquire the bitmap that contains the rendering of LiveSplit.
            HBITMAP bitmap_handle = (HBITMAP)GetCurrentObject(device_context_handle, OBJ_BITMAP);

            // Query size of the window via its buffer bitmap.
            BITMAPCOREHEADER bitmap_core_header;
            bitmap_core_header.bcSize = sizeof(BITMAPCOREHEADER);
            bitmap_core_header.bcBitCount = 0;
            if (GetDIBits(device_context_handle, bitmap_handle, 0, 0, NULL, (BITMAPINFO*)&bitmap_core_header, DIB_RGB_COLORS))
            {
                // Destroy the texture if it doesn't match the window size any more.
                if (g_texture.handle != 0 && bitmap_core_header.bcWidth != g_texture_descriptor.texture.width || bitmap_core_header.bcHeight != g_texture_descriptor.texture.height)
                    release_texture();

                // Create the texture if it doesn't exist.
                if (g_texture.handle == 0)
                {
                    g_texture_descriptor.texture.width = bitmap_core_header.bcWidth;
                    g_texture_descriptor.texture.height = bitmap_core_header.bcHeight;
                    g_bitmap_info_header.biHeight = -bitmap_core_header.bcHeight;
                    g_widest_texture = bitmap_core_header.bcWidth > g_widest_texture ? bitmap_core_header.bcWidth : g_widest_texture;
                    if (g_device->create_resource(g_texture_descriptor, nullptr, resource_usage::shader_resource, &g_texture))
                    {
                        if (!g_device->create_resource_view(g_texture, resource_usage::shader_resource, g_texture_view_descriptor, &g_texture_view))
                            release_texture();
                    }
                }

                // Copy the bitmap into the texture and display it.
                if (g_texture.handle != 0)
                {
                    subresource_data buffer_info;
                    if (g_device->map_texture_region(g_texture, 0, nullptr, map_access::write_discard, &buffer_info))
                    {
                        g_bitmap_info_header.biWidth = buffer_info.row_pitch / 4;
                        GetDIBits(device_context_handle, bitmap_handle, 0, bitmap_core_header.bcHeight, buffer_info.data, (BITMAPINFO*)&g_bitmap_info_header, DIB_RGB_COLORS);
                        g_device->unmap_texture_region(g_texture, 0);
                        ImGui::Image(g_texture_view.handle, ImVec2(bitmap_core_header.bcWidth, bitmap_core_header.bcHeight), ImVec2(0, 0), ImVec2(1, 1), ImColor(1.0f, 1.0f, 1.0f), ImColor());
                        //ImGui::Text("Widest texture: %i", g_widest_texture);
                    }
                }
            }

            // Release the device context from LiveSplit so it can continue rendering.
            ReleaseDC(g_livesplit_window_handle, device_context_handle);
            return;
        }
    }

    // If we don't have a LiveSplit window, show a spinner to remind the user that we are actively burning CPU cycles waiting for the window.
    release_texture();
    ImGui::Text("Waiting for LiveSplit %c", SPINNER_CHARS[GetTickCount64() / 200 % 4]);
}

static void update_livesplit_display(bool show)
{
    if (show)
    {
        if (!g_osd_registered)
            reshade::register_overlay("OSD", &draw_osd_livesplit);
    }
    else
    {
        if (g_osd_registered)
            reshade::unregister_overlay("OSD", &draw_osd_livesplit);
        release_texture();
    }
    g_osd_registered = show;
}

static void draw_settings_overlay(effect_runtime*)
{
    if (ImGui::Checkbox("Show LiveSplit", &g_show_overlay))
    {
        reshade::config_set_value(nullptr, INI_SECTION, INI_SHOW, g_show_overlay);
        update_livesplit_display(g_show_overlay);
    }
}

static void on_init_device(device* device)
{
    // Only react to the first device context.
    if (g_device != nullptr)
        return;
    g_device = device;
    reshade::config_get_value(nullptr, INI_SECTION, INI_SHOW, g_show_overlay);
    if (g_show_overlay)
        update_livesplit_display(true);
}

static void on_destroy_device(device* device)
{
    // Only react to the device we initially picked up.
    if (g_device != device)
        return;
    update_livesplit_display(false);
    g_device = nullptr;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule))
            return FALSE;
        reshade::register_event<reshade::addon_event::init_device>(&on_init_device);
        reshade::register_event<reshade::addon_event::destroy_device>(&on_destroy_device);
        reshade::register_overlay(nullptr, &draw_settings_overlay);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_overlay(nullptr, &draw_settings_overlay);
        reshade::unregister_event<reshade::addon_event::destroy_device>(&on_destroy_device);
        reshade::unregister_event<reshade::addon_event::init_device>(&on_init_device);
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}