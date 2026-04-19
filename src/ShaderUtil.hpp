/*
 * BSD 2-Clause License
 *
 * Copyright (c) 2026, Christoph Neuhauser
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef VKLAYERMEMSTATS_SHADERUTIL_HPP
#define VKLAYERMEMSTATS_SHADERUTIL_HPP

#include <string>
#include <unordered_map>
#include <iostream>

#define SPV_ENABLE_UTILITY_CODE
#include <spirv_reflect.h>

struct MemStatsLayer_ShaderModuleData {
    std::string sourceFileName;
};

struct MemStatsLayer_PipelineShaderStage {
    VkShaderStageFlagBits stage;
    VkShaderModule module;
    std::string pName;
};

struct MemStatsLayer_PipelineData {
    VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    std::vector<MemStatsLayer_PipelineShaderStage> shaderStages;
};

struct MemStatsLayer_ShaderUtilCommandBufferData {
    std::unordered_map<VkPipelineBindPoint, VkPipeline> pipelineBindPointMap;
};

static const char* vulkanShaderStageToString(VkShaderStageFlagBits stage) {
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        return "vertex";
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
        return "tesselation_control";
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
        return "tesselation_evaluation";
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        return "geometry";
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        return "fragment";
    case VK_SHADER_STAGE_COMPUTE_BIT:
        return "compute";
    case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
        return "raygen";
    case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
        return "any_hit";
    case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
        return "closest_hit";
    case VK_SHADER_STAGE_MISS_BIT_KHR:
        return "miss";
    case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
        return "intersection";
    case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
        return "callable";
    case VK_SHADER_STAGE_TASK_BIT_EXT:
        return "task";
    case VK_SHADER_STAGE_MESH_BIT_EXT:
        return "mesh";
    default:
        return "unknown";
    }
}

static void parseSpirvModuleName(
        const uint32_t* code, size_t codeSize, MemStatsLayer_ShaderModuleData& shaderModuleData) {
    SpvReflectShaderModule module;
    SpvReflectResult result = spvReflectCreateShaderModule(codeSize, code, &module);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        std::cerr << "[VkLayer_memstats] spvReflectCreateShaderModule failed." << std::endl;
        return;
    }
    if (module.source_file) {
        shaderModuleData.sourceFileName = module.source_file;
    }
}

class MemStatsLayer_ShaderUtil {
public:
    void addShaderModule(VkShaderModule shaderModule, const VkShaderModuleCreateInfo& createInfo);
    void removeShaderModule(VkShaderModule shaderModule);
    std::string addPipeline(
            VkPipeline pipeline, VkPipelineBindPoint pipelineBindPoint,
            const std::vector<MemStatsLayer_PipelineShaderStage>& shaderStages);
    void removePipeline(VkPipeline pipeline);
    void onBeginCommandBuffer(VkCommandBuffer commandBuffer);
    void bindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline);
    void onEndCommandBuffer(VkCommandBuffer commandBuffer);
    VkPipeline getBoundPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint);

private:
    std::unordered_map<VkShaderModule, MemStatsLayer_ShaderModuleData> shaderModuleMap;
    std::unordered_map<VkPipeline, MemStatsLayer_PipelineData> pipelineMap;
    std::unordered_map<VkCommandBuffer, MemStatsLayer_ShaderUtilCommandBufferData> commandBufferBoundPipelinesMap;
};

static std::map<void*, MemStatsLayer_ShaderUtil*> shaderUtilMap;

void MemStatsLayer_ShaderUtil::addShaderModule(
        VkShaderModule shaderModule, const VkShaderModuleCreateInfo& createInfo) {
    MemStatsLayer_ShaderModuleData shaderModuleData{};
    parseSpirvModuleName(createInfo.pCode, createInfo.codeSize, shaderModuleData);
    shaderModuleMap.insert(std::make_pair(shaderModule, shaderModuleData));
}

void MemStatsLayer_ShaderUtil::removeShaderModule(VkShaderModule shaderModule) {
    shaderModuleMap.erase(shaderModule);
}

std::string MemStatsLayer_ShaderUtil::addPipeline(
        VkPipeline pipeline, VkPipelineBindPoint pipelineBindPoint,
        const std::vector<MemStatsLayer_PipelineShaderStage>& shaderStages) {
    MemStatsLayer_PipelineData pipelineData;
    pipelineData.bindPoint = pipelineBindPoint;
    pipelineData.shaderStages = shaderStages;
    pipelineMap.insert(std::make_pair(pipeline, pipelineData));

    std::string boundShaderNames = "{";
    for (size_t shaderStageIdx = 0; shaderStageIdx < shaderStages.size(); shaderStageIdx++) {
        if (shaderStageIdx != 0) {
            boundShaderNames += ";";
        }
        const auto& shaderStage = shaderStages[shaderStageIdx];
        boundShaderNames += vulkanShaderStageToString(shaderStage.stage);
        boundShaderNames += ":";
        boundShaderNames += shaderStage.pName;
        boundShaderNames += ":";
        auto itModule = shaderModuleMap.find(shaderStage.module);
        if (itModule != shaderModuleMap.end()) {
            boundShaderNames += itModule->second.sourceFileName;
            itModule->second.sourceFileName;
        }
    }
    boundShaderNames += "}";
    return boundShaderNames;
}

void MemStatsLayer_ShaderUtil::removePipeline(VkPipeline pipeline) {
    pipelineMap.erase(pipeline);
}

void MemStatsLayer_ShaderUtil::onBeginCommandBuffer(VkCommandBuffer commandBuffer) {
    commandBufferBoundPipelinesMap[commandBuffer] = {};
}

void MemStatsLayer_ShaderUtil::bindPipeline(
        VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline) {
    auto it = commandBufferBoundPipelinesMap.find(commandBuffer);
    if (it != commandBufferBoundPipelinesMap.end()) {
        it->second.pipelineBindPointMap[pipelineBindPoint] = pipeline;
    }
}

void MemStatsLayer_ShaderUtil::onEndCommandBuffer(VkCommandBuffer commandBuffer) {
    commandBufferBoundPipelinesMap.erase(commandBuffer);
}

VkPipeline MemStatsLayer_ShaderUtil::getBoundPipeline(
        VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint) {
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    auto it = commandBufferBoundPipelinesMap.find(commandBuffer);
    if (it != commandBufferBoundPipelinesMap.end()) {
        auto itBindPoint = it->second.pipelineBindPointMap.find(pipelineBindPoint);
        if (itBindPoint != it->second.pipelineBindPointMap.end()) {
            boundPipeline = itBindPoint->second;
        }
    }
    return boundPipeline;
}

#endif //VKLAYERMEMSTATS_SHADERUTIL_HPP
