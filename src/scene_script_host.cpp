#include "scene_script_host.h"
#include "vk_loader.h"

#include <cmath>
#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_set>
#include <glm/gtc/matrix_transform.hpp>

namespace {

bool finite(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::string runtime_id()
{
    static std::mt19937_64 random(std::random_device{}());
    std::ostringstream text;
    text << "runtime/" << std::hex << std::setfill('0') << std::setw(16) << random() << std::setw(16) << random();
    return text.str();
}

} // namespace

void SceneScriptHost::set_scene(LoadedScene* scene)
{
    nodes_.clear();
    handles_.clear();
    scene_ = scene;
    // Do not reset nextHandle_: an old Lua handle must never alias a new scene.
    sync_nodes();
}

void SceneScriptHost::sync_nodes()
{
    std::unordered_set<const Node*> reachable;
    std::function<void(const std::shared_ptr<Node>&)> visit = [&](const auto& node) {
        if (!node || !reachable.insert(node.get()).second) return;
        if (node->persistentID.empty()) node->persistentID = runtime_id();
        const auto found = handles_.find(node.get());
        // A stale pointer may have been reused after a node expired.
        if (found == handles_.end() || nodes_.at(found->second).lock() != node) {
            const auto handle = nextHandle_++;
            nodes_[handle] = node;
            handles_[node.get()] = handle;
        }
        for (const auto& child : node->children) visit(child);
    };
    if (scene_) for (const auto& root : scene_->topNodes) visit(root);
    std::erase_if(nodes_, [&](const auto& entry) {
        const auto node = entry.second.lock();
        return !node || !reachable.contains(node.get()) || handles_.at(node.get()) != entry.first;
    });
    std::erase_if(handles_, [&](const auto& entry) { return !reachable.contains(entry.first); });
}

std::shared_ptr<Node> SceneScriptHost::resolve(scripting::NodeHandle node) const
{
    const auto found = nodes_.find(node);
    if (found == nodes_.end()) return nullptr;
    return found->second.lock();
}

bool SceneScriptHost::is_valid(scripting::NodeHandle node) const { return bool(resolve(node)); }

scripting::NodeHandle SceneScriptHost::handle_for(const std::shared_ptr<Node>& node) const
{
    const auto found = handles_.find(node.get());
    if (found == handles_.end() || resolve(found->second) != node) return scripting::invalid_node;
    return found->second;
}

scripting::NodeHandle SceneScriptHost::find_node(const std::string& name) const
{
    scripting::NodeHandle result = scripting::invalid_node;
    for (const auto& [handle, weak] : nodes_) {
        if (const auto node = weak.lock(); node && node->name == name) {
            if (result != scripting::invalid_node) return scripting::invalid_node; // ambiguous name
            result = handle;
        }
    }
    return result;
}

scripting::NodeHandle SceneScriptHost::find_persistent(const std::string& id) const
{
    scripting::NodeHandle result = scripting::invalid_node;
    for (const auto& [handle, weak] : nodes_) {
        if (const auto node = weak.lock(); node && node->persistentID == id) {
            if (result != scripting::invalid_node) return scripting::invalid_node;
            result = handle;
        }
    }
    return result;
}

std::string SceneScriptHost::persistent_id(scripting::NodeHandle handle) const
{
    const auto node = resolve(handle);
    return node ? node->persistentID : std::string{};
}

std::optional<glm::vec3> SceneScriptHost::local_position(scripting::NodeHandle handle) const
{
    const auto node = resolve(handle);
    if (!node) return std::nullopt;
    return glm::vec3(node->localTransform[3]);
}

scripting::ScriptResult SceneScriptHost::set_local_position(scripting::NodeHandle handle, const glm::vec3& position)
{
    const auto node = resolve(handle);
    if (!node) return {false, "node handle is no longer valid"};
    if (!finite(position)) return {false, "position must be finite"};
    node->localTransform[3] = glm::vec4(position, node->localTransform[3].w);
    return {true, {}};
}

scripting::ScriptResult SceneScriptHost::rotate_local_y_radians(scripting::NodeHandle handle, double angle)
{
    const auto node = resolve(handle);
    if (!node) return {false, "node handle is no longer valid"};
    if (!std::isfinite(angle)) return {false, "rotation angle must be finite"};
    constexpr double fullTurn = 6.283185307179586;
    const auto rotated = glm::rotate(node->localTransform, static_cast<float>(std::remainder(angle, fullTurn)), glm::vec3(0, 1, 0));
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(rotated[column][row])) return {false, "rotation produced a non-finite transform"};
        }
    }
    node->localTransform = rotated;
    return {true, {}};
}

std::optional<scripting::LightState> SceneScriptHost::light(scripting::NodeHandle handle) const
{
    const auto node = std::dynamic_pointer_cast<LightNode>(resolve(handle));
    if (!node) return std::nullopt;
    return scripting::LightState{node->light.color, node->light.intensity, node->light.range};
}

scripting::ScriptResult SceneScriptHost::set_light(scripting::NodeHandle handle, const scripting::LightState& value)
{
    const auto node = std::dynamic_pointer_cast<LightNode>(resolve(handle));
    if (!node) return {false, "node is unavailable or is not a light"};
    if (!finite(value.color) || !std::isfinite(value.intensity) || !std::isfinite(value.range)
        || value.color.x < 0 || value.color.y < 0 || value.color.z < 0 || value.intensity < 0 || value.range < 0) {
        return {false, "light color, intensity and range must be finite and nonnegative"};
    }
    node->light.color = value.color;
    node->light.intensity = value.intensity;
    node->light.range = value.range;
    return {true, {}};
}

void SceneScriptHost::refresh_transforms()
{
    if (scene_) for (const auto& root : scene_->topNodes) root->refreshTransform(glm::mat4(1.f));
}

SceneScriptHost::Snapshot SceneScriptHost::capture() const
{
    Snapshot snapshot;
    for (const auto& [handle, weak] : nodes_) {
        if (const auto node = weak.lock()) snapshot.push_back({handle, node->localTransform, node->visible, light(handle)});
    }
    return snapshot;
}

void SceneScriptHost::restore(const Snapshot& snapshot)
{
    for (const auto& saved : snapshot) {
        if (const auto node = resolve(saved.handle)) {
            node->localTransform = saved.localTransform;
            node->visible = saved.visible;
            if (const auto lightNode = std::dynamic_pointer_cast<LightNode>(node); lightNode && saved.light) {
                lightNode->light.color = saved.light->color;
                lightNode->light.intensity = saved.light->intensity;
                lightNode->light.range = saved.light->range;
            }
        }
    }
    refresh_transforms();
}
