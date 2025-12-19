// RPCS3 libretro core implementation
// Focuses on OpenGL rendering backend

#include "stdafx.h"

#include "libretro.h"
#include "libretro_core.h"

#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/KeyboardHandler.h"
#include "Emu/Io/MouseHandler.h"
#include "Emu/RSX/GL/GLGSRender.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/IdManager.h"
#include "Emu/VFS.h"
#include "Emu/RSX/RSXThread.h"
#include "Input/pad_thread.h"
#include "util/video_source.h"
#include "Emu/vfs_config.h"
#include "Utilities/File.h"
#include "Utilities/stack_trace.h"

#include "libretro_audio.h"
#include "libretro_input.h"
#include "libretro_video.h"
#include "libretro_firmware.h"
#include "libretro_pad_handler.h"

#include <clocale>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

LOG_CHANNEL(libretro_log, "LIBRETRO");

// Libretro callbacks
static retro_environment_t environ_cb = nullptr;
static retro_video_refresh_t video_cb = nullptr;
static retro_audio_sample_t audio_cb = nullptr;
static retro_audio_sample_batch_t audio_batch_cb = nullptr;
static retro_input_poll_t input_poll_cb = nullptr;
static retro_input_state_t input_state_cb = nullptr;
static retro_log_printf_t log_cb = nullptr;

// Hardware render callback for OpenGL
static retro_hw_render_callback hw_render;

// Core state
static bool core_initialized = false;
static bool game_loaded = false;
static bool pending_game_boot = false;
static std::string game_path;
static std::string system_dir;
static std::string save_dir;

// Pad thread instance for libretro input
static std::unique_ptr<pad_thread> g_libretro_pad_thread;

// Libretro pad handler instance
static std::shared_ptr<LibretroPadHandler> g_libretro_pad_handler;

// Pause watchdog: RetroArch may stop calling retro_run() when paused.
// We pause/resume RPCS3 from a small watchdog thread based on retro_run call gaps.
static std::atomic<bool> s_pause_watchdog_running{false};
static std::atomic<bool> s_pause_watchdog_stop{false};
static std::atomic<bool> s_paused_by_watchdog{false};
static std::atomic<long long> s_last_retro_run_us{0};
static std::thread s_pause_watchdog_thread;

// Thread synchronization
static std::mutex emu_mutex;
static std::atomic<bool> frame_ready{false};

#ifndef RPCS3_LIBRETRO_CORE_TRACE
#define RPCS3_LIBRETRO_CORE_TRACE 1
#endif

static inline unsigned long long lr_core_tid_hash()
{
    return static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

static inline long long lr_now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void stop_pause_watchdog()
{
    if (!s_pause_watchdog_running.exchange(false))
        return;

    s_pause_watchdog_stop.store(true);
    if (s_pause_watchdog_thread.joinable())
    {
        s_pause_watchdog_thread.join();
    }
    s_pause_watchdog_stop.store(false);
    s_paused_by_watchdog.store(false);
    s_last_retro_run_us.store(0);
}

static void start_pause_watchdog()
{
    if (s_pause_watchdog_running.exchange(true))
        return;

    s_pause_watchdog_stop.store(false);
    s_paused_by_watchdog.store(false);
    s_last_retro_run_us.store(lr_now_us());

    s_pause_watchdog_thread = std::thread([]()
    {
        constexpr long long pause_threshold_us = 100'000;
        constexpr long long resume_threshold_us = 40'000;

        while (!s_pause_watchdog_stop.load())
        {
            if (!core_initialized || !game_loaded)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            const long long now_us = lr_now_us();
            const long long last_us = s_last_retro_run_us.load();
            if (last_us == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            const long long gap_us = now_us - last_us;

            if (gap_us > pause_threshold_us)
            {
                if (!s_paused_by_watchdog.load() && Emu.IsRunning())
                {
                    Emu.Pause(false, false);
                    s_paused_by_watchdog.store(true);
                    if (log_cb)
                        log_cb(RETRO_LOG_INFO, "[PAUSE] Watchdog pausing RPCS3 (gap=%lldus)\n", gap_us);
                }
            }
            else if (gap_us < resume_threshold_us)
            {
                if (s_paused_by_watchdog.load() && Emu.IsPaused())
                {
                    Emu.Resume();
                    s_paused_by_watchdog.store(false);
                    if (log_cb)
                        log_cb(RETRO_LOG_INFO, "[PAUSE] Watchdog resuming RPCS3 (gap=%lldus)\n", gap_us);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
}

static void libretro_apply_core_options()
{
    if (!environ_cb)
        return;

    // Frame limiting: In libretro, the frontend typically controls pacing.
    // Default to disabling RPCS3's internal limiter unless user explicitly requests otherwise.
    retro_variable var{};
    var.key = "rpcs3_frame_limit";
    var.value = nullptr;

    std::string_view limit = "Auto";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        limit = var.value;
    }

    // Disable RPCS3 internal vsync in libretro to avoid double-sync.
    // RetroArch controls presentation timing.
    g_cfg.video.vsync.set(false);

    if (limit == "Off")
    {
        // Hard disable any internal frame limiting
        g_disable_frame_limit = true;
        g_cfg.video.frame_limit.set(frame_limit_type::none);
        g_cfg.video.second_frame_limit.set(0.0);
    }
    else if (limit == "30")
    {
        g_disable_frame_limit = false;
        g_cfg.video.frame_limit.set(frame_limit_type::_30);
        g_cfg.video.second_frame_limit.set(0.0);
    }
    else if (limit == "60")
    {
        g_disable_frame_limit = false;
        g_cfg.video.frame_limit.set(frame_limit_type::_60);
        g_cfg.video.second_frame_limit.set(0.0);
    }
    else if (limit == "120")
    {
        g_disable_frame_limit = false;
        g_cfg.video.frame_limit.set(frame_limit_type::_120);
        g_cfg.video.second_frame_limit.set(0.0);
    }
    else
    {
        // Auto: disable limiter in libretro by default.
        // (If we later want dynamic rate control, RetroArch handles it.)
        g_disable_frame_limit = true;
        g_cfg.video.frame_limit.set(frame_limit_type::none);
        g_cfg.video.second_frame_limit.set(0.0);
    }

    // Avoid additional CPU-throttling heuristics that can cause “real FPS” to tank.
    // In libretro, keep this off unless user requests otherwise.
    g_cfg.core.max_cpu_preempt_count_per_frame.set(0);

    if (log_cb)
    {
        log_cb(RETRO_LOG_INFO, "Applied core options: rpcs3_frame_limit=%.*s g_disable_frame_limit=%d frame_limit=%d vsync=%d\n",
            static_cast<int>(limit.size()), limit.data(),
            g_disable_frame_limit.load() ? 1 : 0,
            static_cast<int>(g_cfg.video.frame_limit.get()),
            g_cfg.video.vsync.get() ? 1 : 0);
    }
}

#if RPCS3_LIBRETRO_CORE_TRACE
#define LRCORE_LOG(level, fmt, ...) do { if (log_cb) log_cb(level, "[LRCORE][tid=%llx] " fmt "\n", lr_core_tid_hash() __VA_OPT__(,) __VA_ARGS__); } while (0)
#else
#define LRCORE_LOG(...) do { } while (0)
#endif

// Forward declarations
static void init_emu_callbacks();
static void context_reset();
static void context_destroy();

static void libretro_apply_core_options();

#ifdef _WIN32
static void* g_lrcore_vectored_handler = nullptr;

static std::string lrcore_build_crash_log_path()
{
    std::string base;
    if (!save_dir.empty())
    {
        base = save_dir;
    }
    else if (!system_dir.empty())
    {
        base = system_dir;
    }
    else
    {
        base = ".";
    }
    return base + "/rpcs3_libretro_crash.log";
}

static void lrcore_write_crash_report(const std::string& header, const std::vector<std::string>& lines)
{
    const std::string path = lrcore_build_crash_log_path();
    const std::string dir = fs::get_parent_dir(path, 1);
    if (!dir.empty())
    {
        fs::create_path(dir);
    }
    std::string text;
    text.reserve(header.size() + 1 + lines.size() * 64);
    text += header;
    text += '\n';
    for (const auto& line : lines)
    {
        text += line;
        text += '\n';
    }
    fs::file f(path, fs::rewrite);
    if (f)
    {
        f.write(text);
        f.sync();
    }
}

static bool lrcore_is_fatal_exception(DWORD code)
{
    // NOTE: EXCEPTION_ACCESS_VIOLATION (0xc0000005) is NOT fatal in RPCS3!
    // It's expected during normal operation for VM memory mapping/signal handling.
    // Only log truly fatal exceptions that RPCS3 can't recover from.
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:         // 0xC0000005 - Expected for VM memory access
    case EXCEPTION_IN_PAGE_ERROR:            // 0xC0000006 - Expected for VM paging
        return false;  // Let RPCS3's internal handlers deal with these
    case EXCEPTION_STACK_OVERFLOW:           // 0xC00000FD
    case EXCEPTION_ILLEGAL_INSTRUCTION:      // 0xC000001D
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       // 0xC0000094
    case EXCEPTION_INT_OVERFLOW:             // 0xC0000095
    case EXCEPTION_PRIV_INSTRUCTION:         // 0xC0000096
    case EXCEPTION_INVALID_HANDLE:           // 0xC0000008
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    // 0xC000008C
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       // 0xC000008E
    case EXCEPTION_FLT_OVERFLOW:             // 0xC0000091
    case EXCEPTION_FLT_STACK_CHECK:          // 0xC0000092
    case EXCEPTION_FLT_UNDERFLOW:            // 0xC0000093
        return true;
    default:
        return false;
    }
}

static LONG CALLBACK lrcore_vectored_exception_handler(PEXCEPTION_POINTERS info)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!lrcore_is_fatal_exception(code))
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const void* address = info->ExceptionRecord->ExceptionAddress;
    LRCORE_LOG(RETRO_LOG_ERROR, "FATAL EXCEPTION code=0x%08lx address=%p", static_cast<unsigned long>(code), address);
    const auto stack = utils::get_backtrace_from_context(info->ContextRecord, 256);
    const auto lines = utils::get_backtrace_symbols(stack);
    for (usz i = 0; i < lines.size(); ++i)
    {
        LRCORE_LOG(RETRO_LOG_ERROR, "Crash frame %u: %s", static_cast<unsigned>(i), lines[i].c_str());
    }
    const std::string header = fmt::format("RPCS3 libretro crash: code=0x%08lx address=%p", static_cast<unsigned long>(code), address);
    lrcore_write_crash_report(header, lines);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void lrcore_install_crash_handler()
{
    if (g_lrcore_vectored_handler)
    {
        return;
    }
    g_lrcore_vectored_handler = AddVectoredExceptionHandler(1, lrcore_vectored_exception_handler);
    LRCORE_LOG(RETRO_LOG_INFO, "Installed vectored exception handler: %p", g_lrcore_vectored_handler);
}

static void lrcore_uninstall_crash_handler()
{
    LRCORE_LOG(RETRO_LOG_INFO, "Uninstalling vectored exception handler: %p", g_lrcore_vectored_handler);
    if (g_lrcore_vectored_handler)
    {
        RemoveVectoredExceptionHandler(g_lrcore_vectored_handler);
        g_lrcore_vectored_handler = nullptr;
    }
}
#endif

namespace
{
    struct libretro_log_listener : public logs::listener
    {
        void log(u64 /*stamp*/, const logs::message& msg, std::string_view prefix, std::string_view text) override
        {
            if (!log_cb)
                return;

            retro_log_level retro_level = RETRO_LOG_INFO;
            logs::level sev = static_cast<logs::level>(msg);
            switch (sev)
            {
            case logs::level::always:
            case logs::level::fatal:
            case logs::level::error:
                retro_level = RETRO_LOG_ERROR;
                break;
            case logs::level::todo:
            case logs::level::success:
            case logs::level::warning:
                retro_level = RETRO_LOG_WARN;
                break;
            case logs::level::notice:
            case logs::level::trace:
                retro_level = RETRO_LOG_INFO;
                break;
            }

            log_cb(retro_level, "[%.*s] %.*s\n",
                static_cast<int>(prefix.size()), prefix.data(),
                static_cast<int>(text.size()), text.data());
        }
    };

    libretro_log_listener g_libretro_logs;
}

static std::string find_firmware_pup()
{
    if (system_dir.empty())
        return {};

    const std::string cand1 = system_dir + "/rpcs3/PS3UPDAT.PUP";
    const std::string cand2 = system_dir + "/PS3UPDAT.PUP";
    const std::string cand3 = system_dir + "/rpcs3/firmware/PS3UPDAT.PUP";

    if (fs::is_file(cand1)) return cand1;
    if (fs::is_file(cand2)) return cand2;
    if (fs::is_file(cand3)) return cand3;

    return {};
}

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    // Get log interface
    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
    {
        log_cb = logging.log;
    }

    // Core options
    static const struct retro_variable vars[] = {
        { "rpcs3_renderer", "Renderer; opengl|null" },
        { "rpcs3_resolution", "Internal Resolution; 1280x720|1920x1080|2560x1440|3840x2160" },
        { "rpcs3_frame_limit", "Frame Limit; Auto|Off|30|60|120" },
        { "rpcs3_ppu_decoder", "PPU Decoder; Recompiler (LLVM)|Interpreter" },
        { "rpcs3_spu_decoder", "SPU Decoder; Recompiler (LLVM)|Recompiler (ASMJIT)|Interpreter" },
        { nullptr, nullptr }
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);

    // We don't support no-game
    bool support_no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &support_no_game);

    // Set up input descriptors for PS3 controller mappings
    // This tells RetroArch what buttons map to which PS3 buttons
    libretro_input_set_descriptors(cb);

    // Set up controller info so RetroArch knows what controllers we support
    libretro_input_set_controller_info(cb);

    // Enable joypad bitmasks if supported by frontend (more reliable button polling)
    bool bitmasks_supported = false;
    if (cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, &bitmasks_supported))
    {
        libretro_input_set_bitmask_supported(bitmasks_supported);
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "Input bitmasks supported: %d\n", bitmasks_supported ? 1 : 0);
    }

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Input descriptors and controller info set up\n");

    // Apply defaults for core options early
    libretro_apply_core_options();
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
    video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
    audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
    audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
    input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
    input_state_cb = cb;
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info* info)
{
    info->library_name = "RPCS3";
    info->library_version = "0.0.1";
    info->valid_extensions = "bin|self|elf|pkg|iso";
    info->need_fullpath = true;
    info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info* info)
{
    info->geometry.base_width = 1280;
    info->geometry.base_height = 720;
    info->geometry.max_width = 3840;
    info->geometry.max_height = 2160;
    info->geometry.aspect_ratio = 16.0f / 9.0f;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 48000.0;
}

// Forward declarations
static bool setup_hw_render();

void retro_init(void)
{
    if (core_initialized)
        return;

    LRCORE_LOG(RETRO_LOG_INFO, "retro_init enter core_initialized=%d", core_initialized ? 1 : 0);

#ifdef _WIN32
    lrcore_install_crash_handler();
#endif

    // Get system directory first
    const char* sys_dir = nullptr;
    if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_dir) && sys_dir)
    {
        system_dir = sys_dir;
    }

    // Get save directory
    const char* sav_dir = nullptr;
    if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &sav_dir) && sav_dir)
    {
        save_dir = sav_dir;
    }

    // Forward RPCS3 internal logs (including firmware installer logs) to libretro logger
    static bool s_logs_hooked = false;
    static std::unique_ptr<logs::listener> s_file_logger;
    if (!s_logs_hooked)
    {
        logs::listener::add(&g_libretro_logs);
        s_logs_hooked = true;

        // Also create a native RPCS3 file logger for detailed debugging
        // This captures all internal RPCS3 logs that may not be forwarded to RetroArch
        if (!save_dir.empty())
        {
            const std::string log_path = save_dir + "/rpcs3_detailed.log";
            s_file_logger = logs::make_file_listener(log_path, 100 * 1024 * 1024); // 100MB max
            if (log_cb)
                log_cb(RETRO_LOG_INFO, "RPCS3 detailed logging enabled: %s\n", log_path.c_str());
        }
    }

    // Initialize locale
    std::setlocale(LC_ALL, "C");

    // Set the emulator directory to RetroArch system/rpcs3/
    // This is where firmware (dev_flash) and other RPCS3 data should be stored
    std::string emu_dir = system_dir + "/rpcs3/";
    g_cfg_vfs.emulator_dir.from_string(emu_dir);

    // Ensure RPCS3 can reload the correct EmulatorDir inside Emu.Init()/BootGame.
    // Emu.Init() resets g_cfg_vfs and then loads vfs.yml from fs::get_config_dir(true).
    // If vfs.yml is missing, RPCS3 falls back to RetroArch root, which breaks /dev_flash.
    if (!fs::is_file(cfg_vfs::get_path()))
    {
        g_cfg_vfs.save();
    }

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "RPCS3 emulator directory: %s\n", emu_dir.c_str());

    if (!libretro_is_firmware_installed())
    {
        const std::string pup_path = find_firmware_pup();
        if (pup_path.empty())
        {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR, "PS3 firmware not installed. Place PS3UPDAT.PUP in: %s\n", (system_dir + "/rpcs3/").c_str());
        }
        else
        {
            if (log_cb)
                log_cb(RETRO_LOG_INFO, "Installing PS3 firmware from: %s\n", pup_path.c_str());

            if (!g_fxo->is_init())
            {
                g_fxo->reset();
            }

            bool ok = false;
            try
            {
                ok = libretro_install_firmware(pup_path, [](int cur, int total)
                {
                    if (log_cb)
                        log_cb(RETRO_LOG_INFO, "Firmware install progress: %d/%d\n", cur, total);
                });
            }
            catch (const std::exception& e)
            {
                if (log_cb)
                {
                    log_cb(RETRO_LOG_ERROR, "Firmware installation threw exception: %s\n", e.what());
                }
                ok = false;
            }
            catch (...)
            {
                if (log_cb)
                    log_cb(RETRO_LOG_ERROR, "Firmware installation threw unknown exception\n");
                ok = false;
            }

            if (log_cb)
                log_cb(ok ? RETRO_LOG_INFO : RETRO_LOG_ERROR, ok ? "Firmware installation finished\n" : "Firmware installation failed\n");
        }
    }
    else
    {
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "PS3 firmware already installed (version: %s)\n", libretro_get_firmware_version().c_str());
    }

    // Initialize the emulator
    Emu.SetHasGui(false);
    Emu.SetUsr("00000001");
    Emu.Init();

    LRCORE_LOG(RETRO_LOG_INFO, "Emu.Init done IsRunning=%d IsReady=%d IsStopped=%d", Emu.IsRunning() ? 1 : 0, Emu.IsReady() ? 1 : 0, Emu.IsStopped() ? 1 : 0);

    // Set up callbacks
    init_emu_callbacks();

    // Set up hardware rendering (OpenGL context)
    if (!setup_hw_render())
    {
        if (log_cb)
            log_cb(RETRO_LOG_WARN, "Hardware rendering not available, will use software rendering\n");
    }

    core_initialized = true;

    LRCORE_LOG(RETRO_LOG_INFO, "retro_init done core_initialized=%d", core_initialized ? 1 : 0);

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "RPCS3 libretro core initialized\n");
}

void retro_deinit(void)
{
    if (!core_initialized)
        return;

    stop_pause_watchdog();

    LRCORE_LOG(RETRO_LOG_INFO, "retro_deinit enter game_loaded=%d pending_game_boot=%d", game_loaded ? 1 : 0, pending_game_boot ? 1 : 0);

    if (game_loaded)
    {
        Emu.GracefulShutdown(false, false);
    }

    Emulator::CleanUp();
    core_initialized = false;
    game_loaded = false;

    LRCORE_LOG(RETRO_LOG_INFO, "retro_deinit done core_initialized=%d game_loaded=%d", core_initialized ? 1 : 0, game_loaded ? 1 : 0);

#ifdef _WIN32
    lrcore_uninstall_crash_handler();
#endif

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "RPCS3 libretro core deinitialized\n");
}

static bool do_boot_game();

static void context_reset()
{
    LRCORE_LOG(RETRO_LOG_INFO, "context_reset enter pending_game_boot=%d game_loaded=%d", pending_game_boot ? 1 : 0, game_loaded ? 1 : 0);
    LRCORE_LOG(RETRO_LOG_INFO, "context_reset hw_render.get_current_framebuffer=%p hw_render.get_proc_address=%p", reinterpret_cast<void*>(hw_render.get_current_framebuffer), reinterpret_cast<void*>(hw_render.get_proc_address));

    // Initialize libretro video with the new context
    libretro_video_init(hw_render.get_current_framebuffer, hw_render.get_proc_address);

    LRCORE_LOG(RETRO_LOG_INFO, "context_reset after libretro_video_init");

    // Now that the GL context is ready, boot the game if pending
    if (pending_game_boot && !game_loaded)
    {
        LRCORE_LOG(RETRO_LOG_INFO, "context_reset booting pending game");
        if (do_boot_game())
        {
            game_loaded = true;
            start_pause_watchdog();
        }
        pending_game_boot = false;
    }

    LRCORE_LOG(RETRO_LOG_INFO, "context_reset exit pending_game_boot=%d game_loaded=%d", pending_game_boot ? 1 : 0, game_loaded ? 1 : 0);
}

static void context_destroy()
{
    LRCORE_LOG(RETRO_LOG_INFO, "context_destroy enter");

    libretro_video_deinit();

    LRCORE_LOG(RETRO_LOG_INFO, "context_destroy exit");
}

static bool setup_hw_render()
{
    // Request OpenGL Core profile context
    // version_major/minor specifies MINIMUM required version - RetroArch will provide
    // the highest available context that meets this minimum requirement.
    // RPCS3's OpenGL backend requires 4.3+ for compute shaders and modern features.
    hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
    hw_render.version_major = 4;
    hw_render.version_minor = 3;
    hw_render.context_reset = context_reset;
    hw_render.context_destroy = context_destroy;
    hw_render.depth = true;
    hw_render.stencil = true;
    hw_render.bottom_left_origin = true;
    hw_render.cache_context = true;
    hw_render.debug_context = false;

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Requesting OpenGL Core context (minimum 4.3)\n");

    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "OpenGL Core 4.3+ context request accepted\n");
        return true;
    }

    // Fallback: try OpenGL Core 3.3 (may lack some features but could work on older systems)
    if (log_cb)
        log_cb(RETRO_LOG_WARN, "OpenGL Core 4.3 not available, trying 3.3...\n");

    hw_render.version_major = 3;
    hw_render.version_minor = 3;

    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "OpenGL Core 3.3+ context request accepted\n");
        return true;
    }

    // Final fallback: try legacy OpenGL compatibility context
    if (log_cb)
        log_cb(RETRO_LOG_WARN, "OpenGL Core not available, trying compatibility profile...\n");

    hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
    hw_render.version_major = 3;
    hw_render.version_minor = 0;

    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "OpenGL compatibility context request accepted\n");
        return true;
    }

    if (log_cb)
        log_cb(RETRO_LOG_ERROR, "Failed to set HW render callback - no suitable OpenGL context available\n");
    return false;
}

bool retro_load_game(const struct retro_game_info* game)
{
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "retro_load_game called\n");

    if (!game || !game->path)
    {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "No game info or path provided\n");
        return false;
    }

    // Check if frontend supports frame duping (passing NULL to video_cb to reuse last frame)
    bool can_dupe = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe) && can_dupe)
    {
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "Frame duping supported by frontend\n");
    }
    else
    {
        if (log_cb)
            log_cb(RETRO_LOG_WARN, "Frame duping NOT supported by frontend - may affect performance\n");
    }

    game_path = game->path;
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Game path: %s\n", game_path.c_str());

    // Use OpenGL renderer - game boot will be deferred until context_reset() when GL context is ready
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Using OpenGL renderer\n");
    g_cfg.video.renderer.set(video_renderer::opengl);

    // Configure PPU decoder - use LLVM for best performance
    g_cfg.core.ppu_decoder.set(ppu_decoder_type::llvm);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "PPU decoder set to LLVM recompiler\n");

    // Configure SPU decoder - use LLVM for best performance
    g_cfg.core.spu_decoder.set(spu_decoder_type::llvm);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "SPU decoder set to LLVM recompiler\n");

    // Performance optimizations
    g_cfg.core.spu_loop_detection.set(true);  // Faster SPU loops
    g_cfg.core.llvm_threads.set(std::thread::hardware_concurrency());  // Use all CPU cores for LLVM compilation
    g_cfg.core.llvm_precompilation.set(true);  // Precompile LLVM modules
    g_cfg.video.multithreaded_rsx.set(true);  // Multi-threaded RSX
    g_cfg.video.disable_vertex_cache.set(false);  // Keep vertex cache enabled

    // Shader compilation optimizations
    g_cfg.video.shadermode.set(shader_mode::async_recompiler);  // Async multi-threaded shader compilation
    g_cfg.video.shader_compiler_threads_count.set(0);  // 0 = auto (optimal for CPU)
    g_cfg.video.disable_on_disk_shader_cache.set(false);  // Keep shader cache enabled for faster subsequent loads

    // RSX optimizations
    g_cfg.video.relaxed_zcull_sync.set(true);  // Relaxed ZCULL for better performance
    g_cfg.video.strict_rendering_mode.set(false);  // Disable strict mode for better performance
    g_cfg.video.disable_FIFO_reordering.set(false);  // Keep FIFO reordering enabled

    // Audio optimizations
    g_cfg.audio.enable_buffering.set(true);  // Enable audio buffering
    g_cfg.audio.desired_buffer_duration.set(100);  // 100ms buffer for smoother audio in libretro
    g_cfg.audio.enable_time_stretching.set(false);  // Disable time stretching (RetroArch handles sync)

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Applied performance optimizations (LLVM threads=%u, shader_threads=auto, async_shaders=true, relaxed_zcull=true, audio_buffer=100ms)\n", std::thread::hardware_concurrency());

    // Configure audio - set explicit stereo layout to avoid "Unsupported layout 0" error
    // (audio_channel_layout::automatic = 0 is not handled by default_layout_channel_count)
    g_cfg.audio.channel_layout.set(audio_channel_layout::stereo);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Audio channel layout set to stereo\n");

    // Disable RPCS3's native UI/overlay system completely for libretro.
    // This prevents the overlay manager from being created, which would try to load
    // icon files that don't exist and cause texture creation errors with 0x0 dimensions.
    // RetroArch has its own overlay system.
    g_cfg.misc.use_native_interface.set(false);
    g_cfg.misc.show_shader_compilation_hint.set(false);
    g_cfg.misc.show_ppu_compilation_hint.set(false);
    g_cfg.misc.show_autosave_autoload_hint.set(false);
    g_cfg.misc.show_pressure_intensity_toggle_hint.set(false);
    g_cfg.misc.show_trophy_popups.set(false);
    g_cfg.misc.show_rpcn_popups.set(false);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Disabled RPCS3 native UI for libretro\n");

    // Save config so BootGame() loads the correct settings when it reloads config.yml
    // Note: RPCS3 loads config from fs::get_config_dir(true) which adds "config/" subdirectory
    const std::string config_path = fs::get_config_dir(true) + "config.yml";
    g_cfg.save(config_path);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Saved libretro config to: %s\n", config_path.c_str());

    // For null renderer, boot immediately. For OpenGL, defer until context_reset()
    if (g_cfg.video.renderer.get() == video_renderer::null)
    {
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "Null renderer - booting game immediately\n");
        if (!do_boot_game())
        {
            return false;
        }
        game_loaded = true;
        start_pause_watchdog();
    }
    else
    {
        // Defer game boot until context_reset() when GL context is ready
        pending_game_boot = true;
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "Game boot deferred until GL context is ready\n");
    }

    return true;
}

// Actually boot the game - called from context_reset() when GL context is ready
static bool do_boot_game()
{
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Attempting to boot game...\n");

    // Ensure /dev_flash points to RetroArch system/rpcs3/ so the installed firmware is visible during boot
    if (!system_dir.empty())
    {
        const std::string emu_dir = system_dir + "/rpcs3/";
        g_cfg_vfs.emulator_dir.from_string(emu_dir);
        vfs::mount("/dev_flash", g_cfg_vfs.get_dev_flash());
    }

    // Initialize pad_thread before booting - cellPadInit requires this
    if (!g_libretro_pad_thread)
    {
        g_libretro_pad_thread = std::make_unique<pad_thread>(nullptr, nullptr, "");
        g_libretro_pad_thread->Init();

        // Create and initialize the libretro pad handler if not already done
        if (!g_libretro_pad_handler)
        {
            g_libretro_pad_handler = std::make_shared<LibretroPadHandler>();
            g_libretro_pad_handler->Init();
        }

        // Bind pads to the handler
        const auto& pads = g_libretro_pad_thread->GetPads();
        for (const auto& pad : pads)
        {
            if (pad)
            {
                g_libretro_pad_handler->bindPadToDevice(pad);
            }
        }

        if (log_cb)
            log_cb(RETRO_LOG_INFO, "Initialized pad_thread with LibretroPadHandler (%zu pads bound)\n", pads.size());
    }

    game_boot_result result = game_boot_result::generic_error;
    try
    {
        Emu.SetForceBoot(true);
        result = Emu.BootGame(game_path);
    }
    catch (const std::exception& e)
    {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "Exception during game boot: %s\n", e.what());
        return false;
    }
    catch (...)
    {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "Unknown exception during game boot\n");
        return false;
    }

    if (result != game_boot_result::no_errors)
    {
        const char* error_str = "unknown";
        switch (result)
        {
        case game_boot_result::generic_error: error_str = "generic_error"; break;
        case game_boot_result::nothing_to_boot: error_str = "nothing_to_boot"; break;
        case game_boot_result::wrong_disc_location: error_str = "wrong_disc_location"; break;
        case game_boot_result::invalid_file_or_folder: error_str = "invalid_file_or_folder"; break;
        case game_boot_result::invalid_bdvd_folder: error_str = "invalid_bdvd_folder"; break;
        case game_boot_result::install_failed: error_str = "install_failed"; break;
        case game_boot_result::decryption_error: error_str = "decryption_error"; break;
        case game_boot_result::file_creation_error: error_str = "file_creation_error"; break;
        case game_boot_result::firmware_missing: error_str = "firmware_missing"; break;
        case game_boot_result::firmware_version: error_str = "firmware_version"; break;
        case game_boot_result::unsupported_disc_type: error_str = "unsupported_disc_type"; break;
        case game_boot_result::savestate_corrupted: error_str = "savestate_corrupted"; break;
        case game_boot_result::savestate_version_unsupported: error_str = "savestate_version_unsupported"; break;
        case game_boot_result::still_running: error_str = "still_running"; break;
        case game_boot_result::already_added: error_str = "already_added"; break;
        case game_boot_result::currently_restricted: error_str = "currently_restricted"; break;
        default: break;
        }
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "Failed to boot game: %s (error: %s)\n", game_path.c_str(), error_str);
        return false;
    }

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Game loaded: %s\n", game_path.c_str());

    // Log initial state after boot
    if (log_cb)
    {
        system_state state = Emu.GetStatus();
        log_cb(RETRO_LOG_INFO, "Emulator state after boot: state=%d (IsRunning=%d, IsReady=%d, IsStopped=%d)\n",
            static_cast<int>(state), Emu.IsRunning() ? 1 : 0, Emu.IsReady() ? 1 : 0, Emu.IsStopped() ? 1 : 0);
    }

    // Wait for emulator to transition out of loading/starting states
    // Poll for up to 30 seconds
    constexpr int max_wait_ms = 30000;
    constexpr int poll_interval_ms = 100;
    int waited_ms = 0;

    // Wait while in loading or starting state
    while (waited_ms < max_wait_ms)
    {
        system_state state = Emu.GetStatus();

        // If running, paused, or ready - we can proceed
        if (state == system_state::running || state == system_state::paused ||
            state == system_state::ready || state == system_state::frozen)
        {
            if (log_cb)
                log_cb(RETRO_LOG_INFO, "Emulator reached usable state=%d after %dms\n", static_cast<int>(state), waited_ms);
            break;
        }

        // If stopped, boot failed
        if (state == system_state::stopped)
        {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR, "Emulator stopped unexpectedly\n");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
        waited_ms += poll_interval_ms;

        if (log_cb && (waited_ms % 1000 == 0))
        {
            log_cb(RETRO_LOG_INFO, "Waiting for emulator... %dms state=%d\n", waited_ms, static_cast<int>(state));
        }
    }

    system_state final_state = Emu.GetStatus();
    if (log_cb)
    {
        log_cb(RETRO_LOG_INFO, "Emulator state after wait: state=%d (waited %dms)\n",
            static_cast<int>(final_state), waited_ms);
    }

    // If ready but not running, start it
    if (final_state == system_state::ready)
    {
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "Starting emulation manually...\n");
        Emu.Run(true);
    }
    // If paused, resume
    else if (final_state == system_state::paused || final_state == system_state::frozen)
    {
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "Resuming paused emulation...\n");
        Emu.Resume();
    }
    // If still in starting state after timeout, force transition to running
    else if (final_state == system_state::starting)
    {
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "Emulator stuck in starting state, calling FinalizeRunRequest()...\n");
        Emu.FinalizeRunRequest();
    }

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "Final state: %d (IsRunning=%d)\n", static_cast<int>(Emu.GetStatus()), Emu.IsRunning() ? 1 : 0);

    // Start pause watchdog once the emulator is running so RetroArch pause fully pauses emulation.
    // (This is safe to call multiple times.)
    start_pause_watchdog();

    return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
    (void)game_type;
    (void)info;
    (void)num_info;
    return false;
}

void retro_unload_game(void)
{
    LRCORE_LOG(RETRO_LOG_INFO, "retro_unload_game enter game_loaded=%d", game_loaded ? 1 : 0);
    stop_pause_watchdog();
    if (game_loaded)
    {
        Emu.GracefulShutdown(false, false);
        game_loaded = false;
        game_path.clear();
    }
    LRCORE_LOG(RETRO_LOG_INFO, "retro_unload_game exit game_loaded=%d", game_loaded ? 1 : 0);
}

void retro_run(void)
{
    if (!game_loaded)
        return;

    static u64 s_run_counter = 0;
    s_run_counter++;

    // Update watchdog timestamp.
    s_last_retro_run_us.store(lr_now_us());

    if (log_cb && (s_run_counter <= 300 || (s_run_counter % 60u) == 0u))
    {
        const uintptr_t fbo = libretro_get_current_framebuffer();
        LRCORE_LOG(RETRO_LOG_INFO, "retro_run #%llu Emu(IsRunning=%d IsReady=%d IsStopped=%d) video_cb=%p fbo=0x%llx", static_cast<unsigned long long>(s_run_counter), Emu.IsRunning() ? 1 : 0, Emu.IsReady() ? 1 : 0, Emu.IsStopped() ? 1 : 0, reinterpret_cast<void*>(video_cb), static_cast<unsigned long long>(fbo));
    }

    // Check for variable updates
    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
    {
        libretro_apply_core_options();
    }

    // Poll input
    input_poll_cb();
    libretro_input_poll(input_state_cb);

    // Debug: Log input state periodically (all ports)
    if (log_cb && (s_run_counter % 120u) == 0u)
    {
        for (unsigned port = 0; port < LIBRETRO_MAX_PADS; port++)
        {
            const auto& state = libretro_input_get_state(port);
            bool any_pressed = false;
            for (int i = 0; i <= 15; i++)
            {
                if (state.buttons[i])
                    any_pressed = true;
            }

            if (any_pressed || state.analog[0] != 0 || state.analog[1] != 0 || state.analog[2] != 0 || state.analog[3] != 0)
            {
                log_cb(RETRO_LOG_INFO, "[INPUT][port=%u] B=%d Y=%d SEL=%d STA=%d UP=%d DN=%d LT=%d RT=%d A=%d X=%d L=%d R=%d L2=%d R2=%d L3=%d R3=%d LX=%d LY=%d RX=%d RY=%d\n",
                    port,
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_B],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_Y],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_SELECT],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_START],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_UP],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_DOWN],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_LEFT],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_RIGHT],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_A],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_X],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_L],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_R],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_L2],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_R2],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_L3],
                    state.buttons[RETRO_DEVICE_ID_JOYPAD_R3],
                    state.analog[0], state.analog[1], state.analog[2], state.analog[3]);
            }
        }
    }

    // Process pad handler to update RPCS3's pad states from libretro input
    if (g_libretro_pad_handler)
    {
        g_libretro_pad_handler->process();
    }

    // Copy button values from m_buttons to m_buttons_external so cellPad can read them
    if (g_libretro_pad_thread)
    {
        g_libretro_pad_thread->apply_copilots();
    }

    // Process audio
    libretro_audio_process(audio_batch_cb);

    // Clean up GL state before returning control to frontend
    // Per libretro docs: cores must unbind all GL resources before video_cb
    // This prevents state conflicts between RSX rendering and RetroArch's rendering
    libretro_cleanup_gl_state();

    // Ensure RSX shared-context GPU work is ordered before RetroArch presents.
    libretro_wait_for_present_fence();

    // CRITICAL FIX: Only present frames when RSX has actually produced a new one
    // This prevents showing the same stale frame repeatedly, which causes flickering/flashing
    // Check if RSX has rendered a new frame since our last presentation
    const bool has_new_frame = libretro_has_new_frame();

    if (log_cb && (s_run_counter <= 300 || (s_run_counter % 60u) == 0u))
    {
        LRCORE_LOG(RETRO_LOG_INFO, "retro_run #%llu has_new_frame=%d", static_cast<unsigned long long>(s_run_counter), has_new_frame ? 1 : 0);
    }

    if (has_new_frame)
    {
        // New frame available - blit from RSX's shared texture to RetroArch's FBO, then present
        // CRITICAL: FBOs are NOT shared between GL contexts, so RSX renders to a shared texture,
        // and we blit that texture to RetroArch's actual FBO here on the main thread.
        libretro_blit_to_frontend();

        if (log_cb && (s_run_counter <= 300 || (s_run_counter % 60u) == 0u))
        {
            LRCORE_LOG(RETRO_LOG_INFO, "retro_run #%llu video_cb(RETRO_HW_FRAME_BUFFER_VALID) - presenting new frame after blit", static_cast<unsigned long long>(s_run_counter));
        }
        video_cb(RETRO_HW_FRAME_BUFFER_VALID, 1280, 720, 0);
        libretro_mark_frame_presented();
    }
    else
    {
        // No new frame - tell RetroArch to reuse the previous frame (frame duping)
        // This is the correct behavior per libretro spec when no new frame is available
        if (log_cb && (s_run_counter <= 300 || (s_run_counter % 60u) == 0u))
        {
            LRCORE_LOG(RETRO_LOG_INFO, "retro_run #%llu video_cb(NULL) - reusing previous frame", static_cast<unsigned long long>(s_run_counter));
        }
        video_cb(NULL, 1280, 720, 0);
    }

    if (log_cb && (s_run_counter <= 300 || (s_run_counter % 60u) == 0u))
    {
        LRCORE_LOG(RETRO_LOG_INFO, "retro_run #%llu exit", static_cast<unsigned long long>(s_run_counter));
    }
}

void retro_reset(void)
{
    if (game_loaded)
    {
        Emu.Restart();
    }
}

size_t retro_serialize_size(void)
{
    // Save states not fully implemented yet
    return 0;
}

bool retro_serialize(void* data, size_t size)
{
    (void)data;
    (void)size;
    return false;
}

bool retro_unserialize(const void* data, size_t size)
{
    (void)data;
    (void)size;
    return false;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
    (void)index;
    (void)enabled;
    (void)code;
}

unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

void* retro_get_memory_data(unsigned id)
{
    (void)id;
    return nullptr;
}

size_t retro_get_memory_size(unsigned id)
{
    (void)id;
    return 0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    libretro_input_set_controller(port, device);
}

// Initialize emulator callbacks for libretro integration
static void init_emu_callbacks()
{
    EmuCallbacks callbacks{};

    callbacks.call_from_main_thread = [](std::function<void()> func, atomic_t<u32>* wake_up)
    {
        // In libretro context, we're single-threaded for the main loop
        // Just execute directly
        func();
        if (wake_up)
        {
            *wake_up = true;
            wake_up->notify_one();
        }
    };

    callbacks.try_to_quit = [](bool force_quit, std::function<void()> on_exit) -> bool
    {
        if (force_quit && on_exit)
        {
            on_exit();
        }
        return force_quit;
    };

    callbacks.init_gs_render = [](utils::serial* ar)
    {
        switch (g_cfg.video.renderer.get())
        {
        case video_renderer::null:
            g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
            break;
        case video_renderer::opengl:
            g_fxo->init<rsx::thread, named_thread<GLGSRender>>(ar);
            break;
        default:
            g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
            break;
        }
    };

    callbacks.get_gs_frame = []() -> std::unique_ptr<GSFrameBase>
    {
        return std::make_unique<LibretroGSFrame>();
    };

    callbacks.close_gs_frame = []() {};

    callbacks.get_camera_handler = []() -> std::shared_ptr<camera_handler_base>
    {
        return std::make_shared<null_camera_handler>();
    };

    callbacks.get_music_handler = []() -> std::shared_ptr<music_handler_base>
    {
        return std::make_shared<null_music_handler>();
    };

    callbacks.get_audio = []() -> std::shared_ptr<AudioBackend>
    {
        return std::make_shared<LibretroAudioBackend>();
    };

    callbacks.get_audio_enumerator = [](u64) -> std::shared_ptr<audio_device_enumerator>
    {
        return nullptr;
    };

    callbacks.init_kb_handler = []()
    {
        g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(Emu.DeserialManager());
    };

    callbacks.init_mouse_handler = []()
    {
        g_fxo->init<MouseHandlerBase, NullMouseHandler>(Emu.DeserialManager());
    };

    callbacks.init_pad_handler = [](std::string_view)
    {
        // Initialize libretro input polling
        libretro_input_init();

        // Create and initialize the libretro pad handler
        if (!g_libretro_pad_handler)
        {
            g_libretro_pad_handler = std::make_shared<LibretroPadHandler>();
            g_libretro_pad_handler->Init();
        }

        if (log_cb)
            log_cb(RETRO_LOG_INFO, "Libretro pad handler initialized\n");
    };

    callbacks.get_msg_dialog = []() -> std::shared_ptr<MsgDialogBase>
    {
        return nullptr;
    };

    callbacks.get_osk_dialog = []() -> std::shared_ptr<OskDialogBase>
    {
        return nullptr;
    };

    callbacks.get_save_dialog = []() -> std::unique_ptr<SaveDialogBase>
    {
        return nullptr;
    };

    callbacks.get_trophy_notification_dialog = []() -> std::unique_ptr<TrophyNotificationBase>
    {
        return nullptr;
    };

    callbacks.on_run = [](bool) {};
    callbacks.on_pause = []() {};
    callbacks.on_resume = []() {};
    callbacks.on_stop = []() {};
    callbacks.on_ready = []() {};

    callbacks.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>>, int) {};
    callbacks.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};

    callbacks.enable_disc_eject = [](bool) {};
    callbacks.enable_disc_insert = [](bool) {};
    callbacks.on_missing_fw = []() {};
    callbacks.handle_taskbar_progress = [](s32, s32) {};

    callbacks.get_localized_string = [](localized_string_id, const char*) -> std::string { return {}; };
    callbacks.get_localized_u32string = [](localized_string_id, const char*) -> std::u32string { return {}; };
    callbacks.get_localized_setting = [](const cfg::_base*, u32) -> std::string { return {}; };

    callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
    callbacks.add_breakpoint = [](u32) {};

    callbacks.display_sleep_control_supported = []() { return false; };
    callbacks.enable_display_sleep = [](bool) {};

    callbacks.check_microphone_permissions = []() {};
    callbacks.make_video_source = []() { return nullptr; };

    callbacks.update_emu_settings = []() {};
    callbacks.save_emu_settings = []() {};

    Emu.SetCallbacks(std::move(callbacks));
}
