#include "script_system.h"
#include "script_bindings.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace scripting {
namespace {

ScriptResult success() { return {true, {}}; }
ScriptResult failure(std::string message) { return {false, std::move(message)}; }

bool is_finite(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::optional<PropertyValue> read_value(const sol::object& object, const PropertyValue& expectedType)
{
    if (std::holds_alternative<bool>(expectedType)) {
        if (object.get_type() != sol::type::boolean) return std::nullopt;
        return object.as<bool>();
    }
    if (std::holds_alternative<double>(expectedType)) {
        if (object.get_type() != sol::type::number) return std::nullopt;
        const double value = object.as<double>();
        if (!std::isfinite(value)) return std::nullopt;
        return value;
    }
    if (std::holds_alternative<std::string>(expectedType)) {
        if (object.get_type() != sol::type::string) return std::nullopt;
        return object.as<std::string>();
    }
    if (std::holds_alternative<glm::vec3>(expectedType)) {
        if (object.get_type() != sol::type::table) return std::nullopt;
        const sol::table table = object.as<sol::table>();
        glm::vec3 value{};
        const char* components[] = {"x", "y", "z"};
        for (int i = 0; i < 3; ++i) {
            const sol::object component = table.raw_get<sol::object>(components[i]);
            if (component.get_type() != sol::type::number) return std::nullopt;
            const double number = component.as<double>();
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
                return std::nullopt;
            }
            value[i] = static_cast<float>(number);
        }
        return value;
    }
    return std::nullopt;
}

sol::object make_value(sol::state_view lua, const PropertyValue& value)
{
    return std::visit([&lua](const auto& item) -> sol::object {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, glm::vec3>) {
            return lua.create_table_with("x", item.x, "y", item.y, "z", item.z);
        } else {
            return sol::make_object(lua, item);
        }
    }, value);
}

ScriptResult validate_value(const PropertyDesc& desc, const PropertyValue& value)
{
    if (value.index() != desc.defaultValue.index()) {
        return failure("property '" + desc.name + "': wrong value type");
    }
    if (const auto* number = std::get_if<double>(&value)) {
        if (!std::isfinite(*number)) return failure("property '" + desc.name + "': number must be finite");
        if (desc.range && (*number < desc.range->min || *number > desc.range->max)) {
            return failure("property '" + desc.name + "': number is outside the declared range");
        }
    }
    if (const auto* vector = std::get_if<glm::vec3>(&value)) {
        if (!is_finite(*vector)) return failure("property '" + desc.name + "': vec3 must be finite");
    }
    return success();
}

ScriptResult parse_property(const std::string& name, const sol::table& table, PropertyDesc& desc)
{
    desc.name = name;
    const sol::object type = table.raw_get<sol::object>("type");
    if (type.get_type() != sol::type::string) return failure("property '" + name + "': missing string type");

    const std::string typeName = type.as<std::string>();
    if (typeName == "bool") desc.defaultValue = false;
    else if (typeName == "number") desc.defaultValue = 0.0;
    else if (typeName == "string") desc.defaultValue = std::string{};
    else if (typeName == "vec3") desc.defaultValue = glm::vec3(0.0f);
    else return failure("property '" + name + "': unsupported type '" + typeName + "'");

    const auto defaultValue = read_value(table.raw_get<sol::object>("default"), desc.defaultValue);
    if (!defaultValue) return failure("property '" + name + "': invalid or missing default");
    desc.defaultValue = *defaultValue;

    const sol::object min = table.raw_get<sol::object>("min");
    const sol::object max = table.raw_get<sol::object>("max");
    if (min.valid() || max.valid()) {
        if (typeName != "number" || min.get_type() != sol::type::number || max.get_type() != sol::type::number) {
            return failure("property '" + name + "': min and max must both be numbers on a number property");
        }
        const NumberRange range{min.as<double>(), max.as<double>()};
        if (!std::isfinite(range.min) || !std::isfinite(range.max) || range.min > range.max) {
            return failure("property '" + name + "': invalid number range");
        }
        desc.range = range;
    }
    return validate_value(desc, desc.defaultValue);
}

ScriptResult parse_properties(const sol::object& object, std::vector<PropertyDesc>& descriptions)
{
    if (!object.valid()) return success();
    if (object.get_type() != sol::type::table) return failure("properties must be a table");

    for (const auto& [key, value] : object.as<sol::table>()) {
        if (key.get_type() != sol::type::string || key.as<std::string>().empty()) {
            return failure("property names must be nonempty strings");
        }
        if (value.get_type() != sol::type::table) return failure("each property declaration must be a table");
        PropertyDesc desc;
        const auto result = parse_property(key.as<std::string>(), value.as<sol::table>(), desc);
        if (!result) return result;
        descriptions.push_back(std::move(desc));
    }
    // Lua hash iteration is unordered; keep editor presentation deterministic.
    std::sort(descriptions.begin(), descriptions.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return success();
}

const PropertyDesc* find_property(const std::vector<PropertyDesc>& descriptions, const std::string& name)
{
    const auto it = std::find_if(descriptions.begin(), descriptions.end(), [&name](const auto& desc) {
        return desc.name == name;
    });
    if (it == descriptions.end()) return nullptr;
    return &*it;
}

} // namespace

struct ScriptSystem::Impl {
    struct Instance {
        InstanceId id;
        std::filesystem::path source;
        InstanceStatus status{InstanceStatus::Running};
        std::string lastError;
        std::vector<PropertyDesc> descriptions;
        NodeHandle owner{invalid_node};
        std::shared_ptr<detail::BindingContext> bindings;
        sol::environment environment;
        sol::table self;
        sol::protected_function onCreate;
        sol::protected_function onUpdate;
        sol::protected_function onDestroy;
    };

    // Reverse destruction order: instance references, VM, then the logger.
    // Lua finalizers can still log while lua_close is running.
    LogSink logSink;
    ScriptHost* host{};
    sol::state lua;
    std::map<InstanceId, std::unique_ptr<Instance>> instances;
    InstanceId nextId{1};

    explicit Impl(LogSink sink, ScriptHost* sceneHost) : logSink(std::move(sink)), host(sceneHost)
    {
        lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::utf8);
    }

    void emit(InstanceId id, const std::filesystem::path& source, LogLevel level,
        const std::string& phase, const std::string& text) const noexcept
    {
        // A host logger failure must not escape a Lua callback or the destructor.
        try {
            const ScriptMessage message{level, id, source, phase, text};
            if (logSink) {
                logSink(message);
            } else {
                std::clog << "[script " << id << "][" << phase << "] "
                          << source.generic_string() << ": " << text << '\n';
            }
        } catch (...) {
            // Logging is best-effort; instance.lastError/CreateResult retain errors.
        }
    }

    void fault(Instance& instance, const std::string& phase, const std::string& error)
    {
        instance.status = InstanceStatus::Faulted;
        instance.lastError = phase + ": " + error;
        emit(instance.id, instance.source, LogLevel::Error, phase, error);
    }

    sol::environment make_environment(const Instance& instance)
    {
        sol::environment environment(lua, sol::create);
        const char* baseFunctions[] = {
            "assert", "error", "ipairs", "next", "pairs", "pcall", "select", "tonumber", "tostring",
            "type", "xpcall", "rawequal", "rawget", "rawlen", "rawset", "getmetatable", "setmetatable", "_VERSION"
        };
        for (const char* name : baseFunctions) environment.raw_set(name, lua[name]);
        for (const char* name : {"math", "string", "table", "utf8"}) {
            sol::table copy = lua.create_table();
            for (const auto& [key, value] : lua[name].get<sol::table>()) copy.raw_set(key, value);
            environment.raw_set(name, copy);
        }
        environment.raw_set("_G", environment);
        const InstanceId id = instance.id;
        const auto source = instance.source;
        environment.set_function("log", [this, id, source](const std::string& text) {
            emit(id, source, LogLevel::Info, "log", text);
        });
        detail::bind_scene(environment, instance.bindings);
        return environment;
    }

    static ScriptResult read_callback(const sol::table& definition, const char* name, sol::protected_function& callback)
    {
        const sol::object value = definition.raw_get<sol::object>(name);
        if (!value.valid()) return success();
        if (value.get_type() != sol::type::function) return failure(std::string(name) + " must be a function");
        callback = value.as<sol::protected_function>();
        return success();
    }

    ScriptResult validate_properties(const Instance& instance) const
    {
        const sol::object properties = instance.self.raw_get<sol::object>("properties");
        if (properties.get_type() != sol::type::table) return failure("self.properties must remain a table");
        const sol::table values = properties.as<sol::table>();
        for (const auto& desc : instance.descriptions) {
            const auto value = read_value(values.raw_get<sol::object>(desc.name), desc.defaultValue);
            if (!value) return failure("property '" + desc.name + "': invalid runtime value");
            const auto result = validate_value(desc, *value);
            if (!result) return result;
        }
        return success();
    }

    ScriptResult call(Instance& instance, const sol::protected_function& callback, const char* phase,
        std::optional<double> deltaTime = std::nullopt)
    {
        if (!callback.valid()) return success();
        struct WriteScope {
            detail::BindingContext& context;
            explicit WriteScope(detail::BindingContext& value) : context(value) { context.allowWrites = true; }
            ~WriteScope() { context.allowWrites = false; }
        } scope(*instance.bindings);
        sol::protected_function_result result = deltaTime ? callback(instance.self, *deltaTime) : callback(instance.self);
        if (!result.valid()) {
            const sol::error error = result;
            fault(instance, phase, error.what());
            return failure(instance.lastError);
        }
        const auto validation = validate_properties(instance);
        if (!validation) {
            fault(instance, phase, validation.error);
            return failure(instance.lastError);
        }
        return success();
    }

    std::unique_ptr<Instance> prepare(InstanceId id, const std::filesystem::path& source,
        NodeHandle owner, std::string& error)
    {
        auto instance = std::make_unique<Instance>();
        instance->id = id;
        instance->source = source;
        instance->owner = owner;
        instance->bindings = std::make_shared<detail::BindingContext>();
        instance->bindings->host = host;
        auto reject = [&](const std::string& reason) -> std::unique_ptr<Instance> {
            fault(*instance, "load", reason);
            error = instance->lastError;
            return nullptr;
        };
        if (owner != invalid_node && (!host || !host->is_valid(owner))) return reject("owner node is unavailable");
        std::ifstream input(source, std::ios::binary);
        if (!input) return reject("cannot open script file");
        const std::string code{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (input.bad()) return reject("cannot read script file");

        instance->environment = make_environment(*instance);
        sol::load_result loaded = lua.load(code, "@" + source.generic_string(), sol::load_mode::text);
        if (!loaded.valid()) {
            const sol::error loadError = loaded;
            return reject(loadError.what());
        }
        sol::protected_function chunk = loaded;
        sol::set_environment(instance->environment, chunk);
        sol::protected_function_result evaluated = chunk();
        if (!evaluated.valid()) {
            const sol::error loadError = evaluated;
            return reject(loadError.what());
        }
        if (evaluated.return_count() != 1 || evaluated.get_type() != sol::type::table) {
            return reject("script must return exactly one definition table");
        }
        const sol::table definition = evaluated;
        const auto parsed = parse_properties(definition.raw_get<sol::object>("properties"), instance->descriptions);
        if (!parsed) return reject(parsed.error);
        const std::pair<const char*, sol::protected_function*> callbacks[] = {
            {"on_create", &instance->onCreate}, {"on_update", &instance->onUpdate}, {"on_destroy", &instance->onDestroy}
        };
        for (const auto& [name, callback] : callbacks) {
            const auto result = read_callback(definition, name, *callback);
            if (!result) return reject(result.error);
        }
        sol::table values = lua.create_table();
        for (const auto& desc : instance->descriptions) values.raw_set(desc.name, make_value(lua, desc.defaultValue));
        instance->self = lua.create_table_with("properties", values, "state", lua.create_table());
        if (host && owner != invalid_node) {
            instance->self.raw_set("node", detail::make_node_binding(lua, instance->bindings, owner));
        }
        return instance;
    }

    ScriptResult apply_overrides(Instance& instance, const PropertyOverrides& overrides)
    {
        for (const auto& [name, value] : overrides) {
            const auto* desc = find_property(instance.descriptions, name);
            if (!desc) return failure("unknown property override '" + name + "'");
            const auto result = validate_value(*desc, value);
            if (!result) return result;
        }
        sol::table values = instance.self.raw_get<sol::table>("properties");
        for (const auto& [name, value] : overrides) values.raw_set(name, make_value(lua, value));
        return success();
    }
};

ScriptSystem::ScriptSystem(LogSink logSink, ScriptHost* host) : impl_(std::make_unique<Impl>(std::move(logSink), host)) {}
ScriptSystem::~ScriptSystem() { clear(); }

CreateResult ScriptSystem::create_instance(const std::filesystem::path& source,
    const PropertyOverrides& overrides, NodeHandle owner)
{
    std::string error;
    auto instance = impl_->prepare(impl_->nextId++, source, owner, error);
    if (!instance) return {invalid_instance, error};
    const auto parameters = impl_->apply_overrides(*instance, overrides);
    if (!parameters) {
        impl_->fault(*instance, "load", parameters.error);
        return {invalid_instance, instance->lastError};
    }

    const auto created = impl_->call(*instance, instance->onCreate, "on_create");
    if (!created) {
        // Creation was entered, so offer the script one cleanup attempt.
        impl_->call(*instance, instance->onDestroy, "on_destroy");
        return {invalid_instance, created.error};
    }
    const InstanceId id = instance->id;
    impl_->instances.emplace(id, std::move(instance));
    return {id, {}};
}

DescribeResult ScriptSystem::describe(const std::filesystem::path& source, NodeHandle owner)
{
    std::string error;
    auto definition = impl_->prepare(invalid_instance, source, owner, error);
    if (!definition) return {false, {}, error};
    return {true, definition->descriptions, {}};
}

ScriptResult ScriptSystem::reload_file(const std::filesystem::path& source)
{
    std::vector<std::unique_ptr<Impl::Instance>> candidates;
    for (const auto& [id, current] : impl_->instances) {
        if (current->source != source) continue;
        std::string error;
        auto candidate = impl_->prepare(id, source, current->owner, error);
        if (!candidate) return failure(error);
        PropertyOverrides preserved;
        for (const auto& desc : candidate->descriptions) {
            const auto value = get_property(id, desc.name);
            if (value && validate_value(desc, *value)) {
                preserved.emplace(desc.name, *value);
            } else if (value) {
                impl_->emit(id, source, LogLevel::Info, "reload", "reset changed property '" + desc.name + "' to its default");
            }
        }
        impl_->apply_overrides(*candidate, preserved);
        candidates.push_back(std::move(candidate));
    }
    std::string firstError;
    for (auto& candidate : candidates) {
        auto& current = impl_->instances.at(candidate->id);
        const bool wasDisabled = current->status == InstanceStatus::Disabled;
        impl_->call(*current, current->onDestroy, "on_destroy");
        const auto created = impl_->call(*candidate, candidate->onCreate, "on_create");
        if (!created && firstError.empty()) firstError = created.error;
        if (created && wasDisabled) candidate->status = InstanceStatus::Disabled;
        current = std::move(candidate);
    }
    if (!firstError.empty()) return failure(firstError);
    return success();
}

void ScriptSystem::update(double deltaTime)
{
    if (!std::isfinite(deltaTime) || deltaTime < 0.0) {
        throw std::invalid_argument("script deltaTime must be finite and nonnegative");
    }
    for (auto& [id, instance] : impl_->instances) {
        if (instance->status == InstanceStatus::Running) {
            impl_->call(*instance, instance->onUpdate, "on_update", deltaTime);
        }
    }
}

ScriptResult ScriptSystem::set_enabled(InstanceId id, bool enabled)
{
    const auto it = impl_->instances.find(id);
    if (it == impl_->instances.end()) return failure("unknown script instance");
    if (it->second->status == InstanceStatus::Faulted) return failure("faulted instances must be recreated");
    it->second->status = enabled ? InstanceStatus::Running : InstanceStatus::Disabled;
    return success();
}

ScriptResult ScriptSystem::destroy_instance(InstanceId id)
{
    const auto it = impl_->instances.find(id);
    if (it == impl_->instances.end()) return failure("unknown script instance");
    const auto result = impl_->call(*it->second, it->second->onDestroy, "on_destroy");
    impl_->instances.erase(it);
    return result;
}

void ScriptSystem::clear()
{
    for (auto& [id, instance] : impl_->instances) {
        impl_->call(*instance, instance->onDestroy, "on_destroy");
    }
    impl_->instances.clear();
}

std::optional<InstanceInfo> ScriptSystem::inspect(InstanceId id) const
{
    const auto it = impl_->instances.find(id);
    if (it == impl_->instances.end()) return std::nullopt;
    const auto& instance = *it->second;
    return InstanceInfo{id, instance.source, instance.status, instance.descriptions, instance.lastError};
}

std::optional<PropertyValue> ScriptSystem::get_property(InstanceId id, const std::string& name) const
{
    const auto it = impl_->instances.find(id);
    if (it == impl_->instances.end()) return std::nullopt;
    const auto* desc = find_property(it->second->descriptions, name);
    if (!desc) return std::nullopt;
    const sol::object properties = it->second->self.raw_get<sol::object>("properties");
    if (properties.get_type() != sol::type::table) return std::nullopt;
    return read_value(properties.as<sol::table>().raw_get<sol::object>(name), desc->defaultValue);
}

ScriptResult ScriptSystem::set_property(InstanceId id, const std::string& name, const PropertyValue& value)
{
    const auto it = impl_->instances.find(id);
    if (it == impl_->instances.end()) return failure("unknown script instance");
    const auto* desc = find_property(it->second->descriptions, name);
    if (!desc) return failure("unknown property '" + name + "'");
    const auto result = validate_value(*desc, value);
    if (!result) return result;
    const sol::object properties = it->second->self.raw_get<sol::object>("properties");
    if (properties.get_type() != sol::type::table) return failure("self.properties must be a table");
    properties.as<sol::table>().raw_set(name, make_value(impl_->lua, value));
    return success();
}

std::size_t ScriptSystem::instance_count() const { return impl_->instances.size(); }

ScriptResult ScriptSystem::validate_property(const PropertyDesc& description, const PropertyValue& value)
{
    return validate_value(description, value);
}

} // namespace scripting
