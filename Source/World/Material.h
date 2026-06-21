#pragma once
#include "Core/Pipelines/GraphicsPipeline.h"

enum MaterialType {
	BasicMaterial, 
	TexturedMaterial,
	PBR 
	
};
struct MaterialBasicData {
	glm::vec4 DiffuseColor;
	float time;
};
struct MaterialTexturedData : public MaterialBasicData {
	int DiffuseTexture;
	// Optional 2nd texture (-1 = none). Used by the SDF brick object to bind the
	// sparse COARSE tile pool alongside the fine pool: with [UBOs][textures][SBOs]
	// binding order, DiffuseTexture -> binding 2, this -> binding 3.
	int SecondaryTexture = -1;
};
struct MaterialPBRData : public MaterialBasicData {
	int DiffuseTexture = -1;
};

class Material
{
public:
	void* materialData;
	GraphicsPipeline *pipeline;
	GraphicsPipeline *skinnedPipeline;
	MaterialType type;
};

