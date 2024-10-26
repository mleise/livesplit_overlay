#include "pch.h"

// Constants
extern "C" __declspec(dllexport) const char* const NAME = ADDON_NAME;
extern "C" __declspec(dllexport) const char* const DESCRIPTION = ADDON_DESCRIPTION;
const char* const INI_SECTION = "LIVESPLIT";
const char* const INI_SHOW = "Show";
const char* const INI_ALIGNMENT_X = "AlignmentX";
const char* const INI_ALIGNMENT_Y = "AlignmentY";
const char* const INI_OFFSET_X = "OffsetX";
const char* const INI_OFFSET_Y = "OffsetY";
const char* const SPINNER_CHARS = "|\\-/";
const resource_view_desc TEXTURE_VIEW_DESCRIPTOR = resource_view_desc(format::b8g8r8a8_unorm);

// Settings
static bool g_show_livesplit = true;
static float g_livesplit_alignment[2] = { 0 ,0 };
static int g_livesplit_offsets[2] = { 0 ,0 };

// Other globals
static device* g_device = nullptr;
static HANDLE g_thread;
static HANDLE g_event_worker_go, g_event_worker_done;
static bool g_terminate_thread = false;
static std::string g_osd_text = "";
static bool g_create_texture = false;
static bool g_destroy_texture = false;
static bool g_have_livesplit_copy = false;
static resource_desc g_texture_descriptor = resource_desc(0, 0, 1, 1, format::b8g8r8a8_unorm, 1, memory_heap::cpu_to_gpu, resource_usage::shader_resource_pixel, resource_flags::dynamic);
static BITMAPINFOHEADER g_bitmap_info_header = { sizeof(BITMAPINFOHEADER), 0, 0, 1, 32, BI_RGB };
static void* g_live_split_window_copy;
static resource g_texture = {};
static resource_view g_texture_view = {};
static HWND g_livesplit_window_handle = NULL;

/// <summary>Identifies the LiveSplit window. Used in a callback to EnumWindows().</summary>
/// <param name="hWnd">The current window being examined.</param>
/// <returns>FALSE when LiveSplit has been found to stop the enumeration, TRUE otherwise.</returns>
static BOOL CALLBACK identify_livesplit_window(_In_ HWND hWnd, _In_ LPARAM)
{
    // Window must not be a hidden window.
    if (IsWindowVisible(hWnd))
    {
        // Check that this window belongs to a "LiveSplit.exe" process, as it could be e.g. an Explorer window open on the LiveSplit folder.
        DWORD processId = 0;
        GetWindowThreadProcessId(hWnd, &processId);
        if (processId)
        {
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
            if (hProcess != NULL)
            {
                DWORD fileNameLen;
                static std::vector<WCHAR> textBuffer(64);
                while (true)
                {
                    fileNameLen = GetProcessImageFileNameW(hProcess, &textBuffer[0], (DWORD)textBuffer.size());
                    if (fileNameLen != 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                        break;
                    textBuffer.resize(textBuffer.size() * 2);
                }

                if (fileNameLen >= 14 && CompareStringOrdinal(&textBuffer[fileNameLen - 14], 14, L"\\LiveSplit.exe", 14, FALSE) == CSTR_EQUAL)
                {
                    // Verify that the window's title is actually "LiveSplit".
                    // We do this after identifying the process or else we could query our game's window which might become unresponsive.
                    if (GetWindowTextLength(hWnd) == 9)
                    {
                        int captionLen = GetWindowTextW(hWnd, &textBuffer[0], (int)textBuffer.size());
                        if (CompareStringOrdinal(&textBuffer[0], captionLen, L"LiveSplit", 9, FALSE) == CSTR_EQUAL)
                        {
                            g_livesplit_window_handle = hWnd;
                            return FALSE;
                        }
                    }
                }
                CloseHandle(hProcess);
            }
        }
    }
    return TRUE;
}

/// <summary>
/// The worker thread is always run first. It is responsible for finding the LiveSplit window, getting its dimensions
/// and copying it to an intermediate buffer provided by the main thread.
/// </summary>
/// <returns>0</returns>
static DWORD WINAPI worker_thread(_In_ LPVOID)
{
    while (!g_terminate_thread)
    {
        // Reset the texture (re)creation status.
        g_create_texture = false;
        g_destroy_texture = g_texture_view.handle != 0;

        // Find the LiveSplit window.
        if (g_livesplit_window_handle == NULL)
        {
            static ULONGLONG s_last_window_search_tick = 0;
            ULONGLONG now = GetTickCount64();
            if (now - s_last_window_search_tick >= 1000)
            {
                EnumWindows(&identify_livesplit_window, 0);
                s_last_window_search_tick = now;
            }
        }

        if (g_livesplit_window_handle != NULL)
        {
            if (!IsIconic(g_livesplit_window_handle))
            {
                // Get device context handle from the LiveSplit window, to get at its buffered image.
                HDC device_context_handle = GetWindowDC(g_livesplit_window_handle);
                if (device_context_handle != NULL)
                {
                    // Acquire the bitmap that contains the rendering of LiveSplit.
                    HBITMAP bitmap_handle = (HBITMAP)GetCurrentObject(device_context_handle, OBJ_BITMAP);

                    BITMAPCOREHEADER bitmap_core_header { sizeof(BITMAPCOREHEADER) };
                    if (GetDIBits(device_context_handle, bitmap_handle, 0, 0, NULL, (LPBITMAPINFO)&bitmap_core_header, DIB_RGB_COLORS))
                    {
                        // Inform main thread that it needs to recreate the texture if LiveSplit window size changed.
                        g_create_texture =
                            bitmap_core_header.bcWidth != g_texture_descriptor.texture.width ||
                            bitmap_core_header.bcHeight != g_texture_descriptor.texture.height;

                        if (g_create_texture)
                        {
                            g_bitmap_info_header.biHeight = -bitmap_core_header.bcHeight;
                            g_texture_descriptor.texture.width = bitmap_core_header.bcWidth;
                            g_texture_descriptor.texture.height = bitmap_core_header.bcHeight;
                        }
                        else if (g_live_split_window_copy)
                        {
                            g_have_livesplit_copy = GetDIBits(device_context_handle, bitmap_handle, 0, bitmap_core_header.bcHeight, g_live_split_window_copy, (LPBITMAPINFO)&g_bitmap_info_header, DIB_RGB_COLORS);
                            if (g_have_livesplit_copy)
                            {
                                g_destroy_texture &= g_create_texture;
                            }
                            else
                            {
                                g_osd_text = "Failed to copy LiveSplit window contents.";
                            }
                        }
                    }
                    else
                    {
                        g_osd_text = "Failed to query buffer format of LiveSplit device context.";
                    }

                    // Release the device context from LiveSplit so it can continue rendering.
                    ReleaseDC(g_livesplit_window_handle, device_context_handle);
                }
                else
                {
                    // The LiveSplit window probably closed, let's search for it again next time.
                    g_livesplit_window_handle = NULL;
                    g_osd_text = "Couldn't acquire LiveSplit device context.";
                }
            }
            else
            {
                g_osd_text = "LiveSplit is minimized.";
            }
        }
        else
        {
            g_osd_text = "Waiting for LiveSplit ";
            g_osd_text += SPINNER_CHARS[GetTickCount64() / 200 % 4];
        }

        // Wait until after a frame has been rendered or the LiveSplit overlay is removed.
        SignalObjectAndWait(g_event_worker_done, g_event_worker_go, INFINITE, FALSE);
    }

    return 0;
}

/// <summary>Destroys the LiveSplit texture and the view on it, if they exists. Also frees the offline copy of the LiveSplit window.</summary>
static void destroy_texture()
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
    free(g_live_split_window_copy);
    g_live_split_window_copy = nullptr;
    g_have_livesplit_copy = false;
    if (!g_create_texture)
    {
        g_texture_descriptor.texture.width = 0;
        g_texture_descriptor.texture.height = 0;
    }
    g_destroy_texture = false;
}

/// <summary>
/// Here, on the main thread, we allocate the intermediate buffer for the LiveSplit window copy according to the
/// dimensions provided by the worker thread. Once the worker thread has started filling it, we upload it to a GPU
/// texture and render it as an ImGui image onto the game.
/// </summary>
/// <param name="runtime">The ReShade effect runtime.</param>
static void draw_livesplit(_In_ effect_runtime* runtime)
{
    if (runtime->get_device() != g_device)
        return;

    WaitForSingleObject(g_event_worker_done, INFINITE);

    // Handle worker thread requests to destroy and/or create the texture.
    if (g_destroy_texture)
    {
        destroy_texture();
    }

    if (g_create_texture)
    {
        g_create_texture = false;
        // For Vulkan I'm using cpu_only as a hack, since it enables linear tiling, allowing us to upload linear bitmap data.
        g_texture_descriptor.heap = g_device->get_api() == device_api::vulkan ? memory_heap::cpu_only : memory_heap::cpu_to_gpu;
        if (g_device->create_resource(g_texture_descriptor, nullptr, resource_usage::shader_resource_pixel, &g_texture))
        {
            if (g_device->create_resource_view(g_texture, resource_usage::shader_resource, TEXTURE_VIEW_DESCRIPTOR, &g_texture_view))
            {
                subresource_data buffer_info;
                if (g_device->map_texture_region(g_texture, 0, nullptr, map_access::write_discard, &buffer_info))
                {
                    g_device->unmap_texture_region(g_texture, 0);
                    g_bitmap_info_header.biWidth = buffer_info.row_pitch / 4;
                    g_live_split_window_copy = malloc(buffer_info.row_pitch * -g_bitmap_info_header.biHeight);
                    if (g_live_split_window_copy == nullptr)
                    {
                        g_osd_text = "Failed to allocate intermediate buffer for LiveSplit window copy.";
                        g_destroy_texture = true;
                    }
                }
                else
                {
                    g_osd_text = "Failed to map texture to host memory.";
                    g_destroy_texture = true;
                }
            }
            else
            {
                g_osd_text = "Failed to create resource view of texture.";
            }
        }
        else
        {
            g_osd_text = "Failed to create texture.";
        }

        if (g_destroy_texture)
        {
            destroy_texture();
        }
    }

    if (g_live_split_window_copy && g_have_livesplit_copy)
    {
        // Upload worker thread's copy of LiveSplit into the texture.
        subresource_data buffer_info;
        if (g_device->map_texture_region(g_texture, 0, nullptr, map_access::write_discard, &buffer_info))
        {
            const char* const end = (char*)buffer_info.data + buffer_info.row_pitch * -g_bitmap_info_header.biHeight;
            char* dst = (char*)buffer_info.data;
            const char* src = (char*)g_live_split_window_copy;
            while (dst != end)
            {
                memcpy(dst, src, buffer_info.row_pitch);
                src += buffer_info.row_pitch;
                dst += buffer_info.row_pitch;
            }
            g_device->unmap_texture_region(g_texture, 0);
            g_osd_text = "";
        }
        else
        {
            g_osd_text = "Failed to upload LiveSplit window to texture.";
        }

        // Draw a quad with the LiveSplit texture on the background.
        if (g_texture_view.handle != 0 && g_have_livesplit_copy)
        {
            ImVec2 disp_size = ImGui::GetIO().DisplaySize;
            ImVec2 offsets = {
                min(g_livesplit_offsets[0], (disp_size.x - g_texture_descriptor.texture.width) / 2),
                min(g_livesplit_offsets[1], (disp_size.y - g_texture_descriptor.texture.height) / 2)
            };
            ImVec2 p1 = ImVec2(
                offsets.x + g_livesplit_alignment[0] * (disp_size.x - 2 * offsets.x - g_texture_descriptor.texture.width),
                offsets.y + g_livesplit_alignment[1] * (disp_size.y - 2 * offsets.y - g_texture_descriptor.texture.height)
            );
            ImVec2 p3 = ImVec2(p1.x + g_texture_descriptor.texture.width, p1.y + g_texture_descriptor.texture.height);
            ImVec2 p2 = ImVec2(p3.x, p1.y);
            ImVec2 p4 = ImVec2(p1.x, p3.y);
            ImGui::GetBackgroundDrawList()->AddImageQuad(g_texture_view.handle, p1, p2, p3, p4);
        }
    }
}

/// <summary>This renders our error or informational messages into the default OSD that ReShade provides for its own FPS counter.</summary>
static void draw_message_osd(_In_ effect_runtime*)
{
    // Show any error or informational message on the default OSD.
    {
        if (g_osd_text.length())
        {
            ImGui::TextUnformatted(g_osd_text.c_str(), g_osd_text.c_str() + g_osd_text.length());
        }
    }
}

/// <summary>Sets up all the hooks and resources to show the LiveSplit window and frees resources when the overlay is being hidden.</summary>
/// <param name="show">Whether the LiveSplit overlay should be shown.</param>
static void update_livesplit_visibility(_In_ bool show)
{
    static bool s_livesplit_showing = false;

    if (show && !s_livesplit_showing)
    {
        if (g_device->get_api() == device_api::d3d12)
        {
            g_osd_text = "LiveSplit Overlay: DX12 is not supported.";
        }
        else
        {
            g_event_worker_go = CreateEvent(NULL, FALSE, FALSE, NULL);
            g_event_worker_done = CreateEvent(NULL, FALSE, FALSE, NULL);
            g_thread = CreateThread(NULL, 0, &worker_thread, NULL, 0, NULL);
            reshade::register_event<reshade::addon_event::reshade_overlay>(&draw_livesplit);
        }
        reshade::register_overlay("OSD", &draw_message_osd);
    }
    else if (!show && s_livesplit_showing)
    {
        reshade::unregister_overlay("OSD", &draw_message_osd);
        if (g_device->get_api() != device_api::d3d12)
        {
            reshade::unregister_event<reshade::addon_event::reshade_overlay>(&draw_livesplit);
            // Signal worker thread to terminate and close related handles
            g_terminate_thread = true;
            SignalObjectAndWait(g_event_worker_go, g_thread, INFINITE, FALSE);
            g_terminate_thread = false;
            CloseHandle(g_thread);
            CloseHandle(g_event_worker_done);
            CloseHandle(g_event_worker_go);
            destroy_texture();
        }
    }
    s_livesplit_showing = show;
}

/// <summary>This is the addon configuration section in ReShade.</summary>
static void draw_settings_overlay(_In_ effect_runtime*)
{
    if (ImGui::Checkbox("Show LiveSplit", &g_show_livesplit))
    {
        reshade::set_config_value(nullptr, INI_SECTION, INI_SHOW, g_show_livesplit);
        update_livesplit_visibility(g_show_livesplit);
    }
    if (ImGui::SliderFloat2("Vertical/Horizontal Alignment", g_livesplit_alignment, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp))
    {
        reshade::set_config_value(nullptr, INI_SECTION, INI_ALIGNMENT_X, g_livesplit_alignment[0]);
        reshade::set_config_value(nullptr, INI_SECTION, INI_ALIGNMENT_Y, g_livesplit_alignment[1]);
    }
    if (ImGui::DragInt2("Vertical/Horizontal Offsets", g_livesplit_offsets, 1, 0, INT_MAX, NULL, 0))
    {
        reshade::set_config_value(nullptr, INI_SECTION, INI_OFFSET_X, g_livesplit_offsets[0]);
        reshade::set_config_value(nullptr, INI_SECTION, INI_OFFSET_Y, g_livesplit_offsets[1]);
    }
}

/// <summary>Lets worker thread wake up and fetch the next copy of LiveSplit after a rendered frame.</summary>
static void on_reshade_present(_In_ effect_runtime*)
{
    SetEvent(g_event_worker_go);
}

/// <summary>Reacts to effect runtime creation and checks if the LiveSplit overlay is enabled in the addon settings.</summary>
/// <param name="effect_runtime">The newly initialized ReShade effect runtime.</param>
static void on_init_swapchain(_In_ swapchain* swapchain)
{
    // Only react to the first device.
    if (g_device == nullptr)
    {
        g_device = swapchain->get_device();
        reshade::get_config_value(nullptr, INI_SECTION, INI_SHOW, g_show_livesplit);
        reshade::get_config_value(nullptr, INI_SECTION, INI_ALIGNMENT_X, g_livesplit_alignment[0]);
        reshade::get_config_value(nullptr, INI_SECTION, INI_ALIGNMENT_Y, g_livesplit_alignment[1]);
        reshade::get_config_value(nullptr, INI_SECTION, INI_OFFSET_X, g_livesplit_offsets[0]);
        reshade::get_config_value(nullptr, INI_SECTION, INI_OFFSET_Y, g_livesplit_offsets[1]);
        update_livesplit_visibility(g_show_livesplit);
    }
}

/// <summary>Reacts to effect runtime desctruction and tears down the LiveSplit overlay.</summary>
/// <param name="effect_runtime">The effect_runtime being destroyed.</param>
static void on_destroy_swapchain(_In_ swapchain* swapchain)
{
    // Only react to the device we initially picked up.
    if (g_device == swapchain->get_device())
    {
        update_livesplit_visibility(false);
        g_device = nullptr;
    }
}

/// <summary>Win32 DLL entry point. Registers this addon with ReShade.</summary>
/// <param name="hinstDLL">The Win32 DLL instance, which is equivalent to its module handle.</param>
/// <param name="fdwReason">The reason for why DllMain() is being called.</param>
/// <returns>TRUE if we were able to register the addon with ReShade, FALSE otherwise.</returns>
BOOL WINAPI DllMain(_In_ HINSTANCE hinstDLL, _In_ DWORD fdwReason, _In_ LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hinstDLL))
            return FALSE;
        reshade::register_overlay(nullptr, &draw_settings_overlay);
        reshade::register_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::register_event<reshade::addon_event::init_swapchain>(&on_init_swapchain);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(&on_destroy_swapchain);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_event<reshade::addon_event::destroy_swapchain>(&on_destroy_swapchain);
        reshade::unregister_event<reshade::addon_event::init_swapchain>(&on_init_swapchain);
        reshade::unregister_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::unregister_overlay(nullptr, &draw_settings_overlay);
        reshade::unregister_addon(hinstDLL);
        break;
    }
    return TRUE;
}
