#pragma once

#include "script_types.h"

namespace scripting {

using NodeHandle = uint64_t;
inline constexpr NodeHandle invalid_node = 0;

struct LightState {
    glm::vec3 color{1.f};
    float intensity{1.f};
    float range{10.f};
};

// Implemented by the scene adapter. No scene ownership or renderer types cross
// this boundary. The host must outlive ScriptSystem and its Lua references.
class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual bool is_valid(NodeHandle node) const = 0;
    virtual NodeHandle find_node(const std::string& name) const = 0;
    virtual std::optional<glm::vec3> local_position(NodeHandle node) const = 0;
    virtual ScriptResult set_local_position(NodeHandle node, const glm::vec3& position) = 0;
    virtual ScriptResult rotate_local_y_radians(NodeHandle node, double angle) = 0;
    virtual std::optional<LightState> light(NodeHandle node) const = 0;
    virtual ScriptResult set_light(NodeHandle node, const LightState& light) = 0;
};

} // namespace scripting
