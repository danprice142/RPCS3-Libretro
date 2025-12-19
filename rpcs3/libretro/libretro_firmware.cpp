// RPCS3 Libretro Core - Firmware Installation
// Handles PS3 firmware (PUP) installation for libretro builds

#include "stdafx.h"
#include "libretro_firmware.h"

#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "Crypto/unself.h"
#include "Emu/VFS.h"
#include "Emu/vfs_config.h"
#include "Utilities/File.h"
#include "Utilities/Thread.h"

#include <algorithm>
#include <atomic>

LOG_CHANNEL(fw_log, "FW");

static std::string strip_trailing_separators(std::string s)
{
	while (!s.empty() && (s.back() == '/' || s.back() == '\\'))
	{
		s.pop_back();
	}
	return s;
}

bool libretro_install_firmware(const std::string& pup_path, std::function<void(int, int)> progress_cb)
{
    fw_log.notice("Installing firmware from: %s", pup_path);

    fs::file pup_f(pup_path);
    if (!pup_f)
    {
        fw_log.error("Failed to open PUP file: %s", pup_path);
        return false;
    }

    pup_object pup(std::move(pup_f));

    if (pup.operator pup_error() != pup_error::ok)
    {
        fw_log.error("Invalid PUP file: %s", pup.get_formatted_error());
        return false;
    }

    fs::file update_files_f = pup.get_file(0x300);
    const usz update_files_size = update_files_f ? update_files_f.size() : 0;
    if (!update_files_f)
    {
        fw_log.error("Failed to get update files from PUP");
        return false;
    }
    if (!update_files_size)
    {
        fw_log.error("Firmware installation failed: installation database is empty");
        return false;
    }

    tar_object update_files(update_files_f);

    auto update_filenames = update_files.get_filenames();

    // Filter to only dev_flash_* packages
    update_filenames.erase(std::remove_if(
        update_filenames.begin(), update_filenames.end(),
        [](const std::string& s) { return s.find("dev_flash_") == std::string::npos; }),
        update_filenames.end());

    if (update_filenames.empty())
    {
        fw_log.error("No dev_flash packages found in PUP");
        return false;
    }

    fw_log.notice("Found %zu firmware packages to install", update_filenames.size());

    // Prepare /dev_flash destination
    const std::string dev_flash_cfg = g_cfg_vfs.get_dev_flash();
    const std::string dev_flash_dir = strip_trailing_separators(dev_flash_cfg);

    // Ensure directory exists before querying disk space
    if (!fs::is_dir(dev_flash_dir))
    {
        // create_path creates all missing parents (important for RetroArch system dir layouts)
        if (!fs::create_path(dev_flash_dir))
        {
            fw_log.error("Failed to create dev_flash directory: %s", dev_flash_dir);
            return false;
        }
    }

    // Check available disk space for /dev_flash
    fs::device_stat dev_stat{};
    if (!fs::statfs(dev_flash_cfg, dev_stat))
    {
        fw_log.error("Firmware installation failed: couldn't retrieve available disk space for %s", dev_flash_cfg);
        return false;
    }

    if (dev_stat.avail_free < update_files_size)
    {
        fw_log.error("Firmware installation failed: out of disk space in %s (needed: %u bytes)", dev_flash_cfg, static_cast<u32>(update_files_size - dev_stat.avail_free));
        return false;
    }

    if (!vfs::mount("/dev_flash", dev_flash_cfg))
    {
        fw_log.error("Failed to mount /dev_flash to %s", dev_flash_cfg);
        return false;
    }

    int current = 0;
    const int total = static_cast<int>(update_filenames.size());

    for (const auto& update_filename : update_filenames)
    {
        fw_log.notice("Installing package: %s (%d/%d)", update_filename, current + 1, total);

        auto update_file_stream = update_files.get_file(update_filename);

        if (update_file_stream->m_file_handler)
        {
            update_file_stream->m_file_handler->handle_file_op(*update_file_stream, 0, update_file_stream->get_size(umax), nullptr);
        }

        fs::file update_file = fs::make_stream(std::move(update_file_stream->data));

        SCEDecrypter self_dec(update_file);
        self_dec.LoadHeaders();
        self_dec.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV);
        self_dec.DecryptData();

        auto dev_flash_tar_f = self_dec.MakeFile();
        if (dev_flash_tar_f.size() < 3)
        {
            fw_log.error("Failed to decrypt firmware package: %s", update_filename);
            return false;
        }

        tar_object dev_flash_tar(dev_flash_tar_f[2]);
        if (!dev_flash_tar.extract())
        {
            fw_log.error("Failed to extract firmware package: %s", update_filename);
            return false;
        }

        current++;
        if (progress_cb)
        {
            progress_cb(current, total);
        }
    }

    vfs::unmount("/dev_flash");

    fw_log.success("Firmware installation complete");
    return true;
}

std::string libretro_get_firmware_version()
{
    const std::string dev_flash = g_cfg_vfs.get_dev_flash();
    const std::string version_file = dev_flash + "vsh/etc/version.txt";

    if (fs::file f{version_file})
    {
        std::string version = f.to_string();
        if (const auto pos = version.find(':'); pos != std::string::npos)
        {
            version = version.substr(pos + 1);
            if (const auto pos2 = version.find(':'); pos2 != std::string::npos)
            {
                return version.substr(0, pos2);
            }
        }
    }

    return {};
}

bool libretro_is_firmware_installed()
{
    const std::string dev_flash = g_cfg_vfs.get_dev_flash();
    return fs::is_file(dev_flash + "sys/external/liblv2.sprx");
}
