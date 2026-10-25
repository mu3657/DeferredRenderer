#pragma once

#include "script_types.h"

namespace scripting {

struct AttachmentConfig {
    std::string nodeId;
    std::filesystem::path source; // absolute in memory, relative to the sidecar on disk
    bool enabled{true};
    PropertyOverrides overrides;
};

struct ConfigLoadResult {
    bool success{false};
    std::vector<AttachmentConfig> attachments;
    std::vector<std::string> warnings;
    std::string error;
    explicit operator bool() const { return success; }
};

std::filesystem::path script_config_path(const std::filesystem::path& scenePath);
ConfigLoadResult load_script_config(const std::filesystem::path& path);
ScriptResult save_script_config(const std::filesystem::path& path, const std::vector<AttachmentConfig>& attachments);

} // namespace scripting
