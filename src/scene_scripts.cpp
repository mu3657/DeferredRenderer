#include "scene_scripts.h"

#include <algorithm>
#include <iostream>

namespace {

std::filesystem::path normalized(const std::filesystem::path& path)
{
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) return path.lexically_normal();
    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute.lexically_normal() : canonical;
}

} // namespace

SceneScripts::SceneScripts() : runtime_([this](const auto& message) { receive(message); }, &host_) {}
SceneScripts::~SceneScripts() { stop(); }

void SceneScripts::receive(const scripting::ScriptMessage& message)
{
    if (messages_.size() == 128) messages_.erase(messages_.begin());
    messages_.push_back(message);
    std::clog << "[script " << message.instance << "][" << message.phase << "] "
              << message.source.generic_string() << ": " << message.text << '\n';
}

void SceneScripts::warn(const std::filesystem::path& source, const std::string& text)
{
    receive({scripting::LogLevel::Error, scripting::invalid_instance, source, "configuration", text});
}

std::vector<scripting::AttachmentConfig> SceneScripts::configs() const
{
    std::vector<scripting::AttachmentConfig> result;
    for (const auto& attachment : attachments_) result.push_back(attachment.config);
    return result;
}

void SceneScripts::set_scene(LoadedScene* scene, const std::filesystem::path& scenePath)
{
    stop();
    if (scene_) cachedConfigs_[scenePath_.generic_string()] = {configs(), dirty_};
    attachments_.clear();
    scene_ = scene;
    scenePath_ = scenePath.empty() ? std::filesystem::path{} : normalized(scenePath);
    host_.set_scene(scene);
    dirty_ = false;
    if (!scene_) return;
    const auto cached = cachedConfigs_.find(scenePath_.generic_string());
    if (cached != cachedConfigs_.end()) {
        import_configs(cached->second.attachments);
        dirty_ = dirty_ || cached->second.dirty;
    } else if (!scenePath_.empty()) {
        std::error_code error;
        if (std::filesystem::exists(config_path(), error)) {
            const auto result = load();
            if (!result) warn(config_path(), result.error);
        }
    }
    host_.refresh_transforms();
}

SceneScripts::Attachment* SceneScripts::find(AttachmentId id)
{
    const auto found = std::find_if(attachments_.begin(), attachments_.end(), [id](const auto& item) { return item.id == id; });
    return found == attachments_.end() ? nullptr : &*found;
}

scripting::CreateResult SceneScripts::attach(scripting::NodeHandle node, const std::filesystem::path& source)
{
    if (edits_locked()) return {0, "stop playback before changing script attachments"};
    host_.sync_nodes();
    if (!host_.is_valid(node)) return {0, "select a node in the active scene"};
    const auto path = normalized(source);
    const auto description = runtime_.describe(path, node);
    if (!description) return {0, description.error};
    const auto nodeId = host_.persistent_id(node);
    if (host_.find_persistent(nodeId) != node) return {0, "node persistent ID is ambiguous"};
    const auto id = nextAttachment_++;
    attachments_.push_back({id, node, {nodeId, path, true, {}}, description.properties});
    dirty_ = true;
    return {id, {}};
}

scripting::ScriptResult SceneScripts::remove(AttachmentId id)
{
    if (edits_locked()) return {false, "stop playback before removing a script"};
    const auto count = std::erase_if(attachments_, [id](const auto& item) { return item.id == id; });
    if (!count) return {false, "unknown script attachment"};
    dirty_ = true;
    return {true, {}};
}

scripting::ScriptResult SceneScripts::set_parameter(AttachmentId id, const std::string& name,
    const scripting::PropertyValue& value)
{
    auto* attachment = find(id);
    if (!attachment) return {false, "unknown script attachment"};
    const auto found = std::find_if(attachment->properties.begin(), attachment->properties.end(),
        [&](const auto& property) { return property.name == name; });
    if (found == attachment->properties.end()) return {false, "unknown script property"};
    const auto valid = scripting::ScriptSystem::validate_property(*found, value);
    if (!valid) return valid;
    if (attachment->instance != scripting::invalid_instance) {
        const auto changed = runtime_.set_property(attachment->instance, name, value);
        if (!changed) return changed;
    }
    attachment->config.overrides[name] = value;
    dirty_ = true;
    return {true, {}};
}

void SceneScripts::start(Attachment& attachment)
{
    attachment.error.clear();
    if (!host_.is_valid(attachment.node)) {
        attachment.error = "owner node is missing or ambiguous";
        return;
    }
    const auto created = runtime_.create_instance(attachment.config.source, attachment.config.overrides, attachment.node);
    attachment.instance = created.id;
    attachment.error = created.error;
    if (const auto info = runtime_.inspect(created.id)) attachment.properties = info->properties;
}

scripting::ScriptResult SceneScripts::set_enabled(AttachmentId id, bool enabled)
{
    auto* attachment = find(id);
    if (!attachment) return {false, "unknown script attachment"};
    if (attachment->instance != scripting::invalid_instance) {
        const auto info = runtime_.inspect(attachment->instance);
        if (info && info->status == scripting::InstanceStatus::Faulted) {
            if (enabled) return {false, "reload the faulted script before enabling it"};
        } else {
            const auto result = runtime_.set_enabled(attachment->instance, enabled);
            if (!result) return result;
        }
    }
    attachment->config.enabled = enabled;
    if (enabled && edits_locked() && attachment->instance == scripting::invalid_instance) start(*attachment);
    dirty_ = true;
    return {attachment->error.empty(), attachment->error};
}

scripting::ScriptResult SceneScripts::play()
{
    if (!scene_) return {false, "load a scene before starting scripts"};
    if (mode_ == Mode::Paused) { mode_ = Mode::Playing; return {true, {}}; }
    if (mode_ == Mode::Playing) return {true, {}};
    host_.sync_nodes();
    initialState_ = host_.capture();
    mode_ = Mode::Playing;
    std::string firstError;
    for (auto& attachment : attachments_) {
        if (!attachment.config.enabled) continue;
        start(attachment);
        if (!attachment.error.empty() && firstError.empty()) firstError = attachment.error;
    }
    host_.refresh_transforms();
    return {firstError.empty(), firstError};
}

void SceneScripts::pause() { if (mode_ == Mode::Playing) mode_ = Mode::Paused; }

void SceneScripts::stop()
{
    runtime_.clear();
    if (mode_ != Mode::Stopped) host_.restore(initialState_);
    initialState_.clear();
    for (auto& attachment : attachments_) attachment.instance = scripting::invalid_instance;
    mode_ = Mode::Stopped;
}

void SceneScripts::update(double deltaTime)
{
    host_.sync_nodes();
    for (auto& attachment : attachments_) {
        if (attachment.instance != scripting::invalid_instance && !host_.is_valid(attachment.node)) {
            runtime_.destroy_instance(attachment.instance);
            attachment.instance = scripting::invalid_instance;
            attachment.error = "owner node was removed from the active scene";
        }
    }
    if (mode_ == Mode::Playing) runtime_.update(deltaTime);
    host_.refresh_transforms();
}

void SceneScripts::migrate_parameters(Attachment& attachment)
{
    const auto removed = std::erase_if(attachment.config.overrides, [&](const auto& entry) {
        const auto property = std::find_if(attachment.properties.begin(), attachment.properties.end(),
            [&](const auto& desc) { return desc.name == entry.first; });
        if (property != attachment.properties.end() && scripting::ScriptSystem::validate_property(*property, entry.second)) return false;
        warn(attachment.config.source, "reset missing or changed property '" + entry.first + "' to its default");
        return true;
    });
    if (removed) dirty_ = true;
}

scripting::ScriptResult SceneScripts::reload(const std::filesystem::path& source)
{
    const auto path = normalized(source);
    const auto description = runtime_.describe(path);
    if (!description) {
        for (auto& attachment : attachments_) if (attachment.config.source == path) attachment.error = description.error;
        return {false, description.error};
    }
    const auto result = runtime_.reload_file(path);
    for (auto& attachment : attachments_) {
        if (attachment.config.source != path) continue;
        const auto current = runtime_.inspect(attachment.instance);
        attachment.properties = current ? current->properties : description.properties;
        migrate_parameters(attachment);
        attachment.error = result.error;
        if (edits_locked() && attachment.config.enabled && attachment.instance == scripting::invalid_instance) start(attachment);
    }
    host_.refresh_transforms();
    dirty_ = true;
    return result;
}

void SceneScripts::import_configs(const std::vector<scripting::AttachmentConfig>& configs)
{
    for (const auto& config : configs) {
        Attachment attachment{nextAttachment_++, host_.find_persistent(config.nodeId), config, {}};
        const auto description = runtime_.describe(config.source, attachment.node);
        attachment.properties = description.properties;
        attachment.error = description.error;
        if (description) migrate_parameters(attachment);
        if (!host_.is_valid(attachment.node)) attachment.error = "owner node is missing or its persistent ID is ambiguous: " + config.nodeId;
        if (!attachment.error.empty()) warn(config.source, attachment.error);
        attachments_.push_back(std::move(attachment));
    }
}

scripting::ScriptResult SceneScripts::save()
{
    if (!scene_ || scenePath_.empty()) return {false, "active scene has no configuration path"};
    const auto result = scripting::save_script_config(config_path(), configs());
    if (result) dirty_ = false;
    return result;
}

scripting::ScriptResult SceneScripts::load()
{
    if (!scene_ || scenePath_.empty()) return {false, "active scene has no configuration path"};
    const auto loaded = scripting::load_script_config(config_path());
    if (!loaded) return {false, loaded.error}; // preserve playback/configuration on malformed JSON
    stop();
    attachments_.clear();
    dirty_ = false;
    import_configs(loaded.attachments);
    for (const auto& warning : loaded.warnings) warn(config_path(), warning);
    return {true, {}};
}

std::vector<SceneScripts::AttachmentInfo> SceneScripts::attachments(scripting::NodeHandle node) const
{
    std::vector<AttachmentInfo> result;
    for (const auto& attachment : attachments_) {
        if (node != scripting::invalid_node && attachment.node != node) continue;
        AttachmentInfo info{attachment.id, attachment.node, attachment.config, attachment.properties, {},
            attachment.instance, std::nullopt, attachment.error};
        for (const auto& property : attachment.properties) {
            const auto configured = attachment.config.overrides.find(property.name);
            info.values[property.name] = configured == attachment.config.overrides.end() ? property.defaultValue : configured->second;
            if (const auto runtimeValue = runtime_.get_property(attachment.instance, property.name)) info.values[property.name] = *runtimeValue;
        }
        if (const auto runtime = runtime_.inspect(attachment.instance)) {
            info.status = runtime->status;
            if (info.error.empty()) info.error = runtime->lastError;
        }
        result.push_back(std::move(info));
    }
    return result;
}
