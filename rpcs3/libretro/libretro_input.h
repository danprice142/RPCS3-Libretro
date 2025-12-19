#pragma once

#include "libretro.h"

#include <array>
#include <cstdint>
#include <functional>

// Number of controller ports
constexpr unsigned LIBRETRO_MAX_PADS = 7;

// Number of keyboard keys to track
constexpr unsigned LIBRETRO_MAX_KEYS = 320;

// Libretro input state structure for controllers
struct LibretroInputState
{
    std::array<int16_t, 16> buttons{};  // RETRO_DEVICE_ID_JOYPAD_*
    std::array<int16_t, 4> analog{};    // Left X, Left Y, Right X, Right Y
    bool connected = false;
};

// Libretro mouse state
struct LibretroMouseState
{
    int16_t x = 0;           // Relative X movement
    int16_t y = 0;           // Relative Y movement
    int16_t abs_x = 0;       // Absolute X position (pointer)
    int16_t abs_y = 0;       // Absolute Y position (pointer)
    bool left = false;       // Left button
    bool right = false;      // Right button
    bool middle = false;     // Middle button
    bool button4 = false;    // Button 4
    bool button5 = false;    // Button 5
    int8_t wheel_v = 0;      // Vertical wheel
    int8_t wheel_h = 0;      // Horizontal wheel
};

// Libretro keyboard state
struct LibretroKeyboardState
{
    std::array<bool, LIBRETRO_MAX_KEYS> keys{};  // Key states indexed by RETROK_*
};

// Initialize libretro input system
void libretro_input_init();

// Deinitialize libretro input system
void libretro_input_deinit();

// Poll input state from libretro frontend
void libretro_input_poll(retro_input_state_t input_state_cb);

// Configure whether the frontend supports RETRO_DEVICE_ID_JOYPAD_MASK bitmask polling
void libretro_input_set_bitmask_supported(bool supported);

// Set controller type for a port
void libretro_input_set_controller(unsigned port, unsigned device);

// Get button state for a specific port and button
bool libretro_input_get_button(unsigned port, unsigned button);

// Get analog axis value for a specific port, stick, and axis
int16_t libretro_input_get_analog(unsigned port, unsigned index, unsigned id);

// Get current input state for a port
const LibretroInputState& libretro_input_get_state(unsigned port);

// Get mouse state
const LibretroMouseState& libretro_input_get_mouse();

// Get keyboard state
const LibretroKeyboardState& libretro_input_get_keyboard();

// Check if a specific key is pressed
bool libretro_input_key_pressed(unsigned keycode);

// Set up default input descriptors for RetroArch
void libretro_input_set_descriptors(retro_environment_t environ_cb);

// Set up controller info for RetroArch
void libretro_input_set_controller_info(retro_environment_t environ_cb);
