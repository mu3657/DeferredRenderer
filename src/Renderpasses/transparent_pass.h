#pragma once

#include "../render_pass.h"

class VulkanEngine;

class TransparentPass : public RenderPassBase {
public:
    const char* name() const override { return "TransparentPass"; }

    void init(const RenderPassInitContext& ctx) override;
    void cleanup() override;
    void execute(TransparentPassContext& ctx);

private:
    VulkanEngine* _engine{nullptr};
};
