#include "vk_outliner.h"

#include "vk_engine.h"
#include "vk_loader.h"
#include "vk_types.h"
#include "camera.h"

#include "imgui.h"
#include "ImGuizmo.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static const char* node_icon(SceneNodeType t) {
    switch (t) {
        case SceneNodeType::Mesh:  return "[M]";
        case SceneNodeType::Light: return "[L]";
        default:                   return "[G]";
    }
}

static const char* light_type_name(LightType type) {
    switch (type) {
        case LightType::Directional: return "Directional";
        case LightType::Spot:        return "Spot";
        case LightType::RectArea:    return "Rect Area";
        default:                     return "Point";
    }
}

static SceneNodeType classify(const std::shared_ptr<Node>& n) {
    if (dynamic_cast<MeshNode*>(n.get()))  return SceneNodeType::Mesh;
    if (dynamic_cast<LightNode*>(n.get())) return SceneNodeType::Light;
    return SceneNodeType::Group;
}

static LoadedScene* active_scene(VulkanEngine& engine) {
    if (engine.activeSceneName.empty()) {
        return nullptr;
    }

    auto it = engine.loadedScenes.find(engine.activeSceneName);
    if (it == engine.loadedScenes.end()) {
        return nullptr;
    }

    return it->second.get();
}

static char lower_ascii(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

static bool name_matches_filter(const std::string& name, const char* filter) {
    if (!filter || filter[0] == '\0') {
        return true;
    }

    const std::string needle(filter);
    auto it = std::search(
        name.begin(), name.end(),
        needle.begin(), needle.end(),
        [](char a, char b) { return lower_ascii(a) == lower_ascii(b); });
    return it != name.end();
}

static bool has_children(const std::vector<SceneNodeEntry>& entries, int index) {
    return index + 1 < static_cast<int>(entries.size()) &&
           entries[index + 1].depth > entries[index].depth;
}

static bool has_collapsed_ancestor(const std::vector<SceneNodeEntry>& entries, int index) {
    int depth = entries[index].depth;
    for (int i = index - 1; i >= 0 && depth > 0; --i) {
        if (entries[i].depth < depth) {
            if (!entries[i].open) {
                return true;
            }
            depth = entries[i].depth;
        }
    }

    return false;
}

static bool subtree_matches_filter(const std::vector<SceneNodeEntry>& entries, int index, const char* filter) {
    if (!filter || filter[0] == '\0') {
        return true;
    }

    const int rootDepth = entries[index].depth;
    for (int i = index; i < static_cast<int>(entries.size()); ++i) {
        if (i != index && entries[i].depth <= rootDepth) {
            break;
        }
        if (name_matches_filter(entries[i].name, filter)) {
            return true;
        }
    }

    return false;
}

static std::string unique_node_name(const LoadedScene& scene, const std::string& base) {
    if (scene.nodes.find(base) == scene.nodes.end()) {
        return base;
    }

    for (int i = 1; i < 10000; ++i) {
        std::string candidate = base + " " + std::to_string(i);
        if (scene.nodes.find(candidate) == scene.nodes.end()) {
            return candidate;
        }
    }

    return base + " " + std::to_string(static_cast<int>(scene.nodes.size()));
}

static glm::mat4 spawn_transform(VulkanEngine& engine, const std::shared_ptr<Node>& parent) {
    if (parent) {
        return glm::mat4(1.f);
    }

    return glm::translate(glm::mat4(1.f), engine.mainCamera.pivot);
}

static void attach_node(LoadedScene& scene, const std::shared_ptr<Node>& node, const std::shared_ptr<Node>& parent) {
    if (parent) {
        parent->children.push_back(node);
        node->parent = parent;
        node->refreshTransform(parent->worldTransform);
    } else {
        scene.topNodes.push_back(node);
        node->refreshTransform(glm::mat4(1.f));
    }

    scene.nodes[node->name] = node;
}

static std::shared_ptr<Node> create_empty_node(VulkanEngine& engine, LoadedScene& scene, const std::shared_ptr<Node>& parent) {
    auto node = std::make_shared<Node>();
    node->name = unique_node_name(scene, "Empty Node");
    node->visible = true;
    node->localTransform = spawn_transform(engine, parent);
    node->worldTransform = glm::mat4(1.f);

    attach_node(scene, node, parent);
    return node;
}

static std::shared_ptr<Node> create_light_node(
    VulkanEngine& engine,
    LoadedScene& scene,
    const std::shared_ptr<Node>& parent,
    LightType type)
{
    auto node = std::make_shared<LightNode>();
    node->name = unique_node_name(scene, std::string(light_type_name(type)) + " Light");
    node->visible = true;
    node->localTransform = spawn_transform(engine, parent);
    node->worldTransform = glm::mat4(1.f);
    node->light.type = type;
    node->light.color = glm::vec3(1.f, 0.96f, 0.86f);
    switch (type) {
        case LightType::Directional:
            node->light.intensity = 2.5f;
            break;
        case LightType::Point:
            node->light.intensity = 0.f;
            break;
        case LightType::RectArea:
            node->light.intensity = 40.f;
            break;
        case LightType::Spot:
            node->light.intensity = 80.f;
            break;
    }
    node->light.range = (type == LightType::Directional) ? 0.f : 12.f;
    node->light.width = (type == LightType::RectArea) ? 2.f : 1.f;
    node->light.height = 1.f;
    node->light.worldPosition = glm::vec3(0.f);

    attach_node(scene, node, parent);
    scene.lightNodes.push_back(node);
    return node;
}

static void draw_create_menu_items(
    VulkanEngine& engine,
    LoadedScene& scene,
    const std::shared_ptr<Node>& parent,
    std::shared_ptr<Node>& createdNode)
{
    if (parent) {
        ImGui::TextDisabled("Parent: %s", parent->name.empty() ? "(unnamed)" : parent->name.c_str());
    } else {
        ImGui::TextDisabled("Parent: Scene Root");
    }
    ImGui::Separator();

    if (ImGui::MenuItem("Empty Node")) {
        createdNode = create_empty_node(engine, scene, parent);
    }

    if (ImGui::BeginMenu("Light")) {
        if (ImGui::MenuItem("Point Light")) {
            createdNode = create_light_node(engine, scene, parent, LightType::Point);
        }
        if (ImGui::MenuItem("Directional Light")) {
            createdNode = create_light_node(engine, scene, parent, LightType::Directional);
        }
        if (ImGui::MenuItem("Spot Light")) {
            createdNode = create_light_node(engine, scene, parent, LightType::Spot);
        }
        if (ImGui::MenuItem("Rect Area Light")) {
            createdNode = create_light_node(engine, scene, parent, LightType::RectArea);
        }
        ImGui::EndMenu();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// rebuild()
// ─────────────────────────────────────────────────────────────────────────────
void SceneOutliner::push_node(const std::shared_ptr<Node>& node, int depth) {
    if (!node) return;

    SceneNodeEntry entry;
    entry.name    = node->name.empty() ? "(unnamed)" : node->name;
    entry.node    = node;
    entry.type    = classify(node);
    entry.depth   = depth;
    entry.visible = node->visible;
    entries.push_back(entry);

    for (const auto& child : node->children) {
        push_node(child, depth + 1);
    }
}

void SceneOutliner::rebuild(const LoadedScene& scene) {
    std::unordered_map<Node*, bool> previousOpenState;
    previousOpenState.reserve(entries.size());
    std::shared_ptr<Node> selectedNode = get_selected_node();

    for (const SceneNodeEntry& entry : entries) {
        if (auto node = entry.node.lock()) {
            previousOpenState[node.get()] = entry.open;
        }
    }

    entries.clear();
    selected_index = -1;
    for (const auto& top : scene.topNodes) {
        push_node(top, 0);
    }

    for (SceneNodeEntry& entry : entries) {
        if (auto node = entry.node.lock()) {
            auto it = previousOpenState.find(node.get());
            if (it != previousOpenState.end()) {
                entry.open = it->second;
            }
        }
    }

    reveal_node(selectedNode);
}

// ─────────────────────────────────────────────────────────────────────────────
// get_selected_node()
// ─────────────────────────────────────────────────────────────────────────────
std::shared_ptr<Node> SceneOutliner::get_selected_node() const {
    if (selected_index < 0 || selected_index >= (int)entries.size()) return nullptr;
    return entries[selected_index].node.lock();
}

void SceneOutliner::reveal_node(const std::shared_ptr<Node>& node) {
    selected_index = -1;
    if (!node) {
        return;
    }

    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (entries[i].node.lock() != node) {
            continue;
        }

        selected_index = i;

        int depth = entries[i].depth;
        for (int j = i - 1; j >= 0 && depth > 0; --j) {
            if (entries[j].depth < depth) {
                entries[j].open = true;
                depth = entries[j].depth;
            }
        }
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// draw() – main entry called each frame
// ─────────────────────────────────────────────────────────────────────────────
void SceneOutliner::draw(VulkanEngine& engine) {
    // ImGuizmo must be told about the viewport each frame
    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);

    // Tell ImGuizmo to draw over the full OS window (not just an ImGui child)
    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());

    draw_outliner(engine);
    draw_properties(engine);
    draw_gizmo(engine);
}

// ─────────────────────────────────────────────────────────────────────────────
// draw_outliner()
// ─────────────────────────────────────────────────────────────────────────────
void SceneOutliner::draw_outliner(VulkanEngine& engine) {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(330, 600), ImGuiCond_Once);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("Outliner", nullptr, flags)) {
        ImGui::End();
        return;
    }

    LoadedScene* scene = active_scene(engine);
    std::shared_ptr<Node> createdNode;
    std::shared_ptr<Node> selectedNode = get_selected_node();

    ImGui::BeginDisabled(scene == nullptr);
    if (ImGui::Button("+ Add")) {
        ImGui::OpenPopup("##add_node_popup");
    }
    if (scene && ImGui::BeginPopup("##add_node_popup")) {
        draw_create_menu_items(engine, *scene, selectedNode, createdNode);
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    if (scene == nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("Load a scene before adding nodes.");
    }

    ImGui::SameLine();
    if (ImGui::RadioButton("Move", gizmo_operation == 0)) gizmo_operation = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate", gizmo_operation == 1)) gizmo_operation = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale", gizmo_operation == 2)) gizmo_operation = 2;

    ImGui::Separator();

    // Search filter
    static char search_buf[128] = {};
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##search", "Search...", search_buf, sizeof(search_buf));
    ImGui::Separator();

    const bool searching = (search_buf[0] != '\0');

    ImGui::BeginChild("##node_list", ImVec2(0, -1), true);

    for (int i = 0; i < (int)entries.size(); ++i) {
        auto& e = entries[i];

        if (searching) {
            if (!subtree_matches_filter(entries, i, search_buf)) {
                continue;
            }
        } else if (has_collapsed_ancestor(entries, i)) {
            continue;
        }

        const bool entryHasChildren = has_children(entries, i);
        const bool is_selected = (selected_index == i);
        const float indent = e.depth * 18.f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);

        bool vis = e.visible;
        ImGui::PushID(i * 4);
        if (ImGui::Checkbox("##vis", &vis)) {
            e.visible = vis;
            if (auto n = e.node.lock()) n->visible = vis;
        }
        ImGui::SetItemTooltip("Visible");
        ImGui::PopID();
        ImGui::SameLine();

        std::string label = std::string(node_icon(e.type)) + " " + e.name;
        ImGuiTreeNodeFlags treeFlags =
            ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_SpanFullWidth |
            ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (is_selected) {
            treeFlags |= ImGuiTreeNodeFlags_Selected;
        }
        if (!entryHasChildren) {
            treeFlags |= ImGuiTreeNodeFlags_Leaf;
        }

        if (searching && entryHasChildren) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        } else if (entryHasChildren) {
            ImGui::SetNextItemOpen(e.open, ImGuiCond_Always);
        }

        ImGui::PushID(i * 4 + 1);
        const bool open = ImGui::TreeNodeEx("##node", treeFlags, "%s", label.c_str());
        if (!searching && entryHasChildren) {
            e.open = open;
        }
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            selected_index = i;
        }
        ImGui::PopID();

        if (ImGui::BeginPopupContextItem(("##ctx" + std::to_string(i)).c_str())) {
            selected_index = i;
            if (ImGui::MenuItem("Focus Camera")) {
                if (auto n = e.node.lock()) {
                    glm::vec3 pos = glm::vec3(n->worldTransform[3]);
                    VulkanEngine::Get().mainCamera.focusOn(pos, 5.f);
                }
            }
            ImGui::Separator();
            if (scene) {
                std::shared_ptr<Node> parent = e.node.lock();
                draw_create_menu_items(engine, *scene, parent, createdNode);
                ImGui::Separator();
            }
            if (ImGui::MenuItem(e.visible ? "Hide" : "Show")) {
                e.visible = !e.visible;
                if (auto n = e.node.lock()) n->visible = e.visible;
            }
            if (ImGui::MenuItem("Isolate")) {
                for (int j = 0; j < (int)entries.size(); ++j) {
                    entries[j].visible = (j == i);
                    if (auto n = entries[j].node.lock()) n->visible = entries[j].visible;
                }
            }
            ImGui::EndPopup();
        }
    }

    if (scene && ImGui::BeginPopupContextWindow(
                     "##outliner_background_context",
                     ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        draw_create_menu_items(engine, *scene, nullptr, createdNode);
        ImGui::EndPopup();
    }

    ImGui::EndChild();

    if (scene && createdNode) {
        rebuild(*scene);
        reveal_node(createdNode);
    }

    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
// draw_properties()
// ─────────────────────────────────────────────────────────────────────────────
void SceneOutliner::draw_properties(VulkanEngine& /*engine*/) {
    ImGui::SetNextWindowPos(ImVec2(0, 610), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(330, 300), ImGuiCond_Once);

    if (!ImGui::Begin("Properties", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    auto node = get_selected_node();
    if (!node) {
        ImGui::TextDisabled("(no selection)");
        ImGui::End();
        return;
    }


    ImGui::TextUnformatted(node->name.empty() ? "(unnamed)" : node->name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", node_icon(classify(node)));

    // Decompose worldTransform for display using ImGuizmo
    float matrixTranslation[3], matrixRotation[3], matrixScale[3];
    ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(node->worldTransform), matrixTranslation, matrixRotation, matrixScale);

    bool changed = false;

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::DragFloat3("Position", matrixTranslation, 0.05f);
        changed |= ImGui::DragFloat3("Rotation", matrixRotation, 0.5f);
        changed |= ImGui::DragFloat3("Scale",    matrixScale,       0.01f, 0.001f, 100.f);
    }

    if (changed) {
        // Recompose desired world matrix from edited TRS
        glm::mat4 newWorld;
        ImGuizmo::RecomposeMatrixFromComponents(matrixTranslation, matrixRotation, matrixScale, glm::value_ptr(newWorld));

        // Back-compute localTransform relative to parent's world
        glm::mat4 parentWorld = glm::mat4(1.f);
        if (auto p = node->parent.lock()) parentWorld = p->worldTransform;
        node->localTransform = glm::inverse(parentWorld) * newWorld;

        // Propagate to children
        node->refreshTransform(parentWorld);
    }

    // If it's a light, show light-specific properties
    if (auto* ln = dynamic_cast<LightNode*>(node.get())) {
        if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginCombo("Type", light_type_name(ln->light.type))) {
                const LightType types[] = {
                    LightType::Point,
                    LightType::Directional,
                    LightType::Spot,
                    LightType::RectArea,
                };
                for (LightType type : types) {
                    const bool selected = (ln->light.type == type);
                    if (ImGui::Selectable(light_type_name(type), selected)) {
                        ln->light.type = type;
                        if (type == LightType::Directional) {
                            ln->light.range = 0.f;
                        } else if (ln->light.range <= 0.f) {
                            ln->light.range = 12.f;
                        }
                        if (type == LightType::RectArea) {
                            ln->light.width = std::max(ln->light.width, 1.f);
                            ln->light.height = std::max(ln->light.height, 1.f);
                        }
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::ColorEdit3("Color", glm::value_ptr(ln->light.color));
            ImGui::DragFloat("Intensity", &ln->light.intensity, 0.1f, 0.f, 1000.f);

            ImGui::BeginDisabled(ln->light.type == LightType::Directional);
            ImGui::DragFloat("Range", &ln->light.range, 0.5f, 0.f, 10000.f);
            ImGui::EndDisabled();

            if (ln->light.type == LightType::RectArea) {
                ImGui::DragFloat("Width", &ln->light.width, 0.05f, 0.01f, 10000.f);
                ImGui::DragFloat("Height", &ln->light.height, 0.05f, 0.01f, 10000.f);
            }
        }
    }

    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
// draw_gizmo() – draws the ImGuizmo manipulator over the 3D viewport
// ─────────────────────────────────────────────────────────────────────────────
void SceneOutliner::draw_gizmo(VulkanEngine& engine) {
    auto node = get_selected_node();
    if (!node) return;

    // View matrix – identical to the one used in update_scene()
    glm::mat4 view = engine.mainCamera.getViewMatrix();

    float aspect = (float)engine._windowExtent.width / (float)engine._windowExtent.height;

    // ImGuizmo renders via ImGui's 2D drawlist (not Vulkan clip space),
    // so it needs a standard OpenGL-convention projection:
    //   - standard near/far (NOT reversed-Z)
    //   - NO Vulkan Y-flip
    glm::mat4 proj = glm::perspective(glm::radians(70.f), aspect, 0.1f, 10000.f);


    // Map gizmo operation
    ImGuizmo::OPERATION op;
    switch (gizmo_operation) {
        case 1:  op = ImGuizmo::ROTATE;    break;
        case 2:  op = ImGuizmo::SCALE;     break;
        default: op = ImGuizmo::TRANSLATE; break;
    }

    glm::mat4 worldMat = node->worldTransform;

    ImGuizmo::Manipulate(
        glm::value_ptr(view),
        glm::value_ptr(proj),
        op,
        ImGuizmo::WORLD,
        glm::value_ptr(worldMat));

    if (ImGuizmo::IsUsing()) {
        // Convert world-space result back to local space
        glm::mat4 parentWorld = glm::mat4(1.f);
        if (auto p = node->parent.lock()) parentWorld = p->worldTransform;

        node->localTransform = glm::inverse(parentWorld) * worldMat;
        node->refreshTransform(parentWorld);
    }
}
