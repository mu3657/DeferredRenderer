#pragma once

#include "script_host.h"

#include <memory>

namespace scripting {

// Single-threaded, non-reentrant owner of one VM. Log sinks must not call back
// into this system. Lua/sol2 and all instance references stay in the .cpp file.
class ScriptSystem {
public:
    explicit ScriptSystem(LogSink logSink = {}, ScriptHost* host = nullptr);
    ~ScriptSystem();

    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;
    ScriptSystem(ScriptSystem&&) = delete;
    ScriptSystem& operator=(ScriptSystem&&) = delete;

    // Loads text Lua, validates declarations/overrides, then calls on_create.
    // Failed creation leaves no live instance. IDs are not reused by this VM.
    CreateResult create_instance(const std::filesystem::path& source,
        const PropertyOverrides& overrides = {}, NodeHandle owner = invalid_node);

    // Inspects declarations without calling lifecycle functions or allowing
    // scene writes. Used by the editor and reload preflight.
    DescribeResult describe(const std::filesystem::path& source, NodeHandle owner = invalid_node);

    // Preflights all instances of this file before replacing any of them.
    // Syntax/schema failures preserve the old code and state. Once committed,
    // on_create failures fault the new instance, as during normal creation.
    ScriptResult reload_file(const std::filesystem::path& source);

    // Calls each Running instance once, in creation order. Lua errors fault
    // only that instance. Invalid dt throws std::invalid_argument before work.
    void update(double deltaTime);
    ScriptResult set_enabled(InstanceId id, bool enabled);

    // on_destroy is attempted once, including for Faulted/Disabled instances.
    ScriptResult destroy_instance(InstanceId id);
    void clear();

    std::optional<InstanceInfo> inspect(InstanceId id) const;
    std::optional<PropertyValue> get_property(InstanceId id, const std::string& name) const;
    ScriptResult set_property(InstanceId id, const std::string& name, const PropertyValue& value);
    static ScriptResult validate_property(const PropertyDesc& description, const PropertyValue& value);
    std::size_t instance_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace scripting
