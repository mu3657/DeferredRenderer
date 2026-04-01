#pragma once
#include <vk_types.h>
#include <SDL_events.h>

// DCC-style camera (Blender / Maya convention):
//   RMB held          → FPS fly-through (WASD + look, mouse captured)
//   Alt + LMB drag    → Orbit around pivot
//   Alt + MMB drag    → Pan
//   Alt + RMB drag    → Dolly (zoom in/out)
//   Scroll wheel      → Dolly
//   Numpad 0          → Reset to default position
class Camera {
public:
    // --- State ---
    glm::vec3 position  { 0.f, 0.f, 5.f };
    float     pitch     { 0.f };   // radians, vertical
    float     yaw       { 0.f };   // radians, horizontal

    // Orbit pivot (world-space point the camera orbits around)
    glm::vec3 pivot     { 0.f, 0.f, 0.f };

    // --- Tuning ---
    float moveSpeed     { 5.f };    // FPS fly speed (units/s)
    float sensitivity   { 0.003f }; // look sensitivity (rad/px)
    float orbitSensitivity { 0.005f }; // orbit drag speed
    float panSensitivity   { 0.01f };  // pan drag speed
    float zoomSensitivity  { 0.5f };   // scroll zoom step

    // --- Computed each frame ---
    glm::mat4 getViewMatrix();
    glm::mat4 getRotationMatrix();

    // Called once per SDL_PollEvent
    void processSDLEvent(SDL_Event& e);

    // Called once per frame with deltaTime in seconds
    void update(float deltaTime, struct SDL_Window* window);

    // Focus camera on a world-space point
    void focusOn(const glm::vec3& target, float distance = 5.f);

private:
    // --- Input state ---
    bool _rmbDown   { false };
    bool _altDown   { false };
    bool _lmbDown   { false };
    bool _mmbDown   { false };

    // Which DCC mode is active this drag?
    enum class DragMode { None, Orbit, Pan, Dolly, Fly };
    DragMode _dragMode { DragMode::None };

    bool _mouseCaptured { false };

    void startCapture(struct SDL_Window* window);
    void stopCapture(struct SDL_Window* window);

    // Recompute position from pivot + pitch/yaw + distance
    void applyOrbit(float dYaw, float dPitch);
    void applyPan(float dx, float dy);
    void applyDolly(float delta);

    float _orbitDist { 10.f }; // distance from pivot during orbit
};