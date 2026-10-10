#pragma once

#include "CRenderable.h"
#include "PuppetRig.h"
#include "PuppetMeshParser.h"
#include "PuppetClipping.h"
#include "WallpaperEngine/Render/TextureAnimation.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include "WallpaperEngine/Render/Shaders/Shader.h"

#include "../TextureProvider.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <glm/vec3.hpp>
#include <vector>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Scripting;
namespace WallpaperEngine::Render::Objects::Effects {
class CMaterial;
class CPass;
} // namespace WallpaperEngine::Render::Objects::Effects

namespace WallpaperEngine::Render::Objects {
class CImage final : public CRenderable, public ScriptableObject {
    friend CObject;

public:
    CImage (Wallpapers::CScene& scene, const Image& image);
    ~CImage () override;

    void setup () override;
    void render () override;
    void prepareScriptFrame ();
    PuppetRig& getRig () { return m_rig; }
    [[nodiscard]] glm::mat4 puppetWorld () const;
    void registerAnimationLayerProperties (size_t serial);
    std::shared_ptr<ImageTextureAnimation> getTextureAnimation ();
    const Data::Assets::Frame* getTextureAnimationFrame (const TextureProvider& texture) const;

    [[nodiscard]] const Image& getImage () const;
    [[nodiscard]] glm::vec2 getSize () const;

    [[nodiscard]] GLuint getSceneSpacePosition () const;
    [[nodiscard]] GLuint getCopySpacePosition () const;
    [[nodiscard]] GLuint getPassSpacePosition () const;
    [[nodiscard]] GLuint getTexCoordCopy () const;
    [[nodiscard]] GLuint getTexCoordPass () const;

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;

    /**
     * Performs a ping-pong on the available framebuffers to be able to continue rendering things to them
     *
     * @param drawTo The framebuffer to use
     * @param asInput The last texture used as output (if needed)
     */
    void pinpongFramebuffer (std::shared_ptr<const CFBO>* drawTo, std::shared_ptr<const TextureProvider>* asInput);

protected:
    void setupPasses ();

    void updateScreenSpacePosition ();

private:
    std::optional<PuppetMeshData> m_puppetChannelMesh;
    MaterialUniquePtr m_puppetChannelMaterial;
    std::unique_ptr<MaterialPass> m_puppetChannelBaseMaterial;
    Effects::CPass* m_puppetChannelBasePass = nullptr;
    Effects::CPass* m_puppetChannelPass = nullptr;
    std::shared_ptr<CFBO> m_puppetChannelFBO;
    std::shared_ptr<const CFBO> m_puppetChannelDirectTarget;
    bool m_puppetChannelOffscreen = false;
    GLuint m_puppetChannelPosition = GL_NONE, m_puppetChannelTexcoord = GL_NONE;
    GLuint m_puppetChannelBlendIndices = GL_NONE, m_puppetChannelIndices = GL_NONE;
    GLsizei m_puppetChannelIndexCount = 0;
    glm::mat4 m_puppetChannelProjection {1}, m_puppetChannelProjectionInverse {1};
    std::array<glm::vec4, 4> m_puppetBlendMap {};
    size_t m_puppetBlendRows = 0, m_basePassCount = 0;
    const glm::vec4 m_puppetPrepassColor {1};
    glm::vec4 m_effectiveColor4 {1};
    struct PuppetBone {
	int parent = -1;
	glm::mat4 localBind = glm::mat4 (1.0f);
	glm::mat4 inverseBindWorld = glm::mat4 (1.0f);
    };

    struct PuppetAnimationFrame {
	glm::vec3 position = {};
	glm::vec3 rotation = {};
	glm::vec3 scale = glm::vec3 (1.0f);
    };

    struct PuppetAnimation {
	uint32_t id = 0;
	std::string name = {};
	bool loop = true;
	float fps = 30.0f;
	/** keyframes indexed [bone][frame] */
	std::vector<std::vector<PuppetAnimationFrame>> tracks = {};
    };

    void loadPuppetChannelMesh (const PuppetMeshData& mesh);
    void clearPuppetChannel ();
    void setupPuppetChannelGeometryCallback (Effects::CPass* pass, GLuint position) const;
    void renderPuppetChannelPrepass ();
    void renderPuppetChannelDirect (const std::shared_ptr<const CFBO>& target);
    bool loadPuppetMesh (const glm::vec2& size);
    bool loadPuppetAnimationData (const std::vector<char>& data, size_t mdlsOffset);
    void preparePuppetBones ();
    void updatePuppetAnimation ();
    void evaluatePuppetSkin (const PuppetAnimation& animation, float framePosition, std::vector<glm::mat4>& skin) const;
    [[nodiscard]] glm::vec2 computePuppetCanvasSize (const glm::vec2& size) const;
    void updatePuppetScenePositionBuffer (const glm::vec2& size, const std::vector<GLfloat>& rawPositions);
    void setupPuppetGeometryCallback (Effects::CPass* pass, const GLuint* positionBuffer, bool sourceTexture) const;
    void setupPuppetClipping (const std::shared_ptr<const TextureProvider>& input);
    void renderPuppetClipped (Effects::CPass* pass);
    void renderPuppetClipMask (int record, int draw, bool intermediate);
    void rebuildPuppetClipIndices ();
    ResolvedTransform updateGeometryBuffers ();
    [[nodiscard]] glm::vec2 resolveGeometrySize (float sceneWidth, float sceneHeight, glm::vec3& origin) const;
    void updateScenePosition (
	const glm::vec3& origin, const glm::vec2& size, const glm::vec3& scale, float sceneWidth, float sceneHeight
    );
    void uploadGeometryBuffers (const glm::vec2& size);
    [[nodiscard]] bool shouldRenderFinalPass (bool isLastPass) const;
    bool configurePassTarget (
	Effects::CPass* pass, std::shared_ptr<const CFBO>& drawTo,
	const std::shared_ptr<const TextureProvider>& asInput, std::shared_ptr<const TextureProvider>& effectInput,
	bool& inTargetEffectSequence
    );

    GLuint m_sceneSpacePosition;
    GLuint m_copySpacePosition;
    GLuint m_passSpacePosition;
    GLuint m_texcoordCopy;
    GLuint m_texcoordPass;
    GLuint m_puppetSceneSpacePosition = GL_NONE;
    GLuint m_puppetTexCoord = GL_NONE;
    GLuint m_puppetEffectTexCoord = GL_NONE;
    GLuint m_puppetIndices = GL_NONE;
    GLuint m_puppetAlphaWeights = GL_NONE;
    GLsizei m_puppetIndexCount = 0;
    bool m_hasPuppetMesh = false;
    std::vector<GLfloat> m_puppetRawPositions = {};
    std::vector<glm::uvec4> m_puppetVertexBones = {};
    std::vector<glm::vec4> m_puppetVertexWeights = {};
    std::vector<GLfloat> m_puppetSkinnedPositions = {};
    uint32_t m_puppetMeshFlags = 0;
    std::vector<float> m_puppetMorphIndices;
    std::optional<PuppetMorphData> m_puppetMorph;
    bool m_puppetVertexAlpha = false;
    std::vector<PuppetMeshData::BoneRange> m_puppetParts;
    std::vector<uint32_t> m_puppetPartOrder;
    std::optional<PuppetClipping> m_puppetClipping;
    std::vector<uint16_t> m_puppetMeshIndices;
    GLuint m_puppetClipIndices = GL_NONE;
    int m_puppetClipDraw = -1;
    Effects::CPass* m_puppetMeshPass = nullptr;
    std::unique_ptr<Effects::CPass> m_puppetClipTargetPass;
    std::vector<std::unique_ptr<Effects::CPass>> m_puppetClipMaskPasses;
    std::unique_ptr<Effects::CPass> m_puppetClipComposePass;
    std::shared_ptr<const CFBO> m_puppetClipMask;
    std::shared_ptr<const CFBO> m_puppetClipIntermediate;
    glm::vec4 m_puppetClipRenderVar0 = {};
    glm::mat4 m_puppetClipIdentity = glm::mat4 (1);
    PuppetRig m_rig;
    std::shared_ptr<ImageTextureAnimation> m_textureAnimation;
    std::vector<PuppetBone> m_puppetBones = {};
    std::vector<PuppetAnimation> m_puppetAnimations = {};
    glm::vec2 m_puppetSize = {};

    glm::mat4 m_modelViewProjectionScreen = {};
    glm::mat4 m_modelViewProjectionPass = {};
    glm::mat4 m_modelViewProjectionCopy = {};
    glm::mat4 m_modelViewProjectionScreenInverse = {};
    glm::mat4 m_modelViewProjectionPassInverse = {};
    glm::mat4 m_modelViewProjectionCopyInverse = {};

    glm::mat4 m_modelMatrix = {};
    glm::mat4 m_viewProjectionMatrix = {};

    std::shared_ptr<const CFBO> m_mainFBO = nullptr;
    std::shared_ptr<const CFBO> m_subFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentMainFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentSubFBO = nullptr;

    const Image& m_image;

    std::vector<Effects::CPass*> m_passes = {};
    std::vector<Effects::CPass*> m_activePasses;
    std::map<Effects::CPass*, const ImageEffect*> m_passEffects;
    BlendingMode m_baseBlending = BlendingMode_Normal;
    std::vector<MaterialPassUniquePtr> m_virtualPassess = {};

    glm::vec4 m_pos = {};
    glm::vec3 m_sceneCenter = {};
    glm::vec2 m_size = {};

    bool m_initialized = false;

    struct {
	struct {
	    MaterialUniquePtr material;
	    ImageEffectPassOverrideUniquePtr override;
	} colorBlending;
	std::vector<MaterialUniquePtr> compatibilityMaterials = {};
	std::vector<ImageEffectPassOverrideUniquePtr> compatibilityOverrides = {};
    } m_materials;
};
} // namespace WallpaperEngine::Render::Objects
