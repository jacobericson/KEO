#pragma once
#include <stdint.h>
#include <string>
#include "base/klib_include.h"
#include <kenshi/Kenshi.h>
#include "base/klib_include_end.h"

// Shared production refusal policy. Version detection remains KenshiLib's job.
inline bool KlibSupportsBinary(uintptr_t base, const std::string& version,
                              KenshiLib::BinaryVersion::KenshiPlatform platform)
{
	return base != 0 && version == "1.0.65"
		&& platform == KenshiLib::BinaryVersion::STEAM;
}
