#pragma once

#include "Emu/RSX/GSFrameBase.h"
#include "libretro.h"

#include <functional>
#include <atomic>
#include <condition_variable>
#include <mutex>

// Initialize libretro video subsystem
void libretro_video_init(retro_hw_get_current_framebuffer_t get_fb, retro_hw_get_proc_address_t get_proc);

// Deinitialize libretro video subsystem
void libretro_video_deinit();

// Get the current framebuffer
uintptr_t libretro_get_current_framebuffer();

// Get OpenGL proc address
void* libretro_get_proc_address(const char* sym);

// Frame synchronization - wait for RSX to complete a frame
bool libretro_wait_for_frame(u32 timeout_ms = 20);

void libretro_wait_for_present_fence();

// Signal that a frame is ready (called from RSX flip)
void libretro_signal_frame_ready();

// Check if a new frame is available (non-blocking)
bool libretro_has_new_frame();

// Mark the current frame as presented
void libretro_mark_frame_presented();

// Clean up GL state before returning control to frontend
void libretro_cleanup_gl_state();

// Get the actual FBO dimensions from RetroArch
int libretro_get_fbo_width();
int libretro_get_fbo_height();

// Get the RSX-side FBO that uses the shared render texture
// RSX should render to THIS FBO, not RetroArch's FBO (FBOs are not shared between GL contexts)
unsigned int libretro_get_rsx_fbo();

// Get the shared render texture that RSX renders to
unsigned int libretro_get_shared_texture();

// Get shared texture dimensions (game native resolution)
int libretro_get_shared_texture_width();
int libretro_get_shared_texture_height();

// Ensure shared texture is large enough for the given resolution
// Called when game resolution changes (e.g., resolution scaling)
void libretro_ensure_render_size(int width, int height);

// Blit the shared render texture to RetroArch's actual FBO
// Called from retro_run on RetroArch's main thread before video_cb
void libretro_blit_to_frontend();

// LibretroGSFrame - GSFrameBase implementation for libretro
class LibretroGSFrame : public GSFrameBase
{
private:
    draw_context_t m_context = nullptr;
    int m_width = 1280;
    int m_height = 720;
    bool m_shown = false;

    // Get actual FBO dimensions from OpenGL
    void update_dimensions_from_fbo();

public:
    LibretroGSFrame();
    ~LibretroGSFrame() override;

    void close() override;
    void reset() override;
    bool shown() override;
    void hide() override;
    void show() override;
    void toggle_fullscreen() override;

    void delete_context(draw_context_t ctx) override;
    draw_context_t make_context() override;
    void set_current(draw_context_t ctx) override;
    void flip(draw_context_t ctx, bool skip_frame = false) override;

    int client_width() override;
    int client_height() override;

    // Update dimensions (call when FBO might have changed)
    void set_dimensions(int w, int h);
    f64 client_display_rate() override;
    bool has_alpha() override;

    display_handle_t handle() const override;

    bool can_consume_frame() const override;
    void present_frame(std::vector<u8>&& data, u32 pitch, u32 width, u32 height, bool is_bgra) const override;
    void take_screenshot(std::vector<u8>&& sshot_data, u32 sshot_width, u32 sshot_height, bool is_bgra) override;
};
