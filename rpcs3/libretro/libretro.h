/* Copyright (C) 2010-2020 The RetroArch team
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef LIBRETRO_H__
#define LIBRETRO_H__

#include <stdint.h>
#include <stddef.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __cplusplus
#if defined(_MSC_VER) && _MSC_VER < 1800 && !defined(SN_TARGET_PS3)
typedef unsigned char bool;
#else
#include <stdbool.h>
#endif
#endif

#ifndef RETRO_CALLCONV
#  if defined(__GNUC__) && defined(__i386__) && !defined(__x86_64__)
#    define RETRO_CALLCONV __attribute__((cdecl))
#  elif defined(_MSC_VER) && defined(_M_X86) && !defined(_M_X64)
#    define RETRO_CALLCONV __cdecl
#  else
#    define RETRO_CALLCONV
#  endif
#endif

#ifndef RETRO_API
#  if defined(_WIN32) || defined(__CYGWIN__) || defined(__MINGW32__)
#    ifdef RETRO_IMPORT_SYMBOLS
#      ifdef __GNUC__
#        define RETRO_API RETRO_CALLCONV __attribute__((__dllimport__))
#      else
#        define RETRO_API RETRO_CALLCONV __declspec(dllimport)
#      endif
#    else
#      ifdef __GNUC__
#        define RETRO_API RETRO_CALLCONV __attribute__((__dllexport__))
#      else
#        define RETRO_API RETRO_CALLCONV __declspec(dllexport)
#      endif
#    endif
#  else
#      if defined(__GNUC__) && __GNUC__ >= 4
#        define RETRO_API RETRO_CALLCONV __attribute__((__visibility__("default")))
#      else
#        define RETRO_API RETRO_CALLCONV
#      endif
#  endif
#endif

/* Used for checking API/ABI mismatches that can break libretro
 * implementations. */
#define RETRO_API_VERSION         1

/* libretro's fundamental device abstractions. */
#define RETRO_DEVICE_NONE         0
#define RETRO_DEVICE_JOYPAD       1
#define RETRO_DEVICE_MOUSE        2
#define RETRO_DEVICE_KEYBOARD     3
#define RETRO_DEVICE_LIGHTGUN     4
#define RETRO_DEVICE_ANALOG       5
#define RETRO_DEVICE_POINTER      6

#define RETRO_DEVICE_TYPE_SHIFT         8
#define RETRO_DEVICE_SUBCLASS(base, id) (((id + 1) << RETRO_DEVICE_TYPE_SHIFT) | base)

/* Buttons for the RetroPad (JOYPAD). */
#define RETRO_DEVICE_ID_JOYPAD_B        0
#define RETRO_DEVICE_ID_JOYPAD_Y        1
#define RETRO_DEVICE_ID_JOYPAD_SELECT   2
#define RETRO_DEVICE_ID_JOYPAD_START    3
#define RETRO_DEVICE_ID_JOYPAD_UP       4
#define RETRO_DEVICE_ID_JOYPAD_DOWN     5
#define RETRO_DEVICE_ID_JOYPAD_LEFT     6
#define RETRO_DEVICE_ID_JOYPAD_RIGHT    7
#define RETRO_DEVICE_ID_JOYPAD_A        8
#define RETRO_DEVICE_ID_JOYPAD_X        9
#define RETRO_DEVICE_ID_JOYPAD_L       10
#define RETRO_DEVICE_ID_JOYPAD_R       11
#define RETRO_DEVICE_ID_JOYPAD_L2      12
#define RETRO_DEVICE_ID_JOYPAD_R2      13
#define RETRO_DEVICE_ID_JOYPAD_L3      14
#define RETRO_DEVICE_ID_JOYPAD_R3      15

/* Analog indexes */
#define RETRO_DEVICE_INDEX_ANALOG_LEFT   0
#define RETRO_DEVICE_INDEX_ANALOG_RIGHT  1
#define RETRO_DEVICE_INDEX_ANALOG_BUTTON 2
#define RETRO_DEVICE_ID_ANALOG_X         0
#define RETRO_DEVICE_ID_ANALOG_Y         1

/* Pointer indexes */
#define RETRO_DEVICE_ID_POINTER_X         0
#define RETRO_DEVICE_ID_POINTER_Y         1
#define RETRO_DEVICE_ID_POINTER_PRESSED   2
#define RETRO_DEVICE_ID_POINTER_COUNT     3

/* Lightgun indexes */
#define RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X        13
#define RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y        14
#define RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN    15
#define RETRO_DEVICE_ID_LIGHTGUN_TRIGGER          2
#define RETRO_DEVICE_ID_LIGHTGUN_RELOAD          16
#define RETRO_DEVICE_ID_LIGHTGUN_AUX_A            3
#define RETRO_DEVICE_ID_LIGHTGUN_AUX_B            4
#define RETRO_DEVICE_ID_LIGHTGUN_START            6
#define RETRO_DEVICE_ID_LIGHTGUN_SELECT           7
#define RETRO_DEVICE_ID_LIGHTGUN_AUX_C            8
#define RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP          9
#define RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN       10
#define RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT       11
#define RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT      12
/* deprecated */
#define RETRO_DEVICE_ID_LIGHTGUN_X                0
#define RETRO_DEVICE_ID_LIGHTGUN_Y                1
#define RETRO_DEVICE_ID_LIGHTGUN_CURSOR           3
#define RETRO_DEVICE_ID_LIGHTGUN_TURBO            4
#define RETRO_DEVICE_ID_LIGHTGUN_PAUSE            5

/* Mouse indexes */
#define RETRO_DEVICE_ID_MOUSE_X                   0
#define RETRO_DEVICE_ID_MOUSE_Y                   1
#define RETRO_DEVICE_ID_MOUSE_LEFT                2
#define RETRO_DEVICE_ID_MOUSE_RIGHT               3
#define RETRO_DEVICE_ID_MOUSE_WHEELUP             4
#define RETRO_DEVICE_ID_MOUSE_WHEELDOWN           5
#define RETRO_DEVICE_ID_MOUSE_MIDDLE              6
#define RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP       7
#define RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN     8
#define RETRO_DEVICE_ID_MOUSE_BUTTON_4            9
#define RETRO_DEVICE_ID_MOUSE_BUTTON_5           10

/* Environment commands. */
#define RETRO_ENVIRONMENT_SET_ROTATION  1
#define RETRO_ENVIRONMENT_GET_OVERSCAN  2 /* deprecated */
#define RETRO_ENVIRONMENT_GET_CAN_DUPE  3
#define RETRO_ENVIRONMENT_SET_MESSAGE   6
#define RETRO_ENVIRONMENT_SHUTDOWN      7
#define RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL 8
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY 9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT 10
#define RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS 11
#define RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK 12
#define RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE 13
#define RETRO_ENVIRONMENT_SET_HW_RENDER 14
#define RETRO_ENVIRONMENT_GET_VARIABLE 15
#define RETRO_ENVIRONMENT_SET_VARIABLES 16
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE 17
#define RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME 18
#define RETRO_ENVIRONMENT_GET_LIBRETRO_PATH 19
#define RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK 21
#define RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK 22
#define RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE 23
#define RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES 24
#define RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE (25 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE (26 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE 27
#define RETRO_ENVIRONMENT_GET_PERF_INTERFACE 28
#define RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE 29
#define RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY 30 /* Old name, kept for compatibility. */
#define RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY 30
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY 31
#define RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO 32
#define RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK 33
#define RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO 34
#define RETRO_ENVIRONMENT_SET_CONTROLLER_INFO 35
#define RETRO_ENVIRONMENT_SET_MEMORY_MAPS (36 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_GEOMETRY 37
#define RETRO_ENVIRONMENT_GET_USERNAME 38
#define RETRO_ENVIRONMENT_GET_LANGUAGE 39
#define RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER (40 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE (41 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS (42 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE (43 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS 44
#define RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT (44 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_VFS_INTERFACE (45 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_LED_INTERFACE (46 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE (47 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_MIDI_INTERFACE (48 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_FASTFORWARDING (49 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE (50 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_INPUT_BITMASKS (51 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION 52
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS 53
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL 54
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY 55
#define RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER 56
#define RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION 57
#define RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE 58
#define RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION 59
#define RETRO_ENVIRONMENT_SET_MESSAGE_EXT 60
#define RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS 61
#define RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK 62
#define RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY 63
#define RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE 64
#define RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE 65
#define RETRO_ENVIRONMENT_GET_GAME_INFO_EXT 66
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2 67
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL 68
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK 69
#define RETRO_ENVIRONMENT_SET_VARIABLE 70
#define RETRO_ENVIRONMENT_GET_THROTTLE_STATE (71 | RETRO_ENVIRONMENT_EXPERIMENTAL)

#define RETRO_ENVIRONMENT_EXPERIMENTAL 0x10000

/* Pixel formats. */
#define RETRO_PIXEL_FORMAT_0RGB1555          0
#define RETRO_PIXEL_FORMAT_XRGB8888          1
#define RETRO_PIXEL_FORMAT_RGB565            2
#define RETRO_PIXEL_FORMAT_UNKNOWN           INT_MAX

/* Region */
#define RETRO_REGION_NTSC  0
#define RETRO_REGION_PAL   1

/* Memory types */
#define RETRO_MEMORY_MASK        0xff
#define RETRO_MEMORY_SAVE_RAM    0
#define RETRO_MEMORY_RTC         1
#define RETRO_MEMORY_SYSTEM_RAM  2
#define RETRO_MEMORY_VIDEO_RAM   3

/* Hardware rendering context types */
#define RETRO_HW_CONTEXT_NONE             0
#define RETRO_HW_CONTEXT_OPENGL           1
#define RETRO_HW_CONTEXT_OPENGLES2        2
#define RETRO_HW_CONTEXT_OPENGL_CORE      3
#define RETRO_HW_CONTEXT_OPENGLES3        4
#define RETRO_HW_CONTEXT_OPENGLES_VERSION 5
#define RETRO_HW_CONTEXT_VULKAN           6
#define RETRO_HW_CONTEXT_DIRECT3D         7
#define RETRO_HW_CONTEXT_DUMMY            INT_MAX

/* Log levels */
enum retro_log_level
{
   RETRO_LOG_DEBUG = 0,
   RETRO_LOG_INFO,
   RETRO_LOG_WARN,
   RETRO_LOG_ERROR,
   RETRO_LOG_DUMMY = INT_MAX
};

struct retro_message
{
   const char *msg;
   unsigned    frames;
};

struct retro_message_ext
{
   const char *msg;
   unsigned duration;
   unsigned priority;
   enum retro_log_level level;
   enum retro_log_level target;
   unsigned type;
   int progress;
};

struct retro_input_descriptor
{
   unsigned port;
   unsigned device;
   unsigned index;
   unsigned id;
   const char *description;
};

struct retro_controller_description
{
   const char *desc;
   unsigned id;
};

struct retro_controller_info
{
   const struct retro_controller_description *types;
   unsigned num_types;
};

struct retro_system_info
{
   const char *library_name;
   const char *library_version;
   const char *valid_extensions;
   bool        need_fullpath;
   bool        block_extract;
};

struct retro_game_geometry
{
   unsigned base_width;
   unsigned base_height;
   unsigned max_width;
   unsigned max_height;
   float    aspect_ratio;
};

struct retro_system_timing
{
   double fps;
   double sample_rate;
};

struct retro_system_av_info
{
   struct retro_game_geometry geometry;
   struct retro_system_timing timing;
};

struct retro_variable
{
   const char *key;
   const char *value;
};

struct retro_core_option_display
{
   const char *key;
   bool visible;
};

struct retro_game_info
{
   const char *path;
   const void *data;
   size_t      size;
   const char *meta;
};

typedef void (RETRO_CALLCONV *retro_hw_context_reset_t)(void);
typedef uintptr_t (RETRO_CALLCONV *retro_hw_get_current_framebuffer_t)(void);
typedef void *(RETRO_CALLCONV *retro_hw_get_proc_address_t)(const char *sym);

/* Special value passed to retro_video_refresh_t signaling that the core is done
 * rendering the frame and the frontend should present the current framebuffer. */
#define RETRO_HW_FRAME_BUFFER_VALID ((void*)-1)

struct retro_hw_render_callback
{
   unsigned context_type;
   retro_hw_context_reset_t context_reset;
   retro_hw_get_current_framebuffer_t get_current_framebuffer;
   retro_hw_get_proc_address_t get_proc_address;
   bool depth;
   bool stencil;
   bool bottom_left_origin;
   unsigned version_major;
   unsigned version_minor;
   bool cache_context;
   retro_hw_context_reset_t context_destroy;
   bool debug_context;
};

/* Logging callback */
typedef void (RETRO_CALLCONV *retro_log_printf_t)(enum retro_log_level level, const char *fmt, ...);

struct retro_log_callback
{
   retro_log_printf_t log;
};

/* Frame time callback */
typedef int64_t retro_usec_t;
typedef void (RETRO_CALLCONV *retro_frame_time_callback_t)(retro_usec_t usec);
struct retro_frame_time_callback
{
   retro_frame_time_callback_t callback;
   retro_usec_t reference;
};

/* Audio callback */
typedef void (RETRO_CALLCONV *retro_audio_callback_t)(void);
typedef void (RETRO_CALLCONV *retro_audio_set_state_callback_t)(bool enabled);
struct retro_audio_callback
{
   retro_audio_callback_t callback;
   retro_audio_set_state_callback_t set_state;
};

/* Rumble interface */
enum retro_rumble_effect
{
   RETRO_RUMBLE_STRONG = 0,
   RETRO_RUMBLE_WEAK = 1,
   RETRO_RUMBLE_DUMMY = INT_MAX
};

typedef bool (RETRO_CALLCONV *retro_set_rumble_state_t)(unsigned port, enum retro_rumble_effect effect, uint16_t strength);
struct retro_rumble_interface
{
   retro_set_rumble_state_t set_rumble_state;
};

/* Environment callback */
typedef bool (RETRO_CALLCONV *retro_environment_t)(unsigned cmd, void *data);

/* Video refresh callback */
typedef void (RETRO_CALLCONV *retro_video_refresh_t)(const void *data, unsigned width, unsigned height, size_t pitch);

/* Audio sample callback */
typedef void (RETRO_CALLCONV *retro_audio_sample_t)(int16_t left, int16_t right);

/* Audio sample batch callback */
typedef size_t (RETRO_CALLCONV *retro_audio_sample_batch_t)(const int16_t *data, size_t frames);

/* Input poll callback */
typedef void (RETRO_CALLCONV *retro_input_poll_t)(void);

/* Input state callback */
typedef int16_t (RETRO_CALLCONV *retro_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);

/* Sets callbacks. Must be called before retro_run(). */
RETRO_API void retro_set_environment(retro_environment_t);
RETRO_API void retro_set_video_refresh(retro_video_refresh_t);
RETRO_API void retro_set_audio_sample(retro_audio_sample_t);
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
RETRO_API void retro_set_input_poll(retro_input_poll_t);
RETRO_API void retro_set_input_state(retro_input_state_t);

/* Library global initialization/deinitialization. */
RETRO_API void retro_init(void);
RETRO_API void retro_deinit(void);

/* Must return RETRO_API_VERSION. */
RETRO_API unsigned retro_api_version(void);

/* Gets statically known system info. */
RETRO_API void retro_get_system_info(struct retro_system_info *info);

/* Gets information about system audio/video timings and geometry. */
RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info);

/* Sets device to be used for player 'port'. */
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device);

/* Resets the current game. */
RETRO_API void retro_reset(void);

/* Runs the game for one video frame. */
RETRO_API void retro_run(void);

/* Returns the amount of data the implementation requires to serialize internal state (save states). */
RETRO_API size_t retro_serialize_size(void);

/* Serializes internal state. */
RETRO_API bool retro_serialize(void *data, size_t size);

/* Unserializes internal state. */
RETRO_API bool retro_unserialize(const void *data, size_t size);

/* Cheats handling. */
RETRO_API void retro_cheat_reset(void);
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code);

/* Loads a game. */
RETRO_API bool retro_load_game(const struct retro_game_info *game);

/* Loads a "special" kind of game. */
RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info);

/* Unloads the currently loaded game. */
RETRO_API void retro_unload_game(void);

/* Gets region of game. */
RETRO_API unsigned retro_get_region(void);

/* Gets region of memory. */
RETRO_API void *retro_get_memory_data(unsigned id);
RETRO_API size_t retro_get_memory_size(unsigned id);

#ifdef __cplusplus
}
#endif

#endif
