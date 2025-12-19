// Stub implementations for libretro core
// These provide missing symbols that would normally come from Qt-dependent Input handlers

#include "stdafx.h"
#include "Utilities/Config.h"
#include "Emu/Io/pad_types.h"
#include "Emu/Io/pad_config.h"
#include "Emu/Io/pad_config_types.h"
#include "Emu/Io/PadHandler.h"
#include "Input/product_info.h"

#include <vector>
#include <map>
#include <array>
#include <memory>

// ============================================================================
// cfg_ps_moves stubs - must match Input/ps_move_config.h
// ============================================================================

struct cfg_ps_move final : cfg::node
{
    using cfg::node::node;
    cfg::uint<0, 255> saturation_threshold{ this, "Saturation Threshold", 10, true };
};

struct cfg_ps_moves final : cfg::node
{
    cfg_ps_moves();
    bool load();
    std::array<cfg_ps_move, 4> players{ this };
};

cfg_ps_moves g_cfg_move;

cfg_ps_moves::cfg_ps_moves() : cfg::node(nullptr, "ps_move", false) {}

bool cfg_ps_moves::load()
{
    return true;
}

// ============================================================================
// ps_move_tracker stubs - must match Input/ps_move_tracker.h template
// ============================================================================

template<bool WithQt>
class ps_move_tracker
{
public:
    ps_move_tracker();
    virtual ~ps_move_tracker();
    void set_active(u32, bool);
    void set_hue(u32, u16);
    void process_image();
    void set_image_data(const void*, u64, u32, u32, s32);
};

template<bool WithQt>
ps_move_tracker<WithQt>::ps_move_tracker() {}

template<bool WithQt>
ps_move_tracker<WithQt>::~ps_move_tracker() {}

template<bool WithQt>
void ps_move_tracker<WithQt>::set_active(u32, bool) {}

template<bool WithQt>
void ps_move_tracker<WithQt>::set_hue(u32, u16) {}

template<bool WithQt>
void ps_move_tracker<WithQt>::process_image() {}

template<bool WithQt>
void ps_move_tracker<WithQt>::set_image_data(const void*, u64, u32, u32, s32) {}

// Explicit instantiations
template class ps_move_tracker<false>;
template class ps_move_tracker<true>;

// ============================================================================
// pad_thread class definition and stubs - must match Input/pad_thread.h
// ============================================================================

class pad_thread
{
public:
    pad_thread(void* curthread, void* curwindow, std::string_view title_id);
    ~pad_thread();

    void operator()();

    PadInfo& GetInfo() { return m_info; }
    std::array<std::shared_ptr<Pad>, CELL_PAD_MAX_PORT_NUM>& GetPads() { return m_pads; }
    void SetRumble(u32 pad, u8 large_motor, u8 small_motor);
    void SetIntercepted(bool intercepted);

    s32 AddLddPad();
    void UnregisterLddPad(u32 handle);

    void open_home_menu();

    std::map<pad_handler, std::shared_ptr<PadHandlerBase>>& get_handlers() { return m_handlers; }

    static std::shared_ptr<PadHandlerBase> GetHandler(pad_handler type);
    static void InitPadConfig(cfg_pad& cfg, pad_handler type, std::shared_ptr<PadHandlerBase>& handler);

protected:
    void Init();
    void InitLddPad(u32 handle, const u32* port_status);

    std::map<pad_handler, std::shared_ptr<PadHandlerBase>> m_handlers;
    void* m_curthread = nullptr;
    void* m_curwindow = nullptr;
    PadInfo m_info{ 0, 0, false };
    std::array<std::shared_ptr<Pad>, CELL_PAD_MAX_PORT_NUM> m_pads{};
    std::array<bool, CELL_PAD_MAX_PORT_NUM> m_pads_connected{};
    u32 num_ldd_pad = 0;

private:
    void apply_copilots();
    void update_pad_states();

    u32 m_mask_start_press_to_resume = 0;
    u64 m_track_start_press_begin_timestamp = 0;
    bool m_resume_emulation_flag = false;
    bool m_ps_button_pressed = false;
    atomic_t<bool> m_home_menu_open = false;
};

// pad namespace globals
namespace pad
{
    atomic_t<pad_thread*> g_pad_thread{nullptr};
    shared_mutex g_pad_mutex;
    std::string g_title_id;
    atomic_t<bool> g_enabled{false};
    atomic_t<bool> g_reset{false};
    atomic_t<bool> g_started{false};
    atomic_t<bool> g_home_menu_requested{false};
}

// pad_thread implementations
pad_thread::pad_thread(void*, void*, std::string_view title_id)
{
    pad::g_title_id = title_id;
    pad::g_pad_thread = this;
    pad::g_started = false;
}

pad_thread::~pad_thread()
{
    pad::g_pad_thread = nullptr;
}
void pad_thread::operator()() {}
void pad_thread::SetRumble(u32, u8, u8) {}
void pad_thread::SetIntercepted(bool) {}
s32 pad_thread::AddLddPad() { return -1; }
void pad_thread::UnregisterLddPad(u32) {}
void pad_thread::open_home_menu() {}
void pad_thread::Init() {}
void pad_thread::InitLddPad(u32, const u32*) {}
void pad_thread::apply_copilots() {}
void pad_thread::update_pad_states() {}

std::shared_ptr<PadHandlerBase> pad_thread::GetHandler(pad_handler) { return nullptr; }
void pad_thread::InitPadConfig(cfg_pad&, pad_handler, std::shared_ptr<PadHandlerBase>&) {}

// ============================================================================
// input namespace stubs
// ============================================================================

namespace input
{
    std::vector<product_info> get_products_by_class(int)
    {
        return {};
    }
}
