// RPCS3 libretro core implementation
// Focuses on OpenGL rendering backend

#include "stdafx.h"

#include "libretro.h"
#include "libretro_core.h"

#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellSysutil.h"
#include "Emu/NP/rpcn_config.h"
#include "Crypto/unpkg.h"
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
#include "libretro_vfs.h"

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


// Libretro callbacks
static retro_environment_t environ_cb = nullptr;
static retro_video_refresh_t video_cb = nullptr;
static retro_audio_sample_t audio_cb = nullptr;
static retro_audio_sample_batch_t audio_batch_cb = nullptr;
static retro_input_poll_t input_poll_cb = nullptr;
static retro_input_state_t input_state_cb = nullptr;
retro_log_printf_t log_cb = nullptr;

// Hardware render callback for OpenGL
static retro_hw_render_callback hw_render;

// Core state
static bool core_initialized = false;
static bool game_loaded = false;
static bool pending_game_boot = false;
static std::string game_path;
static std::string system_dir;
static std::string save_dir;
static std::string content_dir;  // RetroArch's content/games directory for PKG installation

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
                }
            }
            else if (gap_us < resume_threshold_us)
            {
                if (s_paused_by_watchdog.load() && Emu.IsPaused())
                {
                    Emu.Resume();
                    s_paused_by_watchdog.store(false);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
}

// Helper to display libretro messages/notifications
static void libretro_show_message(const char* msg, unsigned frames = 180)
{
    if (!environ_cb)
        return;

    retro_message rm{};
    rm.msg = msg;
    rm.frames = frames;
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &rm);
}

// Check if a file path has a PKG extension
static bool is_pkg_file(const std::string& path)
{
    if (path.size() < 4)
        return false;

    std::string ext = path.substr(path.size() - 4);
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return ext == ".pkg";
}

// Install a PKG file and return the path to the installed EBOOT.BIN
static std::string install_pkg_file(const std::string& pkg_path)
{

    libretro_show_message("Installing PKG file...", 300);

    // Determine install directory - use content_dir if available, otherwise system/rpcs3/dev_hdd0/game
    std::string install_base;
    if (!content_dir.empty())
    {
        install_base = content_dir + "/";
    }
    else if (!system_dir.empty())
    {
        install_base = system_dir + "/rpcs3/dev_hdd0/game/";
    }
    else
    {

        libretro_show_message("PKG installation failed: No install directory", 300);
        return "";
    }

    // Create install directory
    if (!fs::create_path(install_base))
    {
        libretro_show_message("PKG installation failed: Cannot create directory", 300);
        return "";
    }

    // Create deque for extraction - use emplace_back since package_reader is non-copyable
    std::deque<package_reader> readers;
    readers.emplace_back(pkg_path);

    // Validate the reader
    if (!readers.front().is_valid())
    {
        libretro_show_message("PKG installation failed: Invalid PKG file", 300);
        return "";
    }

    // Get PKG info
    const auto& header = readers.front().get_header();
    std::string title_id(header.title_id, strnlen(header.title_id, sizeof(header.title_id)));


    std::deque<std::string> bootable_paths;

    // Show progress updates during extraction
    char msg_buf[256];
    int last_progress = -1;

    // Start extraction in a separate thread so we can show progress
    std::atomic<bool> extraction_done{false};
    std::atomic<bool> extraction_success{false};
    std::thread extraction_thread([&]()
    {
        auto result = package_reader::extract_data(readers, bootable_paths);
        extraction_success = (result.error == package_install_result::error_type::no_error);
        extraction_done = true;
    });

    // Poll progress and show updates - show every 1% change for better feedback
    while (!extraction_done)
    {
        if (!readers.empty())
        {
            int progress = readers.front().get_progress(100);
            if (progress != last_progress)
            {
                snprintf(msg_buf, sizeof(msg_buf), "Installing PKG: %d%%", progress);
                libretro_show_message(msg_buf, 120);
                last_progress = progress;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    extraction_thread.join();

    if (!extraction_success)
    {

        libretro_show_message("PKG installation failed: Extraction error", 300);
        return "";
    }

    // Find the bootable EBOOT.BIN
    std::string eboot_path;
    if (!bootable_paths.empty())
    {
        eboot_path = bootable_paths.front();
    }
    else
    {
        // Try to find EBOOT.BIN in common locations
        std::vector<std::string> search_paths = {
            install_base + title_id + "/USRDIR/EBOOT.BIN",
            install_base + title_id + "/PS3_GAME/USRDIR/EBOOT.BIN",
        };

        for (const auto& path : search_paths)
        {
            if (fs::is_file(path))
            {
                eboot_path = path;
                break;
            }
        }
    }

    if (eboot_path.empty())
    {
        libretro_show_message("PKG installed (no bootable content - may be DLC)", 300);
    }
    else
    {
        libretro_show_message("PKG installed successfully!", 180);
    }

    return eboot_path;
}

static std::string get_option_value(const char* key, const char* default_val = "")
{
    if (!environ_cb)
        return default_val;
    retro_variable var{};
    var.key = key;
    var.value = nullptr;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        return var.value;
    return default_val;
}

static void libretro_apply_core_options()
{
    if (!environ_cb)
        return;

    // ==================== CPU OPTIONS ====================
    // PPU Decoder
    std::string ppu_decoder = get_option_value("rpcs3_ppu_decoder", "llvm");
    if (ppu_decoder == "llvm")
        g_cfg.core.ppu_decoder.set(ppu_decoder_type::llvm);
    else
        g_cfg.core.ppu_decoder.set(ppu_decoder_type::_static);

    // SPU Decoder
    std::string spu_decoder = get_option_value("rpcs3_spu_decoder", "llvm");
    if (spu_decoder == "llvm")
        g_cfg.core.spu_decoder.set(spu_decoder_type::llvm);
    else if (spu_decoder == "asmjit")
        g_cfg.core.spu_decoder.set(spu_decoder_type::asmjit);
    else
        g_cfg.core.spu_decoder.set(spu_decoder_type::_static);

    // SPU Block Size
    std::string spu_block = get_option_value("rpcs3_spu_block_size", "safe");
    if (spu_block == "mega")
        g_cfg.core.spu_block_size.set(spu_block_size_type::mega);
    else if (spu_block == "giga")
        g_cfg.core.spu_block_size.set(spu_block_size_type::giga);
    else
        g_cfg.core.spu_block_size.set(spu_block_size_type::safe);

    // Preferred SPU Threads
    std::string spu_threads = get_option_value("rpcs3_preferred_spu_threads", "0");
    g_cfg.core.preferred_spu_threads.set(std::stoi(spu_threads));

    // SPU Loop Detection
    g_cfg.core.spu_loop_detection.set(get_option_value("rpcs3_spu_loop_detection", "enabled") == "enabled");

    // SPU Cache
    g_cfg.core.spu_cache.set(get_option_value("rpcs3_spu_cache", "enabled") == "enabled");

    // LLVM Precompilation
    g_cfg.core.llvm_precompilation.set(get_option_value("rpcs3_llvm_precompilation", "enabled") == "enabled");

    // Accurate DFMA
    g_cfg.core.use_accurate_dfma.set(get_option_value("rpcs3_accurate_dfma", "disabled") == "enabled");

    // Clocks Scale
    std::string clocks = get_option_value("rpcs3_clocks_scale", "100");
    g_cfg.core.clocks_scale.set(std::stoi(clocks));

    // Max SPURS Threads
    std::string spurs = get_option_value("rpcs3_max_spurs_threads", "auto");
    if (spurs == "auto")
        g_cfg.core.max_spurs_threads.set(6);
    else
        g_cfg.core.max_spurs_threads.set(std::stoi(spurs));

    // ==================== GPU OPTIONS ====================
    // Resolution Scale
    std::string res_scale = get_option_value("rpcs3_resolution_scale", "100");
    g_cfg.video.resolution_scale_percent.set(std::stoi(res_scale));

    // Frame Limit
    std::string limit = get_option_value("rpcs3_frame_limit", "auto");
    g_cfg.video.vsync.set(false);  // Disable RPCS3 vsync, RetroArch controls timing

    if (limit == "off" || limit == "Off")
    {
        g_disable_frame_limit = true;
        g_cfg.video.frame_limit.set(frame_limit_type::none);
    }
    else if (limit == "30")
    {
        g_disable_frame_limit = false;
        g_cfg.video.frame_limit.set(frame_limit_type::_30);
    }
    else if (limit == "50")
    {
        g_disable_frame_limit = false;
        g_cfg.video.frame_limit.set(frame_limit_type::_50);
    }
    else if (limit == "60")
    {
        g_disable_frame_limit = false;
        g_cfg.video.frame_limit.set(frame_limit_type::_60);
    }
    else if (limit == "120")
    {
        g_disable_frame_limit = false;
        g_cfg.video.frame_limit.set(frame_limit_type::_120);
    }
    else
    {
        // Auto: disable limiter in libretro by default
        g_disable_frame_limit = true;
        g_cfg.video.frame_limit.set(frame_limit_type::none);
    }

    // Shader Mode
    std::string shader_mode = get_option_value("rpcs3_shader_mode", "async");
    if (shader_mode == "async")
        g_cfg.video.shadermode.set(shader_mode::async_recompiler);
    else if (shader_mode == "async_recompiler")
        g_cfg.video.shadermode.set(shader_mode::async_recompiler);
    else
        g_cfg.video.shadermode.set(shader_mode::recompiler);

    // Anisotropic Filter
    std::string aniso = get_option_value("rpcs3_anisotropic_filter", "auto");
    if (aniso == "auto")
        g_cfg.video.anisotropic_level_override.set(0);
    else
        g_cfg.video.anisotropic_level_override.set(std::stoi(aniso));

    // Write Color Buffers
    g_cfg.video.write_color_buffers.set(get_option_value("rpcs3_write_color_buffers", "disabled") == "enabled");

    // Read Color Buffers
    g_cfg.video.read_color_buffers.set(get_option_value("rpcs3_read_color_buffers", "disabled") == "enabled");

    // Read Depth Buffers
    g_cfg.video.read_depth_buffer.set(get_option_value("rpcs3_read_depth_buffers", "disabled") == "enabled");

    // Write Depth Buffers
    g_cfg.video.write_depth_buffer.set(get_option_value("rpcs3_write_depth_buffers", "disabled") == "enabled");

    // Strict Rendering
    g_cfg.video.strict_rendering_mode.set(get_option_value("rpcs3_strict_rendering", "disabled") == "enabled");

    // Multithreaded RSX
    g_cfg.video.multithreaded_rsx.set(get_option_value("rpcs3_multithreaded_rsx", "enabled") == "enabled");

    // VBlank Rate
    std::string vblank = get_option_value("rpcs3_vblank_rate", "60");
    g_cfg.video.vblank_rate.set(std::stoi(vblank));

    // Driver Wake-Up Delay
    std::string driver_delay = get_option_value("rpcs3_driver_wakeup_delay", "200");
    g_cfg.video.driver_wakeup_delay.set(std::stoi(driver_delay));

    // ==================== AUDIO OPTIONS ====================
    // Audio Buffering
    g_cfg.audio.enable_buffering.set(get_option_value("rpcs3_audio_buffering", "enabled") == "enabled");

    // Audio Buffer Duration
    std::string audio_buf = get_option_value("rpcs3_audio_buffer_duration", "100");
    g_cfg.audio.desired_buffer_duration.set(std::stoi(audio_buf));

    // Time Stretching
    g_cfg.audio.enable_time_stretching.set(get_option_value("rpcs3_time_stretching", "disabled") == "enabled");

    // Master Volume
    std::string volume = get_option_value("rpcs3_master_volume", "100");
    g_cfg.audio.volume.set(std::stoi(volume));

    // ==================== SYSTEM/CORE OPTIONS ====================
    // System Language
    std::string lang = get_option_value("rpcs3_language", "english");
    if (lang == "japanese") g_cfg.sys.language.set(CELL_SYSUTIL_LANG_JAPANESE);
    else if (lang == "french") g_cfg.sys.language.set(CELL_SYSUTIL_LANG_FRENCH);
    else if (lang == "spanish") g_cfg.sys.language.set(CELL_SYSUTIL_LANG_SPANISH);
    else if (lang == "german") g_cfg.sys.language.set(CELL_SYSUTIL_LANG_GERMAN);
    else if (lang == "italian") g_cfg.sys.language.set(CELL_SYSUTIL_LANG_ITALIAN);
    else if (lang == "dutch") g_cfg.sys.language.set(CELL_SYSUTIL_LANG_DUTCH);
    else if (lang == "portuguese") g_cfg.sys.language.set(CELL_SYSUTIL_LANG_PORTUGUESE_PT);
    else if (lang == "russian") g_cfg.sys.language.set(CELL_SYSUTIL_LANG_RUSSIAN);
    else if (lang == "korean") g_cfg.sys.language.set(CELL_SYSUTIL_LANG_KOREAN);
    else if (lang == "chinese_trad") g_cfg.sys.language.set(CELL_SYSUTIL_LANG_CHINESE_T);
    else if (lang == "chinese_simp") g_cfg.sys.language.set(CELL_SYSUTIL_LANG_CHINESE_S);
    else g_cfg.sys.language.set(CELL_SYSUTIL_LANG_ENGLISH_US);

    // Enter Button Assignment
    std::string enter_btn = get_option_value("rpcs3_enter_button", "cross");
    g_cfg.sys.enter_button_assignment.set(enter_btn == "circle" ? enter_button_assign::circle : enter_button_assign::cross);

    // Avoid additional CPU-throttling heuristics
    g_cfg.core.max_cpu_preempt_count_per_frame.set(0);

}



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

    const auto stack = utils::get_backtrace_from_context(info->ContextRecord, 256);
    const auto lines = utils::get_backtrace_symbols(stack);
    for (usz i = 0; i < lines.size(); ++i)
    {

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

}

static void lrcore_uninstall_crash_handler()
{

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
        void log(u64 /*stamp*/, const logs::message& /*msg*/, std::string_view /*prefix*/, std::string_view /*text*/) override
        {
            // Logging disabled
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

    // Request VFS interface (API v3 for directory operations)
    struct retro_vfs_interface_info vfs_info;
    vfs_info.required_interface_version = 3;
    vfs_info.iface = nullptr;

    if (cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info) && vfs_info.iface)
    {
        libretro_vfs::set_vfs_interface(vfs_info.iface);

    }
    else
    {
        // Only log on first call - retro_set_environment may be called multiple times

    }

    // Core options using v2 API for categories
    static struct retro_core_option_v2_category option_cats[] = {
        { "cpu", "CPU", "PPU/SPU decoder, threads, cache, and CPU emulation options." },
        { "gpu", "GPU", "Renderer, resolution, shaders, and graphics options." },
        { "audio", "Audio", "Audio buffering, volume, and microphone options." },
        { "network", "Network", "Network, PSN, RPCN, and online options." },
        { "advanced", "Advanced", "Advanced accuracy and performance tuning options." },
        { "core", "Core", "System language, region, and misc core options." },
        { NULL, NULL, NULL }
    };

    static struct retro_core_option_v2_definition option_defs[] = {
        // ==================== CPU OPTIONS ====================
        {
            "rpcs3_ppu_decoder", "PPU Decoder", NULL,
            "PPU (main CPU) decoder. LLVM Recompiler is fastest.",
            NULL, "cpu",
            { {"llvm", "Recompiler (LLVM)"}, {"interpreter", "Interpreter (Slow)"}, {NULL, NULL} },
            "llvm"
        },
        {
            "rpcs3_spu_decoder", "SPU Decoder", NULL,
            "SPU (co-processor) decoder. LLVM Recompiler is fastest.",
            NULL, "cpu",
            { {"llvm", "Recompiler (LLVM)"}, {"asmjit", "Recompiler (ASMJIT)"}, {"interpreter", "Interpreter (Slow)"}, {NULL, NULL} },
            "llvm"
        },
        {
            "rpcs3_spu_block_size", "SPU Block Size", NULL,
            "SPU recompiler block size. Mega/Giga may improve performance.",
            NULL, "cpu",
            { {"safe", "Safe"}, {"mega", "Mega"}, {"giga", "Giga"}, {NULL, NULL} },
            "safe"
        },
        {
            "rpcs3_preferred_spu_threads", "Preferred SPU Threads", NULL,
            "Number of SPU threads. Auto recommended.",
            NULL, "cpu",
            { {"0", "Auto"}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}, {"5", "5"}, {"6", "6"}, {NULL, NULL} },
            "0"
        },
        {
            "rpcs3_spu_loop_detection", "SPU Loop Detection", NULL,
            "Enable SPU loop detection for performance.",
            NULL, "cpu",
            { {"enabled", "Enabled"}, {"disabled", "Disabled"}, {NULL, NULL} },
            "enabled"
        },
        {
            "rpcs3_spu_cache", "SPU Cache", NULL,
            "Enable SPU cache for faster subsequent loads.",
            NULL, "cpu",
            { {"enabled", "Enabled"}, {"disabled", "Disabled"}, {NULL, NULL} },
            "enabled"
        },
        {
            "rpcs3_llvm_precompilation", "LLVM Precompilation", NULL,
            "Precompile PPU modules at boot for faster subsequent loads.",
            NULL, "cpu",
            { {"enabled", "Enabled"}, {"disabled", "Disabled"}, {NULL, NULL} },
            "enabled"
        },
        {
            "rpcs3_accurate_dfma", "Accurate DFMA", NULL,
            "Use accurate double-precision fused multiply-add.",
            NULL, "cpu",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_ppu_reservations", "PPU Thread Reservations", NULL,
            "Use PPU thread reservations for accurate locking.",
            NULL, "cpu",
            { {"enabled", "Enabled"}, {"disabled", "Disabled"}, {NULL, NULL} },
            "enabled"
        },
        {
            "rpcs3_accurate_xfloat", "Accurate XFLOAT", NULL,
            "More accurate SPU floating-point. May fix some games.",
            NULL, "cpu",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_clocks_scale", "Clocks Scale", NULL,
            "Scale PS3 clock speed percentage.",
            NULL, "cpu",
            { {"50", "50%"}, {"75", "75%"}, {"100", "100%"}, {"150", "150%"}, {"200", "200%"}, {"300", "300%"}, {NULL, NULL} },
            "100"
        },
        {
            "rpcs3_sleep_timers_accuracy", "Sleep Timers Accuracy", NULL,
            "Sleep timers accuracy level.",
            NULL, "cpu",
            { {"usleep", "Usleep"}, {"all_timers", "All Timers"}, {"as_host", "As Host"}, {NULL, NULL} },
            "usleep"
        },
        {
            "rpcs3_max_spurs_threads", "Max SPURS Threads", NULL,
            "Maximum SPURS thread count. Lower may improve performance.",
            NULL, "cpu",
            { {"auto", "Auto"}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}, {"5", "5"}, {"6", "6"}, {NULL, NULL} },
            "auto"
        },
        {
            "rpcs3_enable_tsx", "Enable TSX", NULL,
            "Enable Intel TSX hardware acceleration if available.",
            NULL, "cpu",
            { {"enabled", "Enabled"}, {"disabled", "Disabled"}, {"forced", "Forced"}, {NULL, NULL} },
            "enabled"
        },
        {
            "rpcs3_spu_xfloat_accuracy", "SPU XFloat Accuracy", NULL,
            "SPU floating-point accuracy level.",
            NULL, "cpu",
            { {"relaxed", "Relaxed (Fastest)"}, {"accurate", "Accurate"}, {"ultra", "Ultra (Slowest)"}, {NULL, NULL} },
            "accurate"
        },
        {
            "rpcs3_spu_dma_busy_wait", "SPU DMA Busy Waiting", NULL,
            "Enable SPU DMA busy waiting for timing accuracy.",
            NULL, "cpu",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_ppu_llvm_java_mode", "PPU LLVM Java Mode Handling", NULL,
            "PPU LLVM Java mode compliance level.",
            NULL, "cpu",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },

        // ==================== GPU OPTIONS ====================
        {
            "rpcs3_renderer", "Renderer", NULL,
            "Graphics renderer. OpenGL is recommended for libretro.",
            NULL, "gpu",
            { {"opengl", "OpenGL"}, {"null", "Null (No Video)"}, {NULL, NULL} },
            "opengl"
        },
        {
            "rpcs3_resolution_scale", "Resolution Scale", NULL,
            "Internal rendering resolution scale percentage.",
            NULL, "gpu",
            { {"25", "25%"}, {"30", "30%"}, {"35", "35%"}, {"40", "40%"}, {"45", "45%"}, {"50", "50%"}, {"55", "55%"}, {"60", "60%"}, {"65", "65%"}, {"70", "70%"}, {"75", "75%"}, {"80", "80%"}, {"85", "85%"}, {"90", "90%"}, {"95", "95%"}, {"100", "100% (Native)"}, {"105", "105%"}, {"110", "110%"}, {"115", "115%"}, {"120", "120%"}, {"125", "125%"}, {"130", "130%"}, {"135", "135%"}, {"140", "140%"}, {"145", "145%"}, {"150", "150%"}, {"175", "175%"}, {"200", "200%"}, {"250", "250%"}, {"300", "300%"}, {NULL, NULL} },
            "100"
        },
        {
            "rpcs3_frame_limit", "Frame Limit", NULL,
            "Limit frame rate. Auto uses RetroArch timing.",
            NULL, "gpu",
            { {"auto", "Auto"}, {"off", "Off"}, {"30", "30 FPS"}, {"50", "50 FPS"}, {"60", "60 FPS"}, {"120", "120 FPS"}, {"144", "144 FPS"}, {"240", "240 FPS"}, {NULL, NULL} },
            "auto"
        },
        {
            "rpcs3_shader_mode", "Shader Mode", NULL,
            "Shader compilation mode. Async recommended.",
            NULL, "gpu",
            { {"async", "Async (Recommended)"}, {"async_recompiler", "Async with Recompiler"}, {"sync", "Synchronous"}, {NULL, NULL} },
            "async"
        },
        {
            "rpcs3_anisotropic_filter", "Anisotropic Filtering", NULL,
            "Texture filtering quality.",
            NULL, "gpu",
            { {"auto", "Auto"}, {"1", "1x (Off)"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}, {NULL, NULL} },
            "auto"
        },
        {
            "rpcs3_msaa", "Anti-Aliasing (MSAA)", NULL,
            "Multi-sample anti-aliasing.",
            NULL, "gpu",
            { {"disabled", "Disabled"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_shader_precision", "Shader Precision", NULL,
            "Shader floating-point precision.",
            NULL, "gpu",
            { {"low", "Low (Fastest)"}, {"normal", "Normal"}, {"high", "High (Most Accurate)"}, {NULL, NULL} },
            "normal"
        },
        {
            "rpcs3_write_color_buffers", "Write Color Buffers", NULL,
            "Write color buffers to main memory. Fixes some effects.",
            NULL, "gpu",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_read_color_buffers", "Read Color Buffers", NULL,
            "Read color buffers from main memory.",
            NULL, "gpu",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_read_depth_buffers", "Read Depth Buffers", NULL,
            "Read depth buffers from main memory.",
            NULL, "gpu",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_write_depth_buffers", "Write Depth Buffers", NULL,
            "Write depth buffers to main memory.",
            NULL, "gpu",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_strict_rendering", "Strict Rendering Mode", NULL,
            "Enable strict rendering for accuracy.",
            NULL, "gpu",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_vertex_cache", "Vertex Cache", NULL,
            "Enable vertex cache for performance.",
            NULL, "gpu",
            { {"enabled", "Enabled"}, {"disabled", "Disabled"}, {NULL, NULL} },
            "enabled"
        },
        {
            "rpcs3_multithreaded_rsx", "Multithreaded RSX", NULL,
            "Enable multithreaded RSX for better performance.",
            NULL, "gpu",
            { {"enabled", "Enabled"}, {"disabled", "Disabled"}, {NULL, NULL} },
            "enabled"
        },
        {
            "rpcs3_zcull_accuracy", "ZCULL Accuracy", NULL,
            "ZCULL occlusion query accuracy.",
            NULL, "gpu",
            { {"relaxed", "Relaxed (Fastest)"}, {"approximate", "Approximate"}, {"precise", "Precise (Slowest)"}, {NULL, NULL} },
            "relaxed"
        },
        {
            "rpcs3_cpu_blit", "Force CPU Blit", NULL,
            "Force CPU blit emulation for certain effects.",
            NULL, "gpu",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_driver_wakeup_delay", "Driver Wake-Up Delay", NULL,
            "Driver wake-up delay in microseconds.",
            NULL, "gpu",
            { {"0", "0 (Minimum)"}, {"20", "20"}, {"50", "50"}, {"100", "100"}, {"200", "200 (Default)"}, {"400", "400"}, {"800", "800"}, {NULL, NULL} },
            "200"
        },
        {
            "rpcs3_vblank_rate", "VBlank Rate", NULL,
            "VBlank frequency in Hz.",
            NULL, "gpu",
            { {"50", "50 Hz (PAL)"}, {"60", "60 Hz (NTSC)"}, {"120", "120 Hz"}, {"144", "144 Hz"}, {"240", "240 Hz"}, {NULL, NULL} },
            "60"
        },
        {
            "rpcs3_stretch_to_display", "Stretch to Display", NULL,
            "Stretch game output to fill the display.",
            NULL, "gpu",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },

        // ==================== AUDIO OPTIONS ====================
        {
            "rpcs3_audio_buffering", "Enable Buffering", NULL,
            "Enable audio buffering.",
            NULL, "audio",
            { {"enabled", "Enabled"}, {"disabled", "Disabled"}, {NULL, NULL} },
            "enabled"
        },
        {
            "rpcs3_audio_buffer_duration", "Buffer Duration", NULL,
            "Audio buffer size in milliseconds.",
            NULL, "audio",
            { {"10", "10ms"}, {"20", "20ms"}, {"30", "30ms"}, {"40", "40ms"}, {"50", "50ms"}, {"75", "75ms"}, {"100", "100ms (Default)"}, {"150", "150ms"}, {"200", "200ms"}, {NULL, NULL} },
            "100"
        },
        {
            "rpcs3_time_stretching", "Time Stretching", NULL,
            "Enable audio time stretching to reduce stuttering.",
            NULL, "audio",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_microphone_type", "Microphone Type", NULL,
            "Microphone device type.",
            NULL, "audio",
            { {"null", "Null (Disabled)"}, {"standard", "Standard"}, {"singstar", "SingStar"}, {"real_singstar", "Real SingStar"}, {"rocksmith", "Rocksmith"}, {NULL, NULL} },
            "null"
        },
        {
            "rpcs3_master_volume", "Master Volume", NULL,
            "Master audio volume percentage.",
            NULL, "audio",
            { {"0", "0%"}, {"10", "10%"}, {"20", "20%"}, {"30", "30%"}, {"40", "40%"}, {"50", "50%"}, {"60", "60%"}, {"70", "70%"}, {"80", "80%"}, {"90", "90%"}, {"100", "100%"}, {NULL, NULL} },
            "100"
        },

        // ==================== NETWORK OPTIONS ====================
        {
            "rpcs3_network_enabled", "Network Enabled", NULL,
            "Enable network features.",
            NULL, "network",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_psn_status", "PSN Status", NULL,
            "PlayStation Network status.",
            NULL, "network",
            { {"disabled", "Disabled"}, {"simulated", "Simulated"}, {"rpcn", "RPCN"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_upnp", "UPNP", NULL,
            "Enable UPNP for automatic port forwarding.",
            NULL, "network",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_show_rpcn_popups", "Show RPCN Popups", NULL,
            "Show RPCN notification popups.",
            NULL, "network",
            { {"enabled", "Enabled"}, {"disabled", "Disabled"}, {NULL, NULL} },
            "enabled"
        },
        {
            "rpcs3_show_trophy_popups", "Show Trophy Popups", NULL,
            "Show trophy unlock notifications.",
            NULL, "network",
            { {"enabled", "Enabled"}, {"disabled", "Disabled"}, {NULL, NULL} },
            "enabled"
        },
        {
            "rpcs3_dns", "DNS Server", NULL,
            "DNS server address.",
            NULL, "network",
            { {"8.8.8.8", "Google DNS"}, {"1.1.1.1", "Cloudflare DNS"}, {"208.67.222.222", "OpenDNS"}, {NULL, NULL} },
            "8.8.8.8"
        },
        {
            "rpcs3_rpcn_server", "RPCN Server", NULL,
            "RPCN server address for online play.",
            NULL, "network",
            { {"rpcn.rpcs3.net", "Official RPCN"}, {"custom", "Custom"}, {NULL, NULL} },
            "rpcn.rpcs3.net"
        },

        // ==================== ADVANCED OPTIONS ====================
        {
            "rpcs3_spu_verification", "SPU Verification", NULL,
            "SPU code verification level.",
            NULL, "advanced",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "enabled"
        },
        {
            "rpcs3_spu_cache_line_stores", "SPU Cache Line Stores", NULL,
            "Enable accurate cache line stores.",
            NULL, "advanced",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_rsx_fifo_accuracy", "RSX FIFO Accuracy", NULL,
            "RSX FIFO command accuracy level.",
            NULL, "advanced",
            { {"fast", "Fast"}, {"balanced", "Balanced"}, {"accurate", "Accurate"}, {NULL, NULL} },
            "fast"
        },
        {
            "rpcs3_driver_recovery_timeout", "Driver Recovery Timeout", NULL,
            "GPU driver recovery timeout in milliseconds.",
            NULL, "advanced",
            { {"0", "Disabled"}, {"1000", "1 second"}, {"2000", "2 seconds"}, {"5000", "5 seconds"}, {"10000", "10 seconds"}, {NULL, NULL} },
            "1000"
        },
        {
            "rpcs3_mfc_shuffling", "MFC Commands Shuffling", NULL,
            "Shuffle MFC commands for accuracy.",
            NULL, "advanced",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_spu_delay_penalty", "SPU Delay Penalty", NULL,
            "SPU delay penalty for scheduling.",
            NULL, "advanced",
            { {"0", "0"}, {"1", "1"}, {"2", "2"}, {"3", "3 (Default)"}, {"4", "4"}, {"5", "5"}, {NULL, NULL} },
            "3"
        },
        {
            "rpcs3_zcull_sync", "Relaxed ZCull Sync", NULL,
            "Use relaxed ZCull synchronization.",
            NULL, "advanced",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_async_texture_streaming", "Async Texture Streaming", NULL,
            "Enable asynchronous texture streaming.",
            NULL, "advanced",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_ppu_llvm_greedy", "PPU LLVM Greedy Mode", NULL,
            "Use greedy PPU LLVM compilation.",
            NULL, "advanced",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_spu_nj_fixup", "SPU NJ Fixup", NULL,
            "Apply SPU non-Java mode fixup.",
            NULL, "advanced",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_ppu_nj_mode", "PPU NJ Fixup Mode", NULL,
            "PPU non-Java mode handling.",
            NULL, "advanced",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_ppu_set_sat_bit", "Set Saturation Bit", NULL,
            "Accurately set PPU saturation bit.",
            NULL, "advanced",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_ppu_accurate_vector_nan", "PPU Accurate Vector NaN", NULL,
            "More accurate vector NaN handling.",
            NULL, "advanced",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_ppu_set_fpcc", "PPU Set FPCC", NULL,
            "Accurately set PPU FPCC bits.",
            NULL, "advanced",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },

        // ==================== CORE OPTIONS ====================
        {
            "rpcs3_language", "System Language", NULL,
            "PS3 system language.",
            NULL, "core",
            { {"english", "English"}, {"japanese", "Japanese"}, {"french", "French"}, {"spanish", "Spanish"}, {"german", "German"}, {"italian", "Italian"}, {"dutch", "Dutch"}, {"portuguese", "Portuguese"}, {"russian", "Russian"}, {"korean", "Korean"}, {"chinese_trad", "Chinese (Traditional)"}, {"chinese_simp", "Chinese (Simplified)"}, {NULL, NULL} },
            "english"
        },
        {
            "rpcs3_enter_button", "Confirm Button", NULL,
            "Button used for confirm actions.",
            NULL, "core",
            { {"cross", "Cross (Western)"}, {"circle", "Circle (Japanese)"}, {NULL, NULL} },
            "cross"
        },
        {
            "rpcs3_license_area", "License Area", NULL,
            "PS3 license region.",
            NULL, "core",
            { {"usa", "USA"}, {"eu", "Europe"}, {"jp", "Japan"}, {"hk", "Hong Kong"}, {"kr", "Korea"}, {NULL, NULL} },
            "usa"
        },
        {
            "rpcs3_show_shader_compilation_hint", "Show Shader Compilation Hint", NULL,
            "Show hint when shaders are being compiled.",
            NULL, "core",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_show_ppu_compilation_hint", "Show PPU Compilation Hint", NULL,
            "Show hint when PPU modules are being compiled.",
            NULL, "core",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_vfs_init", "VFS Initialize Mode", NULL,
            "Virtual file system initialization mode.",
            NULL, "core",
            { {"auto", "Auto"}, {"reset", "Reset"}, {NULL, NULL} },
            "auto"
        },
        {
            "rpcs3_silence_all_logs", "Silence All Logs", NULL,
            "Silence all log output for performance.",
            NULL, "core",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_hook_static_funcs", "Hook Static Functions", NULL,
            "Hook static functions for HLE.",
            NULL, "core",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },
        {
            "rpcs3_hle_lwmutex", "HLE lwmutex", NULL,
            "Use HLE implementation for lwmutex.",
            NULL, "core",
            { {"disabled", "Disabled"}, {"enabled", "Enabled"}, {NULL, NULL} },
            "disabled"
        },

        { NULL, NULL, NULL, NULL, NULL, NULL, {{NULL, NULL}}, NULL }
    };

    static struct retro_core_options_v2 options_v2 = {
        option_cats,
        option_defs
    };

    // Try v2 options first, fall back to legacy
    if (!cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_v2))
    {
        // Fallback to legacy options for older frontends
        static const struct retro_variable vars[] = {
            { "rpcs3_ppu_decoder", "PPU Decoder; llvm|interpreter" },
            { "rpcs3_spu_decoder", "SPU Decoder; llvm|asmjit|interpreter" },
            { "rpcs3_spu_block_size", "SPU Block Size; safe|mega|giga" },
            { "rpcs3_renderer", "Renderer; opengl|null" },
            { "rpcs3_resolution_scale", "Resolution Scale; 25|30|35|40|45|50|55|60|65|70|75|80|85|90|95|100|105|110|115|120|125|130|135|140|145|150|175|200|250|300" },
            { "rpcs3_frame_limit", "Frame Limit; auto|off|30|50|60|120|144|240" },
            { "rpcs3_shader_mode", "Shader Mode; async|async_recompiler|sync" },
            { "rpcs3_anisotropic_filter", "Anisotropic Filter; auto|1|2|4|8|16" },
            { "rpcs3_msaa", "Anti-Aliasing; disabled|2|4|8|16" },
            { "rpcs3_write_color_buffers", "Write Color Buffers; disabled|enabled" },
            { "rpcs3_zcull_accuracy", "ZCULL Accuracy; relaxed|approximate|precise" },
            { "rpcs3_multithreaded_rsx", "Multithreaded RSX; enabled|disabled" },
            { "rpcs3_audio_buffer_duration", "Audio Buffer; 10|20|30|40|50|75|100|150|200" },
            { "rpcs3_network_enabled", "Network; disabled|enabled" },
            { "rpcs3_psn_status", "PSN Status; disabled|simulated|rpcn" },
            { "rpcs3_show_rpcn_popups", "Show RPCN Popups; enabled|disabled" },
            { "rpcs3_show_trophy_popups", "Show Trophy Popups; enabled|disabled" },
            { "rpcs3_language", "System Language; english|japanese|french|spanish|german|italian" },
            { "rpcs3_enter_button", "Confirm Button; cross|circle" },
            { nullptr, nullptr }
        };
        cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
    }

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

    }

    // Initialize sensor interface for gyro/accelerometer support
    if (libretro_input_init_sensors(cb))
    {
    }
    else
    {
    }



    // Set minimum audio latency to reduce crackling (per libretro docs recommendation)
    // 64ms = ~4 frames at 60fps, good for emulators with variable frame timing
    unsigned audio_latency_ms = 64;
    if (cb(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY, &audio_latency_ms))
    {

    }

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
    // VFS support: Allow both fullpath (native) and VFS-based loading
    // RPCS3 still works best with full paths for directory structures,
    // but VFS enables loading from archives and virtual filesystems
    info->need_fullpath = false;
    info->block_extract = false;
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


    if (!libretro_is_firmware_installed())
    {
        const std::string pup_path = find_firmware_pup();
        if (pup_path.empty())
        {
        }
        else
        {

            if (!g_fxo->is_init())
            {
                g_fxo->reset();
            }

            bool ok = false;
            try
            {
                ok = libretro_install_firmware(pup_path, [](int cur, int total)
                {

                });
            }
            catch (const std::exception& e)
            {
                (void)e;
                ok = false;
            }
            catch (...)
            {

                ok = false;
            }


        }
    }
    else
    {
    }

    // Initialize the emulator
    Emu.SetHasGui(false);
    Emu.SetUsr("00000001");
    Emu.Init();



    // Set up callbacks
    init_emu_callbacks();

    // Set up hardware rendering (OpenGL context)
    if (!setup_hw_render())
    {

    }

    core_initialized = true;




}

void retro_deinit(void)
{
    if (!core_initialized)
        return;

    stop_pause_watchdog();



    if (game_loaded)
    {
        Emu.GracefulShutdown(false, false);
    }

    Emulator::CleanUp();
    core_initialized = false;
    game_loaded = false;



#ifdef _WIN32
    lrcore_uninstall_crash_handler();
#endif


}

static bool do_boot_game();

static void context_reset()
{



    // Initialize libretro video with the new context
    libretro_video_init(hw_render.get_current_framebuffer, hw_render.get_proc_address);



    // Now that the GL context is ready, boot the game if pending
    if (pending_game_boot && !game_loaded)
    {

        if (do_boot_game())
        {
            game_loaded = true;
            start_pause_watchdog();
        }
        pending_game_boot = false;
    }


}

static void context_destroy()
{


    libretro_video_deinit();


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


    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {

        return true;
    }

    // Fallback: try OpenGL Core 3.3 (may lack some features but could work on older systems)


    hw_render.version_major = 3;
    hw_render.version_minor = 3;

    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {

        return true;
    }

    // Final fallback: try legacy OpenGL compatibility context


    hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
    hw_render.version_major = 3;
    hw_render.version_minor = 0;

    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
    {

        return true;
    }


    return false;
}

bool retro_load_game(const struct retro_game_info* game)
{


    if (!game || !game->path)
    {

        return false;
    }

    // Get content directory from RetroArch (for PKG installation)
    const char* content_dir_ptr = nullptr;
    if (environ_cb(RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY, &content_dir_ptr) && content_dir_ptr)
    {
        content_dir = content_dir_ptr;
    }
    else
    {
        content_dir.clear();

    }

    // Check if frontend supports frame duping (passing NULL to video_cb to reuse last frame)
    bool can_dupe = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe) && can_dupe)
    {

    }
    else
    {

    }

    game_path = game->path;

    // Check if this is a PKG file - install it and boot the installed game
    if (is_pkg_file(game_path))
    {


        std::string installed_eboot = install_pkg_file(game_path);
        if (installed_eboot.empty())
        {

            return false;
        }

        // Update game_path to the installed EBOOT.BIN for booting
        game_path = installed_eboot;
    }
    // Check if this is an ISO file - RPCS3 does NOT support raw ISOs
    else if (game_path.size() >= 4 &&
             (game_path.substr(game_path.size() - 4) == ".iso" ||
              game_path.substr(game_path.size() - 4) == ".ISO"))
    {

        return false;
    }
    // Check if EBOOT.BIN was passed directly - need to find parent game folder
    else if (game_path.size() >= 9)
    {
        std::string filename = game_path;
        // Extract just the filename for comparison
        size_t last_slash = filename.find_last_of("/\\");
        if (last_slash != std::string::npos)
            filename = filename.substr(last_slash + 1);

        // Check if it's an EBOOT.BIN file (case insensitive)
        bool is_eboot = (filename == "EBOOT.BIN" || filename == "eboot.bin" ||
                         filename == "Eboot.bin" || filename == "EBOOT.bin");

        if (is_eboot)
        {


            std::string game_folder;
            std::string current_path = fs::get_parent_dir(game_path);


            // Check up to 4 levels up for PS3_GAME or valid game structure
            for (int i = 0; i < 4 && !current_path.empty(); i++)
            {

                // Check if this directory contains PS3_GAME subdirectory (disc game structure)
                std::string ps3_game_path = current_path + "/PS3_GAME";
                if (fs::is_dir(ps3_game_path))
                {
                    game_folder = current_path;
                    break;
                }

                // Check if current directory name ends with USRDIR (we're inside PS3_GAME folder)
                size_t usrdir_pos = current_path.find("USRDIR");
                if (usrdir_pos != std::string::npos &&
                    (usrdir_pos == current_path.size() - 6 ||
                     current_path[usrdir_pos + 6] == '/' ||
                     current_path[usrdir_pos + 6] == '\\'))
                {
                    // Go up one more level to get PS3_GAME, then check for parent
                    std::string ps3_game = fs::get_parent_dir(current_path);
                    if (ps3_game.size() >= 8)
                    {
                        std::string ps3_game_name = ps3_game;
                        size_t slash = ps3_game_name.find_last_of("/\\");
                        if (slash != std::string::npos)
                            ps3_game_name = ps3_game_name.substr(slash + 1);

                        if (ps3_game_name == "PS3_GAME")
                        {
                            game_folder = fs::get_parent_dir(ps3_game);
                            break;
                        }
                    }
                }

                // Check if this directory contains PARAM.SFO (HDD game structure)
                std::string param_sfo = current_path + "/PARAM.SFO";
                if (fs::is_file(param_sfo))
                {
                    game_folder = current_path;
                    break;
                }

                current_path = fs::get_parent_dir(current_path);
            }

            if (game_folder.empty())
            {

                return false;
            }

            game_path = game_folder;
        }
    }

    // Use OpenGL renderer - game boot will be deferred until context_reset() when GL context is ready

    g_cfg.video.renderer.set(video_renderer::opengl);

    // Configure PPU decoder - use LLVM for best performance
    g_cfg.core.ppu_decoder.set(ppu_decoder_type::llvm);


    // Configure SPU decoder - use LLVM for best performance
    g_cfg.core.spu_decoder.set(spu_decoder_type::llvm);


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


    // Configure audio - set explicit stereo layout to avoid "Unsupported layout 0" error
    // (audio_channel_layout::automatic = 0 is not handled by default_layout_channel_count)
    g_cfg.audio.channel_layout.set(audio_channel_layout::stereo);


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


    // Save config so BootGame() loads the correct settings when it reloads config.yml
    // Note: RPCS3 loads config from fs::get_config_dir(true) which adds "config/" subdirectory
    const std::string config_path = fs::get_config_dir(true) + "config.yml";
    g_cfg.save(config_path);

    // For null renderer, boot immediately. For OpenGL, defer until context_reset()
    if (g_cfg.video.renderer.get() == video_renderer::null)
    {

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

    }

    return true;
}

// Actually boot the game - called from context_reset() when GL context is ready
static bool do_boot_game()
{


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

    }

    game_boot_result result = game_boot_result::generic_error;
    try
    {
        Emu.SetForceBoot(true);
        result = Emu.BootGame(game_path);
    }
    catch (const std::exception& e)
    {
        return false;
    }
    catch (...)
    {

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
        return false;
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
            break;
        }

        // If stopped, boot failed
        if (state == system_state::stopped)
        {

            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
        waited_ms += poll_interval_ms;

    }

    system_state final_state = Emu.GetStatus();

    // If ready but not running, start it
    if (final_state == system_state::ready)
    {

        Emu.Run(true);
    }
    // If paused, resume
    else if (final_state == system_state::paused || final_state == system_state::frozen)
    {

        Emu.Resume();
    }
    // If still in starting state after timeout, force transition to running
    else if (final_state == system_state::starting)
    {
        Emu.FinalizeRunRequest();
    }


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

    stop_pause_watchdog();
    if (game_loaded)
    {
        Emu.GracefulShutdown(false, false);
        game_loaded = false;
        game_path.clear();
    }

}

void retro_run(void)
{
    if (!game_loaded)
        return;

    static u64 s_run_counter = 0;
    s_run_counter++;

    // Update watchdog timestamp.
    s_last_retro_run_us.store(lr_now_us());

    // Check for variable updates
    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
    {
        libretro_apply_core_options();
    }

    // Poll input
    input_poll_cb();
    libretro_input_poll(input_state_cb);
    libretro_input_poll_sensors();  // Poll gyro/accelerometer data

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

    if (has_new_frame)
    {
        // New frame available - blit from RSX's shared texture to RetroArch's FBO, then present
        // CRITICAL: FBOs are NOT shared between GL contexts, so RSX renders to a shared texture,
        // and we blit that texture to RetroArch's actual FBO here on the main thread.
        libretro_blit_to_frontend();
        video_cb(RETRO_HW_FRAME_BUFFER_VALID, 1280, 720, 0);
        libretro_mark_frame_presented();
    }
    else
    {
        // No new frame - tell RetroArch to reuse the previous frame (frame duping)
        video_cb(NULL, 1280, 720, 0);
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

