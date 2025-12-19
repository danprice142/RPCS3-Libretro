#pragma once

#include "Emu/Io/PadHandler.h"
#include "libretro_input.h"

// Libretro pad handler - connects libretro input to RPCS3's pad system
class LibretroPadHandler final : public PadHandlerBase
{
public:
    LibretroPadHandler();
    ~LibretroPadHandler() override = default;

    bool Init() override;
    void process() override;
    std::vector<pad_list_entry> list_devices() override;
    void init_config(cfg_pad* cfg) override;
    bool bindPadToDevice(std::shared_ptr<Pad> pad) override;

private:
    // Button codes for libretro mapping
    enum class LibretroButton : u32
    {
        B = 0,      // Cross
        Y,          // Square
        Select,
        Start,
        Up,
        Down,
        Left,
        Right,
        A,          // Circle
        X,          // Triangle
        L1,
        R1,
        L2,
        R2,
        L3,
        R3,
        LSXNeg,     // Left stick X negative
        LSXPos,     // Left stick X positive
        LSYNeg,     // Left stick Y negative
        LSYPos,     // Left stick Y positive
        RSXNeg,     // Right stick X negative
        RSXPos,     // Right stick X positive
        RSYNeg,     // Right stick Y negative
        RSYPos,     // Right stick Y positive
        Count
    };

    std::shared_ptr<PadDevice> get_device(const std::string& device) override;
    connection update_connection(const std::shared_ptr<PadDevice>& device) override;
    std::unordered_map<u64, u16> get_button_values(const std::shared_ptr<PadDevice>& device) override;
    pad_preview_values get_preview_values(const std::unordered_map<u64, u16>& data) override;

    // Convert libretro analog value (-32768 to 32767) to 0-255 range
    static u16 ConvertAnalogValue(int16_t value);
};
