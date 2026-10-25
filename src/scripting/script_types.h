#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <glm/vec3.hpp>

namespace scripting {

using InstanceId = uint64_t;
inline constexpr InstanceId invalid_instance = 0;

using PropertyValue = std::variant<bool, double, std::string, glm::vec3>;
using PropertyOverrides = std::unordered_map<std::string, PropertyValue>;

struct NumberRange {
    double min;
    double max;
};

struct PropertyDesc {
    std::string name;
    PropertyValue defaultValue;
    std::optional<NumberRange> range;
};

enum class InstanceStatus { Running, Disabled, Faulted };
enum class LogLevel { Info, Error };

struct ScriptMessage {
    LogLevel level;
    InstanceId instance;
    std::filesystem::path source;
    std::string phase;
    std::string text;
};

using LogSink = std::function<void(const ScriptMessage&)>;

struct ScriptResult {
    bool success;
    std::string error;
    explicit operator bool() const { return success; }
};

struct CreateResult {
    InstanceId id{invalid_instance};
    std::string error;
    explicit operator bool() const { return id != invalid_instance; }
};

struct DescribeResult {
    bool success{false};
    std::vector<PropertyDesc> properties;
    std::string error;
    explicit operator bool() const { return success; }
};

// A copy of editor-facing data; no Lua references escape the runtime.
struct InstanceInfo {
    InstanceId id;
    std::filesystem::path source;
    InstanceStatus status;
    std::vector<PropertyDesc> properties;
    std::string lastError;
};

} // namespace scripting
