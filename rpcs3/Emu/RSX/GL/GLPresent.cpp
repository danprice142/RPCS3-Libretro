#include "stdafx.h"

#include "GLGSRender.h"
#include "GLHelpers.h"
#include "GLTextureCache.h"
#if defined(LIBRETRO_CORE)
#include "libretro/libretro_video.h"
#endif
#include "upscalers/fsr_pass.h"
#include "upscalers/bilinear_pass.hpp"
#include "upscalers/nearest_pass.hpp"

#include "Emu/Cell/Modules/cellVideoOut.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/Overlays/overlay_debug_overlay.h"

#include "util/video_provider.h"
#include <thread>
#include <functional>

LOG_CHANNEL(screenshot_log, "SCREENSHOT");

extern atomic_t<bool> g_user_asked_for_screenshot;
extern atomic_t<recording_mode> g_recording_mode;

#if defined(LIBRETRO_CORE)
#ifndef RPCS3_LIBRETRO_RSX_PRESENT_TRACE
#define RPCS3_LIBRETRO_RSX_PRESENT_TRACE 0
#endif
#else
#define RPCS3_LIBRETRO_RSX_PRESENT_TRACE 0
#endif

static inline unsigned long long lrrsx_present_tid_hash()
{
	return static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

#if RPCS3_LIBRETRO_RSX_PRESENT_TRACE
	#define LRRSX_PRESENT_NOTICE(fmt, ...) rsx_log.notice("[LRRSX_PRESENT][tid=%llx] " fmt, lrrsx_present_tid_hash() __VA_OPT__(,) __VA_ARGS__)
	#define LRRSX_PRESENT_WARN(fmt, ...)   rsx_log.warning("[LRRSX_PRESENT][tid=%llx] " fmt, lrrsx_present_tid_hash() __VA_OPT__(,) __VA_ARGS__)
	#define LRRSX_PRESENT_ERR(fmt, ...)    rsx_log.error("[LRRSX_PRESENT][tid=%llx] " fmt, lrrsx_present_tid_hash() __VA_OPT__(,) __VA_ARGS__)
#else
	#define LRRSX_PRESENT_NOTICE(...) do { } while (0)
	#define LRRSX_PRESENT_WARN(...)   do { } while (0)
	#define LRRSX_PRESENT_ERR(...)    do { } while (0)
#endif

#if defined(LIBRETRO_CORE)
static inline void libretro_bind_hw_fbo()
{
	// CRITICAL FIX: FBOs are NOT shared between OpenGL contexts!
	// RSX runs on its own thread with its own GL context.
	// We must render to the RSX-side FBO (which has a shared texture attached),
	// then blit that shared texture to RetroArch's FBO in retro_run().
	const GLuint fbo = libretro_get_rsx_fbo();
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	const GLenum fb_status = DSA_CALL2_RET(CheckNamedFramebufferStatus, fbo, GL_FRAMEBUFFER);
	thread_local u64 tl_call_count = 0;
	thread_local u32 tl_last_fbo = 0xffffffffu;
	thread_local GLenum tl_last_status = GL_FRAMEBUFFER_COMPLETE;
	tl_call_count++;
	const bool log_this = (tl_call_count <= 120ull) || ((tl_call_count % 60ull) == 0ull) || (static_cast<u32>(fbo) != tl_last_fbo) || (fbo == 0) || (fb_status != tl_last_status) || (fb_status != GL_FRAMEBUFFER_COMPLETE);
	if (log_this)
	{
		rsx_log.notice("[LRRSX_FBO][tid=%llx] libretro_bind_hw_fbo rsx_fbo=0x%x last=0x%x status=0x%x last_status=0x%x call=%llu", lrrsx_present_tid_hash(), static_cast<u32>(fbo), tl_last_fbo, static_cast<u32>(fb_status), static_cast<u32>(tl_last_status), static_cast<unsigned long long>(tl_call_count));
	}
	tl_last_fbo = static_cast<u32>(fbo);
	tl_last_status = fb_status;
}
#endif

namespace gl
{
	namespace debug
	{
		std::unique_ptr<texture> g_vis_texture;

		void set_vis_texture(texture* visual)
		{
			const auto target = static_cast<GLenum>(visual->get_target());
			const auto ifmt = static_cast<GLenum>(visual->get_internal_format());
			g_vis_texture.reset(new texture(target, visual->width(), visual->height(), 1, 1, 1, ifmt, visual->format_class()));
			glCopyImageSubData(visual->id(), target, 0, 0, 0, 0, g_vis_texture->id(), target, 0, 0, 0, 0, visual->width(), visual->height(), 1);
		}
	}

	GLenum RSX_display_format_to_gl_format(u8 format)
	{
		switch (format)
		{
		default:
			rsx_log.error("Unhandled video output format 0x%x", static_cast<s32>(format));
			[[fallthrough]];
		case CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8:
			return GL_BGRA8;
		case CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8B8G8R8:
			return GL_RGBA8;
		case CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_R16G16B16X16_FLOAT:
			return GL_RGBA16F;
		}
	}
}

gl::texture* GLGSRender::get_present_source(gl::present_surface_info* info, const rsx::avconf& avconfig)
{
	gl::texture* image = nullptr;

	LRRSX_PRESENT_NOTICE("get_present_source enter info=%p addr=0x%x fmt=0x%x w=%u h=%u pitch=%u eye=%u av(state=%d res=%ux%u fmt=%u stereo=%d)",
		info,
		static_cast<u32>(info ? info->address : 0),
		static_cast<u32>(info ? info->format : 0),
		static_cast<u32>(info ? info->width : 0),
		static_cast<u32>(info ? info->height : 0),
		static_cast<u32>(info ? info->pitch : 0),
		static_cast<u32>(info ? info->eye : 0),
		avconfig.state ? 1 : 0,
		static_cast<u32>(avconfig.resolution_x),
		static_cast<u32>(avconfig.resolution_y),
		static_cast<u32>(avconfig.format),
		avconfig.stereo_enabled ? 1 : 0);

	// @FIXME: This implementation needs to merge into the texture cache's upload_texture routine.
	// See notes on the vulkan implementation on what needs to happen before that is viable.

	// Check the surface store first
	gl::command_context cmd = { gl_state };
	const auto format_bpp = rsx::get_format_block_size_in_bytes(info->format);
	const auto overlap_info = m_rtts.get_merged_texture_memory_region(cmd,
		info->address, info->width, info->height, info->pitch, format_bpp, rsx::surface_access::transfer_read);

	LRRSX_PRESENT_NOTICE("get_present_source merged_region size=%zu format_bpp=%u", overlap_info.size(), static_cast<u32>(format_bpp));

	if (!overlap_info.empty())
	{
		const auto& section = overlap_info.back();
		auto surface = gl::as_rtt(section.surface);
		bool viable = false;

		if (section.base_address >= info->address)
		{
			const auto surface_width = surface->get_surface_width<rsx::surface_metrics::samples>();
			const auto surface_height = surface->get_surface_height<rsx::surface_metrics::samples>();

			if (section.base_address == info->address)
			{
				// Check for fit or crop
				viable = (surface_width >= info->width && surface_height >= info->height);
			}
			else
			{
				// Check for borders and letterboxing
				const u32 inset_offset = section.base_address - info->address;
				const u32 inset_y = inset_offset / info->pitch;
				const u32 inset_x = (inset_offset % info->pitch) / format_bpp;

				const u32 full_width = surface_width + inset_x + inset_x;
				const u32 full_height = surface_height + inset_y + inset_y;

				viable = (full_width == info->width && full_height == info->height);
			}

			if (viable)
			{
				image = section.surface->get_surface(rsx::surface_access::transfer_read);
				LRRSX_PRESENT_NOTICE("get_present_source using RTT surface image=%p before scale w=%u h=%u surface_w=%u surface_h=%u", image, info->width, info->height, surface_width, surface_height);

				std::tie(info->width, info->height) = rsx::apply_resolution_scale<true>(
					std::min(surface_width, info->width),
					std::min(surface_height, info->height));

				LRRSX_PRESENT_NOTICE("get_present_source after scale w=%u h=%u", info->width, info->height);
			}
		}
	}
	else if (auto surface = m_gl_texture_cache.find_texture_from_dimensions<true>(info->address, info->format);
			 surface && surface->get_width() >= info->width && surface->get_height() >= info->height)
	{
		// Hack - this should be the first location to check for output
		// The render might have been done offscreen or in software and a blit used to display
		if (const auto tex = surface->get_raw_texture(); tex) image = tex;
		LRRSX_PRESENT_NOTICE("get_present_source using cache surface=%p raw_tex=%p w=%u h=%u req_w=%u req_h=%u", static_cast<const void*>(surface), image, surface->get_width(), surface->get_height(), info->width, info->height);
	}

	const GLenum expected_format = gl::RSX_display_format_to_gl_format(avconfig.format);
	std::unique_ptr<gl::texture>& flip_image = m_flip_tex_color[info->eye];
	LRRSX_PRESENT_NOTICE("get_present_source expected_format=0x%x flip_image=%p current_size=%ux%u", static_cast<u32>(expected_format), flip_image.get(), flip_image ? flip_image->width() : 0u, flip_image ? flip_image->height() : 0u);

	auto initialize_scratch_image = [&]()
	{
		if (!flip_image || flip_image->size2D() != sizeu{ info->width, info->height })
		{
			LRRSX_PRESENT_NOTICE("get_present_source allocating scratch flip_image old=%p old_size=%ux%u new_size=%ux%u", flip_image.get(), flip_image ? flip_image->width() : 0u, flip_image ? flip_image->height() : 0u, info->width, info->height);
			flip_image = std::make_unique<gl::texture>(GL_TEXTURE_2D, info->width, info->height, 1, 1, 1, expected_format, RSX_FORMAT_CLASS_COLOR);
			LRRSX_PRESENT_NOTICE("get_present_source allocated scratch flip_image new=%p new_size=%ux%u", flip_image.get(), flip_image ? flip_image->width() : 0u, flip_image ? flip_image->height() : 0u);
		}
		else
		{
			LRRSX_PRESENT_NOTICE("get_present_source scratch flip_image reuse=%p size=%ux%u", flip_image.get(), flip_image->width(), flip_image->height());
		}
	};

	if (!image)
	{
		LRRSX_PRESENT_WARN("Flip texture was not found in cache. Uploading surface from CPU (addr=0x%x w=%u h=%u pitch=%u expected_format=0x%x)", static_cast<u32>(info->address), info->width, info->height, info->pitch, static_cast<u32>(expected_format));

		gl::pixel_unpack_settings unpack_settings;
		unpack_settings.alignment(1).row_length(info->pitch / 4);

		initialize_scratch_image();
		LRRSX_PRESENT_NOTICE("get_present_source copy_from begin flip_image=%p", flip_image.get());

		gl::command_context cmd{ gl_state };
		const auto range = utils::address_range32::start_length(info->address, info->pitch * info->height);
		m_gl_texture_cache.invalidate_range(cmd, range, rsx::invalidation_cause::read);

		flip_image->copy_from(vm::base(info->address), static_cast<gl::texture::format>(expected_format), gl::texture::type::uint_8_8_8_8, unpack_settings);
		LRRSX_PRESENT_NOTICE("get_present_source copy_from done flip_image=%p", flip_image.get());
		image = flip_image.get();
	}
	else if (image->get_internal_format() != static_cast<gl::texture::internal_format>(expected_format))
	{
		LRRSX_PRESENT_NOTICE("get_present_source format mismatch image=%p ifmt=0x%x expected=0x%x", image, static_cast<u32>(image->get_internal_format()), static_cast<u32>(expected_format));
		initialize_scratch_image();

		// Copy
		if (gl::formats_are_bitcast_compatible(flip_image.get(), image))
		{
			const position3u offset{};
			gl::g_hw_blitter->copy_image(cmd, image, flip_image.get(), 0, 0, offset, offset, { info->width, info->height, 1 });
		}
		else
		{
			const coord3u region = { {/* offsets */}, { info->width, info->height, 1 } };
			gl::copy_typeless(cmd, flip_image.get(), image, region, region);
		}

		image = flip_image.get();
	}

	return image;
}

void GLGSRender::flip(const rsx::display_flip_info_t& info)
{
	static u64 s_flip_counter = 0;
	s_flip_counter++;
	const bool log_this = (s_flip_counter <= 600u) || ((s_flip_counter % 60u) == 0u);
	if (log_this)
	{
		LRRSX_PRESENT_NOTICE("flip enter #%llu buffer=%u skip_frame=%d emu_flip=%d", static_cast<unsigned long long>(s_flip_counter), static_cast<u32>(info.buffer), info.skip_frame ? 1 : 0, info.emu_flip ? 1 : 0);
	}

	if (info.skip_frame)
	{
		m_frame->flip(m_context, true);
		rsx::thread::flip(info);
		return;
	}

	gl::command_context cmd{ gl_state };

	const u32 buf_index = info.buffer;
	const bool buf_valid = (buf_index < display_buffers_count);
	if (log_this)
	{
		LRRSX_PRESENT_NOTICE("flip buf_valid=%d display_buffers_count=%u", buf_valid ? 1 : 0, static_cast<u32>(display_buffers_count));
	}

	u32 buffer_width = buf_valid ? static_cast<u32>(display_buffers[buf_index].width) : 0u;
	u32 buffer_height = buf_valid ? static_cast<u32>(display_buffers[buf_index].height) : 0u;
	u32 buffer_pitch = buf_valid ? static_cast<u32>(display_buffers[buf_index].pitch) : 0u;
	const u32 buffer_offset = buf_valid ? static_cast<u32>(display_buffers[buf_index].offset) : 0u;
	if (log_this)
	{
		LRRSX_PRESENT_NOTICE("flip display_buf[%u] w=%u h=%u pitch=%u offset=0x%x", buf_index, buffer_width, buffer_height, buffer_pitch, buffer_offset);
	}

	u32 av_format;
	const auto& avconfig = g_fxo->get<rsx::avconf>();
	if (log_this)
	{
		LRRSX_PRESENT_NOTICE("flip avconf state=%d res=%ux%u fmt=%u stereo=%d", avconfig.state ? 1 : 0, static_cast<u32>(avconfig.resolution_x), static_cast<u32>(avconfig.resolution_y), static_cast<u32>(avconfig.format), avconfig.stereo_enabled ? 1 : 0);
	}

	if (!buffer_width)
	{
		buffer_width = avconfig.resolution_x;
		buffer_height = avconfig.resolution_y;
		if (log_this)
		{
			LRRSX_PRESENT_NOTICE("flip buffer dims from avconf w=%u h=%u", buffer_width, buffer_height);
		}
	}

	if (avconfig.state)
	{
		av_format = avconfig.get_compatible_gcm_format();
		if (!buffer_pitch)
			buffer_pitch = buffer_width * avconfig.get_bpp();

		const size2u video_frame_size = avconfig.video_frame_size();
		if (log_this)
		{
			LRRSX_PRESENT_NOTICE("flip avconf video_frame_size=%ux%u", video_frame_size.width, video_frame_size.height);
		}
		buffer_width = std::min(buffer_width, video_frame_size.width);
		buffer_height = std::min(buffer_height, video_frame_size.height);
	}
	else
	{
		av_format = CELL_GCM_TEXTURE_A8R8G8B8;
		if (!buffer_pitch)
			buffer_pitch = buffer_width * 4;
	}

	if (log_this)
	{
		LRRSX_PRESENT_NOTICE("flip computed buffer_w=%u buffer_h=%u buffer_pitch=%u", buffer_width, buffer_height, buffer_pitch);
	}

	// Disable scissor test (affects blit, clear, etc)
	gl_state.disable(GL_SCISSOR_TEST);

	// Enable drawing to window backbuffer
	#if defined(LIBRETRO_CORE)
	libretro_bind_hw_fbo();
	#else
	gl::screen.bind();
	#endif

	gl::texture* image_to_flip = nullptr;
	gl::texture* image_to_flip2 = nullptr;
	if (log_this)
	{
		LRRSX_PRESENT_NOTICE("flip before present selection buffer=%u w=%u h=%u", buf_index, buffer_width, buffer_height);
	}

	if (buf_valid && buffer_width && buffer_height)
	{
		// Find the source image
		gl::present_surface_info present_info
		{
			.address = rsx::get_address(buffer_offset, CELL_GCM_LOCATION_LOCAL),
			.format = av_format,
			.width = buffer_width,
			.height = buffer_height,
			.pitch = buffer_pitch,
			.eye = 0
		};
		if (log_this)
		{
			LRRSX_PRESENT_NOTICE("flip present_info init addr=0x%x fmt=0x%x w=%u h=%u pitch=%u", static_cast<u32>(present_info.address), static_cast<u32>(present_info.format), present_info.width, present_info.height, present_info.pitch);
		}

		image_to_flip = get_present_source(&present_info, avconfig);
		if (log_this)
		{
			LRRSX_PRESENT_NOTICE("flip present_source image=%p present_w=%u present_h=%u", image_to_flip, present_info.width, present_info.height);
			if (image_to_flip)
			{
				LRRSX_PRESENT_NOTICE("flip present_source_tex image=%p tex_w=%u tex_h=%u ifmt=0x%x", image_to_flip, image_to_flip->width(), image_to_flip->height(), static_cast<u32>(image_to_flip->get_internal_format()));
			}
		}

		if (avconfig.stereo_enabled) [[unlikely]]
		{
			const auto [unused, min_expected_height] = rsx::apply_resolution_scale<true>(RSX_SURFACE_DIMENSION_IGNORED, buffer_height + 30);
			if (image_to_flip->height() < min_expected_height)
			{
				// Get image for second eye
				const u32 image_offset = (buffer_height + 30) * buffer_pitch + display_buffers[info.buffer].offset;
				present_info.width = buffer_width;
				present_info.height = buffer_height;
				present_info.address = rsx::get_address(image_offset, CELL_GCM_LOCATION_LOCAL);
				present_info.eye = 1;

				image_to_flip2 = get_present_source(&present_info, avconfig);
			}
			else
			{
				// Account for possible insets
				const auto [unused2, scaled_buffer_height] = rsx::apply_resolution_scale<true>(RSX_SURFACE_DIMENSION_IGNORED, buffer_height);
				buffer_height = std::min<u32>(image_to_flip->height() - min_expected_height, scaled_buffer_height);
			}
		}

		buffer_width = present_info.width;
		buffer_height = present_info.height;
		if (log_this)
		{
			LRRSX_PRESENT_NOTICE("flip buffer dims after present w=%u h=%u", buffer_width, buffer_height);
		}
	}
	else
	{
		if (log_this)
		{
			LRRSX_PRESENT_WARN("flip skipped present selection (buf_valid=%d buffer_w=%u buffer_h=%u)", buf_valid ? 1 : 0, buffer_width, buffer_height);
		}
	}

	if (info.emu_flip)
	{
		evaluate_cpu_usage_reduction_limits();
	}

	// Get window state
	const int width = m_frame->client_width();
	const int height = m_frame->client_height();
	LRRSX_PRESENT_NOTICE("flip client_width=%d client_height=%d buffer_width=%d buffer_height=%d", width, height, buffer_width, buffer_height);

	// Calculate blit coordinates
	areai aspect_ratio;
	if (!g_cfg.video.stretch_to_display_area)
	{
		const sizeu csize(width, height);
		const auto converted = avconfig.aspect_convert_region(size2u{ buffer_width, buffer_height }, csize);
		aspect_ratio = static_cast<areai>(converted);
	}
	else
	{
		aspect_ratio = { 0, 0, width, height };
	}

	if (!image_to_flip || aspect_ratio.x1 || aspect_ratio.y1)
	{
		// Clear the window background to opaque black
		gl_state.clear_color(0, 0, 0, 255);
		#if defined(LIBRETRO_CORE)
		libretro_bind_hw_fbo();
		glClear(GL_COLOR_BUFFER_BIT);
		#else
		gl::screen.clear(gl::buffers::color);
		#endif
	}

	if (m_overlay_manager && m_overlay_manager->has_dirty())
	{
		m_overlay_manager->lock_shared();

		std::vector<u32> uids_to_dispose;
		uids_to_dispose.reserve(m_overlay_manager->get_dirty().size());

		for (const auto& view : m_overlay_manager->get_dirty())
		{
			m_ui_renderer.remove_temp_resources(view->uid);
			uids_to_dispose.push_back(view->uid);
		}

		m_overlay_manager->unlock_shared();
		m_overlay_manager->dispose(uids_to_dispose);
	}

	const auto render_overlays = [this, &cmd](gl::texture* dst, const areau& aspect_ratio, bool flip_vertically = false)
	{
		if (m_overlay_manager && m_overlay_manager->has_visible())
		{
			GLuint target = 0;

			if (dst)
			{
				m_sshot_fbo.bind();
				m_sshot_fbo.color = dst->id();
				target = dst->id();
			}
			else
			{
				#if defined(LIBRETRO_CORE)
				libretro_bind_hw_fbo();
				#else
				gl::screen.bind();
				#endif
			}

			// Lock to avoid modification during run-update chain
			std::lock_guard lock(*m_overlay_manager);

			for (const auto& view : m_overlay_manager->get_views())
			{
				m_ui_renderer.run(cmd, aspect_ratio, target, *view.get(), flip_vertically);
			}
		}
	};

	if (image_to_flip)
	{
#if defined(LIBRETRO_CORE)
		// Debug logging for zoom/crop issue investigation
		static u64 s_flip_debug_count = 0;
		s_flip_debug_count++;
		if (s_flip_debug_count <= 60 || (s_flip_debug_count % 300) == 0)
		{
			rsx_log.notice("[LRFLIP_DEBUG] image_to_flip: %ux%u, buffer: %ux%u, client: %dx%d, aspect_ratio: (%d,%d)-(%d,%d)",
				image_to_flip->width(), image_to_flip->height(),
				buffer_width, buffer_height,
				width, height,
				aspect_ratio.x1, aspect_ratio.y1, aspect_ratio.x2, aspect_ratio.y2);
		}
#endif
		const bool user_asked_for_screenshot = g_user_asked_for_screenshot.exchange(false);

		if (user_asked_for_screenshot || (g_recording_mode != recording_mode::stopped && m_frame->can_consume_frame()))
		{
			static const gl::pixel_pack_settings pack_settings{};

			gl::texture* tex = image_to_flip;

			if (g_cfg.video.record_with_overlays)
			{
				m_sshot_fbo.create();

				if (!m_sshot_tex ||
					m_sshot_tex->get_target() != image_to_flip->get_target() ||
					m_sshot_tex->width() != image_to_flip->width() ||
					m_sshot_tex->height() != image_to_flip->height() ||
					m_sshot_tex->depth() != image_to_flip->depth() ||
					m_sshot_tex->levels() != image_to_flip->levels() ||
					m_sshot_tex->samples() != image_to_flip->samples() ||
					m_sshot_tex->get_internal_format() != image_to_flip->get_internal_format() ||
					m_sshot_tex->format_class() != image_to_flip->format_class())
				{
					m_sshot_tex = std::make_unique<gl::texture>(
						GLenum(image_to_flip->get_target()),
						image_to_flip->width(),
						image_to_flip->height(),
						image_to_flip->depth(),
						image_to_flip->levels(),
						image_to_flip->samples(),
						GLenum(image_to_flip->get_internal_format()),
						image_to_flip->format_class());
				}

				tex = m_sshot_tex.get();

				static const position3u offset{};
				gl::g_hw_blitter->copy_image(cmd, image_to_flip, tex, 0, 0, offset, offset, { tex->width(), tex->height(), 1 });

				render_overlays(tex, areau(0, 0, image_to_flip->width(), image_to_flip->height()), true);
				m_sshot_fbo.remove();
			}

			std::vector<u8> sshot_frame(buffer_height * buffer_width * 4);
			glGetError();

			tex->copy_to(sshot_frame.data(), gl::texture::format::rgba, gl::texture::type::ubyte, pack_settings);

			m_sshot_tex.reset();

			if (GLenum err = glGetError(); err != GL_NO_ERROR)
			{
				screenshot_log.error("Failed to capture image: 0x%x", err);
			}
			else if (user_asked_for_screenshot)
			{
				m_frame->take_screenshot(std::move(sshot_frame), buffer_width, buffer_height, false);
			}
			else
			{
				m_frame->present_frame(std::move(sshot_frame), buffer_width * 4, buffer_width, buffer_height, false);
			}
		}

		const areai screen_area = coordi({}, { static_cast<int>(buffer_width), static_cast<int>(buffer_height) });
		const bool use_full_rgb_range_output = g_cfg.video.full_rgb_range_output.get();
		const bool backbuffer_has_alpha = m_frame->has_alpha();

		if (!m_upscaler || m_output_scaling != g_cfg.video.output_scaling)
		{
			m_output_scaling = g_cfg.video.output_scaling;

			switch (m_output_scaling)
			{
			case output_scaling_mode::nearest:
				m_upscaler = std::make_unique<gl::nearest_upscale_pass>();
				break;
			case output_scaling_mode::fsr:
				m_upscaler = std::make_unique<gl::fsr_upscale_pass>();
				break;
			case output_scaling_mode::bilinear:
			default:
				m_upscaler = std::make_unique<gl::bilinear_upscale_pass>();
				break;
			}
		}

		// LIBRETRO_CORE: Never use UPSCALE_AND_COMMIT path - it blits to gl::screen instead of our FBO
		// We must always go through the else branch which calls libretro_bind_hw_fbo()
		#if !defined(LIBRETRO_CORE)
		if (!backbuffer_has_alpha && use_full_rgb_range_output && rsx::fcmp(avconfig.gamma, 1.f) && !avconfig.stereo_enabled)
		{
			// Blit source image to the screen
			m_upscaler->scale_output(cmd, image_to_flip, screen_area, aspect_ratio.flipped_vertical(), UPSCALE_AND_COMMIT | UPSCALE_DEFAULT_VIEW);
		}
		else
		#endif
		{
			const f32 gamma = avconfig.gamma;
			const bool limited_range = !use_full_rgb_range_output;
			const auto filter = m_output_scaling == output_scaling_mode::nearest ? gl::filter::nearest : gl::filter::linear;
			rsx::simple_array<gl::texture*> images{ image_to_flip, image_to_flip2 };

			if (m_output_scaling == output_scaling_mode::fsr && !avconfig.stereo_enabled) // 3D will be implemented later
			{
				for (unsigned i = 0; i < 2 && images[i]; ++i)
				{
					const rsx::flags32_t mode = (i == 0) ? UPSCALE_LEFT_VIEW : UPSCALE_RIGHT_VIEW;
					images[i] = m_upscaler->scale_output(cmd, image_to_flip, screen_area, aspect_ratio.flipped_vertical(), mode);
				}
			}

			#if defined(LIBRETRO_CORE)
			libretro_bind_hw_fbo();
			#else
			gl::screen.bind();
			#endif
			m_video_output_pass.run(cmd, areau(aspect_ratio), images.map(FN(x ? x->id() : GL_NONE)), gamma, limited_range, avconfig.stereo_enabled, g_cfg.video.stereo_render_mode, filter);
		}
	}

	render_overlays(nullptr, areau(aspect_ratio));

	if (g_cfg.video.debug_overlay)
	{
		const auto num_dirty_textures = m_gl_texture_cache.get_unreleased_textures_count();
		const auto texture_memory_size = m_gl_texture_cache.get_texture_memory_in_use() / (1024 * 1024);
		const auto num_flushes = m_gl_texture_cache.get_num_flush_requests();
		const auto num_mispredict = m_gl_texture_cache.get_num_cache_mispredictions();
		const auto num_speculate = m_gl_texture_cache.get_num_cache_speculative_writes();
		const auto num_misses = m_gl_texture_cache.get_num_cache_misses();
		const auto num_unavoidable = m_gl_texture_cache.get_num_unavoidable_hard_faults();
		const auto cache_miss_ratio = static_cast<u32>(ceil(m_gl_texture_cache.get_cache_miss_ratio() * 100));
		const auto num_texture_upload = m_gl_texture_cache.get_texture_upload_calls_this_frame();
		const auto num_texture_upload_miss = m_gl_texture_cache.get_texture_upload_misses_this_frame();
		const auto texture_upload_miss_ratio = m_gl_texture_cache.get_texture_upload_miss_percentage();
		const auto texture_copies_ellided = m_gl_texture_cache.get_texture_copies_ellided_this_frame();
		const auto vertex_cache_hit_count = (info.stats.vertex_cache_request_count - info.stats.vertex_cache_miss_count);
		const auto vertex_cache_hit_ratio = info.stats.vertex_cache_request_count
			? (vertex_cache_hit_count * 100) / info.stats.vertex_cache_request_count
			: 0;
		const auto program_cache_lookups = info.stats.program_cache_lookups_total;
		const auto program_cache_ellided = info.stats.program_cache_lookups_ellided;
		const auto program_cache_ellision_rate = program_cache_lookups
			? (program_cache_ellided * 100) / program_cache_lookups
			: 0;

		rsx::overlays::set_debug_overlay_text(fmt::format(
			"Internal Resolution:     %s\n"
			"RSX Load:                %3d%%\n"
			"draw calls: %16d\n"
			"draw call setup: %11dus\n"
			"vertex upload time: %8dus\n"
			"textures upload time: %6dus\n"
			"draw call execution: %7dus\n"
			"Unreleased textures: %7d\n"
			"Texture memory: %12dM\n"
			"Flush requests: %12d  = %2d (%3d%%) hard faults, %2d unavoidable, %2d misprediction(s), %2d speculation(s)\n"
			"Texture uploads: %11u (%u from CPU - %02u%%, %u copies avoided)\n"
			"Vertex cache hits: %9u/%u (%u%%)\n"
			"Program cache lookup ellision: %u/%u (%u%%)",
			info.stats.framebuffer_stats.to_string(!backend_config.supports_hw_msaa),
			get_load(), info.stats.draw_calls, info.stats.setup_time, info.stats.vertex_upload_time,
			info.stats.textures_upload_time, info.stats.draw_exec_time, num_dirty_textures, texture_memory_size,
			num_flushes, num_misses, cache_miss_ratio, num_unavoidable, num_mispredict, num_speculate,
			num_texture_upload, num_texture_upload_miss, texture_upload_miss_ratio, texture_copies_ellided,
			vertex_cache_hit_count, info.stats.vertex_cache_request_count, vertex_cache_hit_ratio,
			program_cache_ellided, program_cache_lookups, program_cache_ellision_rate)
		);
	}

	if (gl::debug::g_vis_texture)
	{
		// Optionally renders a single debug texture to framebuffer.
		// Only programmatic access provided at the moment.
		// TODO: Migrate to use overlay system. (kd-11)
		gl::fbo m_vis_buffer;
		m_vis_buffer.create();
		m_vis_buffer.bind();
		m_vis_buffer.color = gl::debug::g_vis_texture->id();
		m_vis_buffer.read_buffer(m_vis_buffer.color);
		m_vis_buffer.draw_buffer(m_vis_buffer.color);

		const u32 vis_width = 320;
		const u32 vis_height = 240;
		areai display_view = areai(aspect_ratio).flipped_vertical();
		display_view.x1 = display_view.x2 - vis_width;
		display_view.y1 = vis_height;

		// Blit
		const auto src_region = areau{ 0u, 0u, gl::debug::g_vis_texture->width(), gl::debug::g_vis_texture->height() };
		m_vis_buffer.blit(gl::screen, static_cast<areai>(src_region), display_view, gl::buffers::color, gl::filter::linear);
		m_vis_buffer.remove();
	}

#if defined(LIBRETRO_CORE)
	// Ensure all GPU commands are submitted before signaling frame ready to libretro
	// Without this, RetroArch may present incomplete/partial frames
	glFlush();
#endif

	m_frame->flip(m_context);
	rsx::thread::flip(info);

	// Cleanup
	m_gl_texture_cache.on_frame_end();
	m_vertex_cache->purge();

	auto removed_textures = m_rtts.trim(cmd);
	m_framebuffer_cache.remove_if([&](auto& fbo)
	{
		if (fbo.unused_check_count() >= 2) return true; // Remove if stale
		if (fbo.references_any(removed_textures)) return true; // Remove if any of the attachments is invalid

		return false;
	});

	if (m_draw_fbo && !m_graphics_state.test(rsx::rtt_config_dirty))
	{
		// Always restore the active framebuffer
		m_draw_fbo->bind();
		set_viewport();
		set_scissor(m_graphics_state & rsx::pipeline_state::scissor_setup_clipped);
	}
}
