#pragma once

#include "render_pass.h"

#include <chrono>
#include <unordered_map>

class TAAPass : public RenderPassBase {
public:
    const char* name() const override { return "TAAPass"; }
    void init(const RenderPassInitContext& ctx) override;
    void cleanup() override;
    void draw_debug_ui() override;

    // Called after acquire/fence success, before recording any raster passes.
    void prepare_frame(RenderPassFrameContext& ctx);
    void execute(RenderPassFrameContext& ctx);
    void commit_frame(); // only after successful queue submission
    void reset_history();
    void set_enabled(bool enabled);
    bool enabled() const { return _enabled; }
    bool history_was_used() const { return _historyWasUsed; }
    const AllocatedImage& resolved_history() const { return _history[1 - _writeIndex]; }
    const AllocatedImage& motion_image() const { return _motion; }

private:
    struct PreviousObject {
        glm::mat4 transform{1.f};
        VkDeviceAddress vertices{0};
    };
    struct TransformBuffer {
        AllocatedBuffer buffer{};
        size_t capacity{0};
    };
    VulkanEngine* _engine{nullptr};
    VkPipeline _pipeline{VK_NULL_HANDLE};
    VkPipelineLayout _pipelineLayout{VK_NULL_HANDLE};
    VkSampler _sampler{VK_NULL_HANDLE};
    AllocatedImage _motion{};
    std::array<AllocatedImage, 2> _history{};
    std::vector<std::array<VkDescriptorSet, 2>> _sets;
    std::vector<TransformBuffer> _transforms;
    std::unordered_map<uint64_t, PreviousObject> _previousObjects, _pendingObjects;
    VkImage _sourceImage{VK_NULL_HANDLE};
    VkImageView _sourceView{VK_NULL_HANDLE}, _depthView{VK_NULL_HANDLE};
    VkExtent2D _extent{};
    glm::mat4 _previousView{1.f}, _previousProjection{1.f}, _previousViewproj{1.f};
    glm::vec2 _previousJitter{};
    std::string _sceneName;
    uint64_t _cameraCutSerial{0};
    std::chrono::steady_clock::time_point _lastSubmission{};
    uint32_t _writeIndex{0}, _sampleIndex{0};
    int _backgroundEffect{-1}, _pbrDebugMode{-1};
    bool _enabled{true}, _valid{false}, _initializedImages{false}, _resolved{false};
    bool _historyWasUsed{false};
    float _historyWeight{0.9f};
    void create_images();
    void destroy_images();
    void write_descriptors();
};
