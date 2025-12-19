#pragma once

#include <string>
#include <functional>

// Install PS3 firmware from a PUP file
bool libretro_install_firmware(const std::string& pup_path, std::function<void(int, int)> progress_cb = nullptr);

// Get installed firmware version (empty if not installed)
std::string libretro_get_firmware_version();

// Check if firmware is installed
bool libretro_is_firmware_installed();
