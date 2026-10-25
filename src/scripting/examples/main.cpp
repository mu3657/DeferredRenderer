#include <scripting/script_system.h>

#include <iostream>

int main(int argc, char** argv)
{
    scripting::ScriptSystem scripts([](const scripting::ScriptMessage& message) {
        std::cout << '[' << message.instance << "][" << message.phase << "] " << message.text << '\n';
    });

    const std::filesystem::path path = argc > 1 ? std::filesystem::path(argv[1]) : SCRIPTING_EXAMPLE_FILE;
    const auto first = scripts.create_instance(path, {{"name", std::string("A")}, {"speed", 1.0}});
    const auto second = scripts.create_instance(path, {{"name", std::string("B")}, {"speed", 2.0}});
    if (!first || !second) return 1;

    for (int frame = 0; frame < 3; ++frame) scripts.update(0.25);

    const auto firstInfo = scripts.inspect(first.id);
    const auto secondInfo = scripts.inspect(second.id);
    if (firstInfo->status == scripting::InstanceStatus::Faulted || secondInfo->status == scripting::InstanceStatus::Faulted) {
        return 1;
    }
    const auto firstDestroyed = scripts.destroy_instance(first.id);
    const auto secondDestroyed = scripts.destroy_instance(second.id);
    return firstDestroyed && secondDestroyed ? 0 : 1;
}
