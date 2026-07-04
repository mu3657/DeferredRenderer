#pragma once

#include "render_pass.h"

class GeometryPass : public RenderPassBase {
public:
    const char* name() const override { return "GeometryPass"; }
    void init(const RenderPassInitContext&) override {}
    void cleanup() override {}

    void execute(GeometryPassContext& ctx);
};
