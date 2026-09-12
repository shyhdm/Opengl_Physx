#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Transform
{
public:
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 rotation = glm::vec3(0.0f); // 欧拉角，单位为度。
    glm::vec3 scale = glm::vec3(1.0f);

    glm::mat4 GetMatrix() const
    {
        glm::mat4 matrix = glm::translate(glm::mat4(1.0f), position);
        matrix = glm::rotate(matrix, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        matrix = glm::rotate(matrix, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        matrix = glm::rotate(matrix, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        matrix = glm::scale(matrix, scale);
        // 列向量约定：T * Rz * Ry * Rx * S。
        // 顶点先缩放，再绕 X/Y/Z 轴旋转，最后平移。
        return matrix;
    }
};
