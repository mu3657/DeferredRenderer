#include <scripting/script_system.h>
#include <scripting/script_config.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace scripting;

namespace {

struct TestRunner {
    int checks{0};
    int failures{0};

    void check(bool condition, const char* expression, int line)
    {
        ++checks;
        if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
    }

    template<typename F> void run(const char* name, F test)
    {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
};

#define CHECK(expression) runner.check(static_cast<bool>(expression), #expression, __LINE__)

// Every generated fixture lives in a new directory under the test working
// directory. Remove only the individual files this helper created.
struct TestFiles {
    std::filesystem::path root;
    std::vector<std::filesystem::path> files;

    TestFiles()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::current_path() / ("scripting-fixtures-" + std::to_string(stamp));
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("cannot create fixture directory");
    }

    ~TestFiles()
    {
        std::error_code error;
        for (const auto& path : files) std::filesystem::remove(path, error);
        std::filesystem::remove(root, error);
    }

    std::filesystem::path write(const std::string& name, const std::string& code)
    {
        const auto path = root / name;
        files.push_back(path);
        std::ofstream output(path, std::ios::binary);
        output << code;
        if (!output) throw std::runtime_error("cannot write fixture");
        return path;
    }
};

size_t count_text(const std::vector<ScriptMessage>& messages, const std::string& text)
{
    return std::count_if(messages.begin(), messages.end(), [&](const auto& message) { return message.text == text; });
}

const char* lifecycleScript = R"lua(
global_count = (global_count or 0) + 1
assert(_G.global_count == 1)
assert(math.instance_marker == nil)
math.instance_marker = true
local calls = 0

return {
    properties = {
        speed = { type = "number", default = 2, min = 0, max = 10 },
        enabled = { type = "bool", default = true },
        label = { type = "string", default = "default" },
        origin = { type = "vec3", default = { x = 1, y = 2, z = 3 } }
    },
    on_create = function(self)
        self.state.elapsed = 0
        log("create:" .. self.properties.label)
    end,
    on_update = function(self, dt)
        calls = calls + 1
        self.state.elapsed = self.state.elapsed + dt * self.properties.speed
        self.properties.origin.x = self.properties.origin.x + 1
        log(string.format("%s:%.2f:%d", self.properties.label, self.state.elapsed, calls))
    end,
    on_destroy = function(self)
        log("destroy:" .. self.properties.label)
    end
}
)lua";

struct TestHost : ScriptHost {
    bool exists{true};
    glm::vec3 position{0.f};
    LightState source;
    double rotation{0};
    bool is_valid(NodeHandle node) const override { return exists && node == 1; }
    NodeHandle find_node(const std::string& name) const override { return name == "target" && exists ? 1 : invalid_node; }
    std::optional<glm::vec3> local_position(NodeHandle node) const override {
        if (!is_valid(node)) return std::nullopt;
        return position;
    }
    ScriptResult set_local_position(NodeHandle node, const glm::vec3& value) override {
        if (!is_valid(node)) return {false, "expired"};
        position = value;
        return {true, {}};
    }
    ScriptResult rotate_local_y_radians(NodeHandle node, double angle) override {
        if (!is_valid(node) || !std::isfinite(angle)) return {false, "invalid rotation"};
        rotation += angle;
        return {true, {}};
    }
    std::optional<LightState> light(NodeHandle node) const override {
        if (!is_valid(node)) return std::nullopt;
        return source;
    }
    ScriptResult set_light(NodeHandle node, const LightState& value) override {
        if (!is_valid(node) || value.intensity < 0) return {false, "invalid light"};
        source = value;
        return {true, {}};
    }
};

} // namespace

int main()
{
    TestRunner runner;
    TestFiles files;
    const auto lifecycle = files.write("lifecycle.lua", lifecycleScript);

    runner.run("lifecycle, state/global isolation and property metadata", [&] {
        std::vector<ScriptMessage> messages;
        ScriptSystem scripts([&](const auto& message) { messages.push_back(message); });
        const auto a = scripts.create_instance(lifecycle, {{"label", std::string("A")}, {"speed", 1.0}});
        const auto b = scripts.create_instance(lifecycle, {{"label", std::string("B")}, {"speed", 3.0}});
        CHECK(a && b && a.id != b.id);
        CHECK(scripts.instance_count() == 2);
        CHECK(count_text(messages, "create:A") == 1);
        CHECK(count_text(messages, "create:B") == 1);
        const auto info = scripts.inspect(a.id);
        CHECK(info && info->source == lifecycle);
        CHECK(info->status == InstanceStatus::Running && info->lastError.empty());
        CHECK(info->properties.size() == 4);
        CHECK(info->properties[0].name == "enabled");
        CHECK(info->properties[1].name == "label");
        CHECK(info->properties[2].name == "origin");
        CHECK(info->properties[3].name == "speed");
        CHECK(std::get<double>(info->properties[3].defaultValue) == 2.0);
        CHECK(info->properties[3].range->min == 0.0 && info->properties[3].range->max == 10.0);
        CHECK(std::get<bool>(*scripts.get_property(a.id, "enabled")));
        scripts.update(0.5);
        CHECK(count_text(messages, "A:0.50:1") == 1);
        CHECK(count_text(messages, "B:1.50:1") == 1);
        CHECK(scripts.set_enabled(a.id, false));
        CHECK(scripts.inspect(a.id)->status == InstanceStatus::Disabled);
        scripts.update(0.5);
        CHECK(count_text(messages, "A:1.00:2") == 0);
        CHECK(count_text(messages, "B:3.00:2") == 1);
        CHECK(std::get<glm::vec3>(*scripts.get_property(a.id, "origin")).x == 2.0f);
        CHECK(std::get<glm::vec3>(*scripts.get_property(b.id, "origin")).x == 3.0f);
        CHECK(std::get<glm::vec3>(info->properties[2].defaultValue).x == 1.0f);
        CHECK(scripts.set_enabled(a.id, true));
        CHECK(scripts.set_property(a.id, "speed", 4.0));
        scripts.update(0.25);
        CHECK(count_text(messages, "A:1.50:2") == 1);
        CHECK(count_text(messages, "B:3.75:3") == 1);
        CHECK(scripts.destroy_instance(a.id));
        CHECK(count_text(messages, "destroy:A") == 1);
        CHECK(!scripts.inspect(a.id));
        CHECK(!scripts.get_property(a.id, "speed"));
        CHECK(!scripts.destroy_instance(a.id));
        scripts.update(0.0);
        CHECK(count_text(messages, "A:1.50:3") == 0);
        scripts.clear();
        scripts.clear();
        CHECK(scripts.instance_count() == 0);
        CHECK(count_text(messages, "destroy:A") == 1);
        CHECK(count_text(messages, "destroy:B") == 1);
        const auto c = scripts.create_instance(lifecycle);
        CHECK(c && c.id > b.id);
        CHECK(scripts.destroy_instance(c.id));
    });

    runner.run("external property validation is atomic", [&] {
        ScriptSystem scripts([](const auto&) {});
        const auto a = scripts.create_instance(lifecycle);
        CHECK(a);
        CHECK(!scripts.set_property(a.id, "speed", true));
        CHECK(!scripts.set_property(a.id, "speed", 11.0));
        CHECK(!scripts.set_property(a.id, "speed", std::numeric_limits<double>::infinity()));
        CHECK(!scripts.set_property(a.id, "speed", std::numeric_limits<double>::quiet_NaN()));
        CHECK(std::get<double>(*scripts.get_property(a.id, "speed")) == 2.0);
        CHECK(!scripts.set_property(a.id, "origin", glm::vec3(std::numeric_limits<float>::infinity())));
        CHECK(!scripts.set_property(a.id, "missing", 1.0));
        CHECK(!scripts.get_property(a.id, "missing"));
        CHECK(!scripts.inspect(invalid_instance));
        CHECK(!scripts.set_property(invalid_instance, "speed", 1.0));
        CHECK(!scripts.set_enabled(invalid_instance, true));
        CHECK(scripts.set_property(a.id, "speed", 0.0));
        CHECK(scripts.set_property(a.id, "speed", 10.0));
        CHECK(scripts.set_property(a.id, "label", std::string("changed")));
        CHECK(scripts.set_property(a.id, "enabled", false));
        CHECK(scripts.set_property(a.id, "origin", glm::vec3(4, 5, 6)));
        CHECK(std::get<std::string>(*scripts.get_property(a.id, "label")) == "changed");
        CHECK(!std::get<bool>(*scripts.get_property(a.id, "enabled")));
        CHECK(std::get<glm::vec3>(*scripts.get_property(a.id, "origin")).z == 6.0f);
        const PropertyOverrides invalidOverrides[] = {
            {{"speed", true}}, {{"speed", -1.0}}, {{"unknown", 1.0}}
        };
        for (const auto& overrides : invalidOverrides) {
            CHECK(!scripts.create_instance(lifecycle, overrides));
            CHECK(scripts.instance_count() == 1);
        }
    });

    runner.run("load and declaration failures leave no instance", [&] {
        std::vector<ScriptMessage> messages;
        ScriptSystem scripts([&](const auto& message) { messages.push_back(message); });
        CHECK(!scripts.create_instance(files.root / "missing.lua"));
        const std::pair<const char*, const char*> invalidScripts[] = {
            {"syntax", "return { on_create = function( }"},
            {"load_error", "local function fail() error('load failed') end; fail()"},
            {"return_nil", "return nil"},
            {"return_many", "return {}, {}"},
            {"callback", "return { on_update = 3 }"},
            {"properties", "return { properties = false }"},
            {"name", "return { properties = { [1] = {} } }"},
            {"empty_name", "return { properties = { [''] = {} } }"},
            {"declaration", "return { properties = { a = 4 } }"},
            {"type", "return { properties = { a = { type = 'matrix', default = 1 } } }"},
            {"missing_type", "return { properties = { a = { default = 1 } } }"},
            {"missing_default", "return { properties = { a = { type = 'number' } } }"},
            {"strict_number", "return { properties = { a = { type = 'number', default = '1' } } }"},
            {"strict_bool", "return { properties = { a = { type = 'bool', default = 1 } } }"},
            {"bad_vector", "return { properties = { a = { type = 'vec3', default = {x=1,y=2} } } }"},
            {"huge_vector", "return { properties = { a = { type = 'vec3', default = {x=1e100,y=2,z=3} } } }"},
            {"nan", "return { properties = { a = { type = 'number', default = 0/0 } } }"},
            {"infinity", "return { properties = { a = { type = 'number', default = math.huge } } }"},
            {"inverted_range", "return { properties = { a = { type = 'number', default = 1, min = 2, max = 0 } } }"},
            {"default_range", "return { properties = { a = { type = 'number', default = 5, min = 0, max = 2 } } }"},
            {"half_range", "return { properties = { a = { type = 'number', default = 1, min = 0 } } }"},
            {"bool_range", "return { properties = { a = { type = 'bool', default = true, min = 0, max = 1 } } }"},
            {"metatable", "return setmetatable({}, {__index=function() error('must not invoke') end})"}
        };
        for (const auto& [name, code] : invalidScripts) {
            const auto path = files.write(std::string(name) + ".lua", code);
            const auto created = scripts.create_instance(path);
            // Empty definitions with a metatable are legal. Host reads use raw_get.
            if (std::string(name) == "metatable") {
                CHECK(created);
                CHECK(scripts.destroy_instance(created.id));
            } else {
                CHECK(!created && !created.error.empty());
            }
            CHECK(scripts.instance_count() == 0);
        }
        CHECK(messages.size() == 23);
        CHECK(messages[1].text.find("syntax.lua:") != std::string::npos);
        CHECK(messages[2].text.find("stack traceback") != std::string::npos);
        CHECK(messages[2].source.filename() == "load_error.lua");
        CHECK(messages[2].phase == "load" && messages[2].level == LogLevel::Error);
    });

    runner.run("update fault isolation, traceback and one cleanup", [&] {
        const auto faulty = files.write("fault_update.lua", R"lua(
return {
    on_update = function(self, dt)
        local function nested() error("expected update failure") end
        nested()
    end,
    on_destroy = function(self) log("fault cleanup") end
}
)lua");
        std::vector<ScriptMessage> messages;
        ScriptSystem scripts([&](const auto& message) { messages.push_back(message); });
        const auto bad = scripts.create_instance(faulty);
        const auto good = scripts.create_instance(lifecycle);
        CHECK(bad && good);
        scripts.update(0.5);
        const auto info = scripts.inspect(bad.id);
        CHECK(info && info->status == InstanceStatus::Faulted);
        CHECK(info->lastError.find("expected update failure") != std::string::npos);
        CHECK(info->lastError.find("stack traceback") != std::string::npos);
        CHECK(info->lastError.find("fault_update.lua:") != std::string::npos);
        CHECK(!scripts.set_enabled(bad.id, true));
        CHECK(count_text(messages, "default:1.00:1") == 1);
        const size_t afterFault = messages.size();
        scripts.update(0.5);
        CHECK(messages.size() == afterFault + 1);
        CHECK(count_text(messages, "default:2.00:2") == 1);
        CHECK(scripts.destroy_instance(bad.id));
        CHECK(count_text(messages, "fault cleanup") == 1);
        scripts.clear();
        CHECK(count_text(messages, "fault cleanup") == 1);
    });

    runner.run("creation and destruction errors still release instances", [&] {
        const auto badCreate = files.write("fault_create.lua", R"lua(
return {
    on_create = function(self) error("create failed") end,
    on_destroy = function(self) log("failed create cleanup") end
}
)lua");
        const auto badDestroy = files.write("fault_destroy.lua", R"lua(
return { on_destroy = function(self) log("destroy entered"); error("destroy failed") end }
)lua");
        std::vector<ScriptMessage> messages;
        {
            ScriptSystem scripts([&](const auto& message) { messages.push_back(message); });
            const auto failed = scripts.create_instance(badCreate);
            CHECK(!failed && failed.error.find("on_create") != std::string::npos);
            CHECK(scripts.instance_count() == 0);
            CHECK(count_text(messages, "failed create cleanup") == 1);
            const auto a = scripts.create_instance(badDestroy);
            CHECK(a);
            CHECK(!scripts.destroy_instance(a.id));
            CHECK(!scripts.inspect(a.id));
            CHECK(scripts.instance_count() == 0);
            const auto b = scripts.create_instance(badDestroy);
            CHECK(b);
            CHECK(scripts.set_enabled(b.id, false));
            // The destructor must clean up Disabled instances, even on errors.
        }
        CHECK(count_text(messages, "destroy entered") == 2);
    });

    runner.run("script writes are validated at callback boundaries", [&] {
        const char* writes[] = {"self.properties.speed = 'wrong'", "self.properties.speed = 20", "self.properties = nil"};
        ScriptSystem scripts([](const auto&) {});
        for (int i = 0; i < 3; ++i) {
            const std::string code = "return { properties={speed={type='number',default=1,min=0,max=10}}, "
                "on_update=function(self,dt) " + std::string(writes[i]) + " end }";
            const auto created = scripts.create_instance(files.write("bad_write_" + std::to_string(i) + ".lua", code));
            CHECK(created);
            scripts.update(0.1);
            CHECK(scripts.inspect(created.id)->status == InstanceStatus::Faulted);
            CHECK(!scripts.inspect(created.id)->lastError.empty());
            CHECK(scripts.destroy_instance(created.id));
        }
    });

    runner.run("optional callbacks, invalid dt and logger exceptions", [&] {
        const auto empty = files.write("empty.lua", "return {}");
        ScriptSystem scripts([](const auto&) { throw std::runtime_error("host logger failed"); });
        const auto a = scripts.create_instance(empty);
        const auto b = scripts.create_instance(lifecycle);
        CHECK(a && b);
        for (double dt : {-0.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            bool threw = false;
            try { scripts.update(dt); }
            catch (const std::invalid_argument&) { threw = true; }
            CHECK(threw);
        }
        scripts.update(0.0);
        CHECK(scripts.inspect(b.id)->status == InstanceStatus::Running);
        CHECK(std::get<glm::vec3>(*scripts.get_property(b.id, "origin")).x == 2.0f);
        scripts.clear();
        CHECK(scripts.instance_count() == 0);
    });

    runner.run("repeated VM construction and instance teardown", [&] {
        for (int vm = 0; vm < 16; ++vm) {
            ScriptSystem scripts([](const auto&) {});
            for (int instance = 0; instance < 8; ++instance) {
                CHECK(scripts.create_instance(lifecycle));
            }
            scripts.update(0.01);
            CHECK(scripts.instance_count() == 8);
            scripts.clear();
            CHECK(scripts.instance_count() == 0);
        }
    });

    runner.run("Lua finalizers can log during VM shutdown", [&] {
        const auto finalizer = files.write("finalizer.lua", R"lua(
local token = setmetatable({}, { __gc = function() log("finalizer ran") end })
return { on_create = function(self) self.state.token = token end }
)lua");
        std::vector<ScriptMessage> messages;
        {
            ScriptSystem scripts([&](const auto& message) { messages.push_back(message); });
            CHECK(scripts.create_instance(finalizer));
        }
        CHECK(count_text(messages, "finalizer ran") == 1);
    });

    runner.run("scene bindings, metadata write isolation and expired handles", [&] {
        const auto path = files.write("host.lua", R"lua(
return {
    on_create=function(self)
        assert(self.node:is_valid())
        assert(not scene.find("missing"):is_valid())
        self.state.target = scene.find("target")
    end,
    on_update=function(self,dt)
        local p = self.state.target.transform:get_local_position()
        self.node.transform:set_local_position({x=p.x+dt,y=2,z=3})
        self.node.transform:rotate_local_y_radians(dt)
        self.node.light:set_intensity(self.node.light:get_intensity()+dt)
        self.node.light:set_range(12)
        self.node.light:set_color({x=0.2,y=0.4,z=0.6})
    end
}
)lua");
        TestHost host;
        ScriptSystem scripts([](const auto&) {}, &host);
        CHECK(scripts.describe(path, 1));
        CHECK(host.position.x == 0 && host.source.intensity == 1);
        const auto a = scripts.create_instance(path, {}, 1);
        CHECK(a);
        scripts.update(0.5);
        CHECK(host.position == glm::vec3(0.5f, 2, 3));
        CHECK(host.rotation == 0.5);
        CHECK(host.source.intensity == 1.5f && host.source.range == 12);
        CHECK(std::abs(host.source.color.z - 0.6f) < 0.0001f);
        const auto topWrite = files.write("top_write.lua", "scene.find('target').light:set_intensity(99); return {}");
        const auto rejected = scripts.describe(topWrite, 1);
        CHECK(!rejected && rejected.error.find("only allowed inside lifecycle") != std::string::npos);
        CHECK(host.source.intensity == 1.5f);
        host.exists = false;
        scripts.update(0.1);
        CHECK(scripts.inspect(a.id)->status == InstanceStatus::Faulted);
        CHECK(scripts.inspect(a.id)->lastError.find("no longer valid") != std::string::npos);
        CHECK(!scripts.create_instance(path, {}, 1));
    });

    runner.run("reload preflight, all instances, preserved values and reset state", [&] {
        const auto path = files.write("reload.lua", lifecycleScript);
        std::vector<ScriptMessage> messages;
        ScriptSystem scripts([&](const auto& message) { messages.push_back(message); });
        const auto a = scripts.create_instance(path, {{"label", std::string("A")}, {"speed", 4.0}});
        const auto b = scripts.create_instance(path, {{"label", std::string("B")}});
        CHECK(a && b);
        scripts.update(0.5);
        files.write("reload.lua", "return { on_update = function( }");
        CHECK(!scripts.reload_file(path));
        CHECK(scripts.inspect(a.id)->status == InstanceStatus::Running);
        scripts.update(0.5);
        CHECK(count_text(messages, "A:4.00:2") == 1);
        CHECK(count_text(messages, "B:2.00:2") == 1);
        CHECK(scripts.set_enabled(b.id, false));
        files.write("reload.lua", R"lua(
return {
    properties={speed={type='number',default=1,min=0,max=10},label={type='string',default='new'},added={type='bool',default=true}},
    on_create=function(self) self.state.count=0; log('new:create') end,
    on_update=function(self,dt) self.state.count=self.state.count+1; log(self.properties.label..':new:'..self.state.count) end
}
)lua");
        CHECK(scripts.reload_file(path));
        CHECK(scripts.instance_count() == 2);
        CHECK(std::get<double>(*scripts.get_property(a.id, "speed")) == 4.0);
        CHECK(std::get<bool>(*scripts.get_property(a.id, "added")));
        CHECK(scripts.inspect(b.id)->status == InstanceStatus::Disabled);
        CHECK(count_text(messages, "destroy:A") == 1 && count_text(messages, "destroy:B") == 1);
        CHECK(count_text(messages, "new:create") == 2);
        scripts.update(0.5);
        CHECK(count_text(messages, "A:new:1") == 1);
        CHECK(count_text(messages, "B:new:1") == 0);
        files.write("reload.lua", "return {properties={speed={type='string',default='reset'}}}");
        CHECK(scripts.reload_file(path));
        CHECK(std::get<std::string>(*scripts.get_property(a.id, "speed")) == "reset");
        CHECK(!scripts.get_property(a.id, "label"));
        files.write("reload.lua", "return {on_create=function(self) error('new create failed') end}");
        CHECK(!scripts.reload_file(path));
        CHECK(scripts.inspect(a.id)->status == InstanceStatus::Faulted);
        CHECK(scripts.inspect(b.id)->status == InstanceStatus::Faulted);
        files.write("reload.lua", "return {}");
        CHECK(scripts.reload_file(path));
        CHECK(scripts.inspect(a.id)->status == InstanceStatus::Running);
    });

    runner.run("typed configuration roundtrip, relative paths and safe replacement", [&] {
        const auto path = files.write("scene.pfb.scripts.json", "{}");
        std::vector<AttachmentConfig> configs{{"prefab/123", lifecycle, false,
            {{"speed", 3.5}, {"enabled", false}, {"label", std::string("saved")}, {"origin", glm::vec3(4, 5, 6)}}}};
        CHECK(save_script_config(path, configs));
        auto loaded = load_script_config(path);
        CHECK(loaded && loaded.warnings.empty() && loaded.attachments.size() == 1);
        CHECK(loaded.attachments[0].nodeId == "prefab/123" && !loaded.attachments[0].enabled);
        CHECK(loaded.attachments[0].source == lifecycle);
        CHECK(std::get<double>(loaded.attachments[0].overrides.at("speed")) == 3.5);
        CHECK(std::get<glm::vec3>(loaded.attachments[0].overrides.at("origin")) == glm::vec3(4, 5, 6));
        std::ifstream input(path);
        const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        CHECK(text.find("\"path\": \"lifecycle.lua\"") != std::string::npos);
        input.close();
        configs[0].overrides["speed"] = 5.0;
        CHECK(save_script_config(path, configs));
        CHECK(std::get<double>(load_script_config(path).attachments[0].overrides.at("speed")) == 5.0);
        configs[0].overrides["speed"] = std::numeric_limits<double>::infinity();
        CHECK(!save_script_config(path, configs));
        CHECK(std::get<double>(load_script_config(path).attachments[0].overrides.at("speed")) == 5.0);
        files.write("scene.pfb.scripts.json", R"json({"version":1,"scripts":[
            {"node_id":"prefab/1","path":"lifecycle.lua","properties":{"speed":{"type":"number","value":"bad"}}},
            {"node_id":"prefab/2","path":"lifecycle.lua","enabled":true}
        ]})json");
        loaded = load_script_config(path);
        CHECK(loaded && loaded.attachments.size() == 1 && loaded.warnings.size() == 1);
        CHECK(loaded.attachments[0].nodeId == "prefab/2");
        files.write("scene.pfb.scripts.json", "{broken");
        CHECK(!load_script_config(path));
        files.write("scene.pfb.scripts.json", R"json({"version":2,"scripts":[]})json");
        CHECK(!load_script_config(path));
        CHECK(!load_script_config(files.root / "missing.json"));
        CHECK(script_config_path("scene.pfb").generic_string() == "scene.pfb.scripts.json");
    });

    std::cout << "Checks: " << runner.checks << ", failures: " << runner.failures << '\n';
    return runner.failures == 0 ? 0 : 1;
}
