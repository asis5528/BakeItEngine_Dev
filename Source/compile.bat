@echo off
set GLSLC=glslc.exe

%GLSLC% "shaders/ObjectShaders/ObjectShaderVS.vert" -o "shaders/ObjectShaders/ObjectShaderVS.spv"
%GLSLC% "shaders/ObjectShaders/ObjectSkinnedVS.vert" -o "shaders/ObjectShaders/ObjectSkinnedVS.spv"
%GLSLC% "shaders/ObjectShaders/ObjectTexturedFS.frag" -o "shaders/ObjectShaders/ObjectTexturedFS.spv"
%GLSLC% "shaders/ObjectShaders/ObjectBasicFS.frag" -o "shaders/ObjectShaders/ObjectBasicFS.spv"
%GLSLC% "shaders/ObjectShaders/ObjectVolumetricVS.vert" -o "shaders/ObjectShaders/ObjectVolumetricVS.spv"
%GLSLC% "shaders/ObjectShaders/ObjectVolumetricFS.frag" -o "shaders/ObjectShaders/ObjectVolumetricFS.spv"

%GLSLC% "shaders/ComputeShaders/MeshtoSdf.comp" -o "shaders/ComputeShaders/MeshtoSdf.spv"
%GLSLC% "shaders/ComputeShaders/sdfCompute.comp" -o "shaders/ComputeShaders/sdfCompute.spv"
%GLSLC% "shaders/ComputeShaders/sdfCompute2.comp" -o "shaders/ComputeShaders/sdfCompute2.spv"
%GLSLC% "shaders/ComputeShaders/GaussianFluidSim.comp" -o "shaders/ComputeShaders/GaussianFluidSim.spv"
%GLSLC% "shaders/ComputeShaders/sdfCompute _pottery.comp" -o "shaders/ComputeShaders/sdfCompute _pottery.spv"
%GLSLC% "shaders/ComputeShaders/sdfCompute_lod1.comp" -o "shaders/ComputeShaders/sdfCompute_lod1.spv"

%GLSLC% "shaders/PostProcessing/VolumePassVS.vert" -o "shaders/PostProcessing/VolumePassVS.spv"
%GLSLC% "shaders/PostProcessing/VolumePassFS.frag" -o "shaders/PostProcessing/VolumePassFS.spv"
%GLSLC% "shaders/PostProcessing/TaaHistoryCopyFS.frag" -o "shaders/PostProcessing/TaaHistoryCopyFS.spv"
