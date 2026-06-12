#pragma once
#include <cstdint>
namespace ResourceTypes
{
	struct Texture {
		int64_t id;
		int width;
		int height;
		int depth;
	};
	struct Texture3D :Texture {

	};
}

