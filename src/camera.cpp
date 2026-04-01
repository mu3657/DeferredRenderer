#include "camera.h"
#include <glm/gtx/transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <SDL.h>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
glm::mat4 Camera::getRotationMatrix()
{
    glm::quat pitchQ = glm::angleAxis(pitch, glm::vec3(1.f, 0.f, 0.f));
    glm::quat yawQ   = glm::angleAxis(yaw,   glm::vec3(0.f, -1.f, 0.f));
    return glm::toMat4(yawQ) * glm::toMat4(pitchQ);
}

glm::mat4 Camera::getViewMatrix()
{
    glm::mat4 t = glm::translate(glm::mat4(1.f), position);
    glm::mat4 r = getRotationMatrix();
    return glm::inverse(t * r);
}

void Camera::focusOn(const glm::vec3& target, float distance)
{
    pivot        = target;
    _orbitDist   = distance;
    glm::vec3 fwd = glm::vec3(getRotationMatrix() * glm::vec4(0.f, 0.f, -1.f, 0.f));
    position     = pivot - fwd * _orbitDist;
}

// ─────────────────────────────────────────────────────────────────────────────
// startCapture / stopCapture
// ─────────────────────────────────────────────────────────────────────────────
void Camera::startCapture(SDL_Window* window)
{
    if (!_mouseCaptured) {
        SDL_SetRelativeMouseMode(SDL_TRUE);
        _mouseCaptured = true;
    }
}

void Camera::stopCapture(SDL_Window* window)
{
    if (_mouseCaptured) {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        _mouseCaptured = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Orbit  – rotate around pivot, reposition camera
// ─────────────────────────────────────────────────────────────────────────────
void Camera::applyOrbit(float dYaw, float dPitch)
{
    yaw   += dYaw;
    pitch += dPitch;
    pitch  = std::clamp(pitch, -1.55f, 1.55f);

    // Camera position = pivot + back-vector * distance
    glm::vec3 fwd = glm::vec3(getRotationMatrix() * glm::vec4(0.f, 0.f, -1.f, 0.f));
    position = pivot - fwd * _orbitDist;
}

// ─────────────────────────────────────────────────────────────────────────────
// Pan  – translate both camera and pivot in camera-right / camera-up plane
// ─────────────────────────────────────────────────────────────────────────────
void Camera::applyPan(float dx, float dy)
{
    glm::mat4 rot   = getRotationMatrix();
    glm::vec3 right = glm::vec3(rot[0]);   // first column
    glm::vec3 up    = glm::vec3(rot[1]);   // second column

    // scale pan speed by orbit distance so far objects don't fly off screen
    float scale = _orbitDist * panSensitivity;
    glm::vec3 delta = -right * dx * scale + up * dy * scale;

    position += delta;
    pivot    += delta;
}

// ─────────────────────────────────────────────────────────────────────────────
// Dolly  – move along look direction, update orbital distance
// ─────────────────────────────────────────────────────────────────────────────
void Camera::applyDolly(float delta)
{
    glm::vec3 fwd = glm::vec3(getRotationMatrix() * glm::vec4(0.f, 0.f, -1.f, 0.f));
    _orbitDist = std::max(0.1f, _orbitDist - delta * zoomSensitivity * (_orbitDist * 0.1f + 0.5f));
    position = pivot - fwd * _orbitDist;
}

// ─────────────────────────────────────────────────────────────────────────────
// processSDLEvent
// ─────────────────────────────────────────────────────────────────────────────
void Camera::processSDLEvent(SDL_Event& e)
{
    // ── Modifier keys ──────────────────────────────────────────────────────
    if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
        bool down = (e.type == SDL_KEYDOWN);
        if (e.key.keysym.sym == SDLK_LALT || e.key.keysym.sym == SDLK_RALT)
            _altDown = down;
    }

    // ── Mouse buttons ──────────────────────────────────────────────────────
    if (e.type == SDL_MOUSEBUTTONDOWN) {
        if (e.button.button == SDL_BUTTON_LEFT  && _altDown) {
            _lmbDown  = true;
            _dragMode = DragMode::Orbit;
        }
        if (e.button.button == SDL_BUTTON_MIDDLE) {
            _mmbDown  = true;
            _dragMode = _altDown ? DragMode::Pan : DragMode::Pan;
        }
        if (e.button.button == SDL_BUTTON_RIGHT && _altDown) {
            // Alt + RMB = dolly
            _rmbDown  = true;
            _dragMode = DragMode::Dolly;
        }
        if (e.button.button == SDL_BUTTON_RIGHT && !_altDown) {
            // Plain RMB = FPS fly
            _rmbDown  = true;
            _dragMode = DragMode::Fly;
        }
    }

    if (e.type == SDL_MOUSEBUTTONUP) {
        if (e.button.button == SDL_BUTTON_LEFT)   { _lmbDown = false; }
        if (e.button.button == SDL_BUTTON_MIDDLE)  { _mmbDown = false; }
        if (e.button.button == SDL_BUTTON_RIGHT)   { _rmbDown = false; }

        // Reset drag mode when no relevant button is held
        if (!_lmbDown && !_mmbDown && !_rmbDown)
            _dragMode = DragMode::None;
    }

    // ── Mouse motion ───────────────────────────────────────────────────────
    if (e.type == SDL_MOUSEMOTION) {
        float dx = (float)e.motion.xrel;
        float dy = (float)e.motion.yrel;

        switch (_dragMode) {
            case DragMode::Orbit:
                applyOrbit(dx * orbitSensitivity, dy * orbitSensitivity);
                break;
            case DragMode::Pan:
                applyPan(dx, dy);
                break;
            case DragMode::Dolly:
                applyDolly(dx - dy);   // rightward / downward = zoom out
                break;
            case DragMode::Fly:
                // FPS look: accumulate into yaw/pitch directly
                yaw   += dx * sensitivity;
                pitch += dy * sensitivity;
                pitch  = std::clamp(pitch, -1.55f, 1.55f);
                break;
            default: break;
        }
    }

    // ── Scroll wheel: dolly ────────────────────────────────────────────────
    if (e.type == SDL_MOUSEWHEEL) {
        applyDolly((float)e.wheel.y * 3.f);   // positive scroll = zoom in
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// update  – per-frame logic (FPS movement + mouse capture management)
// ─────────────────────────────────────────────────────────────────────────────
void Camera::update(float deltaTime, SDL_Window* window)
{
    // Mouse capture: active only while in Fly or any drag mode
    bool needsCapture = (_dragMode != DragMode::None);
    if (needsCapture && !_mouseCaptured)  startCapture(window);
    if (!needsCapture && _mouseCaptured)  stopCapture(window);

    // FPS fly movement (WASD) — only during Fly mode
    if (_dragMode == DragMode::Fly) {
        const Uint8* state = SDL_GetKeyboardState(nullptr);
        glm::vec3 input(0.f);
        if (state[SDL_SCANCODE_W]) input.z -= 1.f;
        if (state[SDL_SCANCODE_S]) input.z += 1.f;
        if (state[SDL_SCANCODE_A]) input.x -= 1.f;
        if (state[SDL_SCANCODE_D]) input.x += 1.f;
        if (state[SDL_SCANCODE_E]) input.y += 1.f;
        if (state[SDL_SCANCODE_Q]) input.y -= 1.f;

        // Shift to sprint
        if (state[SDL_SCANCODE_LSHIFT]) input *= 3.f;

        if (glm::length(input) > 0.01f) {
            glm::mat4 rot = getRotationMatrix();
            glm::vec3 move = glm::vec3(rot * glm::vec4(glm::normalize(input), 0.f));
            position += move * moveSpeed * deltaTime;
            // Update pivot so orbit after fly still makes sense
            glm::vec3 fwd = glm::vec3(rot * glm::vec4(0.f, 0.f, -1.f, 0.f));
            pivot = position + fwd * _orbitDist;
        }
    }
}