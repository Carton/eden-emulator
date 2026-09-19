// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <vector>

#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes;
    FixedPipelineState state;

    size_t Hash() const noexcept;

    bool operator==(const GraphicsPipelineCacheKey& rhs) const noexcept;

    bool operator!=(const GraphicsPipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }

    size_t Size() const noexcept {
        return sizeof(unique_hashes) + state.Size();
    }
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<GraphicsPipelineCacheKey>);

} // namespace Vulkan

namespace std {
template <>
struct hash<Vulkan::GraphicsPipelineCacheKey> {
    size_t operator()(const Vulkan::GraphicsPipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace Vulkan {

class Device;
class PipelineStatistics;
class RenderPassCache;
class RescalingPushConstant;
class RenderAreaPushConstant;
class Scheduler;

// (local-only) P2 depth-1 draw resolver: per-draw state handed between the
// resolve phase (binding lookups, snapshot-driven) and the commit phase
// (uploads + scheduler records, GPU thread). In synchronous mode both phases
// see the live engine; when the resolver thread is active, ctx carries the
// snapshot engine so both phases observe draw N's state regardless of how far
// the GPU thread has parsed into draw N+1.
struct DrawContext {
    Tegra::Engines::Maxwell3D* engine{};
    Tegra::MemoryManager* gpu_memory{};

    // Resolve-phase outputs consumed by the commit phase. Persisted to avoid
    // per-draw heap allocation.
    boost::container::small_vector<VideoCommon::ImageViewInOut, 64> views;
    boost::container::small_vector<VideoCommon::SamplerId, 64> samplers;

    void Reset(Tegra::Engines::Maxwell3D* engine_, Tegra::MemoryManager* gpu_memory_) {
        engine = engine_;
        gpu_memory = gpu_memory_;
        views.clear();
        samplers.clear();
    }
};

class GraphicsPipeline {
    static constexpr size_t NUM_STAGES = Tegra::Engines::Maxwell3D::Regs::MaxShaderStage;

public:
    enum class ConfigurePhase : u8 {
        Resolve, // binding resolution only (safe off the GPU thread)
        Tail,    // uploads + scheduler records (GPU thread only)
        All,     // resolve + tail back to back (synchronous path)
    };
    explicit GraphicsPipeline(
        Scheduler& scheduler, BufferCache& buffer_cache, TextureCache& texture_cache,
        vk::PipelineCache& pipeline_cache, VideoCore::ShaderNotify* shader_notify,
        const Device& device, DescriptorPool& descriptor_pool,
        GuestDescriptorQueue& guest_descriptor_queue,
        DescriptorBufferRing& descriptor_buffer_ring, Common::ThreadWorker* worker_thread,
        PipelineStatistics* pipeline_statistics, RenderPassCache& render_pass_cache,
        const GraphicsPipelineCacheKey& key, std::array<vk::ShaderModule, NUM_STAGES> stages,
        const std::array<const Shader::Info*, NUM_STAGES>& infos);

    bool HasDynamicVertexInput() const noexcept { return key.state.dynamic_vertex_input; }
    bool SupportsAlphaToCoverage() const noexcept {
        return fragment_has_color0_output;
    }

    bool SupportsAlphaToOne() const noexcept {
        return fragment_has_color0_output;
    }

    bool UsesExtendedDynamicState() const noexcept {
        return key.state.extended_dynamic_state != 0;
    }
    GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = delete;
    GraphicsPipeline(GraphicsPipeline&&) noexcept = delete;

    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(const GraphicsPipeline&) = delete;

    void AddTransition(GraphicsPipeline* transition);

    bool Configure(bool is_indexed) {
        DrawContext ctx{maxwell3d, gpu_memory};
        return Configure(ctx, is_indexed);
    }

    bool Configure(DrawContext& ctx, bool is_indexed) {
        return configure_func(this, ctx, is_indexed, ConfigurePhase::All);
    }

    // (local-only) P2 phase split: Resolve is engine-snapshot driven and
    // touches no scheduler state; Tail performs uploads and records commands.
    bool ConfigureResolve(DrawContext& ctx, bool is_indexed) {
        return configure_func(this, ctx, is_indexed, ConfigurePhase::Resolve);
    }

    bool ConfigureTail(DrawContext& ctx, bool is_indexed) {
        return configure_func(this, ctx, is_indexed, ConfigurePhase::Tail);
    }

    // (local-only) P2 uniform epoch / async resolve: the per-pipeline uniform
    // layout is immutable after construction; the resolver reads it instead
    // of channel state (which a concurrently parsing GPU thread may mutate).
    [[nodiscard]] const std::array<u32, 5>& UniformBufferMasks() const noexcept {
        return enabled_uniform_buffer_masks;
    }
    [[nodiscard]] const VideoCommon::UniformBufferSizes& UniformBufferSizeTable() const noexcept {
        return uniform_buffer_sizes;
    }

    [[nodiscard]] GraphicsPipeline* Next(const GraphicsPipelineCacheKey& current_key) noexcept {
        if (key == current_key) {
            return this;
        }
        if (transition_keys.empty()) {
            return nullptr;
        }
        // One candidate still needs equality confirmation; hashing first can
        // only add work, especially for keys that differ near the beginning.
        if (transition_keys.size() == 1) {
            return transition_keys.front() == current_key ? transitions.front() : nullptr;
        }
        // Different pipeline: hash the key once, then scan precomputed transition
        // hashes before falling back to the full key memcmp confirmation.
        const size_t current_hash = current_key.Hash();
        for (size_t i = 0; i < transition_keys.size(); ++i) {
            if (transition_hashes[i] == current_hash && transition_keys[i] == current_key) {
                return transitions[i];
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::relaxed);
    }

    template <typename Spec>
    static auto MakeConfigureSpecFunc() {
        return [](GraphicsPipeline* pl, DrawContext& ctx, bool is_indexed,
                  ConfigurePhase phase) {
            return pl->ConfigureImpl<Spec>(ctx, is_indexed, phase);
        };
    }

    void SetEngine(Tegra::Engines::Maxwell3D* maxwell3d_, Tegra::MemoryManager* gpu_memory_) {
        maxwell3d = maxwell3d_;
        gpu_memory = gpu_memory_;
    }

private:
    template <typename Spec>
    bool ConfigureImpl(DrawContext& ctx, bool is_indexed, ConfigurePhase phase);

    bool ConfigureDraw(const RescalingPushConstant& rescaling,
                       const RenderAreaPushConstant& render_are);

    void MakePipeline(VkRenderPass render_pass);

    void Validate();

    const GraphicsPipelineCacheKey key;
    Tegra::Engines::Maxwell3D* maxwell3d;
    Tegra::MemoryManager* gpu_memory;
    const Device& device;
    TextureCache& texture_cache;
    BufferCache& buffer_cache;
    vk::PipelineCache& pipeline_cache;
    Scheduler& scheduler;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;

    bool (*configure_func)(GraphicsPipeline*, DrawContext&, bool, ConfigurePhase){};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<size_t> transition_hashes;
    std::vector<GraphicsPipeline*> transitions;

    std::array<vk::ShaderModule, NUM_STAGES> spv_modules;

    std::array<Shader::Info, NUM_STAGES> stage_infos;
    std::array<u32, 5> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    u32 num_descriptor_entries{};
    size_t num_image_elements{};
    u32 num_textures{};
    bool fragment_has_color0_output{};

    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;

    DescriptorBufferLayout descriptor_buffer_layout;
    std::vector<DescriptorUpdateEntry> last_descriptor_payload;
    VkDeviceSize last_descriptor_buffer_offset{};
    u32 last_descriptor_buffer_chunk{};
    u64 last_descriptor_buffer_generation{};

    std::condition_variable build_condvar;
    std::mutex build_mutex;
    std::atomic_bool is_built{false};
    bool uses_push_descriptor{false};
    bool uses_descriptor_buffer{false};
};

} // namespace Vulkan
