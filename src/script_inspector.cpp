#include "script_inspector.h"

#include "imgui.h"
#include <algorithm>
#include <cstring>

namespace {

bool edit_property(const scripting::PropertyDesc& desc, scripting::PropertyValue& value)
{
    if (auto* boolean = std::get_if<bool>(&value)) return ImGui::Checkbox(desc.name.c_str(), boolean);
    if (auto* number = std::get_if<double>(&value)) {
        const double step = 0.1;
        return ImGui::DragScalar(desc.name.c_str(), ImGuiDataType_Double, number, static_cast<float>(step),
            desc.range ? &desc.range->min : nullptr, desc.range ? &desc.range->max : nullptr, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    }
    if (auto* vector = std::get_if<glm::vec3>(&value)) return ImGui::DragFloat3(desc.name.c_str(), &vector->x, 0.05f);
    auto& text = std::get<std::string>(value);
    std::vector<char> buffer(text.size() + 256, '\0');
    std::copy(text.begin(), text.end(), buffer.begin());
    if (!ImGui::InputText(desc.name.c_str(), buffer.data(), buffer.size())) return false;
    text = buffer.data();
    return true;
}

const char* status_name(const SceneScripts::AttachmentInfo& info)
{
    if (!info.status) return info.error.empty() ? "Ready" : "Unavailable";
    switch (*info.status) {
    case scripting::InstanceStatus::Running: return "Running";
    case scripting::InstanceStatus::Disabled: return "Disabled";
    case scripting::InstanceStatus::Faulted: return "Faulted";
    }
    return "Unknown";
}

} // namespace

void ScriptInspector::report(const scripting::ScriptResult& result, const char* success)
{
    feedback_ = result ? success : result.error;
}

void ScriptInspector::draw_controls(SceneScripts& scripts)
{
    ImGui::PushID("ScriptTransport");
    ImGui::BeginDisabled(!scripts.has_scene());
    ImGui::BeginDisabled(scripts.mode() == SceneScripts::Mode::Playing);
    if (ImGui::Button(scripts.mode() == SceneScripts::Mode::Paused ? "Resume" : "Play")) report(scripts.play(), "Scripts started");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(scripts.mode() != SceneScripts::Mode::Playing);
    if (ImGui::Button("Pause")) scripts.pause();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!scripts.edits_locked());
    if (ImGui::Button("Stop")) { scripts.stop(); feedback_ = "Stopped; scene properties restored"; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Scripts: %zu", scripts.attachments().size());

    if (ImGui::Button(scripts.dirty() ? "Save scripts *" : "Save scripts")) report(scripts.save(), "Script configuration saved");
    ImGui::SetItemTooltip("%s", scripts.config_path().generic_string().c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(scripts.edits_locked());
    if (ImGui::Button("Load scripts")) report(scripts.load(), "Script configuration loaded");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (!feedback_.empty()) ImGui::TextWrapped("%s", feedback_.c_str());
    if (!scripts.messages().empty() && ImGui::TreeNode("Script messages")) {
        ImGui::BeginChild("Messages", ImVec2(0.f, 135.f), true);
        const auto& messages = scripts.messages();
        const auto first = messages.size() > 12 ? messages.size() - 12 : 0;
        for (size_t i = first; i < messages.size(); ++i) {
            ImGui::TextWrapped("[%s] %s: %s", messages[i].phase.c_str(),
                messages[i].source.filename().string().c_str(), messages[i].text.c_str());
        }
        ImGui::EndChild();
        ImGui::TreePop();
    }
    ImGui::Separator();
    ImGui::PopID();
}

void ScriptInspector::draw_attachments(SceneScripts& scripts, scripting::NodeHandle selected)
{
    if (!scripts.host().is_valid(selected)) return;
    if (!ImGui::CollapsingHeader("Scripts", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::PushID("ScriptAttachments");
    ImGui::BeginDisabled(scripts.edits_locked());
    if (ImGui::BeginCombo("Example", "Choose example script")) {
        for (const char* name : {"move.lua", "pulse_light.lua", "rotate.lua"}) {
            if (ImGui::Selectable(name)) {
                const auto path = (std::filesystem::path(SCRIPTING_EXAMPLES_DIR) / name).generic_string();
                const auto size = std::min(path.size(), source_.size() - 1);
                std::copy_n(path.begin(), size, source_.begin());
                source_[size] = '\0';
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##ScriptPath", "Lua file path", source_.data(), source_.size());
    if (ImGui::Button("Attach script")) {
        const auto result = scripts.attach(selected, source_.data());
        feedback_ = result ? "Script attached" : result.error;
    }
    ImGui::EndDisabled();

    for (const auto& attachment : scripts.attachments(selected)) {
        ImGui::PushID(static_cast<int>(attachment.id));
        ImGui::SeparatorText(attachment.config.source.filename().string().c_str());
        ImGui::TextDisabled("%s", status_name(attachment));
        bool enabled = attachment.config.enabled;
        if (ImGui::Checkbox("Enabled", &enabled)) report(scripts.set_enabled(attachment.id, enabled), "Script enabled state changed");
        ImGui::SameLine();
        if (ImGui::Button("Reload")) report(scripts.reload(attachment.config.source), "All instances of this file reloaded");
        ImGui::SameLine();
        ImGui::BeginDisabled(scripts.edits_locked());
        const bool remove = ImGui::Button("Remove");
        ImGui::EndDisabled();
        if (remove) {
            report(scripts.remove(attachment.id), "Script removed");
            ImGui::PopID();
            break;
        }
        if (!attachment.error.empty()) ImGui::TextWrapped("%s", attachment.error.c_str());
        for (const auto& property : attachment.properties) {
            auto value = attachment.values.at(property.name);
            ImGui::SetNextItemWidth(175.f);
            if (edit_property(property, value)) report(scripts.set_parameter(attachment.id, property.name, value), "Parameter updated");
        }
        ImGui::PopID();
    }
    ImGui::PopID();
}
