#pragma once
#include "Camera.h"
#include "Shader.h"
#include "Mesh.h"
#include "Material.h"
#include "ShadowMap.h"
#include <vector>

class ModelRenderer
{
public:
    // 平行光的传播方向；高光由相机位置决定。
    glm::vec3 lightDirection = glm::vec3(-0.4f, -0.8f, -0.6f);
    glm::vec3 lightColor = glm::vec3(1.0f);
    float ambientStrength = 0.2f;
    float diffuseStrength = 0.8f;

    bool shadowsEnabled = true;
    float shadowStrength = 0.85f;
    float shadowBias = 0.001f;
    int shadowFilterRadius = 1;

    ModelRenderer() : shader("Assets/Shaders/model.glsl", { "shadow", "lit" }) {}

    void BeginShadowPass()
    {
        shadowReady = false;
        if (!shadowsEnabled) return;
        glm::vec3 direction = glm::length(lightDirection) > 0.0001f ? glm::normalize(lightDirection) : glm::vec3(0, -1, 0);
        glm::vec3 up = std::abs(direction.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        // 固定覆盖场景中心，避免相机移动导致阴影采样抖动。
        lightSpaceMatrix = glm::ortho(-18.0f, 18.0f, -18.0f, 18.0f, 1.0f, 100.0f) * glm::lookAt(-direction * 40.0f, glm::vec3(0), up);
        shadowMap.Begin();
        shader.UsePass("shadow");
        shader.SetMatrix4("lightSpaceMatrix", lightSpaceMatrix);
    }

    void DrawShadow(const Mesh& mesh, const glm::mat4& model)
    {
        if (!shadowsEnabled) return;
        shader.SetBool("indexedTransforms", false);
        shader.SetBool("instanced", false);
        shader.SetMatrix4("model", model);
        mesh.Draw(shader.GetProgram());
    }

    void DrawShadowInstanced(const Mesh& mesh, const std::vector<glm::mat4>& instances)
    {
        if (!shadowsEnabled) return;
        if (shader.GetUniformLocation("instanced") < 0)
        {
            for (const glm::mat4& instance : instances) DrawShadow(mesh, instance);
            return;
        }
        shader.SetBool("indexedTransforms", false);
        shader.SetBool("instanced", true);
        shader.SetMatrix4("model", glm::mat4(1.0f));
        mesh.DrawInstanced(shader.GetProgram());
        shader.SetBool("instanced", false);
    }

    bool SupportsIndexedTransforms() const
    {
        return shader.GetUniformLocation("indexedTransforms") >= 0 && shader.GetUniformLocation("transformMatrices") >= 0;
    }

    void DrawShadowIndexed(const Mesh& mesh)
    {
        if (!shadowsEnabled) return;
        shader.SetBool("instanced", false);
        shader.SetBool("indexedTransforms", true);
        shader.SetMatrix4("model", glm::mat4(1.0f));
        mesh.DrawIndexedTransforms(shader.GetProgram());
        shader.SetBool("indexedTransforms", false);
    }

    void EndShadowPass()
    {
        if (shadowsEnabled) shadowMap.End();
        shadowReady = shadowsEnabled;
    }

    void BeginDraw(const Camera& camera, int width, int height) const
    {
        if (width <= 0 || height <= 0) return;
        glViewport(0, 0, width, height);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        viewProjection = camera.GetProjectionMatrix(static_cast<float>(width) / height) * camera.GetViewMatrix();
        shader.UsePass("lit");
        shader.SetMatrix4("lightSpaceMatrix", lightSpaceMatrix);
        shader.SetBool("hasShadowMap", shadowsEnabled && shadowReady);
        shader.SetInt("shadowMap", 1);
        shader.SetFloat("shadowStrength", shadowStrength);
        shader.SetFloat("shadowBias", shadowBias);
        shader.SetInt("shadowFilterRadius", shadowFilterRadius);
        shadowMap.Bind();
        shader.SetVector3("cameraPosition", camera.position);
        shader.SetVector3("lightDirection", lightDirection);
        shader.SetVector3("lightColor", lightColor);
        shader.SetFloat("ambientStrength", ambientStrength);
        shader.SetFloat("diffuseStrength", diffuseStrength);
    }

    void DrawMesh(const Mesh& mesh, const glm::mat4& model, const Material& material = Material{}) const
    {
        shader.SetBool("indexedTransforms", false);
        shader.SetBool("instanced", false);
        shader.SetMatrix4("mvp", viewProjection * model);
        shader.SetMatrix4("model", model);
        shader.SetVector3("materialColor", material.baseColor);
        shader.SetFloat("specularStrength", material.specularStrength);
        shader.SetFloat("shininess", material.shininess);
        shader.SetBool("hasBaseTexture", material.baseTexture != nullptr);
        shader.SetInt("baseTexture", 0);
        shader.SetVector2("textureTiling", material.textureTiling);
        if (material.baseTexture) material.baseTexture->Bind();
        else { GL::ActiveTexture(0x84C0); glBindTexture(GL_TEXTURE_2D, 0); }
        mesh.Draw(shader.GetProgram());
    }

    void DrawMeshInstanced(const Mesh& mesh, const std::vector<glm::mat4>& instances, const Material& material = Material{}) const
    {
        if (shader.GetUniformLocation("instanced") < 0)
        {
            for (const glm::mat4& instance : instances) DrawMesh(mesh, instance, material);
            return;
        }
        shader.SetBool("indexedTransforms", false);
        shader.SetBool("instanced", true);
        shader.SetMatrix4("mvp", viewProjection);
        shader.SetMatrix4("model", glm::mat4(1.0f));
        shader.SetVector3("materialColor", material.baseColor);
        shader.SetFloat("specularStrength", material.specularStrength);
        shader.SetFloat("shininess", material.shininess);
        shader.SetBool("hasBaseTexture", material.baseTexture != nullptr);
        shader.SetInt("baseTexture", 0);
        shader.SetVector2("textureTiling", material.textureTiling);
        if (material.baseTexture) material.baseTexture->Bind();
        else { GL::ActiveTexture(0x84C0); glBindTexture(GL_TEXTURE_2D, 0); }
        mesh.DrawInstanced(shader.GetProgram());
        shader.SetBool("instanced", false);
    }

    void DrawMeshIndexed(const Mesh& mesh, const Material& material = Material{}) const
    {
        shader.SetBool("instanced", false);
        shader.SetBool("indexedTransforms", true);
        shader.SetMatrix4("mvp", viewProjection);
        shader.SetMatrix4("model", glm::mat4(1.0f));
        shader.SetVector3("materialColor", material.baseColor);
        shader.SetFloat("specularStrength", material.specularStrength);
        shader.SetFloat("shininess", material.shininess);
        shader.SetBool("hasBaseTexture", material.baseTexture != nullptr);
        shader.SetInt("baseTexture", 0);
        shader.SetVector2("textureTiling", material.textureTiling);
        if (material.baseTexture) material.baseTexture->Bind();
        else { GL::ActiveTexture(0x84C0); glBindTexture(GL_TEXTURE_2D, 0); }
        mesh.DrawIndexedTransforms(shader.GetProgram());
        shader.SetBool("indexedTransforms", false);
    }

    void Draw(const Mesh& mesh, const Camera& camera, int width, int height, const glm::mat4& model, const Material& material = Material{}) const
    {
        if (width <= 0 || height <= 0) return;
        BeginDraw(camera, width, height);
        DrawMesh(mesh, model, material);
    }

private:
    mutable glm::mat4 viewProjection = glm::mat4(1);
    mutable Shader shader;
    ShadowMap shadowMap;
    glm::mat4 lightSpaceMatrix = glm::mat4(1.0f);
    bool shadowReady = false;
};
