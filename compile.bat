@echo off
set GLSLC=glslc.exe

%GLSLC% "Source/shaders/ObjectShaders/ObjectShaderVS.vert" -o "Source/shaders/ObjectShaders/ObjectShaderVS.spv"
%GLSLC% "Source/shaders/ObjectShaders/ObjectSkinnedVS.vert" -o "Source/shaders/ObjectShaders/ObjectSkinnedVS.spv"
%GLSLC% "Source/shaders/ObjectShaders/ObjectTexturedFS.frag" -o "Source/shaders/ObjectShaders/ObjectTexturedFS.spv"
%GLSLC% "Source/shaders/ObjectShaders/ObjectBasicFS.frag" -o "Source/shaders/ObjectShaders/ObjectBasicFS.spv"
%GLSLC% "Source/shaders/ObjectShaders/ObjectVolumetricVS.vert" -o "Source/shaders/ObjectShaders/ObjectVolumetricVS.spv"
%GLSLC% "Source/shaders/ObjectShaders/ObjectVolumetricFS.frag" -o "Source/shaders/ObjectShaders/ObjectVolumetricFS.spv"

%GLSLC% "Source/shaders/ComputeShaders/MeshtoSdf.comp" -o "Source/shaders/ComputeShaders/MeshtoSdf.spv"
%GLSLC% "Source/shaders/ComputeShaders/sdfCompute.comp" -o "Source/shaders/ComputeShaders/sdfCompute.spv"
%GLSLC% "Source/shaders/ComputeShaders/sdfCompute2.comp" -o "Source/shaders/ComputeShaders/sdfCompute2.spv"
%GLSLC% "Source/shaders/ComputeShaders/GaussianFluidSim.comp" -o "Source/shaders/ComputeShaders/GaussianFluidSim.spv"
%GLSLC% "Source/shaders/ComputeShaders/sdfCompute _pottery.comp" -o "Source/shaders/ComputeShaders/sdfCompute _pottery.spv"

%GLSLC% "Source/shaders/PostProcessing/VolumePassVS.vert" -o "Source/shaders/PostProcessing/VolumePassVS.spv"
%GLSLC% "Source/shaders/PostProcessing/VolumePassFS.frag" -o "Source/shaders/PostProcessing/VolumePassFS.spv"
%GLSLC% "Source/shaders/PostProcessing/TaaHistoryCopyFS.frag" -o "Source/shaders/PostProcessing/TaaHistoryCopyFS.spv"
