#pragma once
#include "Window.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
class Camera
{
public:
    glm::vec3 position = glm::vec3(0.0f, 0.0f, 3.0f);
    float yaw = -90.0f;
    float pitch = 0.0f;
    float moveSpeed = 2.5f;
    float mouseSensitivity = 0.12f;
    float fieldOfView = 60.0f;

    glm::vec3 GetForward() const
    {
        float y = glm::radians(yaw);
        float p = glm::radians(std::clamp(pitch, -89.0f, 89.0f));
        return glm::normalize(glm::vec3(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p)));
    }

    glm::mat4 GetViewMatrix() const
    {
        return glm::lookAt(position, position + GetForward(), glm::vec3(0.0f, 1.0f, 0.0f));
    }

    glm::mat4 GetProjectionMatrix(float aspect) const
    {
        float fov = std::clamp(fieldOfView, 1.0f, 120.0f);
        return glm::perspective(glm::radians(fov), aspect > 0.0f ? aspect : 1.0f, 0.1f, 100.0f);
    }

    void Update(Window& window, float deltaTime, bool mouseBlocked = false)
    {
        unsigned int revision = window.GetFocusRevision();
        bool focused = window.IsFocused();
        if (revision != focusRevision || !focused)
        {
            focusRevision = revision;
            if (looking) window.SetCursorCaptured(false);
            looking = false;
            needsMouseSample = true;
            waitForRelease = false;
            if (!focused) return;
        }
        bool rightDown = window.IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT);
        if (!rightDown) waitForRelease = false;
        if (mouseBlocked)
        {
            if (looking) window.SetCursorCaptured(false);
            looking = false;
            needsMouseSample = true;
            if (rightDown) waitForRelease = true;
        }
        bool wantsLook = rightDown && !waitForRelease && !mouseBlocked;
        bool changed = wantsLook != looking;
        if (changed)
        {
            looking = wantsLook;
            window.SetCursorCaptured(looking);
            needsMouseSample = true;
        }

        bool paused = !std::isfinite(deltaTime) || deltaTime <= 0.0f || deltaTime > 0.1f;
        if (looking && !changed)
        {
            double x = 0.0, y = 0.0;
            window.GetCursorPosition(x, y);
            if (std::isfinite(x) && std::isfinite(y))
            {
                if (!needsMouseSample && !paused)
                {
                    double dx = x - lastX, dy = y - lastY;
                    yaw = static_cast<float>(std::remainder(static_cast<double>(yaw) + dx * mouseSensitivity, 360.0));
                    pitch = static_cast<float>(std::clamp(static_cast<double>(pitch) - dy * mouseSensitivity, -89.0, 89.0));
                }
                lastX = x;
                lastY = y;
                needsMouseSample = false;
            }
            else needsMouseSample = true;
        }
        if (paused) return;

        glm::vec3 forward = GetForward();
        glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
        glm::vec3 direction(0.0f);
        if (window.IsKeyDown(GLFW_KEY_W)) direction += forward;
        if (window.IsKeyDown(GLFW_KEY_S)) direction -= forward;
        if (window.IsKeyDown(GLFW_KEY_D)) direction += right;
        if (window.IsKeyDown(GLFW_KEY_A)) direction -= right;
        if (window.IsKeyDown(GLFW_KEY_E)) direction.y += 1.0f;
        if (window.IsKeyDown(GLFW_KEY_Q)) direction.y -= 1.0f;

        float length = glm::length(direction);
        if (length > 0.0001f)
        {
            float speed = window.IsKeyDown(GLFW_KEY_LEFT_SHIFT) ? moveSpeed * 3.0f : moveSpeed;
            position += direction / length * speed * std::clamp(deltaTime, 0.0f, 0.1f);
        }
    }

private:
    bool looking = false;
    bool needsMouseSample = true;
    bool waitForRelease = false;
    unsigned int focusRevision = 0;
    double lastX = 0.0, lastY = 0.0;
};



