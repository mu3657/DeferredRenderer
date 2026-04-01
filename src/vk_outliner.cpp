#include "vk_outliner.h"

#include "vk_engine.h"
#include "vk_loader.h"
#include "vk_types.h"
#include "camera.h"

#include "imgui.h"
#include "ImGuizmo.h"

#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static const char* node_icon(SceneNodeType t) {
    switch (t) {
        case SceneNodeType::Mesh:  return " [M]";
        case SceneNodeType::Light: return " [L]";
        default:                   return " [G]";
    }
}

static SceneNodeType classify(const std::shared_ptr<Node>& n) {
    if (dynamic_cast<MeshNode*>(n.get()))  return SceneNodeType::Mesh;
    if (dynamic_cast<LightNode*>(n.get())) return SceneNodeType::Light;
    return SceneNodeType::Group;
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
    entries.clear();
    selected_index = -1;
    for (const auto& top : scene.topNodes) {
        push_node(top, 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// get_selected_node()
// ─────────────────────────────────────────────────────────────────────────────
std::shared_ptr<Node> SceneOutliner::get_selected_node() const {
    if (selected_index < 0 || selected_index >= (int)entries.size()) return nullptr;
    return entries[selected_index].node.lock();
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
void SceneOutliner::draw_outliner(VulkanEngine& /*engine*/) {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(280, 600), ImGuiCond_Once);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("Outliner", nullptr, flags)) {
        ImGui::End();
        return;
    }

    // Toolbar: gizmo operation selector
    ImGui::TextDisabled("Mode:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Translate (W)", gizmo_operation == 0)) gizmo_operation = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (E)", gizmo_operation == 1))    gizmo_operation = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale (R)", gizmo_operation == 2))     gizmo_operation = 2;

    ImGui::Separator();

    // Search filter
    static char search_buf[128] = {};
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##search", "Search...", search_buf, sizeof(search_buf));
    ImGui::Separator();

    // Node list
    ImGui::BeginChild("##node_list", ImVec2(0, -1), false);

    for (int i = 0; i < (int)entries.size(); ++i) {
        auto& e = entries[i];

        // Filter
        if (search_buf[0] != '\0' &&
            e.name.find(search_buf) == std::string::npos) continue;

        // Indentation
        float indent = e.depth * 14.f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);

        // Visibility toggle (eye icon)
        bool vis = e.visible;
        ImGui::PushID(i * 3);
        if (ImGui::Checkbox("##vis", &vis)) {
            e.visible = vis;
            if (auto n = e.node.lock()) n->visible = vis;
        }
        ImGui::PopID();
        ImGui::SameLine();

        // Selectable row  (icon + name)
        std::string label = std::string(node_icon(e.type)) + " " + e.name;
        bool is_selected = (selected_index == i);

        ImGui::PushID(i * 3 + 1);
        if (ImGui::Selectable(label.c_str(), is_selected,
                              ImGuiSelectableFlags_SpanAllColumns)) {
            selected_index = i;
        }
        ImGui::PopID();

        // ── Drag-and-Drop (reorder flat list) ──
        ImGui::PushID(i * 3 + 2);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("OUTLINER_ENTRY", &i, sizeof(int));
            ImGui::Text("Move: %s", e.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("OUTLINER_ENTRY")) {
                int src = *(const int*)payload->Data;
                if (src != i && src >= 0 && src < (int)entries.size()) {
                    SceneNodeEntry moved = entries[src];
                    entries.erase(entries.begin() + src);
                    int dst = (src < i) ? i - 1 : i;
                    entries.insert(entries.begin() + dst, moved);
                    selected_index = dst;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();

        // ── Right-click context menu ──
        if (ImGui::BeginPopupContextItem(("##ctx" + std::to_string(i)).c_str())) {
            selected_index = i;
            if (ImGui::MenuItem("Focus Camera")) {
                if (auto n = e.node.lock()) {
                    glm::vec3 pos = glm::vec3(n->worldTransform[3]);
                    VulkanEngine::Get().mainCamera.focusOn(pos, 5.f);
                }
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

    ImGui::EndChild();
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
// draw_properties()
// ─────────────────────────────────────────────────────────────────────────────
void SceneOutliner::draw_properties(VulkanEngine& /*engine*/) {
    ImGui::SetNextWindowPos(ImVec2(0, 610), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(280, 300), ImGuiCond_Once);

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



    ImGui::Text("Name: %s", node->name.empty() ? "(unnamed)" : node->name.c_str());
    ImGui::Separator();

    // Decompose worldTransform for display using ImGuizmo
    float matrixTranslation[3], matrixRotation[3], matrixScale[3];
    ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(node->worldTransform), matrixTranslation, matrixRotation, matrixScale);

    bool changed = false;

    ImGui::TextUnformatted("World Transform");
    changed |= ImGui::DragFloat3("Position", matrixTranslation, 0.05f);
    changed |= ImGui::DragFloat3("Rotation", matrixRotation, 0.5f);
    changed |= ImGui::DragFloat3("Scale",    matrixScale,       0.01f, 0.001f, 100.f);

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
        ImGui::Separator();
        ImGui::TextUnformatted("Light");
        ImGui::ColorEdit3("Color",     glm::value_ptr(ln->light.color));
        ImGui::DragFloat("Intensity", &ln->light.intensity, 0.1f, 0.f, 1000.f);
        ImGui::DragFloat("Range",     &ln->light.range,     0.5f, 0.f, 10000.f);
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
