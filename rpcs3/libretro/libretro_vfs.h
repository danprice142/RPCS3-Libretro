#pragma once

#include "libretro.h"
#include <string>
#include <cstdint>
#include <cstdio>
#include <memory>

// Forward declaration for fs::file_base
namespace fs { struct file_base; }

namespace libretro_vfs
{
	void set_vfs_interface(const struct retro_vfs_interface* vfs_interface);
	const struct retro_vfs_interface* get_vfs_interface();
	bool is_vfs_available();

	// VFS usage statistics
	uint64_t get_vfs_open_count();
	uint64_t get_vfs_read_bytes();
	uint64_t get_vfs_write_bytes();
	uint64_t get_native_fallback_count();
	void reset_vfs_stats();

	class vfs_file
	{
	public:
		vfs_file() = default;
		vfs_file(const std::string& path, const char* mode);
		~vfs_file();

		vfs_file(const vfs_file&) = delete;
		vfs_file& operator=(const vfs_file&) = delete;

		vfs_file(vfs_file&& other) noexcept;
		vfs_file& operator=(vfs_file&& other) noexcept;

		bool is_open() const;
		explicit operator bool() const { return is_open(); }

		int64_t size() const;
		int64_t tell() const;
		int64_t seek(int64_t offset, int whence);
		int64_t read(void* buffer, uint64_t len);
		int64_t write(const void* buffer, uint64_t len);
		int flush();
		void close();

		const std::string& get_path() const { return m_path; }

	private:
		std::string m_path;
		struct retro_vfs_file_handle* m_vfs_handle = nullptr;
		FILE* m_native_handle = nullptr;
		bool m_use_vfs = false;
	};

	bool vfs_stat(const std::string& path, int64_t* size_out);
	bool vfs_is_file(const std::string& path);
	bool vfs_is_dir(const std::string& path);
	bool vfs_remove(const std::string& path);
	bool vfs_rename(const std::string& old_path, const std::string& new_path);
	bool vfs_mkdir(const std::string& path);

	// Create a VFS-backed file_base for use with fs::file
	// Returns nullptr if VFS is not available or open fails
	std::unique_ptr<fs::file_base> create_vfs_file_base(const std::string& path, unsigned int mode);

	// Mode flags matching fs:: namespace
	constexpr unsigned int VFS_MODE_READ   = 1 << 0;
	constexpr unsigned int VFS_MODE_WRITE  = 1 << 1;
	constexpr unsigned int VFS_MODE_APPEND = 1 << 2;
	constexpr unsigned int VFS_MODE_CREATE = 1 << 3;
	constexpr unsigned int VFS_MODE_TRUNC  = 1 << 4;
	constexpr unsigned int VFS_MODE_EXCL   = 1 << 5;
}
