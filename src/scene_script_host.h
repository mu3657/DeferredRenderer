#pragma once

#include "scripting/script_host.h"
#include <glm/mat4x4.hpp>
#include <memory>
#include <unordered_map>

struct Node;
struct LoadedScene;

class SceneScriptHost final : public scripting::ScriptHost {
public:
    struct SavedNode {
        scripting::NodeHandle handle;
        glm::mat4 localTransform;
        bool visible;
        std::optional<scripting::LightState> light;
    };
    using Snapshot = std::vector<SavedNode>;

    void set_scene(LoadedScene* scene);
    void sync_nodes();
    void refresh_transforms();
    scripting::NodeHandle handle_for(const std::shared_ptr<Node>& node) const;
    scripting::NodeHandle find_persistent(const std::string& id) const;
    std::string persistent_id(scripting::NodeHandle node) const;
    Snapshot capture() const;
    void restore(const Snapshot& snapshot);

    bool is_valid(scripting::NodeHandle node) const override;
    scripting::NodeHandle find_node(const std::string& name) const override;
    std::optional<glm::vec3> local_position(scripting::NodeHandle node) const override;
    scripting::ScriptResult set_local_position(scripting::NodeHandle node, const glm::vec3& position) override;
    scripting::ScriptResult rotate_local_y_radians(scripting::NodeHandle node, double angle) override;
    std::optional<scripting::LightState> light(scripting::NodeHandle node) const override;
    scripting::ScriptResult set_light(scripting::NodeHandle node, const scripting::LightState& light) override;

private:
    LoadedScene* scene_{}; // scene owner detaches before releasing it
    scripting::NodeHandle nextHandle_{1};
    std::unordered_map<scripting::NodeHandle, std::weak_ptr<Node>> nodes_;
    std::unordered_map<const Node*, scripting::NodeHandle> handles_;
    std::shared_ptr<Node> resolve(scripting::NodeHandle node) const;
};
