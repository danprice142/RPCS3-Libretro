#include "stdafx.h"

#include "libretro_video.h"
#include "libretro_core.h"

#include "Emu/RSX/GL/OpenGL.h"
#include "Emu/RSX/GL/glutils/common.h"

#include <mutex>
#include <vector>
#include <algorithm>
#include <thread>
#include <functional>

#ifdef _WIN32
#include <Windows.h>
#include <GL/gl.h>
#include <glext.h>

// WGL extension function pointers for context creation
typedef HGLRC(WINAPI* PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC hDC, HGLRC hShareContext, const int* attribList);
static PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB_ptr = nullptr;

// WGL_ARB_create_context attributes
#define WGL_CONTEXT_MAJOR_VERSION_ARB     0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB     0x2092
#define WGL_CONTEXT_FLAGS_ARB             0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB      0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB  0x00000001

// Saved context info from RetroArch's main thread
static HDC s_main_hdc = nullptr;
static HGLRC s_main_hglrc = nullptr;
static std::vector<HGLRC> s_shared_contexts;
static std::vector<HGLRC> s_available_contexts;  // Pre-created shared contexts pool
static std::mutex s_context_pool_mutex;
#endif

LOG_CHANNEL(libretro_video_log, "LibretroVideo");

static retro_hw_get_current_framebuffer_t s_get_current_framebuffer = nullptr;
static retro_hw_get_proc_address_t s_get_proc_address = nullptr;
static std::mutex s_video_mutex;
static bool s_gl_initialized = false;

// Frame synchronization
static std::mutex s_frame_mutex;
static std::condition_variable s_frame_cv;
static std::atomic<bool> s_frame_ready{false};
static std::atomic<u64> s_frame_count{0};
static std::atomic<u64> s_last_presented_frame{0};

static std::atomic<GLsync> s_present_fence{nullptr};
static std::atomic<u64> s_present_fence_counter{0};

// Shared render texture and FBOs
// FBOs are NOT shared between GL contexts, but textures ARE.
// RSX renders to s_rsx_fbo (on RSX context) which has s_shared_texture attached.
// Main thread uses s_main_read_fbo (on main context) to read from s_shared_texture.
// retro_run then blits from s_main_read_fbo to RetroArch's actual FBO.
static GLuint s_shared_texture = 0;
static GLuint s_rsx_fbo = 0;           // FBO on RSX thread's context
static GLuint s_main_read_fbo = 0;     // FBO on main thread's context for reading shared texture
static int s_shared_texture_width = 1280;
static int s_shared_texture_height = 720;
static std::atomic<bool> s_rsx_resources_created{false};
static std::atomic<bool> s_main_fbo_created{false};

// Create the shared render texture (must be called from RSX thread with its context current)
static GLuint s_depth_stencil_rb = 0;

static void create_rsx_render_resources(int width, int height)
{
    if (s_rsx_resources_created.load())
        return;

    // Create shared texture using standard GL calls
    glGenTextures(1, &s_shared_texture);
    glBindTexture(GL_TEXTURE_2D, s_shared_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Create RSX-side FBO
    glGenFramebuffers(1, &s_rsx_fbo);

    // Use traditional bind-and-attach method (more reliable than DSA)
    glBindFramebuffer(GL_FRAMEBUFFER, s_rsx_fbo);

    // Attach color texture
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_shared_texture, 0);

    // Add depth/stencil renderbuffer
    glGenRenderbuffers(1, &s_depth_stencil_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s_depth_stencil_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    // Attach depth/stencil renderbuffer
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_depth_stencil_rb);

    // Unbind FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    s_shared_texture_width = width;
    s_shared_texture_height = height;
    s_rsx_resources_created = true;
}

// Resize shared texture when game resolution changes (called from RSX thread)
static void resize_rsx_render_resources(int new_width, int new_height)
{
    if (new_width <= 0 || new_height <= 0)
        return;

    if (new_width == s_shared_texture_width && new_height == s_shared_texture_height)
        return;

    // Resize texture
    glBindTexture(GL_TEXTURE_2D, s_shared_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, new_width, new_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Resize depth/stencil renderbuffer
    glBindRenderbuffer(GL_RENDERBUFFER, s_depth_stencil_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, new_width, new_height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    s_shared_texture_width = new_width;
    s_shared_texture_height = new_height;

    // Mark main thread FBO as needing recreation (texture changed)
    s_main_fbo_created = false;
}

void libretro_ensure_render_size(int width, int height)
{
    // Called to ensure shared texture can accommodate the requested size
    if (!s_rsx_resources_created.load())
    {
        // First time - create at requested size
        s_shared_texture_width = width;
        s_shared_texture_height = height;
        create_rsx_render_resources(width, height);
    }
    else if (width > s_shared_texture_width || height > s_shared_texture_height)
    {
        // Need to resize to accommodate larger resolution
        resize_rsx_render_resources(width, height);
    }
}

unsigned int libretro_get_rsx_fbo()
{
    // Lazy creation on first call from RSX thread
    // Use game's native resolution (1280x720) - RetroArch handles final scaling to window
    if (!s_rsx_resources_created.load())
    {
        create_rsx_render_resources(s_shared_texture_width, s_shared_texture_height);
    }
    return s_rsx_fbo;
}

unsigned int libretro_get_shared_texture()
{
    return s_shared_texture;
}

int libretro_get_shared_texture_width()
{
    return s_shared_texture_width;
}

int libretro_get_shared_texture_height()
{
    return s_shared_texture_height;
}

// Create main thread's read FBO (must be called from main thread with its context current)
static void create_main_read_fbo()
{
    if (s_main_fbo_created.load() || s_shared_texture == 0)
        return;

    // Create FBO on main thread's context
    glGenFramebuffers(1, &s_main_read_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_main_read_fbo);

    // Attach the shared texture (texture IS shared between contexts)
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_shared_texture, 0);

    glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    s_main_fbo_created = true;
}

void libretro_blit_to_frontend()
{
    if (!s_gl_initialized || !s_rsx_resources_created.load() || !s_get_current_framebuffer)
        return;

    // Lazy-create main thread's read FBO on first call
    if (!s_main_fbo_created.load())
    {
        create_main_read_fbo();
    }

    if (s_main_read_fbo == 0)
        return;

    // Get RetroArch's actual FBO
    GLuint frontend_fbo = static_cast<GLuint>(s_get_current_framebuffer());

    // Blit from main thread's read FBO (which references shared texture) to RetroArch's FBO
    // NOTE: We use s_main_read_fbo, NOT s_rsx_fbo, because FBOs are context-specific!
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_main_read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frontend_fbo);

    glBlitFramebuffer(
        0, 0, s_shared_texture_width, s_shared_texture_height,  // src
        0, 0, s_shared_texture_width, s_shared_texture_height,  // dst (same size)
        GL_COLOR_BUFFER_BIT,
        GL_NEAREST
    );

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void libretro_signal_frame_ready()
{
    {
        std::lock_guard<std::mutex> lock(s_frame_mutex);
        s_frame_ready = true;
        s_frame_count++;
    }
    s_frame_cv.notify_one();
}

bool libretro_wait_for_frame(u32 timeout_ms)
{
    std::unique_lock<std::mutex> lock(s_frame_mutex);
    if (s_frame_ready.exchange(false))
    {
        return true;
    }

    // timeout_ms=0 means wait indefinitely
    if (timeout_ms == 0)
    {
        s_frame_cv.wait(lock, []{ return s_frame_ready.load(); });
        s_frame_ready = false;
        return true;
    }

    bool result = s_frame_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), []{ return s_frame_ready.load(); });
    if (result)
    {
        s_frame_ready = false;
    }
    return result;
}

// Check if a new frame is available (non-blocking, doesn't consume)
bool libretro_has_new_frame()
{
    return s_frame_count.load() > s_last_presented_frame.load();
}

// Mark the current frame as presented
void libretro_mark_frame_presented()
{
    s_last_presented_frame = s_frame_count.load();
}

// Clean up GL state before returning control to frontend
// Per libretro docs: "Don't leave buffers and global objects bound when calling retro_video_refresh_t"
// PERFORMANCE: Keep this minimal - only unbind what's absolutely necessary
void libretro_cleanup_gl_state()
{
    if (!s_gl_initialized)
        return;

    // CRITICAL: Unbind framebuffer so RetroArch can bind its own for presentation
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Unbind shader program to avoid conflicts with RetroArch's rendering
    glUseProgram(0);

    // NOTE: We intentionally do NOT unbind:
    // - VAO/VBO/buffers: RSX will rebind what it needs, unbinding is expensive
    // - Textures: 32 bind calls per frame (16 units × 2 types) is a huge waste
    // - GL state flags: RSX manages its own state, resetting is redundant
    // The previous implementation was doing 40+ GL calls per frame at 60fps = 2400+ calls/sec
    // which was a major performance bottleneck, especially on some drivers.
}

void libretro_wait_for_present_fence()
{
    if (!s_gl_initialized)
        return;

    // Take ownership of the latest fence (if any). If RSX produced multiple fences
    // since the last call, we only wait on the newest.
    GLsync fence = s_present_fence.exchange(nullptr);
    if (!fence)
        return;

    if (!glClientWaitSync || !glDeleteSync)
        return;

    // Use glClientWaitSync to actually wait for RSX GPU work to complete.
    // Use a very short timeout to avoid blocking audio/main thread
    constexpr GLuint64 timeout_ns = 1000000; // 1ms max wait
    glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, timeout_ns);
    glDeleteSync(fence);
}

// Pre-create shared OpenGL contexts while main context is idle
// This must be called on the main thread before the context is used elsewhere
static void precreate_shared_contexts(int count)
{
#ifdef _WIN32
    if (!s_main_hdc || !s_main_hglrc || !wglCreateContextAttribsARB_ptr)
        return;

    // Temporarily unbind main context so we can share with it
    if (!wglMakeCurrent(NULL, NULL))
        return;

    for (int i = 0; i < count; i++)
    {
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
            WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };

        HGLRC shared_ctx = wglCreateContextAttribsARB_ptr(s_main_hdc, s_main_hglrc, attribs);
        if (shared_ctx)
        {
            std::lock_guard<std::mutex> lock(s_context_pool_mutex);
            s_available_contexts.push_back(shared_ctx);
        }
    }

    // Restore main context
    wglMakeCurrent(s_main_hdc, s_main_hglrc);
#endif
}

// Initialize OpenGL function pointers using libretro's get_proc_address callback
static void libretro_gl_init()
{
    if (!s_get_proc_address)
        return;

#ifdef _WIN32
    // Save the main thread's context info for creating shared contexts later
    s_main_hdc = wglGetCurrentDC();
    s_main_hglrc = wglGetCurrentContext();

    // Load wglCreateContextAttribsARB for creating shared contexts
    wglCreateContextAttribsARB_ptr = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
        s_get_proc_address("wglCreateContextAttribsARB"));

    // Load GL functions using libretro's callback instead of wglGetProcAddress
    // Use X-macro pattern with GLProcTable.h
    #define OPENGL_PROC(p, n) \
        if (!(gl##n)) { \
            gl##n = reinterpret_cast<p>(s_get_proc_address("gl" #n)); \
            if (!(gl##n)) libretro_video_log.warning("OpenGL: initialization of gl" #n " failed (may be optional)."); \
        }
    #define WGL_PROC(p, n) \
        if (!(wgl##n)) { \
            wgl##n = reinterpret_cast<p>(s_get_proc_address("wgl" #n)); \
        }
    #define OPENGL_PROC2(p, n, tn) OPENGL_PROC(p, n)
    #include "Emu/RSX/GL/GLProcTable.h"
    #undef OPENGL_PROC
    #undef OPENGL_PROC2
    #undef WGL_PROC

    // Pre-create shared contexts for RSX thread and shader compiler threads
    precreate_shared_contexts(10);
#else
    // On Unix, fall back to RPCS3's normal init (uses GLEW)
    gl::init();
#endif
}

// Store the actual FBO dimensions from RetroArch
static int s_fbo_width = 1280;
static int s_fbo_height = 720;

int libretro_get_fbo_width() { return s_fbo_width; }
int libretro_get_fbo_height() { return s_fbo_height; }

void libretro_video_init(retro_hw_get_current_framebuffer_t get_fb, retro_hw_get_proc_address_t get_proc)
{
    std::lock_guard<std::mutex> lock(s_video_mutex);
    s_get_current_framebuffer = get_fb;
    s_get_proc_address = get_proc;

    if (!s_gl_initialized)
    {
        libretro_gl_init();
        s_gl_initialized = true;
    }

    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] > 0 && viewport[3] > 0)
    {
        s_fbo_width = viewport[2];
        s_fbo_height = viewport[3];
    }
}

void libretro_video_deinit()
{
    std::lock_guard<std::mutex> lock(s_video_mutex);
    s_get_current_framebuffer = nullptr;
    s_get_proc_address = nullptr;
}

uintptr_t libretro_get_current_framebuffer()
{
    if (s_get_current_framebuffer)
        return s_get_current_framebuffer();
    return 0;
}

void* libretro_get_proc_address(const char* sym)
{
    if (s_get_proc_address)
        return s_get_proc_address(sym);
    return nullptr;
}

// LibretroGSFrame implementation

LibretroGSFrame::LibretroGSFrame()
{
}

LibretroGSFrame::~LibretroGSFrame()
{
    close();
}

void LibretroGSFrame::close()
{
    m_shown = false;
}

void LibretroGSFrame::reset()
{
}

bool LibretroGSFrame::shown()
{
    return m_shown;
}

void LibretroGSFrame::hide()
{
    m_shown = false;
}

void LibretroGSFrame::show()
{
    m_shown = true;
}

void LibretroGSFrame::toggle_fullscreen()
{
    // Fullscreen is handled by the frontend
}

void LibretroGSFrame::delete_context(draw_context_t ctx)
{
#ifdef _WIN32
    if (ctx)
    {
        HGLRC hglrc = reinterpret_cast<HGLRC>(ctx);
        if (hglrc != s_main_hglrc)
        {
            std::lock_guard<std::mutex> lock(s_video_mutex);
            auto it = std::find(s_shared_contexts.begin(), s_shared_contexts.end(), hglrc);
            if (it != s_shared_contexts.end())
            {
                wglDeleteContext(hglrc);
                s_shared_contexts.erase(it);
            }
        }
    }
#else
    (void)ctx;
#endif
}

draw_context_t LibretroGSFrame::make_context()
{
#ifdef _WIN32
    // Try to get a pre-created shared context from the pool
    {
        std::lock_guard<std::mutex> lock(s_context_pool_mutex);
        if (!s_available_contexts.empty())
        {
            HGLRC ctx = s_available_contexts.back();
            s_available_contexts.pop_back();
            s_shared_contexts.push_back(ctx);
            m_context = reinterpret_cast<draw_context_t>(ctx);
            return m_context;
        }
    }

    if (!s_main_hdc || !s_main_hglrc)
        return nullptr;

    HGLRC shared_context = nullptr;

    if (wglCreateContextAttribsARB_ptr)
    {
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
            WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };

        shared_context = wglCreateContextAttribsARB_ptr(s_main_hdc, s_main_hglrc, attribs);
        if (!shared_context)
            shared_context = wglCreateContextAttribsARB_ptr(s_main_hdc, nullptr, attribs);
    }

    if (!shared_context)
        shared_context = wglCreateContext(s_main_hdc);

    if (shared_context)
    {
        std::lock_guard<std::mutex> lock(s_context_pool_mutex);
        s_shared_contexts.push_back(shared_context);
        m_context = reinterpret_cast<draw_context_t>(shared_context);
        return m_context;
    }

    return nullptr;
#else
    m_context = reinterpret_cast<draw_context_t>(1);
    return m_context;
#endif
}

void LibretroGSFrame::set_current(draw_context_t ctx)
{
#ifdef _WIN32
    if (ctx && s_main_hdc)
    {
        HGLRC hglrc = reinterpret_cast<HGLRC>(ctx);
        wglMakeCurrent(s_main_hdc, hglrc);
    }
#else
    (void)ctx;
#endif
}

void LibretroGSFrame::flip(draw_context_t ctx, bool skip_frame)
{
    (void)ctx;
    if (skip_frame)
        return;

    if (glFenceSync && glDeleteSync)
    {
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();

        GLsync old = s_present_fence.exchange(fence);
        if (old)
            glDeleteSync(old);
    }

    // RSX uses double-buffering and calls flip() twice per frame
    // Only signal frame ready every other flip to maintain proper 60fps pacing
    static thread_local u64 tl_flip_count = 0;
    tl_flip_count++;

    if ((tl_flip_count % 2) == 0)
        libretro_signal_frame_ready();
}

void LibretroGSFrame::update_dimensions_from_fbo()
{
    // This is intentionally empty - we use the fixed dimensions from retro_get_system_av_info()
    // RetroArch handles scaling from our base resolution (1280x720) to the actual window size
    // Querying the viewport here would get RSX's internal viewport, not RetroArch's window size
}

void LibretroGSFrame::set_dimensions(int w, int h)
{
    if (w > 0 && h > 0)
    {
        m_width = w;
        m_height = h;
    }
}

int LibretroGSFrame::client_width()
{
    // Use shared texture size - RSX renders at game native resolution
    // RetroArch handles scaling from our output to window size
    return libretro_get_shared_texture_width();
}

int LibretroGSFrame::client_height()
{
    // Use shared texture size - RSX renders at game native resolution
    // RetroArch handles scaling from our output to window size
    return libretro_get_shared_texture_height();
}

f64 LibretroGSFrame::client_display_rate()
{
    // Standard 60Hz, frontend may override
    return 60.0;
}

bool LibretroGSFrame::has_alpha()
{
    return false;
}

display_handle_t LibretroGSFrame::handle() const
{
#ifdef _WIN32
    return nullptr;
#else
    return {};
#endif
}

bool LibretroGSFrame::can_consume_frame() const
{
    return true;
}

void LibretroGSFrame::present_frame(std::vector<u8>&& data, u32 pitch, u32 width, u32 height, bool is_bgra) const
{
    // Software rendering path - not used with OpenGL HW rendering
    (void)data;
    (void)pitch;
    (void)width;
    (void)height;
    (void)is_bgra;
}

void LibretroGSFrame::take_screenshot(std::vector<u8>&& sshot_data, u32 sshot_width, u32 sshot_height, bool is_bgra)
{
    // Screenshots can be handled by the frontend
    (void)sshot_data;
    (void)sshot_width;
    (void)sshot_height;
    (void)is_bgra;
}
