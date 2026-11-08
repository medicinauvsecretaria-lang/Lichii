// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>

#if defined(_WIN32) && defined(_MSC_VER)
#include <excpt.h>
#endif

#include "common/logging.h"
#include "video_core/vulkan_common/driver_fault_guard.h"

namespace Vulkan::vk {
namespace {

constexpr unsigned long ACCESS_VIOLATION = 0xC0000005UL;

thread_local bool thread_fault = false;
std::atomic<u32> fault_count{};

#if defined(_WIN32) && defined(_MSC_VER)
// Kept free of C++ objects with destructors: structured exception handling cannot unwind them.
template <typename Function, typename CreateInfo>
VkResult SehCall(Function function, VkDevice device, VkPipelineCache cache,
                 const CreateInfo* create_info, VkPipeline* pipeline, bool* faulted) noexcept {
    __try {
        return function(device, cache, 1, create_info, nullptr, pipeline);
    } __except (_exception_code() == ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                       : EXCEPTION_CONTINUE_SEARCH) {
        *faulted = true;
        return VK_ERROR_UNKNOWN;
    }
}
#endif

template <typename Function, typename CreateInfo>
VkResult Call(const char* kind, Function function, VkDevice device, VkPipelineCache cache,
              const CreateInfo* create_info, VkPipeline* pipeline) noexcept {
#if defined(_WIN32) && defined(_MSC_VER)
    bool faulted = false;
    *pipeline = VK_NULL_HANDLE;
    const VkResult result = SehCall(function, device, cache, create_info, pipeline, &faulted);
    if (faulted) {
        *pipeline = VK_NULL_HANDLE;
        thread_fault = true;
        const u32 count = fault_count.fetch_add(1, std::memory_order_relaxed) + 1;
        LOG_CRITICAL(Render_Vulkan,
                     "Nx2Perf driver fault guard: the Vulkan driver crashed building a {} pipeline "
                     "(access violation); the emulator kept running ({} this session)",
                     kind, count);
    }
    return result;
#else
    (void)kind;
    return function(device, cache, 1, create_info, nullptr, pipeline);
#endif
}

} // Anonymous namespace

VkResult GuardedCreateGraphicsPipelines(PFN_vkCreateGraphicsPipelines function, VkDevice device,
                                        VkPipelineCache cache,
                                        const VkGraphicsPipelineCreateInfo* create_info,
                                        VkPipeline* pipeline) noexcept {
    return Call("graphics", function, device, cache, create_info, pipeline);
}

VkResult GuardedCreateComputePipelines(PFN_vkCreateComputePipelines function, VkDevice device,
                                       VkPipelineCache cache,
                                       const VkComputePipelineCreateInfo* create_info,
                                       VkPipeline* pipeline) noexcept {
    return Call("compute", function, device, cache, create_info, pipeline);
}

bool TakeThreadDriverFault() noexcept {
    const bool fault = thread_fault;
    thread_fault = false;
    return fault;
}

u32 DriverFaultCount() noexcept {
    return fault_count.load(std::memory_order_relaxed);
}

} // namespace Vulkan::vk
