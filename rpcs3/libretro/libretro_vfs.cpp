#include "stdafx.h"
#include "libretro_vfs.h"
#include "Utilities/File.h"
#include <cstring>
#include <cstdarg>
#include <atomic>

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace libretro_vfs
{
	static const struct retro_vfs_interface* s_vfs_interface = nullptr;

	static std::atomic<uint64_t> s_vfs_open_count{0};
	static std::atomic<uint64_t> s_vfs_read_bytes{0};
	static std::atomic<uint64_t> s_vfs_write_bytes{0};
	static std::atomic<uint64_t> s_native_fallback_count{0};

	uint64_t get_vfs_open_count() { return s_vfs_open_count.load(); }
	uint64_t get_vfs_read_bytes() { return s_vfs_read_bytes.load(); }
	uint64_t get_vfs_write_bytes() { return s_vfs_write_bytes.load(); }
	uint64_t get_native_fallback_count() { return s_native_fallback_count.load(); }

	void reset_vfs_stats()
	{
		s_vfs_open_count = 0;
		s_vfs_read_bytes = 0;
		s_vfs_write_bytes = 0;
		s_native_fallback_count = 0;
	}

	void set_vfs_interface(const struct retro_vfs_interface* vfs_interface)
	{
		s_vfs_interface = vfs_interface;
	}

	const struct retro_vfs_interface* get_vfs_interface()
	{
		return s_vfs_interface;
	}

	bool is_vfs_available()
	{
		return s_vfs_interface != nullptr;
	}

	static unsigned mode_to_vfs_flags(const char* mode)
	{
		unsigned flags = 0;

		if (!mode || !*mode)
			return RETRO_VFS_FILE_ACCESS_READ;

		switch (mode[0])
		{
		case 'r':
			flags = RETRO_VFS_FILE_ACCESS_READ;
			if (mode[1] == '+' || (mode[1] && mode[2] == '+'))
				flags |= RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
			break;
		case 'w':
			flags = RETRO_VFS_FILE_ACCESS_WRITE;
			if (mode[1] == '+' || (mode[1] && mode[2] == '+'))
				flags |= RETRO_VFS_FILE_ACCESS_READ | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
			break;
		case 'a':
			flags = RETRO_VFS_FILE_ACCESS_WRITE;
			if (mode[1] == '+' || (mode[1] && mode[2] == '+'))
				flags |= RETRO_VFS_FILE_ACCESS_READ;
			break;
		default:
			flags = RETRO_VFS_FILE_ACCESS_READ;
			break;
		}

		return flags;
	}

	vfs_file::vfs_file(const std::string& path, const char* mode)
		: m_path(path)
		, m_vfs_handle(nullptr)
		, m_native_handle(nullptr)
		, m_use_vfs(false)
	{
		if (is_vfs_available() && s_vfs_interface->open)
		{
			unsigned vfs_mode = mode_to_vfs_flags(mode);
			unsigned hints = RETRO_VFS_FILE_ACCESS_HINT_NONE;

			m_vfs_handle = s_vfs_interface->open(path.c_str(), vfs_mode, hints);
			if (m_vfs_handle)
			{
				m_use_vfs = true;
				s_vfs_open_count++;
				return;
			}
		}

		m_native_handle = std::fopen(path.c_str(), mode);
		if (m_native_handle)
			s_native_fallback_count++;
	}

	vfs_file::~vfs_file()
	{
		close();
	}

	vfs_file::vfs_file(vfs_file&& other) noexcept
		: m_path(std::move(other.m_path))
		, m_vfs_handle(other.m_vfs_handle)
		, m_native_handle(other.m_native_handle)
		, m_use_vfs(other.m_use_vfs)
	{
		other.m_vfs_handle = nullptr;
		other.m_native_handle = nullptr;
		other.m_use_vfs = false;
	}

	vfs_file& vfs_file::operator=(vfs_file&& other) noexcept
	{
		if (this != &other)
		{
			close();
			m_path = std::move(other.m_path);
			m_vfs_handle = other.m_vfs_handle;
			m_native_handle = other.m_native_handle;
			m_use_vfs = other.m_use_vfs;

			other.m_vfs_handle = nullptr;
			other.m_native_handle = nullptr;
			other.m_use_vfs = false;
		}
		return *this;
	}

	bool vfs_file::is_open() const
	{
		return m_use_vfs ? (m_vfs_handle != nullptr) : (m_native_handle != nullptr);
	}

	int64_t vfs_file::size() const
	{
		if (!is_open())
			return -1;

		if (m_use_vfs && s_vfs_interface->size)
		{
			return s_vfs_interface->size(m_vfs_handle);
		}
		else if (m_native_handle)
		{
			long current = std::ftell(m_native_handle);
			std::fseek(m_native_handle, 0, SEEK_END);
			long length = std::ftell(m_native_handle);
			std::fseek(m_native_handle, current, SEEK_SET);
			return static_cast<int64_t>(length);
		}

		return -1;
	}

	int64_t vfs_file::tell() const
	{
		if (!is_open())
			return -1;

		if (m_use_vfs && s_vfs_interface->tell)
		{
			return s_vfs_interface->tell(m_vfs_handle);
		}
		else if (m_native_handle)
		{
			return static_cast<int64_t>(std::ftell(m_native_handle));
		}

		return -1;
	}

	int64_t vfs_file::seek(int64_t offset, int whence)
	{
		if (!is_open())
			return -1;

		if (m_use_vfs && s_vfs_interface->seek)
		{
			int vfs_whence;
			switch (whence)
			{
			case SEEK_SET: vfs_whence = RETRO_VFS_SEEK_POSITION_START; break;
			case SEEK_CUR: vfs_whence = RETRO_VFS_SEEK_POSITION_CURRENT; break;
			case SEEK_END: vfs_whence = RETRO_VFS_SEEK_POSITION_END; break;
			default: vfs_whence = RETRO_VFS_SEEK_POSITION_START; break;
			}
			return s_vfs_interface->seek(m_vfs_handle, offset, vfs_whence);
		}
		else if (m_native_handle)
		{
			if (std::fseek(m_native_handle, static_cast<long>(offset), whence) == 0)
				return std::ftell(m_native_handle);
		}

		return -1;
	}

	int64_t vfs_file::read(void* buffer, uint64_t len)
	{
		if (!is_open())
			return -1;

		if (m_use_vfs && s_vfs_interface->read)
		{
			int64_t result = s_vfs_interface->read(m_vfs_handle, buffer, len);
			if (result > 0)
				s_vfs_read_bytes += result;
			return result;
		}
		else if (m_native_handle)
		{
			size_t bytes_read = std::fread(buffer, 1, static_cast<size_t>(len), m_native_handle);
			return static_cast<int64_t>(bytes_read);
		}

		return -1;
	}

	int64_t vfs_file::write(const void* buffer, uint64_t len)
	{
		if (!is_open())
			return -1;

		if (m_use_vfs && s_vfs_interface->write)
		{
			int64_t result = s_vfs_interface->write(m_vfs_handle, buffer, len);
			if (result > 0)
				s_vfs_write_bytes += result;
			return result;
		}
		else if (m_native_handle)
		{
			size_t bytes_written = std::fwrite(buffer, 1, static_cast<size_t>(len), m_native_handle);
			return static_cast<int64_t>(bytes_written);
		}

		return -1;
	}

	int vfs_file::flush()
	{
		if (!is_open())
			return -1;

		if (m_use_vfs && s_vfs_interface->flush)
		{
			return s_vfs_interface->flush(m_vfs_handle);
		}
		else if (m_native_handle)
		{
			return std::fflush(m_native_handle);
		}

		return -1;
	}

	void vfs_file::close()
	{
		if (m_use_vfs && m_vfs_handle)
		{
			if (s_vfs_interface->close)
				s_vfs_interface->close(m_vfs_handle);
			m_vfs_handle = nullptr;
		}
		else if (m_native_handle)
		{
			std::fclose(m_native_handle);
			m_native_handle = nullptr;
		}

		m_use_vfs = false;
	}

	bool vfs_stat(const std::string& path, int64_t* size_out)
	{
		if (is_vfs_available() && s_vfs_interface->stat)
		{
			int32_t size32 = 0;
			int result = s_vfs_interface->stat(path.c_str(), &size32);
			if (result > 0)
			{
				if (size_out)
					*size_out = static_cast<int64_t>(size32);
				return true;
			}
		}

#ifdef _WIN32
		struct _stat64 st;
		if (_stat64(path.c_str(), &st) == 0)
		{
			if (size_out)
				*size_out = st.st_size;
			return true;
		}
#else
		struct stat st;
		if (stat(path.c_str(), &st) == 0)
		{
			if (size_out)
				*size_out = st.st_size;
			return true;
		}
#endif

		return false;
	}

	bool vfs_is_file(const std::string& path)
	{
		if (is_vfs_available() && s_vfs_interface->stat)
		{
			int32_t size = 0;
			int result = s_vfs_interface->stat(path.c_str(), &size);
			return (result & RETRO_VFS_STAT_IS_VALID) && !(result & RETRO_VFS_STAT_IS_DIRECTORY);
		}

#ifdef _WIN32
		struct _stat64 st;
		if (_stat64(path.c_str(), &st) == 0)
			return (st.st_mode & _S_IFREG) != 0;
#else
		struct stat st;
		if (stat(path.c_str(), &st) == 0)
			return S_ISREG(st.st_mode);
#endif

		return false;
	}

	bool vfs_is_dir(const std::string& path)
	{
		if (is_vfs_available() && s_vfs_interface->stat)
		{
			int32_t size = 0;
			int result = s_vfs_interface->stat(path.c_str(), &size);
			return (result & RETRO_VFS_STAT_IS_VALID) && (result & RETRO_VFS_STAT_IS_DIRECTORY);
		}

#ifdef _WIN32
		struct _stat64 st;
		if (_stat64(path.c_str(), &st) == 0)
			return (st.st_mode & _S_IFDIR) != 0;
#else
		struct stat st;
		if (stat(path.c_str(), &st) == 0)
			return S_ISDIR(st.st_mode);
#endif

		return false;
	}

	bool vfs_remove(const std::string& path)
	{
		if (is_vfs_available() && s_vfs_interface->remove)
		{
			return s_vfs_interface->remove(path.c_str()) == 0;
		}

		return std::remove(path.c_str()) == 0;
	}

	bool vfs_rename(const std::string& old_path, const std::string& new_path)
	{
		if (is_vfs_available() && s_vfs_interface->rename)
		{
			return s_vfs_interface->rename(old_path.c_str(), new_path.c_str()) == 0;
		}

		return std::rename(old_path.c_str(), new_path.c_str()) == 0;
	}

	bool vfs_mkdir(const std::string& path)
	{
		if (is_vfs_available() && s_vfs_interface->mkdir)
		{
			auto vfs_mkdir_func = s_vfs_interface->mkdir;
			int result = vfs_mkdir_func(path.c_str());
			return result == 0 || result == -2;
		}

#ifdef _WIN32
		return _mkdir(path.c_str()) == 0;
#else
		return mkdir(path.c_str(), 0755) == 0;
#endif
	}

	// VFS-backed file_base implementation for integration with fs::file
	class vfs_file_base final : public fs::file_base
	{
		struct retro_vfs_file_handle* m_handle;
		std::string m_path;
		mutable uint64_t m_pos;

	public:
		vfs_file_base(struct retro_vfs_file_handle* handle, const std::string& path)
			: m_handle(handle)
			, m_path(path)
			, m_pos(0)
		{
		}

		~vfs_file_base() override
		{
			if (m_handle && s_vfs_interface && s_vfs_interface->close)
			{
				s_vfs_interface->close(m_handle);
				m_handle = nullptr;
			}
		}

		fs::stat_t get_stat() override
		{
			fs::stat_t info{};
			info.is_directory = false;
			info.is_writable = true;
			info.size = size();
			info.atime = 0;
			info.mtime = 0;
			info.ctime = 0;
			return info;
		}

		void sync() override
		{
			if (m_handle && s_vfs_interface && s_vfs_interface->flush)
			{
				s_vfs_interface->flush(m_handle);
			}
		}

		bool trunc(u64 length) override
		{
			if (m_handle && s_vfs_interface && s_vfs_interface->truncate)
			{
				return s_vfs_interface->truncate(m_handle, length) >= 0;
			}
			return false;
		}

		u64 read(void* buffer, u64 count) override
		{
			if (!m_handle || !s_vfs_interface || !s_vfs_interface->read)
				return 0;

			// Seek to current position first
			if (s_vfs_interface->seek)
			{
				s_vfs_interface->seek(m_handle, m_pos, RETRO_VFS_SEEK_POSITION_START);
			}

			int64_t result = s_vfs_interface->read(m_handle, buffer, count);
			if (result > 0)
			{
				m_pos += result;
				s_vfs_read_bytes += result;
			}
			return result > 0 ? static_cast<u64>(result) : 0;
		}

		u64 read_at(u64 offset, void* buffer, u64 count) override
		{
			if (!m_handle || !s_vfs_interface || !s_vfs_interface->read)
				return 0;

			// Seek to offset
			if (s_vfs_interface->seek)
			{
				s_vfs_interface->seek(m_handle, offset, RETRO_VFS_SEEK_POSITION_START);
			}

			int64_t result = s_vfs_interface->read(m_handle, buffer, count);
			if (result > 0)
			{
				s_vfs_read_bytes += result;
			}
			return result > 0 ? static_cast<u64>(result) : 0;
		}

		u64 write(const void* buffer, u64 count) override
		{
			if (!m_handle || !s_vfs_interface || !s_vfs_interface->write)
				return 0;

			// Seek to current position first
			if (s_vfs_interface->seek)
			{
				s_vfs_interface->seek(m_handle, m_pos, RETRO_VFS_SEEK_POSITION_START);
			}

			int64_t result = s_vfs_interface->write(m_handle, buffer, count);
			if (result > 0)
			{
				m_pos += result;
				s_vfs_write_bytes += result;
			}
			return result > 0 ? static_cast<u64>(result) : 0;
		}

		u64 seek(s64 offset, fs::seek_mode whence) override
		{
			if (!m_handle || !s_vfs_interface || !s_vfs_interface->seek)
				return m_pos;

			int vfs_whence;
			switch (whence)
			{
			case fs::seek_set: vfs_whence = RETRO_VFS_SEEK_POSITION_START; break;
			case fs::seek_cur: vfs_whence = RETRO_VFS_SEEK_POSITION_CURRENT; break;
			case fs::seek_end: vfs_whence = RETRO_VFS_SEEK_POSITION_END; break;
			default: vfs_whence = RETRO_VFS_SEEK_POSITION_START; break;
			}

			int64_t result = s_vfs_interface->seek(m_handle, offset, vfs_whence);
			// VFS seek returns 0 on success, not the new position
			if (result == 0)
			{
				// Calculate new position based on whence
				switch (whence)
				{
				case fs::seek_set:
					m_pos = static_cast<u64>(offset);
					break;
				case fs::seek_cur:
					m_pos = static_cast<u64>(static_cast<int64_t>(m_pos) + offset);
					break;
				case fs::seek_end:
					if (s_vfs_interface->size)
					{
						int64_t file_size = s_vfs_interface->size(m_handle);
						if (file_size >= 0)
							m_pos = static_cast<u64>(file_size + offset);
					}
					break;
				}
			}
			return m_pos;
		}

		u64 size() override
		{
			if (!m_handle || !s_vfs_interface || !s_vfs_interface->size)
				return 0;

			int64_t result = s_vfs_interface->size(m_handle);
			return result > 0 ? static_cast<u64>(result) : 0;
		}
	};

	std::unique_ptr<fs::file_base> create_vfs_file_base(const std::string& path, unsigned int mode)
	{
		if (!is_vfs_available() || !s_vfs_interface->open)
			return nullptr;

		unsigned vfs_flags = 0;
		if (mode & VFS_MODE_READ)
			vfs_flags |= RETRO_VFS_FILE_ACCESS_READ;
		if (mode & VFS_MODE_WRITE)
			vfs_flags |= RETRO_VFS_FILE_ACCESS_WRITE;
		if (!(mode & VFS_MODE_TRUNC) && (mode & VFS_MODE_WRITE))
			vfs_flags |= RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;

		unsigned hints = RETRO_VFS_FILE_ACCESS_HINT_NONE;
		struct retro_vfs_file_handle* handle = s_vfs_interface->open(path.c_str(), vfs_flags, hints);

		if (!handle)
			return nullptr;

		s_vfs_open_count++;
		return std::make_unique<vfs_file_base>(handle, path);
	}
}
