#pragma once
// Maya 風オービットカメラ
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

struct OrbitCamera {
    glm::vec3 target{ 0.0f };
    float yaw = glm::radians(35.0f);
    float pitch = glm::radians(-20.0f);
    float distance = 5.0f;
    float fovY = glm::radians(45.0f);
    float nearClip = 0.01f;
    float farClip = 1000.0f;
    bool ortho = false;

    glm::vec3 Forward() const {
        // yaw=0, pitch=0 で -Z 方向を向く。pitch が負なら見下ろす
        return glm::normalize(glm::vec3(-std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw)));
    }
    glm::vec3 Position() const { return target - Forward() * distance; }
    glm::mat4 View() const { return glm::lookAt(Position(), target, glm::vec3(0, 1, 0)); }
    glm::mat4 Projection(float aspect) const {
        if (ortho) {
            float h = distance * std::tan(fovY * 0.5f);
            return glm::ortho(-h * aspect, h * aspect, -h, h, -farClip, farClip);
        }
        return glm::perspective(fovY, aspect, nearClip, farClip);
    }
    void Orbit(float dx, float dy) {
        yaw -= dx;
        pitch = std::clamp(pitch - dy, glm::radians(-89.5f), glm::radians(89.5f));
    }
    void Pan(float dx, float dy, float viewportHeight) {
        glm::vec3 f = Forward();
        glm::vec3 right = glm::normalize(glm::cross(f, glm::vec3(0, 1, 0)));
        glm::vec3 up = glm::cross(right, f);
        float scale = 2.0f * distance * std::tan(fovY * 0.5f) / std::max(viewportHeight, 1.0f);
        target += (-right * dx + up * dy) * scale;
    }
    void Dolly(float amount) { distance = std::max(distance * std::pow(0.85f, amount), 1e-4f); }
    void Frame(const glm::vec3& mn, const glm::vec3& mx) {
        target = (mn + mx) * 0.5f;
        float radius = std::max(glm::length(mx - mn) * 0.5f, 1e-3f);
        distance = radius / std::sin(fovY * 0.5f) * 1.1f;
        nearClip = std::max(distance * 0.001f, 1e-5f);
        farClip = distance * 100.0f + radius * 10.0f;
    }
    void SetView(float yawDeg, float pitchDeg) {
        yaw = glm::radians(yawDeg);
        pitch = glm::radians(pitchDeg);
    }
};
