#pragma once

#include "scene_scripts.h"
#include <array>

class ScriptInspector {
public:
    void draw_controls(SceneScripts& scripts);
    void draw_attachments(SceneScripts& scripts, scripting::NodeHandle selected);

private:
    std::array<char, 1024> source_{};
    std::string feedback_;
    void report(const scripting::ScriptResult& result, const char* success);
};
