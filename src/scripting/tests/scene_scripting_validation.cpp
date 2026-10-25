// Opt-in integration validation; normal scene content is never modified/saved.
#include "vk_engine.h"
#include "vk_images.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"
#include <glm/gtc/packing.hpp>
#include <json.hpp>
#include <fstream>
#include <cmath>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace {

bool same_matrix(const glm::mat4& a, const glm::mat4& b, float tolerance = 0.00001f)
{
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (std::abs(a[column][row] - b[column][row]) > tolerance) return false;
    return true;
}

} // namespace

int VulkanEngine::run_scripting_validation()
{
    namespace fs = std::filesystem;
    using namespace scripting;
    const fs::path output = fs::absolute("../.runtime/scripting/full");
    fs::create_directories(output);
    std::ofstream report(output / "checks.txt");
    unsigned checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) {
        ++checks;
        failures += !ok;
        report << (ok ? "PASS " : "FAIL ") << label << '\n';
        report.flush();
        if (!ok) fmt::println("FAIL scripting: {}", label);
    };
    const auto moveFile = output / "move.lua";
    fs::copy_file(fs::path(SCRIPTING_EXAMPLES_DIR) / "move.lua", moveFile, fs::copy_options::overwrite_existing);
    std::ifstream originalInput(moveFile);
    const std::string originalMove{std::istreambuf_iterator<char>(originalInput), std::istreambuf_iterator<char>()};
    originalInput.close();

    // Persist bindings for a real imported prefab, then construct fresh nodes
    // from that asset and change their display names before loading the sidecar.
    if (!activeSceneName.empty()) {
        const auto importedPath = activeSceneName;
        const auto imported = loadedScenes.at(importedPath);
        SceneScripts saved;
        const auto sidecarScene = output / "imported.pfb";
        fs::remove(script_config_path(sidecarScene));
        saved.set_scene(imported.get(), sidecarScene);
        const auto root = imported->topNodes.front();
        const auto handle = saved.host().handle_for(root);
        const auto attached = saved.attach(handle, moveFile);
        check(bool(attached), "attach to real prefab node");
        check(root->persistentID.starts_with("prefab/"), "imported ID comes from prefab rather than display name");
        check(bool(saved.set_parameter(attached.id, "amplitude", 0.4)), "configure imported script parameter");
        check(bool(saved.set_enabled(attached.id, false)), "configure disabled attachment");
        check(bool(saved.save()), "save imported sidecar");
        const auto fresh = assetManager.load_prefab(importedPath);
        check(bool(fresh), "reload actual prefab into fresh node objects");
        if (fresh) {
            SceneScripts reopened;
            reopened.set_scene(fresh.get(), sidecarScene);
            const auto restored = reopened.attachments();
            check(restored.size() == 1, "fresh controller automatically loads sidecar");
            if (restored.size() == 1) {
                check(restored[0].node != invalid_node && !restored[0].config.enabled, "persistent ID and enabled flag survive reload");
                check(std::get<double>(restored[0].values.at("amplitude")) == 0.4, "property override survives fresh prefab load");
                const auto persistent = root->persistentID;
                for (const auto& [name, node] : fresh->nodes) if (node->persistentID == persistent) node->name += " renamed";
                check(bool(reopened.load()), "reload configuration after renaming node");
                check(reopened.attachments()[0].node != invalid_node, "node rename preserves script binding");
            }
        }
        saved.set_scene(nullptr);
    }

    auto fixture = std::make_shared<LoadedScene>();
    fixture->creator = this;
    fixture->materialDataBuffer = {};
    auto addMesh = [&](const char* name, PrimitiveShape shape, const glm::vec3& position, const glm::vec3& scale) {
        auto node = std::make_shared<MeshNode>();
        node->name = name;
        node->mesh = assetManager.get_primitive(shape);
        node->localTransform = glm::translate(glm::mat4(1.f), position) * glm::scale(glm::mat4(1.f), scale);
        fixture->topNodes.push_back(node);
        fixture->nodes[node->name] = node;
        return node;
    };
    addMesh("Floor", PrimitiveShape::Plane, {0,-1,0}, {8,1,8});
    addMesh("Back wall", PrimitiveShape::Cube, {0,1,-3}, {7,4,0.2f});
    addMesh("Left wall", PrimitiveShape::Cube, {-3,1,0}, {0.2f,4,6});
    const auto first = addMesh("Moving cube A", PrimitiveShape::Cube, {-1.2f,-0.3f,0}, {0.7f,1.4f,0.7f});
    const auto second = addMesh("Moving cube B", PrimitiveShape::Cube, {1.3f,-0.5f,-1}, {0.8f,1,0.8f});
    auto parent = std::make_shared<Node>();
    parent->name = "Scripted mesh parent";
    parent->localTransform = glm::translate(glm::mat4(1.f), glm::vec3(0.2f,0,-0.2f))
        * glm::scale(glm::mat4(1.f), glm::vec3(1.1f,1.f,0.8f));
    parent->children.push_back(first);
    first->parent = parent;
    std::erase(fixture->topNodes, first);
    fixture->topNodes.push_back(parent);
    fixture->nodes[parent->name] = parent;
    auto source = std::make_shared<LightNode>();
    source->name = "Pulsing point light";
    source->localTransform = glm::translate(glm::mat4(1.f), glm::vec3(0,2,1));
    source->light.type = LightType::Point;
    source->light.color = {1.f,0.75f,0.45f};
    source->light.intensity = 25.f;
    source->light.range = 12.f;
    fixture->topNodes.push_back(source);
    fixture->nodes[source->name] = source;
    fixture->lightNodes.push_back(source);
    const auto fixtureName = (output / "fixture.pfb").generic_string();
    fs::remove(script_config_path(fixtureName));
    loadedScenes[fixtureName] = fixture;
    set_active_scene(fixtureName);
    auto& host = sceneScripts.host();
    const auto firstHandle = host.handle_for(first);
    const auto secondHandle = host.handle_for(second);
    const auto lightHandle = host.handle_for(source);
    const auto firstInitial = first->localTransform;
    const auto firstInitialWorld = first->worldTransform;
    const auto secondInitial = second->localTransform;

    // Test reachability, even if external shared_ptrs keep a removed node alive.
    auto removed = std::make_shared<Node>();
    removed->name = "Removed node";
    fixture->topNodes.push_back(removed);
    sceneScripts.update(0);
    const auto removedHandle = host.handle_for(removed);
    std::erase(fixture->topNodes, removed);
    sceneScripts.update(0);
    check(!host.is_valid(removedHandle), "removed node handle expires despite external shared ownership");
    check(!host.set_local_position(removedHandle, {1,2,3}), "expired handle cannot modify detached node");
    check(bool(host.set_local_position(firstHandle, {2,3,4})), "scene adapter sets local position");
    check(glm::mat3(first->localTransform) == glm::mat3(firstInitial), "position setter preserves scale and rotation");
    first->localTransform = firstInitial;
    check(bool(host.rotate_local_y_radians(firstHandle, 0.2)), "local rotation API accepts radians");
    check(same_matrix(first->localTransform, glm::rotate(firstInitial, 0.2f, glm::vec3(0,1,0))), "local rotation follows matrix composition");
    first->localTransform = firstInitial;
    const auto a = sceneScripts.attach(firstHandle, moveFile);
    const auto b = sceneScripts.attach(secondHandle, moveFile);
    const auto light = sceneScripts.attach(lightHandle, fs::path(SCRIPTING_EXAMPLES_DIR) / "pulse_light.lua");
    check(a && b && light, "attach two movement instances and a light script");
    check(bool(sceneScripts.set_parameter(a.id, "speed", 1.2)), "configure first speed");
    check(bool(sceneScripts.set_parameter(b.id, "speed", 2.4)), "configure independent second speed");
    check(bool(sceneScripts.set_parameter(b.id, "axis", glm::vec3(0,1,0))), "configure vector parameter");
    check(!sceneScripts.set_parameter(a.id, "speed", -1.0), "controller rejects parameter outside metadata range");
    check(bool(sceneScripts.save()), "save procedural fixture script configuration");
    check(!sceneScripts.dirty(), "successful save clears dirty flag");

    check(bool(sceneScripts.play()), "Play creates enabled instances");
    sceneScripts.update(0.5);
    check(first->localTransform[3].x != firstInitial[3].x && second->localTransform[3].y != secondInitial[3].y,
        "same script drives independent node states and parameters");
    check(source->light.intensity != 25.f, "Lua changes actual LightNode intensity");
    check(sceneScripts.edits_locked(), "scene edits are locked while playing");
    check(!sceneScripts.remove(a.id), "attachment removal is blocked during playback");
    const auto pausedTransform = first->localTransform;
    sceneScripts.pause();
    sceneScripts.update(10.0);
    check(same_matrix(pausedTransform, first->localTransform), "Pause freezes script time and transforms");
    check(bool(sceneScripts.play()), "Resume keeps existing instances");
    sceneScripts.update(0.1);
    check(!same_matrix(pausedTransform, first->localTransform), "Resume continues execution");
    check(bool(sceneScripts.set_enabled(b.id, false)), "disable live instance");
    const auto disabledTransform = second->localTransform;
    sceneScripts.update(0.1);
    check(same_matrix(disabledTransform, second->localTransform), "disabled instance stops updating");
    check(bool(sceneScripts.set_enabled(b.id, true)), "reenable live instance");

    const auto liveBeforeReload = sceneScripts.attachments(firstHandle).front().instance;
    std::ofstream(moveFile) << "return { on_update = function( }";
    check(!sceneScripts.reload(moveFile), "syntax-error reload is rejected");
    check(sceneScripts.attachments(firstHandle).front().instance == liveBeforeReload, "failed reload preserves old runtime instance");
    const auto beforeFailedUpdate = first->localTransform;
    sceneScripts.update(0.1);
    check(!same_matrix(beforeFailedUpdate, first->localTransform), "old code continues after failed reload");
    std::ofstream(moveFile) << originalMove;
    check(bool(sceneScripts.reload(moveFile)), "valid file reloads every instance");
    check(std::get<double>(sceneScripts.attachments(firstHandle).front().values.at("speed")) == 1.2,
        "reload preserves configured parameter values");
    sceneScripts.stop();
    check(same_matrix(firstInitial, first->localTransform) && same_matrix(secondInitial, second->localTransform),
        "Stop exactly restores initial local transforms");
    check(source->light.intensity == 25.f, "Stop restores original light parameters");
    check(!sceneScripts.edits_locked(), "Stop unlocks scene editing");
    std::ofstream(sceneScripts.config_path()) << "{broken";
    check(!sceneScripts.load() && sceneScripts.attachments().size() == 3, "malformed sidecar preserves current attachments");
    check(bool(sceneScripts.save()), "replace corrupt sidecar with current configuration");
    check(bool(sceneScripts.load()) && sceneScripts.attachments().size() == 3, "load restores three attachments");

    const auto cleanConfiguration = load_script_config(sceneScripts.config_path()).attachments;
    auto missingConfiguration = cleanConfiguration;
    missingConfiguration.push_back({"prefab/missing", moveFile, true, {}});
    missingConfiguration.push_back({first->persistentID, output / "missing.lua", true, {}});
    check(bool(save_script_config(sceneScripts.config_path(), missingConfiguration)), "prepare missing-node and missing-file configuration");
    check(bool(sceneScripts.load()), "missing resources do not prevent loading valid script entries");
    const auto missingEntries = sceneScripts.attachments();
    check(missingEntries.size() == 5, "unresolved records remain available for later save or repair");
    check(!missingEntries[3].error.empty() && !missingEntries[4].error.empty(), "missing node and missing script have actionable diagnostics");
    auto changedConfiguration = cleanConfiguration;
    changedConfiguration[0].overrides["speed"] = std::string("old type");
    check(bool(save_script_config(sceneScripts.config_path(), changedConfiguration)), "prepare changed property type");
    check(bool(sceneScripts.load()), "changed property type is migrated during load");
    check(std::get<double>(sceneScripts.attachments()[0].values.at("speed")) == 1.0, "changed property uses its new declared default");
    check(sceneScripts.dirty(), "migrated configuration is marked unsaved");
    check(bool(save_script_config(sceneScripts.config_path(), cleanConfiguration)) && bool(sceneScripts.load()), "restore valid configuration after negative tests");

    const auto faultFile = output / "fault.lua";
    std::ofstream(faultFile) << "return {on_update=function(self,dt) error('isolated scene script failure') end}";
    const auto fault = sceneScripts.attach(host.handle_for(first), faultFile);
    check(bool(fault) && bool(sceneScripts.play()), "start a deliberately faulty scene script beside valid scripts");
    sceneScripts.update(0.2);
    bool faultIsolated = false;
    for (const auto& item : sceneScripts.attachments()) if (item.id == fault.id) {
        faultIsolated = item.status == InstanceStatus::Faulted && item.error.find("isolated scene script failure") != std::string::npos;
    }
    check(faultIsolated, "scene controller exposes per-instance runtime failure");
    check(!same_matrix(firstInitial, first->localTransform), "valid scene script continues beside faulty script");
    sceneScripts.stop();
    check(bool(sceneScripts.remove(fault.id)), "faulty attachment can be removed after Stop");

    mainCamera.position = {5.f,3.f,8.f};
    const auto direction = glm::normalize(glm::vec3(0.f,0.2f,-0.5f) - mainCamera.position);
    mainCamera.pitch = std::asin(direction.y);
    mainCamera.yaw = std::atan2(direction.x, -direction.z);
    mainCamera.mark_cut();
    lightSystem.set_fallback_directional_enabled(false);
    contactShadowPass.set_enabled(false);
    ddgiProbeDebugPass.set_enabled(false);
    ddgiTiming.automaticBudget = false;
    ddgiVolume.set_probes_updated_per_frame(64);
    environmentLighting.intensity = 0.15f;
    taaPass.set_enabled(true);
    SDL_SetWindowSize(_window, 1280, 800);
    resize_swapchain();
    for (size_t i = 0; i < sceneOutliner.entries.size(); ++i) {
        if (sceneOutliner.entries[i].node.lock() == first) sceneOutliner.selected_index = static_cast<int>(i);
    }
    auto frames = [&](unsigned count, double dt) {
        unsigned submitted = 0;
        for (unsigned attempt = 0; attempt < count + 10 && submitted < count; ++attempt) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) ImGui_ImplSDL2_ProcessEvent(&event);
            if (SDL_GetWindowFlags(_window) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(_window);
            if (resize_requested) resize_swapchain();
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            draw_editor_ui();
            sceneScripts.update(dt);
            ImGui::Render();
            const int before = _frameNumber;
            draw();
            VK_CHECK(vkDeviceWaitIdle(_device));
            submitted += _frameNumber > before;
        }
        check(submitted == count, "requested scripting GPU frames submitted");
    };
    auto capture = [&](const char* name) {
        const size_t count = size_t(_drawExtent.width) * _drawExtent.height;
        auto buffer = create_buffer(count * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
        immediate_submit([&](VkCommandBuffer cmd) {
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {_drawExtent.width, _drawExtent.height, 1};
            vkCmdCopyImageToBuffer(cmd, _drawImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &copy);
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        });
        vmaInvalidateAllocation(_allocator, buffer.allocation, 0, VK_WHOLE_SIZE);
        std::vector<unsigned char> pixels(count * 3);
        const auto* halves = static_cast<const uint16_t*>(buffer.info.pMappedData);
        bool finite = true;
        for (size_t i = 0; i < count; ++i) for (int channel = 0; channel < 3; ++channel) {
            const float value = glm::unpackHalf1x16(halves[i * 4 + channel]);
            finite &= std::isfinite(value);
            pixels[i * 3 + channel] = static_cast<unsigned char>(std::clamp(value, 0.f, 1.f) * 255.f);
        }
        destroy_buffer(buffer);
        check(finite, "rendered image contains finite colors");
        const auto path = (output / (std::string(name) + ".png")).string();
        check(stbi_write_png(path.c_str(), _drawExtent.width, _drawExtent.height, 3, pixels.data(), _drawExtent.width * 3) != 0,
            "write actual GPU scene capture");
        return pixels;
    };
    frames(48, 0);
    const auto before = capture("stopped");
    const auto historySerial = ddgiVolume.history_clear_serial();
    check(bool(sceneScripts.play()), "start scripts in rendered scene");
    frames(40, 0.025);
    sceneScripts.pause();
    frames(8, 0.025);
    const auto after = capture("playing");
    check(taaPass.history_was_used(), "TAA history remains active during scripted animation");
    check(ddgiVolume.history_clear_serial() == historySerial, "continuous script animation does not clear DDGI history every frame");
    check(ddgiProbeTracePass.stats().probeCount > 0, "DDGI traces the script-driven scene");
    check(ImGui::GetDrawData()->TotalVtxCount > 0, "Inspector and transport controls generate UI draw data");
    bool rasterMatches = false, rayMatches = false, tlasMatches = false;
    // GeometryPass consumes and clears OpaqueSurfaces. Re-run the same scene
    // traversal while paused; ray/TLAS records below remain from the GPU frame.
    DrawContext rasterContext;
    fixture->Draw(glm::mat4(1.f), rasterContext);
    for (const auto& object : rasterContext.OpaqueSurfaces) {
        if (object.temporalID == first->temporalID) rasterMatches = same_matrix(object.transform, first->worldTransform);
    }
    for (const auto& object : mainDrawContext.RayTracingInstances) {
        if (object.mesh == first->mesh.get() && same_matrix(object.transform, first->worldTransform)) rayMatches = true;
    }
    for (const auto& instance : _lastRayTracingInstances) {
        if (same_matrix(instance.transform, first->worldTransform)) tlasMatches = true;
    }
    check(rasterMatches, "raster traversal matches parented scripted world transform");
    check(rayMatches, "ray-tracing draw collection matches scripted world transform");
    check(tlasMatches, "submitted TLAS instances match scripted world transform");
    check(lightSystem.lights().size() == 1 && std::abs(lightSystem.lights()[0].colorIntensity.w - source->light.intensity) < 0.001f,
        "GPU light collection sees script-updated intensity");
    double imageDifference = 0;
    for (size_t i = 0; i < before.size(); ++i) imageDifference += std::abs(int(before[i]) - int(after[i]));
    imageDifference /= before.size() * 255.0;
    check(imageDifference > 0.001, "actual rendered output changes under movement and light scripts");
    sceneScripts.stop();
    taaPass.reset_history();
    frames(48, 0);
    capture("restored");
    check(same_matrix(firstInitialWorld, first->worldTransform), "Stop restoration reaches parented renderer object and world cache");

    const auto oldHandle = firstHandle;
    auto emptyScene = std::make_shared<LoadedScene>();
    emptyScene->creator = this;
    emptyScene->materialDataBuffer = {};
    const auto alternate = (output / "alternate.pfb").generic_string();
    loadedScenes[alternate] = emptyScene;
    check(bool(sceneScripts.play()), "restart before switching scene");
    sceneScripts.update(0.5);
    set_active_scene(alternate);
    check(!host.is_valid(oldHandle), "scene switch invalidates old runtime handles");
    check(same_matrix(firstInitial, first->localTransform), "scene switch stops and restores outgoing scene");
    set_active_scene(fixtureName);
    check(sceneScripts.attachments().size() == 3, "scene switch retains in-memory script configuration");
    check(host.handle_for(first) != oldHandle, "returning to a scene allocates fresh runtime handles");

    nlohmann::json summary = {{"checks", checks}, {"failures", failures},
        {"mean_absolute_display_rgb_difference", imageDifference}, {"captures", {"stopped.png", "playing.png", "restored.png"}},
        {"width", _drawExtent.width}, {"height", _drawExtent.height}};
    std::ofstream(output / "report.json") << summary.dump(2);
    fmt::println("Scripting integration: {} checks, {} failures; image difference {}", checks, failures, imageDifference);
    sceneScripts.set_scene(nullptr);
    return failures ? 1 : 0;
}
