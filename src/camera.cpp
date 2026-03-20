#include "camera.h"
#include <glm/gtx/transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>

void Camera::update(float deltaTime)
{
    const Uint8* state = SDL_GetKeyboardState(NULL);
    glm::vec3 inputVelocity(0.f);
    if (state[SDL_SCANCODE_W]) inputVelocity.z -= 10.f;
    if (state[SDL_SCANCODE_S]) inputVelocity.z += 10.f;
    if (state[SDL_SCANCODE_A]) inputVelocity.x -= 10.f;
    if (state[SDL_SCANCODE_D]) inputVelocity.x += 10.f;


    // if (glm::length(inputVelocity) > 0.1f) {
    //     inputVelocity = glm::normalize(inputVelocity);
    // }

    glm::mat4 cameraRotation = getRotationMatrix();
    position += glm::vec3(cameraRotation * glm::vec4(inputVelocity, 0.f)) * moveSpeed * deltaTime;
}

void Camera::processSDLEvent(SDL_Event& e)
{

    if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
        if (e.key.keysym.sym == SDLK_LALT || e.key.keysym.sym == SDLK_RALT) {
            isAltPressed = (e.type == SDL_KEYDOWN);
        }
    }

    if (e.type == SDL_MOUSEMOTION && !isAltPressed) {
        yaw += (float)e.motion.xrel * sensitivity;
        pitch -= (float)e.motion.yrel * sensitivity;

        // 限制俯仰角
        pitch = std::clamp(pitch, -1.55f, 1.55f);
    }

}

glm::mat4 Camera::getViewMatrix()
{
    // to create a correct model view, we need to move the world in opposite
    // direction to the camera
    //  so we will create the camera model matrix and invert
    glm::mat4 cameraTranslation = glm::translate(glm::mat4(1.f), position);
    glm::mat4 cameraRotation = getRotationMatrix();
    return glm::inverse(cameraTranslation * cameraRotation);
}

glm::mat4 Camera::getRotationMatrix()
{
    // fairly typical FPS style camera. we join the pitch and yaw rotations into
    // the final rotation matrix

    glm::quat pitchRotation = glm::angleAxis(pitch, glm::vec3 { 1.f, 0.f, 0.f });
    glm::quat yawRotation = glm::angleAxis(yaw, glm::vec3 { 0.f, -1.f, 0.f });

    return glm::toMat4(yawRotation) * glm::toMat4(pitchRotation);
}