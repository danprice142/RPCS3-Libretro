#include "stdafx.h"

#include "Emu/Io/pad_config.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

cfg_input_configurations g_cfg_input_configs;
std::string g_input_config_override;

[[noreturn]] void report_fatal_error(std::string_view text, bool is_html, bool include_help_text)
{
	(void)is_html;
	(void)include_help_text;

	std::fprintf(stderr, "RPCS3(libretro) fatal error: %.*s\n", static_cast<int>(text.size()), text.data());
	std::fflush(stderr);
	throw std::runtime_error(std::string(text));
}

void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op)
{
	if (!wrapped_op)
		return;

	if (repeat_duration_ms < 0)
		repeat_duration_ms = 0;

	const auto delay = std::chrono::milliseconds(repeat_duration_ms);

	while (!wrapped_op())
	{
		if (repeat_duration_ms == 0)
		{
			std::this_thread::yield();
		}
		else
		{
			std::this_thread::sleep_for(delay);
		}
	}
}
