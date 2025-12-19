#pragma once

#include "libretro.h"

// Global libretro callback accessors
retro_environment_t get_environ_cb();
retro_video_refresh_t get_video_cb();
retro_audio_sample_batch_t get_audio_batch_cb();
retro_input_state_t get_input_state_cb();
retro_log_printf_t get_log_cb();

// Hardware render callback
const retro_hw_render_callback& get_hw_render();

// Core directories
const char* get_system_dir();
const char* get_save_dir();
