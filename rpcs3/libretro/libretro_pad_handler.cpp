#include "stdafx.h"
#include "libretro_pad_handler.h"
#include "libretro_input.h"

LibretroPadHandler::LibretroPadHandler()
    : PadHandlerBase(pad_handler::keyboard)  // Use keyboard type as base since we're a custom handler
{
    m_name_string = "Libretro";
    m_max_devices = LIBRETRO_MAX_PADS;
    m_trigger_threshold = 0;
    m_thumb_threshold = 0;

    // Libretro handles all input, no special hardware features
    b_has_led = false;
    b_has_rgb = false;
    b_has_player_led = false;
    b_has_battery = false;
    b_has_battery_led = false;
    b_has_deadzones = true;
    b_has_rumble = false;  // TODO: Could support via RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE
    b_has_motion = false;
    b_has_config = false;
    b_has_pressure_intensity_button = false;
    b_has_analog_limiter_button = false;

    // Button mapping from libretro to RPCS3
    button_list = {
        { static_cast<u32>(LibretroButton::B),      "Cross" },
        { static_cast<u32>(LibretroButton::A),      "Circle" },
        { static_cast<u32>(LibretroButton::Y),      "Square" },
        { static_cast<u32>(LibretroButton::X),      "Triangle" },
        { static_cast<u32>(LibretroButton::Start),  "Start" },
        { static_cast<u32>(LibretroButton::Select), "Select" },
        { static_cast<u32>(LibretroButton::Up),     "Up" },
        { static_cast<u32>(LibretroButton::Down),   "Down" },
        { static_cast<u32>(LibretroButton::Left),   "Left" },
        { static_cast<u32>(LibretroButton::Right),  "Right" },
        { static_cast<u32>(LibretroButton::L1),     "L1" },
        { static_cast<u32>(LibretroButton::R1),     "R1" },
        { static_cast<u32>(LibretroButton::L2),     "L2" },
        { static_cast<u32>(LibretroButton::R2),     "R2" },
        { static_cast<u32>(LibretroButton::L3),     "L3" },
        { static_cast<u32>(LibretroButton::R3),     "R3" },
        { static_cast<u32>(LibretroButton::LSXNeg), "LS X-" },
        { static_cast<u32>(LibretroButton::LSXPos), "LS X+" },
        { static_cast<u32>(LibretroButton::LSYNeg), "LS Y-" },
        { static_cast<u32>(LibretroButton::LSYPos), "LS Y+" },
        { static_cast<u32>(LibretroButton::RSXNeg), "RS X-" },
        { static_cast<u32>(LibretroButton::RSXPos), "RS X+" },
        { static_cast<u32>(LibretroButton::RSYNeg), "RS Y-" },
        { static_cast<u32>(LibretroButton::RSYPos), "RS Y+" },
    };
}

bool LibretroPadHandler::Init()
{
    m_is_init = true;
    return true;
}

std::vector<pad_list_entry> LibretroPadHandler::list_devices()
{
    std::vector<pad_list_entry> devices;
    for (unsigned i = 0; i < LIBRETRO_MAX_PADS; i++)
    {
        devices.emplace_back(fmt::format("Libretro Pad %d", i + 1), false);
    }
    return devices;
}

void LibretroPadHandler::init_config(cfg_pad* cfg)
{
    if (!cfg) return;

    // Set default button mappings
    cfg->ls_left.def  = button_list.at(static_cast<u32>(LibretroButton::LSXNeg));
    cfg->ls_down.def  = button_list.at(static_cast<u32>(LibretroButton::LSYPos));
    cfg->ls_right.def = button_list.at(static_cast<u32>(LibretroButton::LSXPos));
    cfg->ls_up.def    = button_list.at(static_cast<u32>(LibretroButton::LSYNeg));
    cfg->rs_left.def  = button_list.at(static_cast<u32>(LibretroButton::RSXNeg));
    cfg->rs_down.def  = button_list.at(static_cast<u32>(LibretroButton::RSYPos));
    cfg->rs_right.def = button_list.at(static_cast<u32>(LibretroButton::RSXPos));
    cfg->rs_up.def    = button_list.at(static_cast<u32>(LibretroButton::RSYNeg));
    cfg->start.def    = button_list.at(static_cast<u32>(LibretroButton::Start));
    cfg->select.def   = button_list.at(static_cast<u32>(LibretroButton::Select));
    cfg->ps.def       = "";  // PS button not mapped by default
    cfg->square.def   = button_list.at(static_cast<u32>(LibretroButton::Y));
    cfg->cross.def    = button_list.at(static_cast<u32>(LibretroButton::B));
    cfg->circle.def   = button_list.at(static_cast<u32>(LibretroButton::A));
    cfg->triangle.def = button_list.at(static_cast<u32>(LibretroButton::X));
    cfg->left.def     = button_list.at(static_cast<u32>(LibretroButton::Left));
    cfg->down.def     = button_list.at(static_cast<u32>(LibretroButton::Down));
    cfg->right.def    = button_list.at(static_cast<u32>(LibretroButton::Right));
    cfg->up.def       = button_list.at(static_cast<u32>(LibretroButton::Up));
    cfg->r1.def       = button_list.at(static_cast<u32>(LibretroButton::R1));
    cfg->r2.def       = button_list.at(static_cast<u32>(LibretroButton::R2));
    cfg->r3.def       = button_list.at(static_cast<u32>(LibretroButton::R3));
    cfg->l1.def       = button_list.at(static_cast<u32>(LibretroButton::L1));
    cfg->l2.def       = button_list.at(static_cast<u32>(LibretroButton::L2));
    cfg->l3.def       = button_list.at(static_cast<u32>(LibretroButton::L3));

    cfg->pressure_intensity_button.def = "";
    cfg->analog_limiter_button.def = "";

    // Apply defaults
    cfg->from_default();
}

bool LibretroPadHandler::bindPadToDevice(std::shared_ptr<Pad> pad)
{
    if (!pad)
        return false;

    // Create a device for this pad
    auto device = std::make_shared<PadDevice>();
    m_bindings.emplace_back(pad, device, nullptr);

    // CRITICAL: Initialize the pad's buttons and sticks
    // Without this, m_buttons is empty and process() has nothing to update
    pad->m_buttons.clear();
    pad->m_buttons.reserve(17);

    // Button constructor: Button(u32 offset, std::set<u32> key_codes, u32 outKeyCode)
    // We pass empty set for key_codes since we handle input directly in process()

    // Digital buttons - DIGITAL1 group (directly from cellPad)
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{}, CELL_PAD_CTRL_UP);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{}, CELL_PAD_CTRL_DOWN);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{}, CELL_PAD_CTRL_LEFT);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{}, CELL_PAD_CTRL_RIGHT);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{}, CELL_PAD_CTRL_SELECT);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{}, CELL_PAD_CTRL_START);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{}, CELL_PAD_CTRL_L3);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{}, CELL_PAD_CTRL_R3);

    // Digital buttons - DIGITAL2 group
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{}, CELL_PAD_CTRL_CROSS);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{}, CELL_PAD_CTRL_CIRCLE);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{}, CELL_PAD_CTRL_SQUARE);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{}, CELL_PAD_CTRL_TRIANGLE);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{}, CELL_PAD_CTRL_L1);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{}, CELL_PAD_CTRL_R1);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{}, CELL_PAD_CTRL_L2);
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{}, CELL_PAD_CTRL_R2);

    // PS button (optional)
    pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{}, CELL_PAD_CTRL_PS);

    // Initialize analog sticks (center position = 128, default value)
    pad->m_sticks[0].m_offset = CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X;
    pad->m_sticks[1].m_offset = CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y;
    pad->m_sticks[2].m_offset = CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X;
    pad->m_sticks[3].m_offset = CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y;

    // Set pad as connected
    pad->m_port_status |= CELL_PAD_STATUS_CONNECTED;
    return true;
}

std::shared_ptr<PadDevice> LibretroPadHandler::get_device(const std::string& device)
{
    // Extract port number from device name "Libretro Pad X"
    if (device.find("Libretro Pad") != std::string::npos)
    {
        return std::make_shared<PadDevice>();
    }
    return nullptr;
}

PadHandlerBase::connection LibretroPadHandler::update_connection(const std::shared_ptr<PadDevice>& device)
{
    if (!device)
        return connection::disconnected;

    // Libretro controllers are always connected (frontend handles actual connection)
    return connection::connected;
}

u16 LibretroPadHandler::ConvertAnalogValue(int16_t value)
{
    // Convert from -32768..32767 to 0..255
    // First normalize to 0..65535, then scale to 0..255
    int normalized = static_cast<int>(value) + 32768;
    return static_cast<u16>((normalized * 255) / 65535);
}

std::unordered_map<u64, u16> LibretroPadHandler::get_button_values(const std::shared_ptr<PadDevice>& device)
{
    std::unordered_map<u64, u16> values;

    if (!device)
        return values;

    // Get the port from player_id
    unsigned port = device->player_id;
    if (port >= LIBRETRO_MAX_PADS)
        port = 0;

    const auto& state = libretro_input_get_state(port);

    // Digital buttons (0 or 255)
    values[static_cast<u64>(LibretroButton::B)]      = state.buttons[RETRO_DEVICE_ID_JOYPAD_B] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::Y)]      = state.buttons[RETRO_DEVICE_ID_JOYPAD_Y] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::Select)] = state.buttons[RETRO_DEVICE_ID_JOYPAD_SELECT] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::Start)]  = state.buttons[RETRO_DEVICE_ID_JOYPAD_START] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::Up)]     = state.buttons[RETRO_DEVICE_ID_JOYPAD_UP] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::Down)]   = state.buttons[RETRO_DEVICE_ID_JOYPAD_DOWN] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::Left)]   = state.buttons[RETRO_DEVICE_ID_JOYPAD_LEFT] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::Right)]  = state.buttons[RETRO_DEVICE_ID_JOYPAD_RIGHT] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::A)]      = state.buttons[RETRO_DEVICE_ID_JOYPAD_A] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::X)]      = state.buttons[RETRO_DEVICE_ID_JOYPAD_X] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::L1)]     = state.buttons[RETRO_DEVICE_ID_JOYPAD_L] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::R1)]     = state.buttons[RETRO_DEVICE_ID_JOYPAD_R] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::L2)]     = state.buttons[RETRO_DEVICE_ID_JOYPAD_L2] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::R2)]     = state.buttons[RETRO_DEVICE_ID_JOYPAD_R2] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::L3)]     = state.buttons[RETRO_DEVICE_ID_JOYPAD_L3] ? 255 : 0;
    values[static_cast<u64>(LibretroButton::R3)]     = state.buttons[RETRO_DEVICE_ID_JOYPAD_R3] ? 255 : 0;

    // Analog sticks - state.analog[] contains values in range -32768 to 32767
    // Left stick X: analog[0], Y: analog[1]
    // Right stick X: analog[2], Y: analog[3]
    int16_t lsx = state.analog[0];
    int16_t lsy = state.analog[1];
    int16_t rsx = state.analog[2];
    int16_t rsy = state.analog[3];

    // Convert to directional values (0-255 range)
    // Negative X = left, Positive X = right
    // Negative Y = up, Positive Y = down (libretro uses Y-down convention)
    values[static_cast<u64>(LibretroButton::LSXNeg)] = lsx < 0 ? static_cast<u16>((-lsx * 255) / 32768) : 0;
    values[static_cast<u64>(LibretroButton::LSXPos)] = lsx > 0 ? static_cast<u16>((lsx * 255) / 32767) : 0;
    values[static_cast<u64>(LibretroButton::LSYNeg)] = lsy < 0 ? static_cast<u16>((-lsy * 255) / 32768) : 0;
    values[static_cast<u64>(LibretroButton::LSYPos)] = lsy > 0 ? static_cast<u16>((lsy * 255) / 32767) : 0;
    values[static_cast<u64>(LibretroButton::RSXNeg)] = rsx < 0 ? static_cast<u16>((-rsx * 255) / 32768) : 0;
    values[static_cast<u64>(LibretroButton::RSXPos)] = rsx > 0 ? static_cast<u16>((rsx * 255) / 32767) : 0;
    values[static_cast<u64>(LibretroButton::RSYNeg)] = rsy < 0 ? static_cast<u16>((-rsy * 255) / 32768) : 0;
    values[static_cast<u64>(LibretroButton::RSYPos)] = rsy > 0 ? static_cast<u16>((rsy * 255) / 32767) : 0;

    return values;
}

pad_preview_values LibretroPadHandler::get_preview_values(const std::unordered_map<u64, u16>& data)
{
    return {
        data.at(static_cast<u64>(LibretroButton::LSXNeg)) - data.at(static_cast<u64>(LibretroButton::LSXPos)),
        data.at(static_cast<u64>(LibretroButton::LSYPos)) - data.at(static_cast<u64>(LibretroButton::LSYNeg)),
        data.at(static_cast<u64>(LibretroButton::RSXNeg)) - data.at(static_cast<u64>(LibretroButton::RSXPos)),
        data.at(static_cast<u64>(LibretroButton::RSYPos)) - data.at(static_cast<u64>(LibretroButton::RSYNeg)),
        0,
        0
    };
}

void LibretroPadHandler::process()
{
    // Process each bound pad
    for (usz i = 0; i < m_bindings.size(); i++)
    {
        auto& binding = m_bindings[i];
        if (!binding.pad || !binding.device)
            continue;

        // Set player ID for device
        binding.device->player_id = static_cast<u8>(i);

        // Check connection
        const auto status = update_connection(binding.device);
        if (status == connection::connected)
        {
            binding.pad->m_port_status |= CELL_PAD_STATUS_CONNECTED;
        }
        else
        {
            binding.pad->m_port_status &= ~CELL_PAD_STATUS_CONNECTED;
            continue;
        }

        // Get button values from libretro input
        auto button_values = get_button_values(binding.device);

        // Update pad button states in m_buttons vector
        for (auto& button : binding.pad->m_buttons)
        {
            u16 value = 0;

            switch (button.m_offset)
            {
            case CELL_PAD_BTN_OFFSET_DIGITAL1:
                switch (button.m_outKeyCode)
                {
                case CELL_PAD_CTRL_UP:     value = button_values[static_cast<u64>(LibretroButton::Up)];     break;
                case CELL_PAD_CTRL_DOWN:   value = button_values[static_cast<u64>(LibretroButton::Down)];   break;
                case CELL_PAD_CTRL_LEFT:   value = button_values[static_cast<u64>(LibretroButton::Left)];   break;
                case CELL_PAD_CTRL_RIGHT:  value = button_values[static_cast<u64>(LibretroButton::Right)];  break;
                case CELL_PAD_CTRL_SELECT: value = button_values[static_cast<u64>(LibretroButton::Select)]; break;
                case CELL_PAD_CTRL_START:  value = button_values[static_cast<u64>(LibretroButton::Start)];  break;
                case CELL_PAD_CTRL_L3:     value = button_values[static_cast<u64>(LibretroButton::L3)];     break;
                case CELL_PAD_CTRL_R3:     value = button_values[static_cast<u64>(LibretroButton::R3)];     break;
                default: break;
                }
                break;
            case CELL_PAD_BTN_OFFSET_DIGITAL2:
                switch (button.m_outKeyCode)
                {
                case CELL_PAD_CTRL_CROSS:    value = button_values[static_cast<u64>(LibretroButton::B)];  break;
                case CELL_PAD_CTRL_CIRCLE:   value = button_values[static_cast<u64>(LibretroButton::A)];  break;
                case CELL_PAD_CTRL_SQUARE:   value = button_values[static_cast<u64>(LibretroButton::Y)];  break;
                case CELL_PAD_CTRL_TRIANGLE: value = button_values[static_cast<u64>(LibretroButton::X)];  break;
                case CELL_PAD_CTRL_L1:       value = button_values[static_cast<u64>(LibretroButton::L1)]; break;
                case CELL_PAD_CTRL_R1:       value = button_values[static_cast<u64>(LibretroButton::R1)]; break;
                case CELL_PAD_CTRL_L2:       value = button_values[static_cast<u64>(LibretroButton::L2)]; break;
                case CELL_PAD_CTRL_R2:       value = button_values[static_cast<u64>(LibretroButton::R2)]; break;
                default: break;
                }
                break;
            default:
                break;
            }

            button.m_value = value;
            button.m_pressed = value > 0;
        }

        // Update analog sticks
        // Convert from directional 0-255 to centered 0-255 format (128 = center)
        u16 lsx_neg = button_values[static_cast<u64>(LibretroButton::LSXNeg)];
        u16 lsx_pos = button_values[static_cast<u64>(LibretroButton::LSXPos)];
        u16 lsy_neg = button_values[static_cast<u64>(LibretroButton::LSYNeg)];
        u16 lsy_pos = button_values[static_cast<u64>(LibretroButton::LSYPos)];
        u16 rsx_neg = button_values[static_cast<u64>(LibretroButton::RSXNeg)];
        u16 rsx_pos = button_values[static_cast<u64>(LibretroButton::RSXPos)];
        u16 rsy_neg = button_values[static_cast<u64>(LibretroButton::RSYNeg)];
        u16 rsy_pos = button_values[static_cast<u64>(LibretroButton::RSYPos)];

        // Convert to centered format: 128 = center, 0 = full left/up, 255 = full right/down
        u8 analog_left_x  = static_cast<u8>(128 + (lsx_pos / 2) - (lsx_neg / 2));
        u8 analog_left_y  = static_cast<u8>(128 + (lsy_pos / 2) - (lsy_neg / 2));
        u8 analog_right_x = static_cast<u8>(128 + (rsx_pos / 2) - (rsx_neg / 2));
        u8 analog_right_y = static_cast<u8>(128 + (rsy_pos / 2) - (rsy_neg / 2));

        // Update analog stick axes in m_sticks array
        for (auto& stick : binding.pad->m_sticks)
        {
            switch (stick.m_offset)
            {
            case CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X:  stick.m_value = analog_left_x;  break;
            case CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y:  stick.m_value = analog_left_y;  break;
            case CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X: stick.m_value = analog_right_x; break;
            case CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y: stick.m_value = analog_right_y; break;
            default: break;
            }
        }
    }
}
