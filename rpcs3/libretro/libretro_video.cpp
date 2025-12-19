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

    libretro_video_log.notice("[LRGL] create_rsx_render_resources: starting width=%d height=%d", width, height);

    // Create shared texture using standard GL calls
    glGenTextures(1, &s_shared_texture);
    libretro_video_log.notice("[LRGL] create_rsx_render_resources: glGenTextures returned texture=%u", s_shared_texture);

    glBindTexture(GL_TEXTURE_2D, s_shared_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Create RSX-side FBO
    glGenFramebuffers(1, &s_rsx_fbo);
    libretro_video_log.notice("[LRGL] create_rsx_render_resources: glGenFramebuffers returned fbo=%u", s_rsx_fbo);

    // Use traditional bind-and-attach method (more reliable than DSA)
    glBindFramebuffer(GL_FRAMEBUFFER, s_rsx_fbo);

    // Attach color texture
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_shared_texture, 0);
    libretro_video_log.notice("[LRGL] create_rsx_render_resources: attached texture %u to fbo %u", s_shared_texture, s_rsx_fbo);

    // Add depth/stencil renderbuffer
    glGenRenderbuffers(1, &s_depth_stencil_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s_depth_stencil_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    // Attach depth/stencil renderbuffer
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_depth_stencil_rb);
    libretro_video_log.notice("[LRGL] create_rsx_render_resources: attached depth_rb %u to fbo %u", s_depth_stencil_rb, s_rsx_fbo);

    // Check framebuffer status
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    // Unbind FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    s_shared_texture_width = width;
    s_shared_texture_height = height;
    s_rsx_resources_created = true;

    libretro_video_log.notice("[LRGL] Created RSX render resources: texture=%u fbo=%u depth_rb=%u size=%dx%d status=0x%x (complete=0x%x)",
        s_shared_texture, s_rsx_fbo, s_depth_stencil_rb, width, height, status, GL_FRAMEBUFFER_COMPLETE);
}

// Resize shared texture when game resolution changes (called from RSX thread)
static void resize_rsx_render_resources(int new_width, int new_height)
{
    if (new_width <= 0 || new_height <= 0)
        return;

    if (new_width == s_shared_texture_width && new_height == s_shared_texture_height)
        return;

    libretro_video_log.notice("[LRGL] resize_rsx_render_resources: resizing from %dx%d to %dx%d",
        s_shared_texture_width, s_shared_texture_height, new_width, new_height);

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

    libretro_video_log.notice("[LRGL] resize_rsx_render_resources: completed, new size=%dx%d", new_width, new_height);
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

    libretro_video_log.notice("[LRGL] create_main_read_fbo: creating FBO for shared_texture=%u", s_shared_texture);

    // Create FBO on main thread's context
    glGenFramebuffers(1, &s_main_read_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_main_read_fbo);

    // Attach the shared texture (texture IS shared between contexts)
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_shared_texture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    s_main_fbo_created = true;

    libretro_video_log.notice("[LRGL] create_main_read_fbo: created main_read_fbo=%u status=0x%x (complete=0x%x)",
        s_main_read_fbo, status, GL_FRAMEBUFFER_COMPLETE);
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

    thread_local u64 tl_blit_count = 0;
    tl_blit_count++;
    const bool log_this = (tl_blit_count <= 120ull) || ((tl_blit_count % 60ull) == 0ull);

    if (log_this)
    {
        libretro_video_log.notice("[LRGL] libretro_blit_to_frontend: shared_tex=%u main_read_fbo=%u frontend_fbo=%u size=%dx%d call=%llu",
            s_shared_texture, s_main_read_fbo, frontend_fbo, s_shared_texture_width, s_shared_texture_height,
            static_cast<unsigned long long>(tl_blit_count));
    }

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

#ifndef RPCS3_LIBRETRO_GL_TRACE
#define RPCS3_LIBRETRO_GL_TRACE 1
#endif

static inline unsigned long long lrgl_tid_hash()
{
	return static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

#if RPCS3_LIBRETRO_GL_TRACE
	#ifdef _WIN32
		#define LRGL_TRACE(fmt, ...) libretro_video_log.notice("[LRGL][tid=%llx][cur_hdc=%p][cur_hglrc=%p] " fmt, lrgl_tid_hash(), wglGetCurrentDC(), wglGetCurrentContext() __VA_OPT__(,) __VA_ARGS__)
		#define LRGL_WARN(fmt, ...)  libretro_video_log.warning("[LRGL][tid=%llx][cur_hdc=%p][cur_hglrc=%p] " fmt, lrgl_tid_hash(), wglGetCurrentDC(), wglGetCurrentContext() __VA_OPT__(,) __VA_ARGS__)
		#define LRGL_ERR(fmt, ...)   libretro_video_log.error("[LRGL][tid=%llx][cur_hdc=%p][cur_hglrc=%p] " fmt, lrgl_tid_hash(), wglGetCurrentDC(), wglGetCurrentContext() __VA_OPT__(,) __VA_ARGS__)
	#else
		#define LRGL_TRACE(fmt, ...) libretro_video_log.notice("[LRGL][tid=%llx] " fmt, lrgl_tid_hash() __VA_OPT__(,) __VA_ARGS__)
		#define LRGL_WARN(fmt, ...)  libretro_video_log.warning("[LRGL][tid=%llx] " fmt, lrgl_tid_hash() __VA_OPT__(,) __VA_ARGS__)
		#define LRGL_ERR(fmt, ...)   libretro_video_log.error("[LRGL][tid=%llx] " fmt, lrgl_tid_hash() __VA_OPT__(,) __VA_ARGS__)
	#endif
#else
	#define LRGL_TRACE(...) do { } while (0)
	#define LRGL_WARN(...)  do { } while (0)
	#define LRGL_ERR(...)   do { } while (0)
#endif

void libretro_wait_for_present_fence()
{
    if (!s_gl_initialized)
        return;

    // Take ownership of the latest fence (if any). If RSX produced multiple fences
    // since the last call, we only wait on the newest.
    GLsync fence = s_present_fence.exchange(nullptr);
    if (!fence)
        return;

    thread_local u64 tl_wait_calls = 0;
    thread_local u64 tl_timeout_count = 0;
    tl_wait_calls++;

    if (!glClientWaitSync || !glDeleteSync)
    {
        LRGL_ERR("libretro_wait_for_present_fence missing glClientWaitSync/glDeleteSync fence=%p", fence);
        return;
    }

    // Use glClientWaitSync to actually wait for RSX GPU work to complete.
    // This ensures the shared texture data is fully written before we read it.
    // Use a short timeout to avoid blocking audio - if we timeout, still proceed.
    constexpr GLuint64 timeout_ns = 8000000; // 8ms max wait (half a frame at 60fps)
    GLenum result = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, timeout_ns);

    if (result == GL_TIMEOUT_EXPIRED || result == GL_WAIT_FAILED)
    {
        tl_timeout_count++;
    }

    const bool log_this = (tl_wait_calls <= 120ull) || ((tl_wait_calls % 60ull) == 0ull) || result != GL_ALREADY_SIGNALED;
    if (log_this)
    {
        const char* result_str = (result == GL_ALREADY_SIGNALED) ? "SIGNALED" :
                                  (result == GL_CONDITION_SATISFIED) ? "SATISFIED" :
                                  (result == GL_TIMEOUT_EXPIRED) ? "TIMEOUT" : "FAILED";
        LRGL_TRACE("libretro_wait_for_present_fence fence=%p result=%s timeouts=%llu call=%llu",
            fence, result_str, static_cast<unsigned long long>(tl_timeout_count), static_cast<unsigned long long>(tl_wait_calls));
    }

    glDeleteSync(fence);
}

// Pre-create shared OpenGL contexts while main context is idle
// This must be called on the main thread before the context is used elsewhere
static void precreate_shared_contexts(int count)
{
#ifdef _WIN32
    LRGL_TRACE("precreate_shared_contexts enter count=%d s_main_hdc=%p s_main_hglrc=%p wglCreateContextAttribsARB_ptr=%p", count, s_main_hdc, s_main_hglrc, wglCreateContextAttribsARB_ptr);
    if (!s_main_hdc || !s_main_hglrc || !wglCreateContextAttribsARB_ptr)
    {
        LRGL_ERR("Cannot pre-create shared contexts: prerequisites not met");
        return;
    }

    LRGL_TRACE("Pre-creating %d shared OpenGL contexts...", count);

    // Temporarily unbind main context so we can share with it
    // (wglShareLists and wglCreateContextAttribsARB with share require contexts to be idle)
    if (!wglMakeCurrent(NULL, NULL))
    {
        DWORD error = GetLastError();
        LRGL_ERR("Failed to unbind main context for sharing (error=%u)", error);
        return;
    }

    LRGL_TRACE("Main context unbound for pre-creation");

    int created = 0;
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
            created++;
            LRGL_TRACE("Pre-created shared context idx=%d hglrc=%p pool_size=%zu", i, shared_ctx, s_available_contexts.size());
        }
        else
        {
            DWORD error = GetLastError();
            LRGL_ERR("Failed to pre-create shared context idx=%d (error=%u)", i, error);
        }
    }

    // Restore main context
    if (!wglMakeCurrent(s_main_hdc, s_main_hglrc))
    {
        DWORD error = GetLastError();
        LRGL_ERR("Failed to restore main context after pre-creation (error=%u)", error);
    }
    else
    {
        LRGL_TRACE("Main context restored after pre-creation");
    }

    LRGL_TRACE("Pre-created %d/%d shared contexts", created, count);
#endif
}

// Initialize OpenGL function pointers using libretro's get_proc_address callback
static void libretro_gl_init()
{
    if (!s_get_proc_address)
    {
        LRGL_ERR("Cannot initialize GL: no proc address callback");
        return;
    }

    LRGL_TRACE("libretro_gl_init enter s_get_proc_address=%p", reinterpret_cast<void*>(s_get_proc_address));

#ifdef _WIN32
    // Save the main thread's context info for creating shared contexts later
    s_main_hdc = wglGetCurrentDC();
    s_main_hglrc = wglGetCurrentContext();

    if (s_main_hdc && s_main_hglrc)
    {
        LRGL_TRACE("Captured main OpenGL context: HDC=%p, HGLRC=%p", s_main_hdc, s_main_hglrc);
    }
    else
    {
        LRGL_ERR("Failed to capture main OpenGL context!");
    }

    // Load wglCreateContextAttribsARB for creating shared contexts
    wglCreateContextAttribsARB_ptr = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
        s_get_proc_address("wglCreateContextAttribsARB"));
    if (!wglCreateContextAttribsARB_ptr)
    {
        LRGL_WARN("wglCreateContextAttribsARB not available - shared contexts may not work");
    }
    else
    {
        LRGL_TRACE("Loaded wglCreateContextAttribsARB_ptr=%p", wglCreateContextAttribsARB_ptr);
    }

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
    // RPCS3 needs: 1 for RSX main thread + up to 8 for shader compiler threads
    precreate_shared_contexts(10);
#else
    // On Unix, fall back to RPCS3's normal init (uses GLEW)
    gl::init();
#endif

    LRGL_TRACE("OpenGL initialization complete");
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

    LRGL_TRACE("libretro_video_init get_fb=%p get_proc=%p s_gl_initialized=%d", reinterpret_cast<void*>(get_fb), reinterpret_cast<void*>(get_proc), s_gl_initialized ? 1 : 0);

    // Initialize OpenGL using libretro's proc address callback
    // This ensures we use the same context that RetroArch created
    if (!s_gl_initialized)
    {
        libretro_gl_init();
        s_gl_initialized = true;
        LRGL_TRACE("libretro_video_init finished initial GL init");
    }
    else
    {
        LRGL_TRACE("libretro_video_init GL already initialized; skipping libretro_gl_init");
    }

    // Query the actual FBO/viewport size from RetroArch's context
    // This is done once during init when RetroArch's viewport is set correctly
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] > 0 && viewport[3] > 0)
    {
        s_fbo_width = viewport[2];
        s_fbo_height = viewport[3];
        LRGL_TRACE("libretro_video_init: captured FBO size %dx%d", s_fbo_width, s_fbo_height);
    }
}

void libretro_video_deinit()
{
    std::lock_guard<std::mutex> lock(s_video_mutex);
    LRGL_TRACE("libretro_video_deinit clearing callbacks");
    s_get_current_framebuffer = nullptr;
    s_get_proc_address = nullptr;
}

uintptr_t libretro_get_current_framebuffer()
{
    if (s_get_current_framebuffer)
    {
        const auto fbo = s_get_current_framebuffer();
#if RPCS3_LIBRETRO_GL_TRACE
        thread_local u64 tl_call_count = 0;
        thread_local unsigned long long tl_last_fbo = ~0ull;
        tl_call_count++;
        const bool log_this = (tl_call_count <= 120ull) || ((tl_call_count % 60ull) == 0ull) || (static_cast<unsigned long long>(fbo) != tl_last_fbo);
        if (log_this)
        {
            LRGL_TRACE("libretro_get_current_framebuffer -> 0x%llx (last=0x%llx call=%llu)", static_cast<unsigned long long>(fbo), tl_last_fbo, static_cast<unsigned long long>(tl_call_count));
        }
        tl_last_fbo = static_cast<unsigned long long>(fbo);
#endif
        return fbo;
    }
    LRGL_WARN("libretro_get_current_framebuffer called but callback missing");
    return 0;
}

void* libretro_get_proc_address(const char* sym)
{
    if (s_get_proc_address)
    {
        void* const p = s_get_proc_address(sym);
        LRGL_TRACE("libretro_get_proc_address sym=%s -> %p", sym ? sym : "(null)", p);
        return p;
    }
    LRGL_WARN("libretro_get_proc_address called but callback missing (sym=%s)", sym ? sym : "(null)");
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
        LRGL_TRACE("LibretroGSFrame::delete_context ctx=%p hglrc=%p s_main_hglrc=%p", ctx, hglrc, s_main_hglrc);
        // Don't delete the main context
        if (hglrc != s_main_hglrc)
        {
            std::lock_guard<std::mutex> lock(s_video_mutex);
            auto it = std::find(s_shared_contexts.begin(), s_shared_contexts.end(), hglrc);
            if (it != s_shared_contexts.end())
            {
                wglDeleteContext(hglrc);
                s_shared_contexts.erase(it);
                LRGL_TRACE("Deleted shared OpenGL context: %p", hglrc);
            }
            else
            {
                LRGL_WARN("Requested delete of context not tracked in s_shared_contexts: %p", hglrc);
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
    LRGL_TRACE("LibretroGSFrame::make_context enter s_main_hdc=%p s_main_hglrc=%p pool=%zu shared=%zu", s_main_hdc, s_main_hglrc, s_available_contexts.size(), s_shared_contexts.size());
    // Try to get a pre-created shared context from the pool
    {
        std::lock_guard<std::mutex> lock(s_context_pool_mutex);
        if (!s_available_contexts.empty())
        {
            HGLRC ctx = s_available_contexts.back();
            s_available_contexts.pop_back();
            s_shared_contexts.push_back(ctx);
            LRGL_TRACE("Allocated pre-created shared context: %p (pool_remaining=%zu shared_total=%zu)", ctx, s_available_contexts.size(), s_shared_contexts.size());
            m_context = reinterpret_cast<draw_context_t>(ctx);
            return m_context;
        }
    }

    // Pool exhausted - try to create a new context (may fail if main context is busy)
    LRGL_WARN("Shared context pool exhausted, attempting dynamic creation...");

    if (!s_main_hdc || !s_main_hglrc)
    {
        LRGL_ERR("Cannot create context: main context not captured");
        return nullptr;
    }

    HGLRC shared_context = nullptr;

    if (wglCreateContextAttribsARB_ptr)
    {
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
            WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };

        // Try with sharing (will likely fail if main context is current)
        shared_context = wglCreateContextAttribsARB_ptr(s_main_hdc, s_main_hglrc, attribs);
        if (!shared_context)
        {
            DWORD error = GetLastError();
            LRGL_WARN("wglCreateContextAttribsARB(shared) failed (error=%u)", error);
            // Create without sharing as fallback (won't share resources)
            shared_context = wglCreateContextAttribsARB_ptr(s_main_hdc, nullptr, attribs);
            if (shared_context)
            {
                LRGL_WARN("Created non-shared context (resources won't be shared): %p", shared_context);
            }
            else
            {
                DWORD error2 = GetLastError();
                LRGL_ERR("wglCreateContextAttribsARB(non-shared) failed (error=%u)", error2);
            }
        }
        else
        {
            LRGL_TRACE("Created shared context via wglCreateContextAttribsARB: %p", shared_context);
        }
    }

    if (!shared_context)
    {
        shared_context = wglCreateContext(s_main_hdc);
        if (!shared_context)
        {
            DWORD error = GetLastError();
            LRGL_ERR("wglCreateContext failed (error=%u)", error);
        }
        else
        {
            LRGL_TRACE("Created context via wglCreateContext: %p", shared_context);
        }
    }

    if (shared_context)
    {
        std::lock_guard<std::mutex> lock(s_context_pool_mutex);
        s_shared_contexts.push_back(shared_context);
        LRGL_TRACE("Created OpenGL context: %p shared_total=%zu", shared_context, s_shared_contexts.size());
        m_context = reinterpret_cast<draw_context_t>(shared_context);
        return m_context;
    }

    LRGL_ERR("Failed to create OpenGL context");
    return nullptr;
#else
    // On Unix, return dummy - proper implementation needed
    m_context = reinterpret_cast<draw_context_t>(1);
    return m_context;
#endif
}

void LibretroGSFrame::set_current(draw_context_t ctx)
{
#ifdef _WIN32
    LRGL_TRACE("LibretroGSFrame::set_current ctx=%p s_main_hdc=%p", ctx, s_main_hdc);
    if (ctx && s_main_hdc)
    {
        HGLRC hglrc = reinterpret_cast<HGLRC>(ctx);
        HGLRC prev = wglGetCurrentContext();
        HDC prev_dc = wglGetCurrentDC();
        LRGL_TRACE("wglMakeCurrent request prev_hdc=%p prev_hglrc=%p new_hglrc=%p", prev_dc, prev, hglrc);
        if (!wglMakeCurrent(s_main_hdc, hglrc))
        {
            DWORD error = GetLastError();
            LRGL_ERR("wglMakeCurrent failed (error=%u) target_hdc=%p target_hglrc=%p", error, s_main_hdc, hglrc);
        }
        else
        {
            LRGL_TRACE("Made OpenGL context current: %p", hglrc);
        }
    }
    else
    {
        LRGL_WARN("LibretroGSFrame::set_current skipped (ctx=%p s_main_hdc=%p)", ctx, s_main_hdc);
    }
#else
    (void)ctx;
#endif
}

void LibretroGSFrame::flip(draw_context_t ctx, bool skip_frame)
{
    if (skip_frame)
    {
        LRGL_TRACE("LibretroGSFrame::flip skip_frame=1 ctx=%p", ctx);
        return;
    }

    LRGL_TRACE("LibretroGSFrame::flip skip_frame=0 ctx=%p dims=%dx%d", ctx, m_width, m_height);

    if (!glFenceSync || !glDeleteSync)
    {
        LRGL_ERR("LibretroGSFrame::flip missing glFenceSync/glDeleteSync (ctx=%p)", ctx);
    }
    else
    {
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();

        const auto fence_id = s_present_fence_counter.fetch_add(1) + 1;
        GLsync old = s_present_fence.exchange(fence);
        if (old)
        {
            glDeleteSync(old);
        }

        const bool log_this = (fence_id <= 120ull) || ((fence_id % 60ull) == 0ull) || old;
        if (log_this)
        {
            LRGL_TRACE("LibretroGSFrame::flip present_fence id=%llu fence=%p old=%p", static_cast<unsigned long long>(fence_id), fence, old);
        }
    }

    // RSX uses double-buffering and calls flip() twice per frame (push + pop)
    // Only signal frame ready every other flip to maintain proper 60fps pacing
    static thread_local u64 tl_flip_count = 0;
    tl_flip_count++;

    if ((tl_flip_count % 2) == 0)
    {
        // Signal that a frame is ready for the frontend (every other flip = actual presentation)
        libretro_signal_frame_ready();
    }
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
        LRGL_TRACE("set_dimensions: %dx%d", m_width, m_height);
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
