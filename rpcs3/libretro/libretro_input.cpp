#include "stdafx.h"

#include "libretro_input.h"
#include "libretro_core.h"

#include "Emu/Io/pad_config.h"
#include "Emu/Io/pad_types.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"

#include <cstring>

static std::array<LibretroInputState, LIBRETRO_MAX_PADS> s_input_states;
static std::array<unsigned, LIBRETRO_MAX_PADS> s_device_types;
static LibretroMouseState s_mouse_state;
static LibretroKeyboardState s_keyboard_state;
static bool s_initialized = false;
static bool s_bitmask_supported = false;

#ifndef RETRO_DEVICE_ID_JOYPAD_MASK
// Standard libretro ID for bitmask joypad polling
#define RETRO_DEVICE_ID_JOYPAD_MASK 16
#endif

// retro_key enum is defined in libretro.h

void libretro_input_init()
{
    for (auto& state : s_input_states)
    {
        state = LibretroInputState{};
        state.connected = true;
    }

    for (auto& type : s_device_types)
    {
        type = RETRO_DEVICE_JOYPAD;
    }

    s_mouse_state = LibretroMouseState{};
    s_keyboard_state = LibretroKeyboardState{};
    s_initialized = true;
}

void libretro_input_set_bitmask_supported(bool supported)
{
    s_bitmask_supported = supported;
}

void libretro_input_deinit()
{
    s_initialized = false;
}

void libretro_input_poll(retro_input_state_t input_state_cb)
{
    if (!s_initialized || !input_state_cb)
        return;

    // Poll controllers
    for (unsigned port = 0; port < LIBRETRO_MAX_PADS; port++)
    {
        auto& state = s_input_states[port];

        if (s_device_types[port] == RETRO_DEVICE_NONE)
        {
            state.connected = false;
            continue;
        }

        state.connected = true;

        // Poll digital buttons
        // Prefer bitmask polling if supported (more reliable and faster)
        if (s_bitmask_supported)
        {
            const u16 mask = static_cast<u16>(input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK));
            for (unsigned btn = 0; btn <= RETRO_DEVICE_ID_JOYPAD_R3; btn++)
            {
                state.buttons[btn] = (mask & (1u << btn)) ? 1 : 0;
            }
        }
        else
        {
            for (unsigned btn = 0; btn <= RETRO_DEVICE_ID_JOYPAD_R3; btn++)
            {
                state.buttons[btn] = input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, btn);
            }
        }

        // Poll analog sticks
        state.analog[0] = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
        state.analog[1] = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
        state.analog[2] = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
        state.analog[3] = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
    }

    // Poll mouse (port 0)
    s_mouse_state.x = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
    s_mouse_state.y = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
    s_mouse_state.left = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
    s_mouse_state.right = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0;
    s_mouse_state.middle = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE) != 0;
    s_mouse_state.button4 = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_BUTTON_4) != 0;
    s_mouse_state.button5 = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_BUTTON_5) != 0;
    s_mouse_state.wheel_v = static_cast<int8_t>(
        (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP) ? 1 : 0) -
        (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN) ? 1 : 0));
    s_mouse_state.wheel_h = static_cast<int8_t>(
        (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP) ? 1 : 0) -
        (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN) ? 1 : 0));

    // Poll pointer for absolute mouse position
    s_mouse_state.abs_x = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
    s_mouse_state.abs_y = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);

    // Poll keyboard (common keys)
    const unsigned keys_to_poll[] = {
        RETROK_BACKSPACE, RETROK_TAB, RETROK_RETURN, RETROK_ESCAPE, RETROK_SPACE,
        RETROK_0, RETROK_1, RETROK_2, RETROK_3, RETROK_4, RETROK_5, RETROK_6, RETROK_7, RETROK_8, RETROK_9,
        RETROK_a, RETROK_b, RETROK_c, RETROK_d, RETROK_e, RETROK_f, RETROK_g, RETROK_h, RETROK_i, RETROK_j,
        RETROK_k, RETROK_l, RETROK_m, RETROK_n, RETROK_o, RETROK_p, RETROK_q, RETROK_r, RETROK_s, RETROK_t,
        RETROK_u, RETROK_v, RETROK_w, RETROK_x, RETROK_y, RETROK_z,
        RETROK_DELETE, RETROK_UP, RETROK_DOWN, RETROK_RIGHT, RETROK_LEFT,
        RETROK_INSERT, RETROK_HOME, RETROK_END, RETROK_PAGEUP, RETROK_PAGEDOWN,
        RETROK_F1, RETROK_F2, RETROK_F3, RETROK_F4, RETROK_F5, RETROK_F6,
        RETROK_F7, RETROK_F8, RETROK_F9, RETROK_F10, RETROK_F11, RETROK_F12,
        RETROK_RSHIFT, RETROK_LSHIFT, RETROK_RCTRL, RETROK_LCTRL, RETROK_RALT, RETROK_LALT
    };

    for (unsigned key : keys_to_poll)
    {
        if (key < LIBRETRO_MAX_KEYS)
        {
            s_keyboard_state.keys[key] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, key) != 0;
        }
    }
}

void libretro_input_set_controller(unsigned port, unsigned device)
{
    if (port < LIBRETRO_MAX_PADS)
    {
        s_device_types[port] = device;
        s_input_states[port].connected = (device != RETRO_DEVICE_NONE);
    }
}

bool libretro_input_get_button(unsigned port, unsigned button)
{
    if (port >= LIBRETRO_MAX_PADS || button >= 16)
        return false;

    return s_input_states[port].buttons[button] != 0;
}

int16_t libretro_input_get_analog(unsigned port, unsigned index, unsigned id)
{
    if (port >= LIBRETRO_MAX_PADS)
        return 0;

    unsigned analog_index = (index * 2) + id;
    if (analog_index >= 4)
        return 0;

    return s_input_states[port].analog[analog_index];
}

const LibretroInputState& libretro_input_get_state(unsigned port)
{
    static LibretroInputState dummy{};
    if (port >= LIBRETRO_MAX_PADS)
        return dummy;

    return s_input_states[port];
}

const LibretroMouseState& libretro_input_get_mouse()
{
    return s_mouse_state;
}

const LibretroKeyboardState& libretro_input_get_keyboard()
{
    return s_keyboard_state;
}

bool libretro_input_key_pressed(unsigned keycode)
{
    if (keycode >= LIBRETRO_MAX_KEYS)
        return false;
    return s_keyboard_state.keys[keycode];
}

// Input descriptors define the default button mappings shown in RetroArch
// These map RetroPad buttons to PS3 DualShock 3 buttons
void libretro_input_set_descriptors(retro_environment_t environ_cb)
{
    static const struct retro_input_descriptor desc[] = {
        // Port 1 - Player 1
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Cross" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Square" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Circle" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Triangle" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L1" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R1" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "L2" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "R2" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "L3" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "R3" },
        { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
        { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
        { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
        { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

        // Port 2 - Player 2
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Cross" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Square" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Circle" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Triangle" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L1" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R1" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "L2" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "R2" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "L3" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "R3" },
        { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
        { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
        { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
        { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

        // Terminator
        { 0, 0, 0, 0, NULL }
    };

    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)desc);
}

// Controller info tells RetroArch what controller types are supported
void libretro_input_set_controller_info(retro_environment_t environ_cb)
{
    // Define the controller types supported per port
    // Use RETRO_DEVICE_JOYPAD (RetroPad) which works with ANY controller
    // RetroArch handles mapping from physical controller (Xbox, PS, etc.) to RetroPad
    static const struct retro_controller_description controllers[] = {
        { "RetroPad", RETRO_DEVICE_JOYPAD },
        { "None", RETRO_DEVICE_NONE },
    };

    static const struct retro_controller_info ports[] = {
        { controllers, 2 },  // Port 1
        { controllers, 2 },  // Port 2
        { controllers, 2 },  // Port 3
        { controllers, 2 },  // Port 4
        { controllers, 2 },  // Port 5
        { controllers, 2 },  // Port 6
        { controllers, 2 },  // Port 7
        { NULL, 0 }          // Terminator
    };

    environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

// Sensor interface for gyro/accelerometer support
static retro_set_sensor_state_t s_sensor_set_state_cb = nullptr;
static retro_sensor_get_input_t s_sensor_get_input_cb = nullptr;
static bool s_sensors_available = false;

// Sensor data storage
struct SensorData
{
    float gyro_x = 0.0f;
    float gyro_y = 0.0f;
    float gyro_z = 0.0f;
    float accel_x = 0.0f;
    float accel_y = 0.0f;
    float accel_z = 0.0f;
};
static std::array<SensorData, LIBRETRO_MAX_PADS> s_sensor_data;

bool libretro_input_init_sensors(retro_environment_t environ_cb)
{
    if (!environ_cb)
        return false;

    // Get sensor interface from frontend
    struct retro_sensor_interface sensor_interface;
    if (environ_cb(RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE, &sensor_interface))
    {
        s_sensor_set_state_cb = sensor_interface.set_sensor_state;
        s_sensor_get_input_cb = sensor_interface.get_sensor_input;

        if (s_sensor_set_state_cb && s_sensor_get_input_cb)
        {
            // Try to enable gyro and accelerometer for port 0
            bool gyro_enabled = s_sensor_set_state_cb(0, RETRO_SENSOR_GYROSCOPE_ENABLE, 1);
            bool accel_enabled = s_sensor_set_state_cb(0, RETRO_SENSOR_ACCELEROMETER_ENABLE, 1);

            s_sensors_available = gyro_enabled || accel_enabled;
            return s_sensors_available;
        }
    }

    s_sensors_available = false;
    return false;
}

void libretro_input_poll_sensors()
{
    if (!s_sensors_available || !s_sensor_get_input_cb)
        return;

    // Poll sensor data for port 0 (primary controller)
    s_sensor_data[0].gyro_x = s_sensor_get_input_cb(0, RETRO_SENSOR_GYROSCOPE_X);
    s_sensor_data[0].gyro_y = s_sensor_get_input_cb(0, RETRO_SENSOR_GYROSCOPE_Y);
    s_sensor_data[0].gyro_z = s_sensor_get_input_cb(0, RETRO_SENSOR_GYROSCOPE_Z);
    s_sensor_data[0].accel_x = s_sensor_get_input_cb(0, RETRO_SENSOR_ACCELEROMETER_X);
    s_sensor_data[0].accel_y = s_sensor_get_input_cb(0, RETRO_SENSOR_ACCELEROMETER_Y);
    s_sensor_data[0].accel_z = s_sensor_get_input_cb(0, RETRO_SENSOR_ACCELEROMETER_Z);
}

void libretro_input_get_gyro(unsigned port, float& x, float& y, float& z)
{
    if (port >= LIBRETRO_MAX_PADS)
    {
        x = y = z = 0.0f;
        return;
    }

    x = s_sensor_data[port].gyro_x;
    y = s_sensor_data[port].gyro_y;
    z = s_sensor_data[port].gyro_z;
}

void libretro_input_get_accel(unsigned port, float& x, float& y, float& z)
{
    if (port >= LIBRETRO_MAX_PADS)
    {
        x = y = z = 0.0f;
        return;
    }

    x = s_sensor_data[port].accel_x;
    y = s_sensor_data[port].accel_y;
    z = s_sensor_data[port].accel_z;
}
