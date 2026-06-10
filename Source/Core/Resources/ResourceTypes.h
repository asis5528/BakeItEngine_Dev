#pragma once
namespace ResourceTypes
{
	struct Texture {
		__int64 id;
		int width;
		int height;
		int depth;
	};
	struct Texture3D :Texture {

	};
}

