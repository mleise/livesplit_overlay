#include "pch.h"

const char* const SPINNER_CHARS = "|\\-/";
const char* const INI_SECTION = "LIVESPLIT";
const char* const INI_SHOW = "Show";
const resource_view_desc TEXTURE_VIEW_DESCRIPTOR = resource_view_desc(format::b8g8r8a8_unorm);

bool g_show_overlay = true;
bool g_osd_registered = false;

device* g_device = nullptr;
resource_desc g_texture_descriptor = resource_desc(0, 0, 1, 1, format::b8g8r8a8_unorm, 1, memory_heap::cpu_to_gpu, resource_usage::shader_resource, resource_flags::dynamic);
resource g_texture = {};
resource_view g_texture_view = {};
uint32_t g_texture_row_pitch = 0;

HANDLE g_thread;
HANDLE g_event;
HANDLE g_mutex;
bool g_terminate_thread = false;
ULONGLONG g_last_window_search_tick = 0;
HWND g_livesplit_window_handle = NULL;
bool g_livesplit_window_is_minimized = false;
BITMAPCOREHEADER g_bitmap_core_header = { sizeof(BITMAPCOREHEADER) };
BITMAPINFOHEADER g_bitmap_info_header = { sizeof(BITMAPINFOHEADER), 0, 0, 1, 32, BI_RGB };
void* g_live_split_copy = NULL;
bool g_live_split_copy_ready = false;

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
        if (g_live_split_copy != NULL)
        {
            free(g_live_split_copy);
            g_live_split_copy = NULL;
        }
    }
}

static void draw_osd_livesplit(effect_runtime* runtime)
{
    if (runtime->get_device() != g_device)
        return;

    WaitForSingleObject(g_mutex, INFINITE);

    if (g_livesplit_window_handle != NULL)
    {
        // Destroy the texture if it doesn't match the window size any more.
        if (g_texture.handle != 0 && (g_bitmap_core_header.bcWidth != g_texture_descriptor.texture.width || g_bitmap_core_header.bcHeight != g_texture_descriptor.texture.height))
            release_texture();

        // Create the texture if it doesn't exist.
        if (g_texture.handle == 0)
        {
            g_texture_descriptor.texture.width = g_bitmap_core_header.bcWidth;
            g_texture_descriptor.texture.height = g_bitmap_core_header.bcHeight;
            if (g_device->create_resource(g_texture_descriptor, nullptr, resource_usage::shader_resource, &g_texture))
            {
                if (!g_device->create_resource_view(g_texture, resource_usage::shader_resource, TEXTURE_VIEW_DESCRIPTOR, &g_texture_view))
                    release_texture();

                subresource_data buffer_info;
                if (g_device->map_texture_region(g_texture, 0, nullptr, map_access::write_discard, &buffer_info))
                {
                    g_device->unmap_texture_region(g_texture, 0);
                    g_texture_row_pitch = buffer_info.row_pitch;
                    g_live_split_copy = malloc(g_texture_row_pitch * g_bitmap_core_header.bcHeight);
                }
                else
                {
                    release_texture();
                }
            }
        }

        if (g_live_split_copy && g_live_split_copy_ready)
        {
            // Write our copy of the LiveSplit window into the texture.
            subresource_data buffer_info;
            if (g_device->map_texture_region(g_texture, 0, nullptr, map_access::write_discard, &buffer_info))
            {
                memcpy(buffer_info.data, g_live_split_copy, g_texture_row_pitch * g_bitmap_core_header.bcHeight);
                g_device->unmap_texture_region(g_texture, 0);
                ImGui::Image(g_texture_view.handle, ImVec2(g_bitmap_core_header.bcWidth, g_bitmap_core_header.bcHeight));
            }
            else
            {
                ImGui::Text("Failed to copy LiveSplit to texture");
            }
        }
        else if (g_livesplit_window_is_minimized)
        {
            ImGui::Text("LiveSplit is minimized");
        }
    }
    else
    {
        // If we don't have a LiveSplit window, show a spinner to remind the user that we are actively burning CPU cycles waiting for the window.
        ImGui::Text("Waiting for LiveSplit %c", SPINNER_CHARS[GetTickCount64() / 200 % 4]);
    }

    ReleaseMutex(g_mutex);
    SetEvent(g_event);
}

DWORD WINAPI update_live_split_texture(_In_ LPVOID)
{
    while (true)
    {
        // Wait until after a frame has been rendered or the LiveSplit overlay is removed.
        WaitForSingleObject(g_event, INFINITE);
        WaitForSingleObject(g_mutex, INFINITE);

        if (g_terminate_thread)
            break;

        // Reset the status.
        g_live_split_copy_ready = false;
        g_livesplit_window_is_minimized = false;

        // Find the LiveSplit window.
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
            g_livesplit_window_is_minimized = IsIconic(g_livesplit_window_handle);

            if (!g_livesplit_window_is_minimized)
            {
                // Get device context handle from the LiveSplit window, to get at its buffered image.
                HDC device_context_handle = GetWindowDC(g_livesplit_window_handle);
                if (device_context_handle == NULL)
                {
                    // The LiveSplit window probably closed, let's search for it again next time.
                    g_livesplit_window_handle = NULL;
                    g_livesplit_window_is_minimized = false;
                }
                else
                {
                    // Acquire the bitmap that contains the rendering of LiveSplit.
                    HBITMAP bitmap_handle = (HBITMAP)GetCurrentObject(device_context_handle, OBJ_BITMAP);

                    // Query size of the window via its buffer bitmap. Bit count needs to be reset or else the size fields wont update.
                    g_bitmap_core_header.bcBitCount = 0;
                    if (GetDIBits(device_context_handle, bitmap_handle, 0, 0, NULL, (BITMAPINFO*)&g_bitmap_core_header, DIB_RGB_COLORS))
                    {
                        // Copy the bitmap into the RAM buffer if the window is still the same size as the texture.
                        if (g_live_split_copy != nullptr && g_bitmap_core_header.bcWidth == g_texture_descriptor.texture.width && g_bitmap_core_header.bcHeight == g_texture_descriptor.texture.height)
                        {
                            g_bitmap_info_header.biWidth = g_texture_row_pitch / 4;
                            g_bitmap_info_header.biHeight = -g_bitmap_core_header.bcHeight;
                            GetDIBits(device_context_handle, bitmap_handle, 0, g_bitmap_core_header.bcHeight, g_live_split_copy, (BITMAPINFO*)&g_bitmap_info_header, DIB_RGB_COLORS);
                            g_live_split_copy_ready = true;
                        }
                    }

                    // Release the device context from LiveSplit so it can continue rendering.
                    ReleaseDC(g_livesplit_window_handle, device_context_handle);
                }
            }
        }
        else
        {
            release_texture();
        }
        ReleaseMutex(g_mutex);
    }

    ReleaseMutex(g_mutex);
    return 0;
}

static void update_livesplit_display(bool show)
{
    if (show && !g_osd_registered)
    {
        g_mutex = CreateMutex(NULL, FALSE, NULL);
        g_event = CreateEvent(NULL, FALSE, TRUE, NULL);
        g_thread = CreateThread(NULL, 0, &update_live_split_texture, NULL, 0, NULL);
        reshade::register_overlay("OSD", &draw_osd_livesplit);
    }
    else if (!show && g_osd_registered)
    {
        reshade::unregister_overlay("OSD", &draw_osd_livesplit);
        // Signal LiveSplit texture update thread to terminate
        WaitForSingleObject(g_mutex, INFINITE);
        g_terminate_thread = true;
        SetEvent(g_event);
        ReleaseMutex(g_mutex);
        // Wait for thread to terminate and close related handles
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        CloseHandle(g_mutex);
        CloseHandle(g_event);
        release_texture();
        g_live_split_copy_ready = false;
        g_livesplit_window_is_minimized = false;
        g_terminate_thread = false;
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
    update_livesplit_display(g_show_overlay);
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