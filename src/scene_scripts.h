#pragma once

#include "scene_script_host.h"
#include "scripting/script_config.h"
#include "scripting/script_system.h"

// Coordinates editor configuration and runtime execution. Owns neither scene
// nodes nor GPU resources; all scene access goes through SceneScriptHost.
class SceneScripts {
public:
    using AttachmentId = uint64_t;
    enum class Mode { Stopped, Playing, Paused };

    struct AttachmentInfo {
        AttachmentId id;
        scripting::NodeHandle node;
        scripting::AttachmentConfig config;
        std::vector<scripting::PropertyDesc> properties;
        scripting::PropertyOverrides values;
        scripting::InstanceId instance;
        std::optional<scripting::InstanceStatus> status;
        std::string error;
    };

    SceneScripts();
    ~SceneScripts();
    void set_scene(LoadedScene* scene, const std::filesystem::path& scenePath = {});
    scripting::CreateResult attach(scripting::NodeHandle node, const std::filesystem::path& source);
    scripting::ScriptResult remove(AttachmentId id);
    scripting::ScriptResult set_parameter(AttachmentId id, const std::string& name, const scripting::PropertyValue& value);
    scripting::ScriptResult set_enabled(AttachmentId id, bool enabled);
    scripting::ScriptResult play();
    void pause();
    void stop();
    void update(double deltaTime);
    scripting::ScriptResult reload(const std::filesystem::path& source);
    scripting::ScriptResult save();
    scripting::ScriptResult load();

    Mode mode() const { return mode_; }
    bool edits_locked() const { return mode_ != Mode::Stopped; }
    bool has_scene() const { return scene_ != nullptr; }
    bool dirty() const { return dirty_; }
    SceneScriptHost& host() { return host_; }
    const SceneScriptHost& host() const { return host_; }
    std::vector<AttachmentInfo> attachments(scripting::NodeHandle node = scripting::invalid_node) const;
    const std::vector<scripting::ScriptMessage>& messages() const { return messages_; }
    std::filesystem::path config_path() const { return scripting::script_config_path(scenePath_); }

private:
    struct Attachment {
        AttachmentId id;
        scripting::NodeHandle node;
        scripting::AttachmentConfig config;
        std::vector<scripting::PropertyDesc> properties;
        scripting::InstanceId instance{scripting::invalid_instance};
        std::string error;
    };
    // Keep messages and host alive until runtime and Lua finalizers are gone.
    std::vector<scripting::ScriptMessage> messages_;
    SceneScriptHost host_;
    scripting::ScriptSystem runtime_;
    LoadedScene* scene_{};
    std::filesystem::path scenePath_;
    std::vector<Attachment> attachments_;
    struct CachedConfig {
        std::vector<scripting::AttachmentConfig> attachments;
        bool dirty;
    };
    std::unordered_map<std::string, CachedConfig> cachedConfigs_;
    SceneScriptHost::Snapshot initialState_;
    Mode mode_{Mode::Stopped};
    AttachmentId nextAttachment_{1};
    bool dirty_{false};

    Attachment* find(AttachmentId id);
    void start(Attachment& attachment);
    void receive(const scripting::ScriptMessage& message);
    void warn(const std::filesystem::path& source, const std::string& text);
    void import_configs(const std::vector<scripting::AttachmentConfig>& configs);
    void migrate_parameters(Attachment& attachment);
    std::vector<scripting::AttachmentConfig> configs() const;
};
