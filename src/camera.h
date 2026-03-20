#pragma once
#include <vk_types.h>
#include <SDL_events.h>

class Camera {
public:
    glm::vec3 velocity;
    glm::vec3 position;
    // vertical rotation
    float pitch { 0.f };
    // horizontal rotation
    float yaw { 0.f };

    // 属性配置
    float moveSpeed { 5.0f };     // 每秒移动单位
    float sensitivity { 0.002f }; // 鼠标灵敏度

    bool isAltPressed = false;
    glm::mat4 getViewMatrix();
    glm::mat4 getRotationMatrix();

    void processSDLEvent(SDL_Event& e);

    void update(float deltaTime);
};