#include "script_bindings.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace scripting::detail {
namespace {

void require_success(const ScriptResult& result)
{
    if (!result) throw std::runtime_error(result.error);
}

ScriptHost& require_host(const BindingContext& context, NodeHandle node, bool write)
{
    if (!context.host || !context.host->is_valid(node)) throw std::runtime_error("node handle is no longer valid");
    if (write && !context.allowWrites) throw std::runtime_error("scene writes are only allowed inside lifecycle callbacks");
    return *context.host;
}

float read_number(const sol::object& object)
{
    if (object.get_type() != sol::type::number) throw std::runtime_error("expected a finite number");
    const double value = object.as<double>();
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
        throw std::runtime_error("number is outside the finite float range");
    }
    return static_cast<float>(value);
}

glm::vec3 read_vector(const sol::table& table)
{
    return {read_number(table.raw_get<sol::object>("x")),
        read_number(table.raw_get<sol::object>("y")), read_number(table.raw_get<sol::object>("z"))};
}

sol::table make_vector(sol::state_view lua, const glm::vec3& value)
{
    return lua.create_table_with("x", value.x, "y", value.y, "z", value.z);
}

LightState require_light(const BindingContext& context, NodeHandle node)
{
    const auto light = require_host(context, node, false).light(node);
    if (!light) throw std::runtime_error("node does not contain a light");
    return *light;
}

} // namespace

sol::table make_node_binding(sol::state_view lua, const std::shared_ptr<BindingContext>& context, NodeHandle node)
{
    sol::table binding = lua.create_table();
    binding.set_function("is_valid", [context, node](sol::table) {
        return context->host && context->host->is_valid(node);
    });

    sol::table transform = lua.create_table();
    transform.set_function("get_local_position", [context, node](sol::table, sol::this_state state) {
        const auto position = require_host(*context, node, false).local_position(node);
        if (!position) throw std::runtime_error("cannot read node transform");
        return make_vector(sol::state_view(state), *position);
    });
    transform.set_function("set_local_position", [context, node](sol::table, const sol::table& position) {
        auto& host = require_host(*context, node, true);
        require_success(host.set_local_position(node, read_vector(position)));
    });
    transform.set_function("rotate_local_y_radians", [context, node](sol::table, double angle) {
        require_success(require_host(*context, node, true).rotate_local_y_radians(node, angle));
    });
    binding.raw_set("transform", transform);

    sol::table light = lua.create_table();
    light.set_function("get_intensity", [context, node](sol::table) { return require_light(*context, node).intensity; });
    light.set_function("get_range", [context, node](sol::table) { return require_light(*context, node).range; });
    light.set_function("get_color", [context, node](sol::table, sol::this_state state) {
        return make_vector(sol::state_view(state), require_light(*context, node).color);
    });
    light.set_function("set_intensity", [context, node](sol::table, const sol::object& value) {
        auto& host = require_host(*context, node, true);
        auto updated = require_light(*context, node);
        updated.intensity = read_number(value);
        require_success(host.set_light(node, updated));
    });
    light.set_function("set_range", [context, node](sol::table, const sol::object& value) {
        auto& host = require_host(*context, node, true);
        auto updated = require_light(*context, node);
        updated.range = read_number(value);
        require_success(host.set_light(node, updated));
    });
    light.set_function("set_color", [context, node](sol::table, const sol::table& value) {
        auto& host = require_host(*context, node, true);
        auto updated = require_light(*context, node);
        updated.color = read_vector(value);
        require_success(host.set_light(node, updated));
    });
    binding.raw_set("light", light);
    return binding;
}

void bind_scene(sol::environment& environment, const std::shared_ptr<BindingContext>& context)
{
    if (!context->host) return;
    sol::state_view lua(environment.lua_state());
    sol::table scene = lua.create_table();
    scene.set_function("find", [context](const std::string& name, sol::this_state state) {
        const auto node = context->host->find_node(name);
        return make_node_binding(sol::state_view(state), context, node);
    });
    environment.raw_set("scene", scene);
}

} // namespace scripting::detail
