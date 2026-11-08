// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::vk {

/// Pipeline creation that survives an access violation inside the Vulkan driver (Windows only;
/// elsewhere it is a plain call). AMD's Windows driver has crashed in amdvlk64.dll while building
/// graphics pipelines on RDNA2; a fault is reported as VK_ERROR_UNKNOWN, so the caller's normal
/// failure path runs (a library pipeline falls back to a monolithic one) instead of the process
/// dying. The driver may be left in a bad state, so callers stop using the risky path for the
/// rest of the session (see GplCrashGuard).
VkResult GuardedCreateGraphicsPipelines(PFN_vkCreateGraphicsPipelines function, VkDevice device,
                                        VkPipelineCache cache,
                                        const VkGraphicsPipelineCreateInfo* create_info,
                                        VkPipeline* pipeline) noexcept;

VkResult GuardedCreateComputePipelines(PFN_vkCreateComputePipelines function, VkDevice device,
                                       VkPipelineCache cache,
                                       const VkComputePipelineCreateInfo* create_info,
                                       VkPipeline* pipeline) noexcept;

/// True once (then cleared) when the last guarded call on this thread faulted in the driver.
bool TakeThreadDriverFault() noexcept;

/// Driver faults caught in this session.
u32 DriverFaultCount() noexcept;

} // namespace Vulkan::vk
