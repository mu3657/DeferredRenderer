#pragma once

// Private to the runtime: Lua types must not appear in public API headers.
#include "script_host.h"
#include <sol/sol.hpp>

namespace scripting::detail {

struct BindingContext {
    ScriptHost* host{};
    bool allowWrites{false};
};

sol::table make_node_binding(sol::state_view lua,
    const std::shared_ptr<BindingContext>& context, NodeHandle node);
void bind_scene(sol::environment& environment, const std::shared_ptr<BindingContext>& context);

} // namespace scripting::detail
