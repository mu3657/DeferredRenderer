#include "script_config.h"

#include <json.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace scripting {
namespace {

using nlohmann::json;

json encode_value(const PropertyValue& value)
{
    if (const auto* item = std::get_if<bool>(&value)) return {{"type", "bool"}, {"value", *item}};
    if (const auto* item = std::get_if<std::string>(&value)) return {{"type", "string"}, {"value", *item}};
    if (const auto* item = std::get_if<double>(&value)) {
        if (!std::isfinite(*item)) throw std::runtime_error("cannot save a non-finite number");
        return {{"type", "number"}, {"value", *item}};
    }
    const auto vector = std::get<glm::vec3>(value);
    if (!std::isfinite(vector.x) || !std::isfinite(vector.y) || !std::isfinite(vector.z)) {
        throw std::runtime_error("cannot save a non-finite vec3");
    }
    return {{"type", "vec3"}, {"value", {vector.x, vector.y, vector.z}}};
}

PropertyValue decode_value(const json& property)
{
    const auto type = property.at("type").get<std::string>();
    const auto& value = property.at("value");
    if (type == "bool" && value.is_boolean()) return value.get<bool>();
    if (type == "string" && value.is_string()) return value.get<std::string>();
    if (type == "number" && value.is_number()) {
        const double number = value.get<double>();
        if (std::isfinite(number)) return number;
    }
    if (type == "vec3" && value.is_array() && value.size() == 3) {
        glm::vec3 vector;
        for (int i = 0; i < 3; ++i) {
            if (!value[i].is_number()) throw std::runtime_error("vec3 components must be numbers");
            const double number = value[i].get<double>();
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
                throw std::runtime_error("vec3 component is outside the finite float range");
            }
            vector[i] = static_cast<float>(number);
        }
        return vector;
    }
    throw std::runtime_error("invalid typed property value");
}

ScriptResult replace_file(const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return {true, {}};
    }
    return {false, std::error_code(GetLastError(), std::system_category()).message()};
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return {!error, error.message()};
#endif
}

} // namespace

std::filesystem::path script_config_path(const std::filesystem::path& scenePath)
{
    auto path = scenePath;
    path += ".scripts.json";
    return path;
}

ConfigLoadResult load_script_config(const std::filesystem::path& path)
{
    ConfigLoadResult result;
    try {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("cannot open script configuration: " + path.generic_string());
        const auto root = json::parse(input);
        if (!root.is_object() || root.at("version") != 1 || !root.at("scripts").is_array()) {
            throw std::runtime_error("expected configuration version 1 and a scripts array");
        }
        const auto folder = std::filesystem::absolute(path).parent_path();
        for (size_t index = 0; index < root["scripts"].size(); ++index) {
            try {
                const auto& entry = root["scripts"][index];
                AttachmentConfig config;
                config.nodeId = entry.at("node_id").get<std::string>();
                const auto source = entry.at("path").get<std::string>();
                if (config.nodeId.empty() || source.empty()) throw std::runtime_error("node_id and path must not be empty");
                config.source = (folder / std::filesystem::path(source)).lexically_normal();
                config.enabled = entry.value("enabled", true);
                if (entry.contains("properties")) {
                    if (!entry["properties"].is_object()) throw std::runtime_error("properties must be an object");
                    for (const auto& [name, value] : entry["properties"].items()) {
                        if (name.empty()) throw std::runtime_error("property name must not be empty");
                        config.overrides.emplace(name, decode_value(value));
                    }
                }
                result.attachments.push_back(std::move(config));
            } catch (const std::exception& error) {
                result.warnings.push_back("script entry " + std::to_string(index) + ": " + error.what());
            }
        }
        result.success = true;
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    return result;
}

ScriptResult save_script_config(const std::filesystem::path& path, const std::vector<AttachmentConfig>& attachments)
{
    std::filesystem::path temporary;
    try {
        const auto destination = std::filesystem::absolute(path).lexically_normal();
        json root = {{"version", 1}, {"scripts", json::array()}};
        for (const auto& config : attachments) {
            if (config.nodeId.empty() || config.source.empty()) throw std::runtime_error("node_id and script path must not be empty");
            auto source = std::filesystem::absolute(config.source).lexically_normal();
            const auto relative = source.lexically_relative(destination.parent_path());
            if (!relative.empty()) source = relative;
            json properties = json::object();
            for (const auto& [name, value] : config.overrides) properties[name] = encode_value(value);
            root["scripts"].push_back({{"node_id", config.nodeId}, {"path", source.generic_string()},
                {"enabled", config.enabled}, {"properties", properties}});
        }
        std::filesystem::create_directories(destination.parent_path());
        temporary = destination;
        temporary += ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << root.dump(2) << '\n';
            output.flush();
            if (!output) throw std::runtime_error("cannot write script configuration");
            output.close();
            if (!output) throw std::runtime_error("cannot close script configuration");
        }
        const auto replaced = replace_file(temporary, destination);
        if (!replaced) throw std::runtime_error("cannot replace configuration: " + replaced.error);
        return {true, {}};
    } catch (const std::exception& error) {
        if (!temporary.empty()) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        return {false, error.what()};
    }
}

} // namespace scripting
