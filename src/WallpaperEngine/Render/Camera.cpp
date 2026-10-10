#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;

Camera::Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera) :
    m_width (0), m_height (0), m_camera (camera), m_scene (scene) {
    // get the lookat position
    // TODO: ENSURE THIS IS ONLY USED WHEN NOT DOING AN ORTOGRAPHIC CAMERA AS IT THROWS OFF POINTS
    this->m_lookat = glm::lookAt (this->getEye (), this->getCenter (), this->getUp ());
    m_scriptEye = getEye ();
    m_scriptCenter = getCenter ();
    m_scriptUp = getUp ();
}

Camera::~Camera () = default;

const glm::vec3& Camera::getCenter () const { return m_hasScriptTransforms ? m_scriptCenter : m_camera.configuration.center; }

const glm::vec3& Camera::getEye () const { return m_hasScriptTransforms ? m_scriptEye : m_camera.configuration.eye; }

const glm::vec3& Camera::getUp () const { return m_hasScriptTransforms ? m_scriptUp : m_camera.configuration.up; }

const glm::mat4& Camera::getProjection () const { return this->m_projection; }

const glm::mat4& Camera::getLookAt () const { return this->m_lookat; }

bool Camera::isOrthogonal () const { return this->m_isOrthogonal; }

Wallpapers::CScene& Camera::getScene () const { return this->m_scene; }

float Camera::getWidth () const { return this->m_width; }

float Camera::getHeight () const { return this->m_height; }

float Camera::getFov () const { return this->m_camera.projection.fov->value->getFloat (); }

float Camera::getNearZ () const { return this->m_camera.projection.nearz->value->getFloat (); }

float Camera::getFarZ () const { return this->m_camera.projection.farz->value->getFloat (); }

void Camera::setOrthogonalProjection (const float width, const float height) {
    this->m_width = width;
    this->m_height = height;

    float nearz = this->m_camera.projection.nearz->value->getFloat ();
    float farz = this->m_camera.projection.farz->value->getFloat ();

    this->m_projection = glm::ortho<float> (-width / 2.0, width / 2.0, -height / 2.0, height / 2.0, nearz, farz);
    if (m_hasScriptTransforms) {
        m_projection = glm::ortho (-width * .5f, width * .5f, -height * .5f, height * .5f, -2000.0f, 2000.0f)
            * glm::scale (glm::mat4 (1), glm::vec3 (m_scriptZoom, m_scriptZoom, 1));
        const auto bridge = glm::translate (glm::mat4 (1), glm::vec3 (-width * .5f, height * .5f, 0))
            * glm::scale (glm::mat4 (1), glm::vec3 (1, -1, 1));
        m_lookat = bridge * glm::lookAt (m_scriptEye, m_scriptCenter, m_scriptUp) * glm::inverse (bridge);
    } else this->m_projection = glm::translate (this->m_projection, this->getEye ());
    this->m_isOrthogonal = true;
}

void Camera::setTransforms (const glm::vec3* eye, const glm::vec3* center, const glm::vec3* up, const float* zoom) {
    if (!eye && !center && !up && !zoom) return;
    if (eye) m_scriptEye = *eye;
    if (center) m_scriptCenter = *center;
    if (up) m_scriptUp = *up;
    if (zoom) m_scriptZoom = *zoom;
    m_hasScriptTransforms = true;
    setOrthogonalProjection (m_width, m_height);
}
