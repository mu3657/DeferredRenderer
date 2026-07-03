#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

// Forward declarations
struct Node;
struct LoadedScene;
class  VulkanEngine;

// ─────────────────────────────────────────────────────────────────────────────
// SceneNodeEntry – lightweight proxy for one node in the outliner list.
// The list is kept *flat* (depth-tagged) so ImGui can iterate linearly.
// ─────────────────────────────────────────────────────────────────────────────
enum class SceneNodeType : uint8_t {
    Group  = 0,   // plain Node (no mesh, no light)
    Mesh   = 1,   // MeshNode
    Light  = 2,   // LightNode
};

struct SceneNodeEntry {
    std::string            name;
    std::weak_ptr<Node>    node;
    SceneNodeType          type    { SceneNodeType::Group };
    int                    depth   { 0 };   // tree indent level
    bool                   visible { true };
    bool                   open    { true }; // subtree expanded in UI
};

// ─────────────────────────────────────────────────────────────────────────────
// SceneOutliner – owns the entry list + all editor UI
// ─────────────────────────────────────────────────────────────────────────────
class SceneOutliner {
public:
    // Call once after a scene is loaded (or reloaded)
    void rebuild(const LoadedScene& scene);

    // Call every ImGui frame (inside ImGui::NewFrame / ImGui::Render block)
    // engine is used to read/write camera + node transforms.
    void draw(VulkanEngine& engine);

    // Returns the currently selected node (nullptr if none)
    std::shared_ptr<Node> get_selected_node() const;

    // Index of the single-selected entry (-1 = none)
    int  selected_index { -1 };

    // Which ImGuizmo operation is active (translation=0, rotation=1, scale=2)
    int  gizmo_operation { 0 };

    std::vector<SceneNodeEntry> entries;

private:
    // Sub-panel drawing helpers
    void draw_outliner(VulkanEngine& engine);
    void draw_properties(VulkanEngine& engine);
    void draw_gizmo(VulkanEngine& engine);

    // Recursive builder helper
    void push_node(const std::shared_ptr<Node>& node, int depth);
    void reveal_node(const std::shared_ptr<Node>& node);
};
