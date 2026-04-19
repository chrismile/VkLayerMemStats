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

#ifndef VKLAYERMEMSTATS_PROFILER_HPP
#define VKLAYERMEMSTATS_PROFILER_HPP

#include <vector>
#include <list>
#include <cmath>
#include <cstdint>

enum class MemStatsLayerQueryType : uint32_t {
    UNKNOWN,
    COMMAND_BUFFER_SUBMISSION,
    CMD_UPDATE_BUFFER, CMD_COPY_BUFFER, CMD_COPY_IMAGE, CMD_COPY_BUFFER_TO_IMAGE, CMD_COPY_IMAGE_TO_BUFFER,
    CMD_DISPATCH, CMD_DISPATCH_INDIRECT,
    CMD_DRAW, CMD_DRAW_INDIRECT, CMD_DRAW_INDIRECT_COUNT,
    CMD_DRAW_INDEXED, CMD_DRAW_INDEXED_INDIRECT, CMD_DRAW_INDEXED_INDIRECT_COUNT,
    CMD_DRAW_MESH_TASKS, CMD_DRAW_MESH_TASKS_INDIRECT, CMD_DRAW_MESH_TASKS_INDIRECT_COUNT,
    CMD_TRACE_RAYS, CMD_TRACE_RAYS_INDIRECT
};

static const char* const QUERY_TYPE_NAMES[] = {
    "ERROR",
    "command_buffer_submission",
    "update_buffer", "copy_buffer", "copy_image", "copy_buffer_to_image", "copy_image_to_buffer",
    "dispatch", "dispatch_indirect",
    "draw", "draw_indirect", "draw_indirect_count",
    "draw_indexed", "draw_indexed_indirect", "draw_indexed_indirect_count",
    "draw_mesh_tasks", "draw_mesh_tasks_indirect", "draw_mesh_tasks_indirect_count",
    "trace_rays", "trace_rays_indirect"
};

struct MemStatsLayer_Query {
    MemStatsLayerQueryType queryType = MemStatsLayerQueryType::UNKNOWN;
    uint64_t recordTimeStamp = 0;
    uint64_t vulkanCommandIdx = 0;
};

struct MemStatsLayer_QueryAdder;

struct MemStatsLayer_SubmitData {
    MemStatsLayer_SubmitData(
            VkDevice device,
            PFN_vkCreateQueryPool pCreateQueryPool,
            PFN_vkDestroyQueryPool pDestroyQueryPool,
            PFN_vkResetQueryPool pResetQueryPool,
            PFN_vkCmdResetQueryPool pCmdResetQueryPool,
            PFN_vkGetQueryPoolResults pGetQueryPoolResults,
            PFN_vkGetCalibratedTimestampsKHR pGetCalibratedTimestampsKHR,
            float timestampPeriod);
    ~MemStatsLayer_SubmitData();
    // Resets to initial state to be reused for next command buffer submit.
    void reset();
    // Reads back query data.
    void readBack();

    // Global data supplied by MemStatsLayer_Profiler.
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkCreateQueryPool pCreateQueryPool = nullptr;
    PFN_vkDestroyQueryPool pDestroyQueryPool = nullptr;
    PFN_vkResetQueryPool pResetQueryPool = nullptr;
    PFN_vkCmdResetQueryPool pCmdResetQueryPool = nullptr;
    PFN_vkGetQueryPoolResults pGetQueryPoolResults = nullptr;
    PFN_vkGetCalibratedTimestampsKHR pGetCalibratedTimestampsKHR = nullptr;
    float timestampPeriod = 1.0f;

    // Query pool data.
    bool submitted = false;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    uint64_t* queryBuffer = nullptr;
    uint64_t globalFrameIndex = 0;
    uint64_t submitTimestamp = std::numeric_limits<uint64_t>::max();
    std::shared_ptr<MemStatsLayer_QueryAdder> submissionQueryAdder{};
    std::vector<MemStatsLayer_Query> queries;
    const uint32_t maxNumQueries = 4096;
    uint32_t numDirtyQueries = maxNumQueries;

    // Data added by vkQueueSubmit.
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<VkSemaphore> waitSemaphores;
    std::vector<uint64_t> waitSemaphoreValues;
    std::vector<VkSemaphore> signalSemaphores;
    std::vector<uint64_t> signalSemaphoreValues;
};

struct MemStatsLayer_Profiler {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkCreateQueryPool pCreateQueryPool = nullptr;
    PFN_vkDestroyQueryPool pDestroyQueryPool = nullptr;
    PFN_vkResetQueryPool pResetQueryPool = nullptr;
    PFN_vkCmdResetQueryPool pCmdResetQueryPool = nullptr;
    PFN_vkGetQueryPoolResults pGetQueryPoolResults = nullptr;
    PFN_vkCmdWriteTimestamp pCmdWriteTimestamp = nullptr;
    PFN_vkGetCalibratedTimestampsKHR pGetCalibratedTimestampsKHR = nullptr;
    uint64_t currentFrameIndex = 0;
    uint64_t commandIndex = 0;
    bool supportsQueries = false;
    float timestampPeriod = 1.0f;

    MemStatsLayer_SubmitData* findSubmitInFlight(VkCommandBuffer commandBuffer) {
        for (auto& submitData : submitsInFlight) {
            if (submitData->commandBuffer == commandBuffer) {
                return submitData;
            }
        }
        return nullptr;
    }
    const uint32_t maxNumSubmitsInFlight = 16;
    std::list<MemStatsLayer_SubmitData*> submitsInFlight;
    std::vector<MemStatsLayer_SubmitData*> freeSubmitDataList; /// New submit data gets added by @see MemStatsLayer_QueryAdder.
};

static std::map<void*, MemStatsLayer_Profiler*> profilerMap;

inline MemStatsLayer_SubmitData::MemStatsLayer_SubmitData(
        VkDevice device,
        PFN_vkCreateQueryPool pCreateQueryPool,
        PFN_vkDestroyQueryPool pDestroyQueryPool,
        PFN_vkResetQueryPool pResetQueryPool,
        PFN_vkCmdResetQueryPool pCmdResetQueryPool,
        PFN_vkGetQueryPoolResults pGetQueryPoolResults,
        PFN_vkGetCalibratedTimestampsKHR pGetCalibratedTimestampsKHR,
        float timestampPeriod)
        : device(device),
          pCreateQueryPool(pCreateQueryPool), pDestroyQueryPool(pDestroyQueryPool),
          pResetQueryPool(pResetQueryPool), pCmdResetQueryPool(pCmdResetQueryPool),
          pGetQueryPoolResults(pGetQueryPoolResults), pGetCalibratedTimestampsKHR(pGetCalibratedTimestampsKHR),
          timestampPeriod(timestampPeriod) {
    VkQueryPoolCreateInfo queryPoolCreateInfo = {};
    queryPoolCreateInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryPoolCreateInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryPoolCreateInfo.queryCount = maxNumQueries;
    pCreateQueryPool(device, &queryPoolCreateInfo, nullptr, &queryPool);
    //pResetQueryPool(device, queryPool, 0, maxNumQueries); // needs hostQueryReset

    queryBuffer = new uint64_t[maxNumQueries];
    queries.reserve(maxNumQueries);
}

inline MemStatsLayer_SubmitData::~MemStatsLayer_SubmitData() {
    pDestroyQueryPool(device, queryPool, nullptr);
    delete[] queryBuffer;
}

void MemStatsLayer_SubmitData::reset() {
    commandBuffer = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    queries.clear();
    submitted = false;
    submitTimestamp = std::numeric_limits<uint64_t>::max();

    fence = VK_NULL_HANDLE;
    signalSemaphores.clear();
    signalSemaphoreValues.clear();
    waitSemaphores.clear();
    waitSemaphoreValues.clear();
}

inline void MemStatsLayer_SubmitData::readBack() {
    if (queries.empty()) {
        return;
    }
    size_t numQueries = queries.size();

    pGetQueryPoolResults(
            device, queryPool, 0, static_cast<uint32_t>(numQueries) * 2,
            numQueries * 2 * sizeof(uint64_t), queryBuffer, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    //pResetQueryPool(device, queryPool, 0, maxNumQueries); // needs hostQueryReset
    auto readBackTimeStamp = getTimeStamp();

    uint64_t calibratedTimestamps[2];
    uint64_t maxDeviationNs = 0;
    VkCalibratedTimestampInfoKHR calibratedTimestampInfos[2];
    VkCalibratedTimestampInfoKHR& calibratedTimestampInfoDevice = calibratedTimestampInfos[0];
    calibratedTimestampInfoDevice.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    calibratedTimestampInfoDevice.pNext = nullptr;
    calibratedTimestampInfoDevice.timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
    VkCalibratedTimestampInfoKHR& calibratedTimestampInfoHost = calibratedTimestampInfos[1];
    calibratedTimestampInfoHost.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    calibratedTimestampInfoHost.pNext = nullptr;
#ifdef _WIN32
    calibratedTimestampInfoHost.timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
    calibratedTimestampInfoHost.timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#endif
    auto res = pGetCalibratedTimestampsKHR(device, 2, calibratedTimestampInfos, calibratedTimestamps, &maxDeviationNs);
    if (res != VK_SUCCESS) {
        std::cerr << "[VkLayer_memstats] vkGetCalibratedTimestampsKHR failed." << std::endl;
    }

    for (size_t queryIdx = 0; queryIdx < numQueries; ++queryIdx) {
        auto& query = queries[queryIdx];
        uint64_t startTimestampDevice = queryBuffer[queryIdx * 2];
        uint64_t stopTimestampDevice = queryBuffer[queryIdx * 2 + 1];
        auto executionTimeNanoseconds = static_cast<uint64_t>(std::round(
                double(stopTimestampDevice - startTimestampDevice) * timestampPeriod));
        auto executionStartTimeStamp = deviceTimeStampToHostTimeStamp(
                startTimestampDevice, timestampPeriod, calibratedTimestamps[0], calibratedTimestamps[1]);
        auto executionStopTimeStamp = deviceTimeStampToHostTimeStamp(
                stopTimestampDevice, timestampPeriod, calibratedTimestamps[0], calibratedTimestamps[1]);
        fprintf_save(MemStatsLayer_outFile,
                "profiler_event,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%s\n",
                query.recordTimeStamp, readBackTimeStamp, executionStartTimeStamp, executionStopTimeStamp,
                executionTimeNanoseconds, query.vulkanCommandIdx, globalFrameIndex,
                QUERY_TYPE_NAMES[static_cast<int>(query.queryType)]);
    }
    numDirtyQueries = static_cast<uint32_t>(queries.size()) * 2;
}

struct MemStatsLayer_QueryAdder {
public:
    MemStatsLayer_QueryAdder(VkCommandBuffer commandBuffer, MemStatsLayerQueryType queryType);
    ~MemStatsLayer_QueryAdder();
    bool isValid = false;
    uint32_t queryEndIdx = 0;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    MemStatsLayer_SubmitData* submitData = nullptr;
};

MemStatsLayer_QueryAdder::MemStatsLayer_QueryAdder(
        VkCommandBuffer commandBuffer, MemStatsLayerQueryType queryType) : commandBuffer(commandBuffer) {
    const auto& settings = deviceLayerSettings[getDispatchKey(commandBuffer)];
    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];

    if (settings.useProfiler && profiler->supportsQueries) {
        submitData = profiler->findSubmitInFlight(commandBuffer);
        if (!submitData) {
            if (profiler->freeSubmitDataList.empty()) {
                if (profiler->submitsInFlight.size() > profiler->maxNumSubmitsInFlight) {
                    std::cerr << "[VkLayer_memstats] Exceeded maximum number of submits in flight." << std::endl;
                    return;
                }
                auto* submitData = new MemStatsLayer_SubmitData(
                        profiler->device,
                        profiler->pCreateQueryPool, profiler->pDestroyQueryPool,
                        profiler->pResetQueryPool, profiler->pCmdResetQueryPool,
                        profiler->pGetQueryPoolResults,
                        profiler->pGetCalibratedTimestampsKHR,
                        profiler->timestampPeriod);
                profiler->freeSubmitDataList.push_back(submitData);
            }
            auto* frameDataNew = profiler->freeSubmitDataList.back();
            frameDataNew->commandBuffer = commandBuffer;
            frameDataNew->globalFrameIndex = profiler->currentFrameIndex;
            profiler->freeSubmitDataList.pop_back();
            profiler->pCmdResetQueryPool(commandBuffer, frameDataNew->queryPool, 0, frameDataNew->numDirtyQueries);
            profiler->submitsInFlight.push_back(frameDataNew);
            submitData = frameDataNew;
        }
        auto numQueries = static_cast<uint32_t>(submitData->queries.size()) * 2;
        if (numQueries <= submitData->maxNumQueries) {
            MemStatsLayer_Query query{};
            query.queryType = queryType;
            query.recordTimeStamp = getTimeStamp();
            query.vulkanCommandIdx = profiler->commandIndex - 1;
            submitData->queries.push_back(query);
            profiler->pCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, submitData->queryPool, numQueries);
            queryPool = submitData->queryPool;
            queryEndIdx = numQueries + 1;
            isValid = true;
        } else {
            std::cerr << "[VkLayer_memstats] Exceeded maximum number of timestamp queries." << std::endl;
        }
    }
}

MemStatsLayer_QueryAdder::~MemStatsLayer_QueryAdder() {
    if (isValid) {
        auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
        profiler->pCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, queryEndIdx);
    }
}

#define PROFILER_CREATE_QUERY(commandBuffer, queryType) MemStatsLayer_QueryAdder queryAdder(commandBuffer, queryType);

#endif //VKLAYERMEMSTATS_PROFILER_HPP
