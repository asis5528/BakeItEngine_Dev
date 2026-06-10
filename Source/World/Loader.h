#pragma once
#include "World/Scene.h"
#include "EngineContext.h"
class Loader
{
public:
	VulkanBase* base;
	Scene* scene;
	 Loader(EngineContext& ctx);
	 void load(int path_count, const char* paths[]);

};

