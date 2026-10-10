#include "CImage.h"
#include "WallpaperEngine/Scripting/PuppetScriptObject.h"

#include "CRenderable.h"
#include "PuppetMeshParser.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <optional>
#include <sstream>
#include <numeric>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/rotate_vector.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Data/Utils/MemoryStream.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Builders;
using namespace WallpaperEngine::Data::Utils;

namespace {
bool isMagentaNeonTint (const glm::vec3& color) { return color.r > 0.55f && color.g < 0.25f && color.b > 0.45f; }

std::optional<glm::vec3> findMagentaCompositeTint (const Image& image, const std::vector<int>& skippedEffectIds) {
    for (const auto& effect : image.effects) {
	if (std::find (skippedEffectIds.begin (), skippedEffectIds.end (), static_cast<int> (effect->id))
	    != skippedEffectIds.end ()) {
	    continue;
	}
	if (!effect->visible->value->getBool ()) {
	    continue;
	}

	for (const auto& passOverride : effect->passOverrides) {
	    const auto compositeCombo = passOverride->combos.find ("COMPOSITE");
	    if (compositeCombo == passOverride->combos.end () || compositeCombo->second != 2) {
		continue;
	    }

	    const auto compositeColor = passOverride->constants.find ("compositecolor");
	    if (compositeColor == passOverride->constants.end () || compositeColor->second == nullptr
		|| compositeColor->second->value == nullptr) {
		continue;
	    }

	    const auto tint = compositeColor->second->value->getVec3 ();
	    if (isMagentaNeonTint (tint)) {
		return tint;
	    }
	}
    }

    return std::nullopt;
}


}

CImage::CImage (Wallpapers::CScene& scene, const Image& image) :
    CObject (scene, image), CRenderable (scene, image, *image.model->material), ScriptableObject (scene, image),
    m_sceneSpacePosition (GL_NONE), m_copySpacePosition (GL_NONE), m_passSpacePosition (GL_NONE),
    m_texcoordCopy (GL_NONE), m_texcoordPass (GL_NONE), m_modelViewProjectionScreen (),
    m_modelViewProjectionPass (glm::mat4 (1.0)), m_modelViewProjectionCopy (), m_modelViewProjectionScreenInverse (),
    m_modelViewProjectionPassInverse (glm::inverse (m_modelViewProjectionPass)), m_modelViewProjectionCopyInverse (),
    m_modelMatrix (), m_viewProjectionMatrix (), m_image (image), m_pos (), m_initialized (false) {
    // register any properties in use on this object
    this->registerProperty ("origin", *image.origin->value);
    this->registerProperty ("scale", *image.scale->value);
    this->registerProperty ("angles", *image.angles->value);
    this->registerProperty ("visible", *image.visible->value);
    this->registerProperty ("brightness", *image.brightness->value, DynamicValue::Float);
    this->registerProperty ("alpha", *image.alpha->value, DynamicValue::Float);
    this->registerProperty ("color", *image.color->value);
    this->registerProperty ("parallaxDepth", *image.parallaxDepth->value);

    // get scene width and height to calculate positions
    auto scene_width = static_cast<float> (scene.getWidth ());
    auto scene_height = static_cast<float> (scene.getHeight ());

    const auto transform = this->resolveTransform (this->getImage ());
    glm::vec3 origin = transform.origin;
    glm::vec2 size = this->getSize ();
    glm::vec3 scale = transform.scale;

    this->detectTexture ();

    // detect texture (if any)
    if (this->m_texture == nullptr) {
	if (this->m_image.model->solidlayer && size.x == 0.0f && size.y == 0.0f) {
	    size.x = scene_width;
	    size.y = scene_height;
	}
	// if (this->m_image->isSolid ()) // layer receives cursor events:
	// https://docs.wallpaperengine.io/en/scene/scenescript/reference/event/cursor.html same applies to effects
	// TODO: create a dummy texture of correct size, fbo constructors should be enough, but this should be properly
	// handled
	this->m_texture = std::make_shared<CFBO> (
	    "", TextureFormat_ARGB8888, TextureFlags_NoFlags, 1, size.x, size.y, size.x, size.y
	);
    }

    // If the wallpaper doesn't specify a size, fall back to the texture or model dimensions
    if ((size.x == 0.0f || size.y == 0.0f) && this->m_texture != nullptr) {
	size.x = static_cast<float> (this->m_texture->getRealWidth ());
	size.y = static_cast<float> (this->m_texture->getRealHeight ());
    } else if (
	(size.x == 0.0f || size.y == 0.0f) && this->getImage ().model->width.has_value ()
	&& this->getImage ().model->height.has_value ()
    ) {
	size.x = static_cast<float> (this->getImage ().model->width.value ());
	size.y = static_cast<float> (this->getImage ().model->height.value ());
    }

    // fullscreen layers should use the whole projection's size
    // TODO: WHAT SHOULD AUTOSIZE DO?
    if (this->getImage ().model->fullscreen) {
	size = { scene_width, scene_height };
	origin = { scene_width / 2, scene_height / 2, 0 };

	// TODO: CHANGE ALIGNMENT TOO?
    }
    // load the puppet mesh early: the canvas may need to grow to fit the animated mesh,
    // which affects the quad, the FBO sizes and the local projections below
    this->m_hasPuppetMesh = this->loadPuppetMesh (size);
    if (this->m_hasPuppetMesh) {
	size = this->m_puppetSize;
    }
    this->m_size = size;

    glm::vec2 scaledSize = size * glm::vec2 (scale);

    // calculate the center and shift from there
    this->m_pos.x = origin.x - (scaledSize.x / 2);
    this->m_pos.w = origin.y + (scaledSize.y / 2);
    this->m_pos.z = origin.x + (scaledSize.x / 2);
    this->m_pos.y = origin.y - (scaledSize.y / 2);

    if (this->getImage ().alignment.find ("top") != std::string::npos) {
	this->m_pos.y -= scaledSize.y / 2;
	this->m_pos.w -= scaledSize.y / 2;
    } else if (this->getImage ().alignment.find ("bottom") != std::string::npos) {
	this->m_pos.y += scaledSize.y / 2;
	this->m_pos.w += scaledSize.y / 2;
    }

    if (this->getImage ().alignment.find ("left") != std::string::npos) {
	this->m_pos.x += scaledSize.x / 2;
	this->m_pos.z += scaledSize.x / 2;
    } else if (this->getImage ().alignment.find ("right") != std::string::npos) {
	this->m_pos.x -= scaledSize.x / 2;
	this->m_pos.z -= scaledSize.x / 2;
    }

    // wallpaper engine
    this->m_pos.x -= scene_width / 2;
    this->m_pos.y = scene_height / 2 - this->m_pos.y;
    this->m_pos.z -= scene_width / 2;
    this->m_pos.w = scene_height / 2 - this->m_pos.w;

    // register both FBOs into the scene
    std::ostringstream nameA, nameB;

    // TODO: determine when _rt_imageLayerComposite and _rt_imageLayerAlbedo is used
    nameA << "_rt_imageLayerComposite_" << this->getImage ().id << "_a";
    nameB << "_rt_imageLayerComposite_" << this->getImage ().id << "_b";

    this->m_currentMainFBO = this->m_mainFBO = scene.create (
	nameA.str (), TextureFormat_ARGB8888, this->m_texture->getFlags (), 1, { size.x, size.y }, { size.x, size.y }
    );
    this->m_currentSubFBO = this->m_subFBO = scene.create (
	nameB.str (), TextureFormat_ARGB8888, this->m_texture->getFlags (), 1, { size.x, size.y }, { size.x, size.y }
    );

    // build a list of vertices, these might need some change later (or maybe invert the camera)
    GLfloat sceneSpacePosition[] = { this->m_pos.x, this->m_pos.y, 0.0f, this->m_pos.x, this->m_pos.w, 0.0f,
				     this->m_pos.z, this->m_pos.y, 0.0f, this->m_pos.z, this->m_pos.y, 0.0f,
				     this->m_pos.x, this->m_pos.w, 0.0f, this->m_pos.z, this->m_pos.w, 0.0f };

    float width = 1.0f;
    float height = 1.0f;

    if (this->getTexture ()->isAnimated ()) {
	// animated images use different coordinates as they're essentially a texture atlas
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }
    // calculate the correct texCoord limits for the texture based on the texture screen size and real size
    else if (
	this->getTexture () != nullptr
	&& (this->getTexture ()->getTextureWidth (0) != this->getTexture ()->getRealWidth ()
	    || this->getTexture ()->getTextureHeight (0) != this->getTexture ()->getRealHeight ())
    ) {
	// Account for padding in non-power-of-two textures: clamp UVs to the real content
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }

    // TODO: RECALCULATE THESE POSITIONS FOR PASSTHROUGH SO THEY TAKE THE RIGHT PART OF THE TEXTURE
    float x = 0.0f;
    float y = 0.0f;

    if (this->getTexture ()->isAnimated ()) {
	// animations should be copied completely
	x = 0.0f;
	y = 0.0f;
	width = 1.0f;
	height = 1.0f;
    }

    GLfloat realWidth = size.x;
    GLfloat realHeight = size.y;
    GLfloat realX = 0.0;
    GLfloat realY = 0.0;

    if (this->getImage ().model->passthrough) {
	// Passthrough shaders fill the destination FBO from texcoords and sample the scene using positions.
	// Keep the destination quad full-screen in local FBO space, but pass scene-space positions through.
	x = 0.0f;
	y = 0.0f;
	width = 1.0f;
	height = 1.0f;
	realX = this->m_pos.x;
	realY = this->m_pos.w;
	realWidth = this->m_pos.z;
	realHeight = this->m_pos.y;

	if (this->getImage ().model->fullscreen) {
	    realX = -1.0;
	    realY = -1.0;
	    realWidth = 1.0;
	    realHeight = 1.0;
	}
    }

    GLfloat texcoordCopy[] = { x, height, x, y, width, height, width, height, x, y, width, y };

    GLfloat copySpacePosition[] = { realX,     realHeight, 0.0f, realX, realY, 0.0f, realWidth, realHeight, 0.0f,
				    realWidth, realHeight, 0.0f, realX, realY, 0.0f, realWidth, realY,      0.0f };

    GLfloat texcoordPass[] = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

    GLfloat passSpacePosition[]
	= { -1.0, 1.0, 0.0f, -1.0, -1.0, 0.0f, 1.0, 1.0, 0.0f, 1.0, 1.0, 0.0f, -1.0, -1.0, 0.0f, 1.0, -1.0, 0.0f };

    // bind vertex list to the openGL buffers
    glGenBuffers (1, &this->m_sceneSpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_sceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (sceneSpacePosition), sceneSpacePosition, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_copySpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_copySpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (copySpacePosition), copySpacePosition, GL_STATIC_DRAW);

    // bind pass' vertex list to the openGL buffers
    glGenBuffers (1, &this->m_passSpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_passSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (passSpacePosition), passSpacePosition, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_texcoordCopy);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordCopy);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordCopy), texcoordCopy, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_texcoordPass);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordPass);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordPass), texcoordPass, GL_STATIC_DRAW);

    // compute the center of the image in scene space for rotation
    this->m_sceneCenter
	= glm::vec3 ((this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f);

    this->m_modelViewProjectionScreen
	= this->getScene ().getCamera ().getProjection () * this->getScene ().getCamera ().getLookAt ();

    if (this->getImage ().model->passthrough) {
	this->m_modelViewProjectionCopy = this->m_modelViewProjectionScreen;
    } else {
	this->m_modelViewProjectionCopy = glm::ortho<float> (0.0, size.x, 0.0, size.y);
    }
    this->m_modelViewProjectionCopyInverse = glm::inverse (this->m_modelViewProjectionCopy);
    this->m_modelMatrix = glm::ortho<float> (0.0, size.x, 0.0, size.y);
    this->m_viewProjectionMatrix = glm::mat4 (1.0);

    // ensure the input texture is marked as used
    // this makes video playback start if it's not already
    this->m_texture->incrementUsageCount ();
}

CImage::~CImage () {
    clearPuppetChannel ();
    this->m_texture->decrementUsageCount ();

    // delete passes first as they depend on the image's data
    for (auto* pass : this->m_passes) {
	delete pass;
    }

    this->m_passes.clear ();

    // free any gl resources
    glDeleteBuffers (1, &this->m_sceneSpacePosition);
    glDeleteBuffers (1, &this->m_copySpacePosition);
    glDeleteBuffers (1, &this->m_passSpacePosition);
    glDeleteBuffers (1, &this->m_texcoordCopy);
    glDeleteBuffers (1, &this->m_texcoordPass);
    if (this->m_puppetSceneSpacePosition != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetSceneSpacePosition);
    }
    if (this->m_puppetTexCoord != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetTexCoord);
    }
    if (this->m_puppetEffectTexCoord != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetEffectTexCoord);
    }
    if (this->m_puppetIndices != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetIndices);
    }
    if (this->m_puppetAlphaWeights != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetAlphaWeights);
    }
    glDeleteBuffers (1, &this->m_puppetClipIndices);
}

bool CImage::loadPuppetMesh (const glm::vec2& size) {
    if (!this->getImage ().model->puppet.has_value ()) {
	return false;
    }

    try {
	const auto stream = this->getScene ().getScene ().project.assetLocator->read (*this->getImage ().model->puppet);
	std::vector<char> data { std::istreambuf_iterator<char> (*stream), std::istreambuf_iterator<char> () };

	const auto model = parsePuppetMeshes ({ reinterpret_cast<const uint8_t*> (data.data ()), data.size () });
	const auto& mesh = model.meshes.front ();
	this->m_puppetMeshFlags = mesh.meshFlags;
	this->m_puppetMorphIndices = mesh.morphIndices;
	if (mesh.meshFlags & 8) this->m_puppetParts = mesh.boneRanges;
	const size_t vertexCount = mesh.positions.size ();
	const auto uvScale = puppetTexcoordScale (
	    this->m_texture->getRealWidth (), this->m_texture->getRealHeight (), this->m_texture->getTextureWidth (0),
	    this->m_texture->getTextureHeight (0), this->m_texture->isAnimated ()
	);
	std::vector<GLfloat> texcoords;
	std::vector<GLfloat> effectTexcoords;
	this->m_puppetRawPositions.clear ();
	this->m_puppetVertexBones.clear ();
	this->m_puppetVertexWeights.clear ();
	for (size_t vertex = 0; vertex < vertexCount; vertex++) {
	    const auto& position = mesh.positions[vertex];
	    this->m_puppetRawPositions.insert (this->m_puppetRawPositions.end (), position.begin (), position.end ());
	    this->m_puppetVertexBones.emplace_back (glm::make_vec4 (mesh.blendIndices[vertex].data ()));
	    this->m_puppetVertexWeights.emplace_back (glm::make_vec4 (mesh.blendWeights[vertex].data ()));
	    texcoords.push_back (mesh.texcoords[vertex][0] * uvScale[0]);
	    texcoords.push_back (mesh.texcoords[vertex][1] * uvScale[1]);
	    effectTexcoords.push_back (mesh.texcoords[vertex][0]);
	    effectTexcoords.push_back (mesh.texcoords[vertex][1]);
	}
	const auto& indices = mesh.indices;
	try {
	    this->m_puppetClipping = PuppetClipping::read (data, mesh.payloadEndOffset, mesh.version, indices.size ());
	    if (this->m_puppetClipping) {
		this->m_puppetMeshIndices = indices;
		glGenBuffers (1, &this->m_puppetClipIndices);
		this->rebuildPuppetClipIndices ();
	    }
	} catch (const std::exception& ex) {
	    this->m_puppetClipping.reset ();
	    sLog.error ("Ignoring puppet clipping: ", ex.what ());
	}
	const size_t mdlsOffset = model.sectionEndOffset;
	if (!this->loadPuppetAnimationData (data, mdlsOffset)) {
	    this->m_puppetBones.clear ();
	    this->m_puppetAnimations.clear ();
	}
	this->preparePuppetBones ();
	// A failed optional rig must still leave a usable static mesh.
	try {
	    if (mdlsOffset < data.size ()) {
		this->m_rig.load (data, mdlsOffset, model.meshes.size (), *this->getImage ().model->puppet);
		this->m_rig.morphBlendClamped = false;
		this->m_rig.boneAlphaEnabled = (mesh.meshFlags & 4) != 0;
		this->m_rig.drawOrderEnabled = !this->m_puppetParts.empty ();
		this->m_rig.addSceneLayers (this->getImage ().animationLayers);
		for (const auto& layer : this->m_rig.layers) this->registerAnimationLayerProperties (layer.serial);
	    }
	} catch (const std::exception& ex) {
	    this->m_rig.clear ();
	    sLog.error ("Could not load puppet rig: ", ex.what (), " (using the bind pose)");
	}
	sortPuppetParts (this->m_puppetParts, this->m_rig.boneDrawOrder, {}, this->m_puppetPartOrder);
	if ((mesh.vertexMask & 0x10000) && this->m_rig.getMorphSection () != 0) {
	    try {
		this->m_puppetMorph = parsePuppetMorph (
		    { reinterpret_cast<const uint8_t*> (data.data ()), data.size () }, this->m_rig.getMorphSection (),
		    mesh.meshFlags
		);
		if (std::ranges::any_of (this->m_puppetMorph->boneRules, [this] (const auto& rule) {
			return rule.bone >= this->m_rig.bones.size ();
		    })) {
		    throw std::runtime_error ("MDMP bone rule is outside the skeleton");
		}
	    } catch (const std::exception& ex) {
		this->m_puppetMorph.reset ();
		sLog.error ("Ignoring puppet morphs: ", ex.what ());
	    }
	}
	this->m_puppetVertexAlpha = !this->m_rig.bones.empty ()
	    && ((mesh.meshFlags & 4) != 0 || (this->m_puppetMorph && this->m_puppetMorph->hasAlpha));
	if (this->m_puppetVertexAlpha) {
	    const std::vector<glm::vec4> weights (vertexCount, glm::vec4 (1, 0, 0, 0));
	    glGenBuffers (1, &this->m_puppetAlphaWeights);
	    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetAlphaWeights);
	    glBufferData (GL_ARRAY_BUFFER, weights.size () * sizeof (glm::vec4), weights.data (), GL_DYNAMIC_DRAW);
	}

	// autosize: the canvas may need to grow so the animated mesh never clips
	this->m_puppetSize = this->computePuppetCanvasSize (size);

	glGenBuffers (1, &this->m_puppetTexCoord);
	glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetTexCoord);
	glBufferData (GL_ARRAY_BUFFER, texcoords.size () * sizeof (GLfloat), texcoords.data (), GL_STATIC_DRAW);
	glGenBuffers (1, &this->m_puppetEffectTexCoord);
	glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetEffectTexCoord);
	glBufferData (
	    GL_ARRAY_BUFFER, effectTexcoords.size () * sizeof (GLfloat), effectTexcoords.data (), GL_STATIC_DRAW
	);

	glGenBuffers (1, &this->m_puppetIndices);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
	glBufferData (GL_ELEMENT_ARRAY_BUFFER, indices.size () * sizeof (GLushort), indices.data (), GL_STATIC_DRAW);

	this->m_puppetIndexCount = static_cast<GLsizei> (indices.size ());
        if (model.meshes.size () == 2 && !(mesh.meshFlags & 2)) {
            try { loadPuppetChannelMesh (model.meshes[1]); }
            catch (const std::exception& error) {
                clearPuppetChannel ();
                sLog.error ("Ignoring puppet texture channels: ", error.what ());
            }
        }
	sLog.out (
	    "Loaded puppet mesh ", *this->getImage ().model->puppet, " version=", model.version,
	    " vertices=", vertexCount, " indices=", this->m_puppetIndexCount
	);

	return true;
    } catch (const std::exception& ex) {
	sLog.error ("Could not load puppet mesh ", *this->getImage ().model->puppet, ": ", ex.what ());
	return false;
    }
}

bool CImage::loadPuppetAnimationData (const std::vector<char>& data, const size_t mdlsOffset) {
    constexpr size_t markerSize = 9;

    this->m_puppetBones.clear ();
    this->m_puppetAnimations.clear ();

    if (mdlsOffset + markerSize > data.size ()) {
	return false;
    }

    const std::string skeletonVersion (data.data () + mdlsOffset, strlen ("MDLS0001"));
    if (skeletonVersion != "MDLS0001" && skeletonVersion != "MDLS0003") {
	sLog.error ("Unsupported puppet skeleton header ", skeletonVersion, " in ", *this->getImage ().model->puppet);
	return false;
    }

    size_t cursor = mdlsOffset + markerSize;
    const auto ensure = [&data, &cursor] (const size_t bytes) {
	if (cursor + bytes > data.size ()) {
	    throw std::runtime_error ("puppet skeleton data out of bounds");
	}
    };
    const auto readUInt32 = [&data, &cursor, &ensure] () {
	ensure (sizeof (uint32_t));
	uint32_t value;
	std::memcpy (&value, data.data () + cursor, sizeof (value));
	cursor += sizeof (value);
	return value;
    };
    const auto readFloat = [&data, &cursor, &ensure] () {
	ensure (sizeof (float));
	float value;
	std::memcpy (&value, data.data () + cursor, sizeof (value));
	cursor += sizeof (value);
	return value;
    };
    const auto readString = [&data, &cursor, &ensure] () {
	std::string value;
	while (true) {
	    ensure (1);
	    const char character = data[cursor++];
	    if (character == 0) {
		return value;
	    }
	    value += character;
	}
    };

    try {
	readUInt32 (); // offset of the next section
	const uint32_t boneCount = readUInt32 ();
	if (boneCount == 0 || boneCount > 256) {
	    return false;
	}

	for (uint32_t index = 0; index < boneCount; index++) {
	    readString (); // bone name, usually empty
	    readUInt32 (); // flags
	    const auto parent = static_cast<int32_t> (readUInt32 ());
	    const uint32_t matrixSize = readUInt32 ();
	    if (matrixSize != sizeof (float) * 16 || parent < -1 || parent >= static_cast<int32_t> (index)) {
		return false;
	    }

	    glm::mat4 local;
	    ensure (matrixSize);
	    std::memcpy (glm::value_ptr (local), data.data () + cursor, matrixSize);
	    cursor += matrixSize;
	    readString (); // per-bone metadata (empty in older files)

	    this->m_puppetBones.push_back ({ .parent = parent, .localBind = local });
	}

	// animation data follows the skeleton in its own section
	const size_t mdlaOffset = [&data, cursor] () -> size_t {
	    for (size_t offset = cursor; offset + strlen ("MDLA") < data.size (); offset++) {
		if (std::memcmp (data.data () + offset, "MDLA", strlen ("MDLA")) == 0) {
		    return offset;
		}
	    }
	    return data.size ();
	}();
	if (mdlaOffset + markerSize > data.size ()) {
	    return false;
	}

	cursor = mdlaOffset + markerSize;
	readUInt32 (); // offset of the next section
	const uint32_t animationCount = readUInt32 ();
	if (animationCount > 64) {
	    return false;
	}

	for (uint32_t index = 0; index < animationCount; index++) {
	    PuppetAnimation animation;
	    animation.id = readUInt32 ();
	    readUInt32 (); // unknown
	    animation.name = readString ();
	    animation.loop = readString () == "loop";
	    animation.fps = readFloat ();
	    if (animation.fps <= 0.0f) {
		animation.fps = 30.0f;
	    }
	    readUInt32 (); // last frame number
	    readUInt32 (); // unknown
	    const uint32_t trackCount = readUInt32 ();
	    if (trackCount != boneCount) {
		return false;
	    }

	    animation.tracks.reserve (trackCount);
	    for (uint32_t track = 0; track < trackCount; track++) {
		readUInt32 (); // unknown
		const uint32_t trackBytes = readUInt32 ();
		constexpr uint32_t frameSize = sizeof (float) * 9;
		if (trackBytes % frameSize != 0) {
		    return false;
		}

		std::vector<PuppetAnimationFrame> frames (trackBytes / frameSize);
		for (auto& frame : frames) {
		    for (int component = 0; component < 3; component++) {
			frame.position[component] = readFloat ();
		    }
		    for (int component = 0; component < 3; component++) {
			frame.rotation[component] = readFloat ();
		    }
		    for (int component = 0; component < 3; component++) {
			frame.scale[component] = readFloat ();
		    }
		}

		animation.tracks.push_back (std::move (frames));
	    }

	    // all tracks must carry the same number of samples for interpolation
	    bool consistent = true;
	    for (const auto& track : animation.tracks) {
		if (track.size () != animation.tracks.front ().size ()) {
		    consistent = false;
		    break;
		}
	    }
	    if (consistent && !animation.tracks.empty () && !animation.tracks.front ().empty ()) {
		this->m_puppetAnimations.push_back (std::move (animation));
	    }

	    // each animation is followed by a zero footer (4 bytes in MDLA0001, 35 in
	    // MDLA0006) — skip it so the next animation's id is read from the right spot
	    while (cursor < data.size () && data[cursor] == 0) {
		cursor++;
	    }
	}
    } catch (const std::exception& ex) {
	sLog.error ("Could not load puppet animation ", *this->getImage ().model->puppet, ": ", ex.what ());
	return false;
    }

    return !this->m_puppetAnimations.empty ();
}

void CImage::preparePuppetBones () {
    if (this->m_puppetBones.empty ()) {
	return;
    }

    std::vector<glm::mat4> bindWorld (this->m_puppetBones.size ());
    for (size_t index = 0; index < this->m_puppetBones.size (); index++) {
	const auto& bone = this->m_puppetBones[index];
	bindWorld[index] = bone.parent >= 0 ? bindWorld[bone.parent] * bone.localBind : bone.localBind;
	this->m_puppetBones[index].inverseBindWorld = glm::inverse (bindWorld[index]);
    }
}

glm::mat4 CImage::puppetWorld () const {
    const auto transform = this->resolveTransform (this->getImage ());
    glm::mat4 world = glm::translate (glm::mat4 (1.0f), transform.origin);
    world = glm::rotate (world, transform.angle, glm::vec3 (0, 0, 1));
    return glm::scale (world, transform.scale);
}

void CImage::registerAnimationLayerProperties (size_t serial) {
    const auto* entry = this->m_rig.findLayer (serial);
    if (!entry) return;
    const auto prefix = "animationlayer" + std::to_string (getId ()) + "[" + std::to_string (serial) + "].";
    JSContext* ctx = getScene ().getScriptEngine ().getContext ();
    JSValue owner = getScene ().getScriptEngine ().getPuppetScripts ().animation (*this, serial);
    for (const auto& [name, value] : { std::pair { "visible", entry->layer->visible->value.get () },
                                    { "rate", entry->layer->rate->value.get () },
                                    { "blend", entry->layer->blend->value.get () } })
        getScene ().getScriptEngine ().queueScript (prefix + name, *value, *this, owner);
    JS_FreeValue (ctx, owner);
}

void CImage::prepareScriptFrame () {
    if (!this->m_rig.bones.empty ()) this->m_rig.updatePose (this->puppetWorld ());
    getScene ().getScriptEngine ().getPuppetScripts ().finishFrame (*this);
    if (this->m_textureAnimation) {
        this->m_textureAnimation->sharedPlayback ()->sync (getContext ().getDriver ().getRenderTime ());
        this->m_textureAnimation->advance (getScene ().getDeltaTime ());
    }
}

std::shared_ptr<ImageTextureAnimation> CImage::getTextureAnimation () {
    if (!m_textureAnimation && m_texture && !m_texture->getFrames ().empty ()) {
        m_textureAnimation = std::make_shared<ImageTextureAnimation> (m_texture->getFrames ());
        m_textureAnimation->sharedPlayback ()->sync (getContext ().getDriver ().getRenderTime ());
    }
    return m_textureAnimation;
}

const Data::Assets::Frame* CImage::getTextureAnimationFrame (const TextureProvider& texture) const {
    if (!m_textureAnimation || !m_textureAnimation->hasOverride () || &texture != m_texture.get ()) return nullptr;
    const auto& frames = texture.getFrames ();
    const auto index = m_textureAnimation->getFrame ();
    return frames.empty () ? nullptr : frames[index < frames.size () ? index : 0].get ();
}

void CImage::updatePuppetAnimation () {
    if (this->m_rig.bones.empty () || this->m_puppetVertexBones.empty ()) {
	return;
    }
    sortPuppetParts (this->m_puppetParts, this->m_rig.boneDrawOrder, this->m_rig.drawOrder, this->m_puppetPartOrder);
    if (this->m_puppetClipping && this->m_puppetClipping->order != this->m_puppetPartOrder
	&& !this->m_puppetPartOrder.empty ()) {
	this->m_puppetClipping->order = this->m_puppetPartOrder;
	this->rebuildPuppetClipIndices ();
    }
    const size_t boneCount = this->m_rig.bones.size ();
    const auto skin = this->m_rig.skinMatrices ();

    std::vector<std::pair<uint32_t, float>> targets;
    if (this->m_puppetMorph && !this->m_rig.morphWeights.empty ()) {
	const auto& state = this->m_rig.morphWeights.front ();
	for (uint32_t target = 0; target < state.weights.size () && target < 64 && targets.size () < 11; target++) {
	    if (state.active & (uint64_t (1) << target)) {
		targets.emplace_back (target, state.weights[target]);
	    }
	}
	std::ranges::stable_sort (targets, std::ranges::greater {}, &std::pair<uint32_t, float>::second);
    }
    // A rule bone's inverse is shared by every vertex of this target.
    std::vector<glm::mat4> ruleInverse (targets.size (), glm::mat4 (1));
    for (size_t index = 0; index < targets.size (); index++) {
	const auto target = targets[index].first;
	if (target < this->m_puppetMorph->boneRules.size ()) {
	    ruleInverse[index] = glm::inverse (this->m_rig.boneModel[this->m_puppetMorph->boneRules[target].bone]);
	}
    }
    const auto deform = [&] (size_t index, const glm::vec4& position) {
	glm::vec3 result (0);
	float total = 0;
	for (int component = 0; component < 4; component++) {
	    const float weight = this->m_puppetVertexWeights[index][component];
	    const uint32_t bone = this->m_puppetVertexBones[index][component];
	    if (weight > 0 && bone < boneCount) {
		result += weight * glm::vec3 (skin[bone] * position);
		total += weight;
	    }
	}
	return total > 0 ? result / total : glm::vec3 (position);
    };
    std::vector<glm::vec4> alphaWeights;

    this->m_puppetSkinnedPositions.resize (this->m_puppetRawPositions.size ());
    const size_t vertexCount = this->m_puppetRawPositions.size () / 3;
    for (size_t index = 0; index < vertexCount; index++) {
	glm::vec4 base (
	    this->m_puppetRawPositions[index * 3], this->m_puppetRawPositions[index * 3 + 1],
	    this->m_puppetRawPositions[index * 3 + 2], 1.0f
	);
	float alpha = 1;
	if (!targets.empty () && this->m_puppetMorphIndices[index] > 0) {
	    const auto& morph = *this->m_puppetMorph;
	    const glm::vec4 preMorph (morph.boneRules.empty () ? glm::vec3 (base) : deform (index, base), 1);
	    for (size_t targetIndex = 0; targetIndex < targets.size (); targetIndex++) {
		const auto& [target, value] = targets[targetIndex];
		float weight = value;
		if (target < morph.boneRules.size ()) {
		    const auto& rule = morph.boneRules[target];
		const glm::vec3 local (ruleInverse[targetIndex] * preMorph);
		    const float x = rule.axis ? local.x : glm::length (glm::vec2 (local));
		    float t = (x - rule.edge0) / (rule.edge1 - rule.edge0);
		    t = std::isnan (t) ? 0 : std::clamp (t, 0.0f, 1.0f);
		    weight *= t * t * (3 - 2 * t);
		}
		const auto delta = morph.sample (static_cast<uint32_t> (this->m_puppetMorphIndices[index]), target);
		base += glm::vec4 (delta[0], delta[1], delta[2], 0) * (weight * morph.scale);
		// Native shaders modify displacement, but alpha uses the layer weight.
		alpha *= 1 + (delta[3] - 1) * value;
	    }
	}
	if (this->m_puppetVertexAlpha) {
	    if (this->m_puppetMeshFlags & 4) {
		float bones = 0;
		for (int component = 0; component < 4; component++) {
		    const auto bone = this->m_puppetVertexBones[index][component];
		    if (bone < boneCount) {
			bones += this->m_puppetVertexWeights[index][component]
			    * (bone < this->m_rig.boneAlpha.size () ? this->m_rig.boneAlpha[bone] : 1);
		    }
		}
		alpha *= std::clamp (bones, 0.0f, 1.0f);
	    }
	    alpha = std::clamp (alpha, 0.0f, 1.0f);
	    alphaWeights.emplace_back (alpha, 1 - alpha, 0, 0);
	}
	const glm::vec3 result = deform (index, base);

	this->m_puppetSkinnedPositions[index * 3] = result.x;
	this->m_puppetSkinnedPositions[index * 3 + 1] = result.y;
	// flatten depth: 3D bone rotations otherwise push vertices out of the
	// FBO ortho projection's [-1, 1] clip range, cutting the mesh
	this->m_puppetSkinnedPositions[index * 3 + 2] = 0.0f;
    }
    this->updatePuppetScenePositionBuffer (this->m_puppetSize, this->m_puppetSkinnedPositions);
    if (this->m_puppetVertexAlpha) {
	glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetAlphaWeights);
	glBufferData (GL_ARRAY_BUFFER, alphaWeights.size () * sizeof (glm::vec4), alphaWeights.data (), GL_DYNAMIC_DRAW);
    }
}

void CImage::evaluatePuppetSkin (
    const PuppetAnimation& animation, const float framePosition, std::vector<glm::mat4>& skin
) const {
    const size_t boneCount = this->m_puppetBones.size ();
    const size_t sampleCount = animation.tracks.front ().size ();
    const auto frame = std::min (static_cast<size_t> (framePosition), sampleCount - 1);
    const size_t nextFrame = std::min (frame + 1, sampleCount - 1);
    const float blend = framePosition - static_cast<float> (frame);

    std::vector<glm::mat4> world (boneCount);
    for (size_t index = 0; index < boneCount; index++) {
	const auto& from = animation.tracks[index][frame];
	const auto& to = animation.tracks[index][nextFrame];
	const glm::vec3 position = glm::mix (from.position, to.position, blend);
	const glm::vec3 rotation = glm::mix (from.rotation, to.rotation, blend);
	const glm::vec3 scale = glm::mix (from.scale, to.scale, blend);

	const auto& bone = this->m_puppetBones[index];
	const auto& track = animation.tracks[index];
	const bool hasAuthoredTrack = std::any_of (track.begin (), track.end (), [] (const PuppetAnimationFrame& sample) {
	    constexpr float epsilon = 1e-6f;
	    constexpr float epsilonSquared = epsilon * epsilon;
	    const bool nonZeroTransform
		= glm::dot (sample.position, sample.position) > epsilonSquared
		|| glm::dot (sample.rotation, sample.rotation) > epsilonSquared;
	    const bool zeroScale = glm::dot (sample.scale, sample.scale) <= epsilonSquared;
	    const glm::vec3 scaleFromUnit = sample.scale - glm::vec3 (1.0f);
	    const bool unitScale = glm::dot (scaleFromUnit, scaleFromUnit) <= epsilonSquared;
	    return nonZeroTransform || (!zeroScale && !unitScale);
	});

	glm::mat4 local = bone.localBind;
	if (hasAuthoredTrack) {
	    // Keep animation transforms in model space; screen-space Y is flipped when positions are uploaded.
	    local = glm::translate (glm::mat4 (1.0f), position);
	    local = glm::rotate (local, rotation.z, glm::vec3 (0.0f, 0.0f, 1.0f));
	    local = glm::rotate (local, rotation.y, glm::vec3 (0.0f, 1.0f, 0.0f));
	    local = glm::rotate (local, rotation.x, glm::vec3 (1.0f, 0.0f, 0.0f));
	    local = glm::scale (local, scale);
	}

	world[index] = bone.parent >= 0 ? world[bone.parent] * local : local;
	skin[index] = world[index] * this->m_puppetBones[index].inverseBindWorld;
    }
}

glm::vec2 CImage::computePuppetCanvasSize (const glm::vec2& size) const {
    // autosize: measure the mesh across every animation frame and grow the canvas
    // symmetrically so the animated puppet never clips at the FBO edges
    float maxAbsX = size.x / 2.0f;
    float maxAbsY = size.y / 2.0f;

    const size_t vertexCount = this->m_puppetRawPositions.size () / 3;
    const size_t boneCount = this->m_puppetBones.size ();

    std::vector<glm::mat4> skin (boneCount);
    for (const auto& animation : this->m_puppetAnimations) {
	if (animation.tracks.size () != boneCount) {
	    continue;
	}

	const size_t sampleCount = animation.tracks.front ().size ();
	for (size_t frame = 0; frame < sampleCount; frame++) {
	    this->evaluatePuppetSkin (animation, static_cast<float> (frame), skin);

	    for (size_t index = 0; index < vertexCount; index++) {
		const glm::vec4 base (
		    this->m_puppetRawPositions[index * 3], this->m_puppetRawPositions[index * 3 + 1],
		    this->m_puppetRawPositions[index * 3 + 2], 1.0f
		);
		glm::vec3 result (0.0f);
		float totalWeight = 0.0f;
		for (int component = 0; component < 4; component++) {
		    const float weight = this->m_puppetVertexWeights[index][component];
		    const uint32_t bone = this->m_puppetVertexBones[index][component];
		    if (weight <= 0.0f || bone >= boneCount) {
			continue;
		    }

		    result += weight * glm::vec3 (skin[bone] * base);
		    totalWeight += weight;
		}

		if (totalWeight <= 0.0f) {
		    result = glm::vec3 (base);
		} else {
		    result /= totalWeight;
		}

		maxAbsX = std::max (maxAbsX, std::abs (result.x));
		maxAbsY = std::max (maxAbsY, std::abs (result.y));
	    }
	}
    }

    return { std::ceil (maxAbsX) * 2.0f, std::ceil (maxAbsY) * 2.0f };
}

void CImage::updatePuppetScenePositionBuffer (
    const glm::vec2& size, const std::vector<GLfloat>& rawPositions
) {
    if (rawPositions.empty ()) {
	return;
    }

    std::vector<GLfloat> positions;
    positions.reserve (rawPositions.size ());
    const float width = this->m_pos.z - this->m_pos.x;
    const float height = this->m_pos.y - this->m_pos.w;
    for (size_t index = 0; index + 2 < rawPositions.size (); index += 3) {
	const float localX = size.x / 2.0f + rawPositions[index];
	const float localY = size.y / 2.0f - rawPositions[index + 1];
	positions.push_back (this->m_pos.x + (localX / size.x) * width);
	positions.push_back (this->m_pos.w + (localY / size.y) * height);
	positions.push_back (rawPositions[index + 2]);
    }

    if (this->m_puppetSceneSpacePosition == GL_NONE) {
	glGenBuffers (1, &this->m_puppetSceneSpacePosition);
    }
    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetSceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, positions.size () * sizeof (GLfloat), positions.data (), GL_DYNAMIC_DRAW);
}

void CImage::setupPuppetGeometryCallback (
    Effects::CPass* pass, const GLuint* positionBuffer, bool sourceTexture
) const {
    pass->setGeometryCallback (
	[this, pass, positionBuffer, sourceTexture] () {
	    const GLint position = glGetAttribLocation (pass->getProgramID (), "a_Position");
	    const GLint texCoord = glGetAttribLocation (pass->getProgramID (), "a_TexCoord");

	    if (position >= 0) {
		glEnableVertexAttribArray (position);
		glBindBuffer (GL_ARRAY_BUFFER, *positionBuffer);
		glVertexAttribPointer (position, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }

	    if (texCoord >= 0) {
		glEnableVertexAttribArray (texCoord);
		// Intermediate effects already cropped the source's padding.
		glBindBuffer (GL_ARRAY_BUFFER, sourceTexture ? this->m_puppetTexCoord : this->m_puppetEffectTexCoord);
		glVertexAttribPointer (texCoord, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }
	    if (this->m_puppetVertexAlpha) {
		// Two identity bones carry CPU-computed alpha without skinning twice.
		const GLuint program = pass->getProgramID ();
		const GLint weights = glGetAttribLocation (program, "a_BlendWeights");
		if (weights >= 0) {
		    glEnableVertexAttribArray (weights);
		    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetAlphaWeights);
		    glVertexAttribPointer (weights, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
		}
		const GLint indices = glGetAttribLocation (program, "a_BlendIndices");
		if (indices >= 0) {
		    glDisableVertexAttribArray (indices);
		    glVertexAttribI4ui (indices, 0, 1, 0, 0);
		}
		constexpr GLfloat bones[24] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0,
					       1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
		constexpr GLfloat alpha[2] = { 1, 0 };
		const GLint boneUniform = glGetUniformLocation (program, "g_Bones");
		if (boneUniform >= 0) glUniformMatrix4x3fv (boneUniform, 2, GL_FALSE, bones);
		const GLint alphaUniform = glGetUniformLocation (program, "g_BonesAlpha");
		if (alphaUniform >= 0) glUniform1fv (alphaUniform, 2, alpha);
	    }
	},
	[this] () {
	    GLint currentFramebuffer = 0;
	    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &currentFramebuffer);
	    if (this->m_puppetClipDraw < 0
		&& currentFramebuffer != static_cast<GLint> (this->getScene ().getFBO ()->getFramebuffer ())) {
		GLfloat previousClearColor[4] = {};
		glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
		glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		glClearColor (
		    previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]
		);
	    }
	    const GLboolean cullFaceEnabled = glIsEnabled (GL_CULL_FACE);
	    glDisable (GL_CULL_FACE);
	    if (this->m_puppetClipDraw >= 0) {
		const auto& draw = this->m_puppetClipping->draws.at (this->m_puppetClipDraw);
		glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetClipIndices);
		glDrawElements (GL_TRIANGLES, draw.count, GL_UNSIGNED_SHORT,
				 reinterpret_cast<const void*> (size_t (draw.offset) * sizeof (GLushort)));
	    } else if (this->m_puppetPartOrder.empty ()) {
		glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
		glDrawElements (GL_TRIANGLES, this->m_puppetIndexCount, GL_UNSIGNED_SHORT, nullptr);
	    } else {
		glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
		for (const auto index : this->m_puppetPartOrder) {
		    const auto& part = this->m_puppetParts[index];
		    glDrawElements (GL_TRIANGLES, part.indexCount, GL_UNSIGNED_SHORT,
				    reinterpret_cast<const void*> (size_t (part.firstIndex) * sizeof (GLushort)));
		}
	    }
	    if (cullFaceEnabled) {
		glEnable (GL_CULL_FACE);
	    }
	},
	[pass] () {
	    const GLint position = glGetAttribLocation (pass->getProgramID (), "a_Position");
	    const GLint texCoord = glGetAttribLocation (pass->getProgramID (), "a_TexCoord");

	    if (position >= 0) {
		glDisableVertexAttribArray (position);
	    }

	    if (texCoord >= 0) {
		glDisableVertexAttribArray (texCoord);
	    }
	    const GLint weights = glGetAttribLocation (pass->getProgramID (), "a_BlendWeights");
	    if (weights >= 0) glDisableVertexAttribArray (weights);
	}
    );
}

void CImage::rebuildPuppetClipIndices () {
    this->m_puppetClipping->build (this->m_puppetMeshIndices);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetClipIndices);
    const auto& indices = this->m_puppetClipping->indices;
    glBufferData (GL_ELEMENT_ARRAY_BUFFER, indices.size () * sizeof (GLushort), indices.data (), GL_DYNAMIC_DRAW);
}

void CImage::setupPuppetClipping (const std::shared_ptr<const TextureProvider>& input) {
    const auto scene = this->getScene ().getFBO ();
    const auto width = scene->getRealWidth ();
    const auto height = scene->getRealHeight ();
    const auto makeMask = [&] (const std::string& name) {
	if (const auto existing = this->getScene ().find (name)) return existing;
	return this->getScene ().create (name, TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1,
					{ width, height }, { width, height });
    };
    this->m_puppetClipMask = makeMask ("_rt_FullAlphaMask");
    const auto configure = [&] (CPass* pass, bool sourceTexture) {
	pass->setInput (sourceTexture ? this->m_texture : input);
	pass->setModelMatrix (&this->m_modelMatrix);
	pass->setViewProjectionMatrix (&this->m_viewProjectionMatrix);
	pass->setModelViewProjectionMatrix (&this->m_modelViewProjectionScreen);
	pass->setModelViewProjectionMatrixInverse (&this->m_modelViewProjectionScreenInverse);
	this->setupPuppetGeometryCallback (pass, &this->m_puppetSceneSpacePosition, sourceTexture);
    };
    auto material = std::make_unique<MaterialPass> (MaterialPass {
	.blending = BlendingMode_Translucent,
	.cullmode = CullingMode_Disable,
	.depthtest = DepthtestMode_Disabled,
	.depthwrite = DepthwriteMode_Disabled,
	.shader = "genericimage4",
	.combos = this->m_puppetMeshPass->getPass ().combos,
    });
    material->combos.insert_or_assign ("CLIPPINGUVS", 1);
    material->combos.insert_or_assign ("CLIPPINGTARGET", 1);
    const auto& targetConfig = *this->m_virtualPassess.emplace_back (std::move (material));
    this->m_puppetClipTargetPass = std::make_unique<CPass> (
	*this, std::make_shared<FBOProvider> (this), targetConfig, std::nullopt, std::nullopt, std::nullopt
    );
    auto* target = this->m_puppetClipTargetPass.get ();
    configure (target, false);
    target->setDestination (scene);
    target->setTexture (8, this->m_puppetClipMask);
    static const glm::vec4 white (1);
    target->addUniform ("g_Color4", &white);
    for (const auto& record : this->m_puppetClipping->records) {
	auto mask = std::make_unique<MaterialPass> (MaterialPass {
	    .blending = BlendingMode_Translucent,
	    .cullmode = CullingMode_Disable,
	    .depthtest = DepthtestMode_Disabled,
	    .depthwrite = DepthwriteMode_Disabled,
	    .shader = "clippingmaskimage4",
	    .textures = { { 1, record.mask } },
	    .combos = this->m_puppetMeshPass->getPass ().combos,
	});
	const auto& maskConfig = *this->m_virtualPassess.emplace_back (std::move (mask));
	auto pass = std::make_unique<CPass> (
	    *this, std::make_shared<FBOProvider> (this), maskConfig, std::nullopt, std::nullopt, std::nullopt
	);
	configure (pass.get (), true);
	pass->addUniform ("g_RenderVar0", &this->m_puppetClipRenderVar0);
	this->m_puppetClipMaskPasses.push_back (std::move (pass));
    }
    if (std::ranges::none_of (this->m_puppetClipping->records, [] (const auto& record) { return record.parent != -1; })) {
	return;
    }
    this->m_puppetClipIntermediate = makeMask ("_rt_FullAlphaMaskIntermediate");
    auto compose = std::make_unique<MaterialPass> (MaterialPass {
	.blending = BlendingMode_Translucent,
	.cullmode = CullingMode_Disable,
	.depthtest = DepthtestMode_Disabled,
	.depthwrite = DepthwriteMode_Disabled,
	.shader = "minimalalpha",
    });
    const auto& composeConfig = *this->m_virtualPassess.emplace_back (std::move (compose));
    this->m_puppetClipComposePass = std::make_unique<CPass> (
	*this, std::make_shared<FBOProvider> (this), composeConfig, std::nullopt, std::nullopt, std::nullopt
    );
    auto* pass = this->m_puppetClipComposePass.get ();
    pass->setDestination (this->m_puppetClipMask);
    pass->setInput (this->m_puppetClipIntermediate);
    pass->setPosition (this->m_passSpacePosition);
    pass->setTexCoord (this->m_texcoordPass);
    pass->setModelMatrix (&this->m_puppetClipIdentity);
    pass->setViewProjectionMatrix (&this->m_puppetClipIdentity);
    pass->setModelViewProjectionMatrix (&this->m_puppetClipIdentity);
    pass->setModelViewProjectionMatrixInverse (&this->m_puppetClipIdentity);
    static const float alpha = 1;
    pass->addUniform ("g_Alpha", &alpha);
    pass->setGeometryCallback ({}, [] () {
	GLboolean colorMask[4];
	glGetBooleanv (GL_COLOR_WRITEMASK, colorMask);
	glBlendFuncSeparate (GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
	glColorMask (true, true, true, false);
	glDrawArrays (GL_TRIANGLES, 0, 6);
	glColorMask (colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    }, {});
}

void CImage::renderPuppetClipMask (int record, int draw, bool intermediate) {
    const auto destination = intermediate ? this->m_puppetClipIntermediate : this->m_puppetClipMask;
    const bool inverted = (this->m_puppetClipping->records[record].flags & PuppetClipping::Inverted) != 0;
    GLfloat previousColor[4];
    glGetFloatv (GL_COLOR_CLEAR_VALUE, previousColor);
    glBindFramebuffer (GL_FRAMEBUFFER, destination->getFramebuffer ());
    glColorMask (true, true, true, true);
    const float clear = inverted ? 1 : 0;
    glClearColor (clear, clear, clear, clear);
    glClear (GL_COLOR_BUFFER_BIT);
    glClearColor (previousColor[0], previousColor[1], previousColor[2], previousColor[3]);
    auto* pass = this->m_puppetClipMaskPasses[record].get ();
    pass->setDestination (destination);
    this->m_puppetClipRenderVar0.x = clear;
    this->m_puppetClipDraw = draw;
    pass->render ();
    if (intermediate) this->m_puppetClipComposePass->render ();
}

void CImage::renderPuppetClipped (CPass* pass) {
    const auto& commands = this->m_puppetClipping->commands;
    int draw = 0;
    const auto targets = [&] (int record) {
	this->m_puppetClipDraw = draw + 1;
	this->m_puppetClipTargetPass->setBlendingMode (
	    this->m_puppetClipping->records[record].flags & PuppetClipping::Additive
		? BlendingMode_Additive : BlendingMode_Translucent
	);
	this->m_puppetClipTargetPass->render ();
	draw += 2;
    };
    for (size_t index = 0; index < commands.size (); index++) {
	switch (commands[index]) {
	    case PuppetClipping::PlainDraw:
		this->m_puppetClipDraw = draw++;
		pass->render ();
		break;
	    case PuppetClipping::Mask: {
		const int record = commands[++index];
		this->renderPuppetClipMask (record, draw, false);
		targets (record);
		break;
	    }
	    case PuppetClipping::NestedMask: {
		const int chain = commands[++index];
		for (int link = 0; link < chain; link++) {
		    const int parent = commands[++index];
		    const int parentDraw = commands[++index];
		    this->renderPuppetClipMask (parent, parentDraw, link != 0);
		}
		const int record = commands[++index];
		this->renderPuppetClipMask (record, draw, true);
		targets (record);
		break;
	    }
	}
    }
    this->m_puppetClipDraw = -1;
}

void CImage::loadPuppetChannelMesh (const PuppetMeshData& channelMesh) {
    if (!(channelMesh.meshFlags & 2) || channelMesh.vertexMask != 0x00800021)
        throw std::runtime_error ("Unsupported texture channel mesh layout");
	    // Native material flag 0x10 comes from the compiled shader's LIGHTING
	    // combo; flag 0x08 comes from an active _rt_MipMappedFrameBuffer sampler.
	    // genericimage2's hidden sampler is compiled only for REFLECTION and
	    // NORMALMAP together. These two flags select the albedo prepass.
	    m_puppetChannelOffscreen = false;
	    for (const auto& base : m_image.model->material->passes) {
		const auto combo = [&] (const char* name) {
		    const auto it = base->combos.find (name);
		    return it == base->combos.end () ? 0 : it->second;
		};
		const bool explicitMipSampler = std::any_of (
		    base->textures.begin (), base->textures.end (), [] (const auto& slot) {
			return slot.second == "_rt_MipMappedFrameBuffer";
		    });
		m_puppetChannelOffscreen |= combo ("LIGHTING") != 0 || explicitMipSampler
		    || (base->shader == "genericimage2" && combo ("REFLECTION") != 0
		        && combo ("NORMALMAP") != 0);
	    }
	    m_puppetChannelMaterial = MaterialParser::load (
	        getScene ().getScene ().project, channelMesh.material);
	    if (m_puppetChannelMaterial->passes.size () != 1 ||
	        m_puppetChannelMaterial->passes.front ()->shader.find ("puppettexturechannels") == std::string::npos)
	        throw std::runtime_error ("Unsupported puppet channel material");
	    const auto& channel = channelMesh;
	    std::vector<GLfloat> channelPositions;
	    std::vector<GLfloat> channelUv;
	    std::vector<GLuint> channelIndices;
	    for (const auto& position : channel.positions)
	        channelPositions.insert (channelPositions.end (), position.begin (), position.end ());
	    for (const auto& uv : channel.texcoordsFull)
	        channelUv.insert (channelUv.end (), uv.begin (), uv.end ());
	    for (const auto& lanes : channel.blendIndices)
	        channelIndices.insert (channelIndices.end (), lanes.begin (), lanes.end ());
	    glGenBuffers (1, &m_puppetChannelPosition);
	    glBindBuffer (GL_ARRAY_BUFFER, m_puppetChannelPosition);
	    glBufferData (GL_ARRAY_BUFFER, channelPositions.size () * sizeof (GLfloat),
	                  channelPositions.data (), GL_STATIC_DRAW);
	    glGenBuffers (1, &m_puppetChannelTexcoord);
	    glBindBuffer (GL_ARRAY_BUFFER, m_puppetChannelTexcoord);
	    glBufferData (GL_ARRAY_BUFFER, channelUv.size () * sizeof (GLfloat), channelUv.data (), GL_STATIC_DRAW);
	    glGenBuffers (1, &m_puppetChannelBlendIndices);
	    glBindBuffer (GL_ARRAY_BUFFER, m_puppetChannelBlendIndices);
	    glBufferData (GL_ARRAY_BUFFER, channelIndices.size () * sizeof (GLuint),
	                  channelIndices.data (), GL_STATIC_DRAW);
	    glGenBuffers (1, &m_puppetChannelIndices);
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_puppetChannelIndices);
	    glBufferData (GL_ELEMENT_ARRAY_BUFFER, channel.indices.size () * sizeof (GLushort),
	                  channel.indices.data (), GL_STATIC_DRAW);
	    m_puppetChannelIndexCount = static_cast<GLsizei> (channel.indices.size ());
	    const uint32_t highestIndex = std::accumulate (
	        channel.blendIndices.begin (), channel.blendIndices.end (), uint32_t (0),
	        [] (uint32_t value, const std::array<uint32_t, 4>& lanes) { return std::max (value, lanes[0]); });
	    if (channel.blendRowCount == 0 || channel.blendRowCount > 4 ||
	        highestIndex >= channel.blendRowCount * 4)
	        throw std::runtime_error ("Puppet channel index exceeds native 16-float map");
	    m_puppetBlendRows = channel.blendRowCount;
	    const auto& combos = m_puppetChannelMaterial->passes.front ()->combos;
	    const auto rows = combos.find ("BLENDROWCOUNT");
	    if (rows == combos.end () || rows->second != int (m_puppetBlendRows))
	        throw std::runtime_error ("Puppet channel BLENDROWCOUNT mismatch");
	    const uint32_t targetWidth = m_texture->getRealWidth ();
	    const uint32_t targetHeight = m_texture->getRealHeight ();
	    if (targetWidth == 0 || targetHeight == 0)
		throw std::runtime_error ("Puppet channel target has zero dimensions");
	    m_puppetChannelProjection = glm::ortho (
		0.0f, float (targetWidth), float (targetHeight), 0.0f, -1000.0f, 1000.0f);
	    m_puppetChannelProjectionInverse = glm::inverse (m_puppetChannelProjection);
	    if (m_puppetChannelOffscreen) {
		m_puppetChannelFBO = std::make_shared<CFBO> (
		    "_rt_imageLayerAlbedo_" + std::to_string (getId ()), TextureFormat_ARGB8888,
		    m_texture->getFlags (), 1.0f, targetWidth, targetHeight, targetWidth, targetHeight);
	    }
    m_puppetChannelMesh = channelMesh;
 }

void CImage::clearPuppetChannel () {
    delete m_puppetChannelBasePass;
    delete m_puppetChannelPass;
    m_puppetChannelBasePass = nullptr;
    m_puppetChannelPass = nullptr;
    for (GLuint* buffer : {&m_puppetChannelPosition, &m_puppetChannelTexcoord,
                          &m_puppetChannelBlendIndices, &m_puppetChannelIndices}) {
        glDeleteBuffers (1, buffer);
        *buffer = GL_NONE;
    }
    m_puppetChannelMesh.reset ();
    m_puppetChannelMaterial.reset ();
    m_puppetChannelFBO.reset ();
    m_puppetChannelOffscreen = false;
    m_puppetBlendRows = 0;
}

void CImage::setupPuppetChannelGeometryCallback (Effects::CPass* pass, GLuint positionBuffer) const {
    pass->setGeometryCallback (
	[this, pass, positionBuffer] () {
            // Channel overlays compose albedo coverage; squaring alpha here
            // would dim the completed puppet again during its final draw.
            if (pass->getBlendingMode () == BlendingMode_Translucent)
                glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	    const GLint position = glGetAttribLocation (pass->getProgramID (), "a_Position");
	    const GLint texcoord = glGetAttribLocation (pass->getProgramID (), "a_TexCoordVec4");
	    const GLint channel = glGetAttribLocation (pass->getProgramID (), "a_BlendIndices");
	    if (position >= 0) {
		glEnableVertexAttribArray (position);
		glBindBuffer (GL_ARRAY_BUFFER, positionBuffer);
		glVertexAttribPointer (position, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }
	    if (texcoord >= 0) {
		glEnableVertexAttribArray (texcoord);
		glBindBuffer (GL_ARRAY_BUFFER, m_puppetChannelTexcoord);
		glVertexAttribPointer (texcoord, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }
	    if (channel >= 0) {
		glEnableVertexAttribArray (channel);
		glBindBuffer (GL_ARRAY_BUFFER, m_puppetChannelBlendIndices);
		glVertexAttribIPointer (channel, 4, GL_UNSIGNED_INT, 0, nullptr);
	    }
	},
	[this] () {
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_puppetChannelIndices);
	    glDrawElements (GL_TRIANGLES, m_puppetChannelIndexCount, GL_UNSIGNED_SHORT, nullptr);
	},
	[pass] () {
	    for (const char* name : {"a_Position", "a_TexCoordVec4", "a_BlendIndices"}) {
		const GLint location = glGetAttribLocation (pass->getProgramID (), name);
		if (location >= 0) glDisableVertexAttribArray (location);
	    }
	});
}

void CImage::renderPuppetChannelPrepass () {
    if (!m_puppetChannelOffscreen || !m_puppetChannelFBO || !m_puppetChannelBasePass || !m_puppetChannelPass) return;
    glBindFramebuffer (GL_FRAMEBUFFER, m_puppetChannelFBO->getFramebuffer ());
    const GLfloat transparent[] {0, 0, 0, 0};
    glClearBufferfv (GL_COLOR, 0, transparent);
    auto* base = m_puppetChannelBasePass;
    base->setDestination (m_puppetChannelFBO);
    base->setInput (m_texture);
    base->setPreviousInput (nullptr);
    base->setPosition (m_passSpacePosition);
    base->setTexCoord (m_texcoordCopy);
    base->setModelMatrix (&m_modelMatrix);
    base->setViewProjectionMatrix (&m_viewProjectionMatrix);
    base->setModelViewProjectionMatrix (&m_puppetChannelProjection);
    base->setModelViewProjectionMatrixInverse (&m_puppetChannelProjectionInverse);
    base->render ();

    auto* channel = m_puppetChannelPass;
    channel->setDestination (m_puppetChannelFBO);
    channel->setInput (m_texture);
    channel->setPreviousInput (nullptr);
    channel->setPosition (m_puppetChannelPosition);
    channel->setTexCoord (m_puppetChannelTexcoord);
    channel->setModelMatrix (&m_modelMatrix);
    channel->setViewProjectionMatrix (&m_viewProjectionMatrix);
    channel->setModelViewProjectionMatrix (&m_puppetChannelProjection);
    channel->setModelViewProjectionMatrixInverse (&m_puppetChannelProjectionInverse);
    setupPuppetChannelGeometryCallback (channel, m_puppetChannelPosition);
    channel->render ();
}

void CImage::renderPuppetChannelDirect (const std::shared_ptr<const CFBO>& target) {
    if (m_puppetChannelOffscreen || !m_puppetChannelPass) return;
    auto* channel = m_puppetChannelPass;
    channel->setDestination (target);
    channel->setInput (m_texture);
    channel->setPreviousInput (nullptr);
    channel->setPosition (m_puppetChannelPosition);
    channel->setTexCoord (m_puppetChannelTexcoord);
    channel->setModelMatrix (&m_modelMatrix);
    channel->setViewProjectionMatrix (&m_viewProjectionMatrix);
    channel->setModelViewProjectionMatrix (&m_puppetChannelProjection);
    channel->setModelViewProjectionMatrixInverse (&m_puppetChannelProjectionInverse);
    setupPuppetChannelGeometryCallback (channel, m_puppetChannelPosition);
    channel->render ();
}

void CImage::setup () {
    // do not double-init stuff, that's bad!
    if (this->m_initialized) {
	return;
    }

    // TODO: CHECK ORDER OF THINGS, 2419444134'S ID 27 DEPENDS ON 104'S COMPOSITE_A WHEN OUR LAST RENDER IS ON
    // COMPOSITE_B
    // TODO: SUPPORT PASSTHROUGH (IT'S A SHADER)
    if (this->m_image.model->passthrough) {
	// passthrough images without effects are bad, do not draw them
	if (this->m_image.effects.empty ()) {
	    return;
	}

    }

    const auto& debug = this->getScene ().getContext ().getApp ().getContext ().settings.render.debug;

    // copy pass to the composite layer
    for (const auto& cur : this->getImage ().model->material->passes) {
	this->m_passes.push_back (
	    new CPass (*this, std::make_shared<FBOProvider> (this), *cur, std::nullopt, std::nullopt, std::nullopt)
	);
    }

    m_basePassCount = m_passes.size ();

    // prepare the passes list
    if (!debug.baseOnly && !this->getImage ().effects.empty ()) {
	// generate the effects used by this material
	for (const auto& cur : this->m_image.effects) {
	    if (std::find (debug.skipEffects.begin (), debug.skipEffects.end (), static_cast<int> (cur->id))
		!= debug.skipEffects.end ()) {
		continue;
	    }

	    // do not add non-visible effects, this might need some adjustements tho as some effects might not be
	    // visible but affect the output of the image...
	    const auto fboProvider = std::make_shared<FBOProvider> (this);

	    // create all the fbos for this effect
	    for (const auto& fbo : cur->effect->fbos) {
		if (!effectConditionsMatch (fbo->conditions, cur->conditionCombos)) {
		    continue;
		}
		fboProvider->create (*fbo, this->m_texture->getFlags (), this->getSize ());
	    }

	    // TODO: MAKE USE OF ZIP OPERATOR IN BOOST? WAY OVERKILL JUST FOR THIS...

	    auto curEffect = cur->effect->passes.begin ();
	    auto endEffect = cur->effect->passes.end ();
	    auto curOverride = cur->passOverrides.begin ();
	    auto endOverride = cur->passOverrides.end ();

	    for (; curEffect != endEffect; ++curEffect) {
		const auto passIndex
		    = std::count_if (cur->effect->passes.begin (), curEffect, [] (const auto& descriptor) {
			  return descriptor->material.has_value ();
		      });
		curOverride = cur->passOverrides.begin () + std::min<size_t> (passIndex, cur->passOverrides.size ());
		if (!effectConditionsMatch ((*curEffect)->conditions, cur->conditionCombos)) {
		    continue;
		}
		if (!(*curEffect)->material.has_value ()) {
		    if (!(*curEffect)->command.has_value ()) {
			sLog.error ("Pass without material and command not supported");
			continue;
		    }

		    if (!(*curEffect)->source.has_value ()) {
			sLog.error ("Pass without material and source not supported");
			continue;
		    }

		    if (!(*curEffect)->target.has_value ()) {
			sLog.error ("Pass without material and target not supported");
			continue;
		    }

		    if ((*curEffect)->command != Command_Copy) {
			sLog.error ("Only copy command is supported for pass without material");
			continue;
		    }

		    auto virtualPass
			= std::make_unique<MaterialPass> (MaterialPass { .blending = BlendingMode_Normal,
									 .cullmode = CullingMode_Disable,
									 .depthtest = DepthtestMode_Disabled,
									 .depthwrite = DepthwriteMode_Disabled,
									 .shader = "commands/copy",
									 .textures = { { 0, *(*curEffect)->source } },
									 .combos = {},
									 .constants = {} });

		    const auto& config = *this->m_virtualPassess.emplace_back (std::move (virtualPass));

		    // build a pass for a copy shader
		    this->m_passes.push_back (new CPass (
			*this, fboProvider, config, std::nullopt, std::nullopt, (*curEffect)->target.value ()
		    ));
                    this->m_passEffects.emplace (this->m_passes.back (), cur.get ());
		} else {
		    for (auto& pass : (*curEffect)->material.value ()->passes) {
			const auto override = curOverride != endOverride
			    ? **curOverride
			    : std::optional<std::reference_wrapper<const ImageEffectPassOverride>> (std::nullopt);
			const auto target = (*curEffect)->target.has_value ()
			    ? *(*curEffect)->target
			    : std::optional<std::reference_wrapper<std::string>> (std::nullopt);

			this->m_passes.push_back (
			    new CPass (*this, fboProvider, *pass, override, (*curEffect)->binds, target)
			);
                        this->m_passEffects.emplace (this->m_passes.back (), cur.get ());
		    }

		    if (curOverride != endOverride) {
			++curOverride;
		    }
		}
	    }
	}
    }

    if (!debug.baseOnly) {
	const auto magentaCompositeTint = findMagentaCompositeTint (this->m_image, debug.skipEffects);
	if (magentaCompositeTint.has_value ()) {
	    auto tintOverride = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
		.id = -1,
		.combos = {
		    { "BLENDMODE", 30 },
		},
		.constants = {},
		.textures = {},
	    });
	    tintOverride->constants.emplace ("color", UserSettingBuilder::fromValue (magentaCompositeTint.value ()));
	    tintOverride->constants.emplace ("alpha", UserSettingBuilder::fromValue (1.0f));

	    this->m_materials.compatibilityMaterials.emplace_back (
		MaterialParser::load (this->getScene ().getScene ().project, "materials/effects/tint.json")
	    );
	    this->m_materials.compatibilityOverrides.emplace_back (std::move (tintOverride));

	    this->m_passes.push_back (new CPass (
		*this, std::make_shared<FBOProvider> (this),
		**this->m_materials.compatibilityMaterials.back ()->passes.begin (),
		*this->m_materials.compatibilityOverrides.back (), std::nullopt, std::nullopt
	    ));
	}
    }

    // extra render pass if there's any blending to be done
    if (!debug.baseOnly && this->m_image.colorBlendMode->value->getInt () > 0) {
	this->m_materials.colorBlending.material
	    = MaterialParser::load (this->getScene ().getScene ().project, "materials/util/effectpassthrough.json");
	this->m_materials.colorBlending.override = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
            .id = -1,
            .combos = {
                {"BLENDMODE", this->m_image.colorBlendMode->value->getInt()},
            },
            .constants = {},
            .textures = {},
        });

	this->m_passes.push_back (new CPass (
	    *this, std::make_shared<FBOProvider> (this), **this->m_materials.colorBlending.material->passes.begin (),
	    *this->m_materials.colorBlending.override, std::nullopt, std::nullopt
	));
    }

    if (m_puppetChannelMesh && m_passes.size () == m_basePassCount
        && !m_puppetVertexAlpha && !m_puppetClipping) {
        auto material = MaterialParser::load (getScene ().getScene ().project, "materials/util/effectpassthrough.json");
        m_materials.compatibilityMaterials.emplace_back (std::move (material));
        m_passes.push_back (new CPass (*this, std::make_shared<FBOProvider> (this),
            *m_materials.compatibilityMaterials.back ()->passes.front (), std::nullopt, std::nullopt, std::nullopt));
    }

    if (this->m_hasPuppetMesh && (this->m_puppetVertexAlpha || this->m_puppetClipping) && !this->m_passes.empty ()) {
	// Apply vertex alpha once, after authored effects, without replacing their shaders.
	auto material = std::make_unique<MaterialPass> (MaterialPass {
	    .blending = BlendingMode_Translucent,
	    .cullmode = CullingMode_Disable,
	    .depthtest = DepthtestMode_Disabled,
	    .depthwrite = DepthwriteMode_Disabled,
	    .shader = this->m_puppetClipping ? "genericimage4" : "genericimage3",
	    .combos = this->m_puppetVertexAlpha ? ComboMap { { "SKINNING", 1 }, { "SKINNING_ALPHA", 1 }, { "BONECOUNT", 2 } }
					    : ComboMap {},
	});
	const auto& config = *this->m_virtualPassess.emplace_back (std::move (material));
	auto* pass = new CPass (*this, std::make_shared<FBOProvider> (this), config, std::nullopt, std::nullopt, std::nullopt);
	static const glm::vec4 white (1);
	pass->addUniform ("g_Color4", &white);
	this->m_passes.push_back (pass);
    }

    this->m_baseBlending = this->m_passes.empty () ? BlendingMode_Normal : this->m_passes.front ()->getBlendingMode ();
    // if there's more than one pass the blendmode has to be moved from the beginning to the end
    if (this->m_passes.size () > 1) {
	const auto first = this->m_passes.begin ();
	const auto last = this->m_passes.rbegin ();

	(*last)->setBlendingMode ((*first)->getBlendingMode ());
	(*first)->setBlendingMode (BlendingMode_Normal);
    }

    if (m_puppetChannelMesh) {
	if (m_puppetChannelOffscreen) {
	    m_puppetChannelBaseMaterial = std::make_unique<MaterialPass> (MaterialPass {
		.blending = BlendingMode_Normal,
		.cullmode = CullingMode_Disable,
		.depthtest = DepthtestMode_Disabled,
		.depthwrite = DepthwriteMode_Disabled,
		.shader = "passthrough",
		.textures = {}, .combos = {}, .constants = {}
	    });
	    m_puppetChannelBasePass = new CPass (
		*this, std::make_shared<FBOProvider> (this), *m_puppetChannelBaseMaterial,
		std::nullopt, std::nullopt, std::nullopt);
	}
	m_puppetChannelPass = new CPass (
	    *this, std::make_shared<FBOProvider> (this),
	    **m_puppetChannelMaterial->passes.begin (), std::nullopt, std::nullopt, std::nullopt);
	m_puppetChannelPass->addUniform (
	    "g_BlendMap", m_puppetBlendMap.data (), int (m_puppetBlendRows));
	// Native 140207740 leaves the prepass device color at unity while the
	// `_rt_imageLayerAlbedo_` route is active. The final image material applies
	// the authored color once after sampling this texture.
	if (m_puppetChannelOffscreen)
	    m_puppetChannelPass->addUniform ("g_Color4", &m_puppetPrepassColor);
    }

    CRenderable::setup ();

    this->setupPasses ();
    this->m_initialized = true;
}

void CImage::setupPasses () {
    this->m_puppetMeshPass = nullptr;
    for (auto* pass : this->m_passes) pass->setGeometryCallback ({}, {}, {});
    this->m_activePasses.clear ();
    for (auto* pass : this->m_passes) {
        const auto effect = this->m_passEffects.find (pass);
        if (effect == this->m_passEffects.end () || effect->second->visible->value->getBool ())
            this->m_activePasses.push_back (pass);
    }
    this->m_currentMainFBO = this->m_mainFBO;
    this->m_currentSubFBO = this->m_subFBO;
    for (auto* pass : this->m_passes) pass->setBlendingMode (pass->getPass ().blending);
    if (!this->m_activePasses.empty ()) {
        this->m_activePasses.back ()->setBlendingMode (this->m_baseBlending);
        if (this->m_activePasses.size () > 1) this->m_activePasses.front ()->setBlendingMode (BlendingMode_Normal);
    }
    // do a pass on everything and setup proper inputs and values
    std::shared_ptr<const CFBO> drawTo = this->m_currentMainFBO;
    std::shared_ptr<const TextureProvider> asInput = m_puppetChannelFBO
        ? std::static_pointer_cast<const TextureProvider> (m_puppetChannelFBO) : getTexture ();
    GLuint texcoord = m_puppetChannelFBO ? getTexCoordPass () : getTexCoordCopy ();

    auto cur = this->m_activePasses.begin ();
    auto end = this->m_activePasses.end ();
    size_t passIndex = 0;
    bool first = true;
    bool inTargetEffectSequence = false;
    std::shared_ptr<const TextureProvider> effectInput = nullptr;

    for (; cur != end; ++cur, ++passIndex) {
	Effects::CPass* pass = *cur;
	std::shared_ptr<const CFBO> prevDrawTo = drawTo;
	bool writesToTarget = false;
	const bool isFirstPass = first;
	GLuint spacePosition = isFirstPass ? this->getCopySpacePosition () : this->getPassSpacePosition ();
	const glm::mat4* projection
	    = (isFirstPass) ? &this->m_modelViewProjectionCopy : &this->m_modelViewProjectionPass;
	const glm::mat4* inverseProjection
	    = (isFirstPass) ? &this->m_modelViewProjectionCopyInverse : &this->m_modelViewProjectionPassInverse;
	first = false;

	pass->setModelMatrix (&this->m_modelMatrix);
	pass->setViewProjectionMatrix (&this->m_viewProjectionMatrix);

	writesToTarget = this->configurePassTarget (pass, drawTo, asInput, effectInput, inTargetEffectSequence);
	// determine if it's the last element in the list as this is a screen-copy-like process
	// TODO: PROPERLY CHECK IF THIS IS ALL THAT'S NEEDED
	const bool isFinalPass = !writesToTarget && this->shouldRenderFinalPass (std::next (cur) == end);
	if (isFinalPass) {
	    spacePosition = this->m_hasPuppetMesh ? this->m_puppetSceneSpacePosition : this->getSceneSpacePosition ();
	    drawTo = this->getScene ().getFBO ();
	    projection = &this->m_modelViewProjectionScreen;
	    inverseProjection = &this->m_modelViewProjectionScreenInverse;
	}

	// Effects and their masks use the source atlas coordinates; warp the fully effected atlas into the scene.
	if (isFinalPass && this->m_hasPuppetMesh) {
	    pass->setBlendingMode (BlendingMode_Translucent);
	    this->setupPuppetGeometryCallback (pass, &this->m_puppetSceneSpacePosition, isFirstPass);
	    this->m_puppetMeshPass = pass;
	    if (this->m_puppetClipping) this->setupPuppetClipping (asInput);
	}

        if (m_puppetChannelFBO && passIndex < m_basePassCount) pass->setTexture (0, asInput);
        if (m_puppetChannelMesh && !m_puppetChannelOffscreen && passIndex + 1 == m_basePassCount)
            m_puppetChannelDirectTarget = drawTo;
	pass->setDestination (drawTo);
	pass->setInput (asInput);
	pass->setPreviousInput (inTargetEffectSequence ? effectInput : nullptr);
	pass->setPosition (spacePosition);
	pass->setTexCoord (texcoord);
	pass->setModelViewProjectionMatrix (projection);
	pass->setModelViewProjectionMatrixInverse (inverseProjection);

	texcoord = this->getTexCoordPass ();

	if (writesToTarget) {
	    asInput = drawTo;
	    drawTo = prevDrawTo;
	} else {
	    drawTo = prevDrawTo;
	    this->pinpongFramebuffer (&drawTo, &asInput);
	    inTargetEffectSequence = false;
	    effectInput = nullptr;
	}
    }
}

bool CImage::shouldRenderFinalPass (bool isLastPass) const {
    if (!isLastPass || !this->getImage ().visible->value->getBool ()) {
	return false;
    }

    const auto& debug = this->getScene ().getContext ().getApp ().getContext ().settings.render.debug;
    return !(debug.noSolidFinal && this->getImage ().model->solidlayer);
}

bool CImage::configurePassTarget (
    Effects::CPass* pass, std::shared_ptr<const CFBO>& drawTo, const std::shared_ptr<const TextureProvider>& asInput,
    std::shared_ptr<const TextureProvider>& effectInput, bool& inTargetEffectSequence
) {
    if (!pass->getTarget ().has_value ()) {
	return false;
    }

    const std::string target = pass->getTarget ().value ();
    std::shared_ptr<const CFBO> resolved = pass->getFBOProvider ()->find (target);
    if (resolved == nullptr) {
	resolved = this->getScene ().findFBO (target);
    }
    if (resolved == nullptr) {
	sLog.error (
	    "Pass target FBO '", target, "' could not be resolved for object ", pass->getRenderable ().getId (),
	    " shader=", pass->getPass ().shader
	);
	return false;
    }

    if (!inTargetEffectSequence) {
	effectInput = asInput;
	inTargetEffectSequence = true;
    }
    drawTo = resolved;
    return true;
}

void CImage::pinpongFramebuffer (std::shared_ptr<const CFBO>* drawTo, std::shared_ptr<const TextureProvider>* asInput) {
    // temporarily store FBOs used
    std::shared_ptr<const CFBO> currentMainFBO = this->m_currentMainFBO;
    std::shared_ptr<const CFBO> currentSubFBO = this->m_currentSubFBO;

    if (drawTo != nullptr) {
	*drawTo = currentSubFBO;
    }
    if (asInput != nullptr) {
	*asInput = currentMainFBO;
    }

    // swap the FBOs
    this->m_currentMainFBO = currentSubFBO;
    this->m_currentSubFBO = currentMainFBO;
}

void CImage::render () {
    // do not try to render something that did not initialize successfully
    if (!this->m_initialized) {
	return;
    }

    if (!this->getImage ().visible->value->getBool ()) {
	return;
    }

    glColorMask (true, true, true, true);
    std::vector<CPass*> active;
    for (auto* pass : this->m_passes) {
        const auto effect = this->m_passEffects.find (pass);
        if (effect == this->m_passEffects.end () || effect->second->visible->value->getBool ()) active.push_back (pass);
    }
    if (active != this->m_activePasses) this->setupPasses ();

    // Always update screen transform (handles rotation + parallax dynamically)
    this->updateScreenSpacePosition ();
    m_effectiveColor4 = m_image.color->value->getVec4 ();
    m_effectiveColor4.a *= m_image.alpha->value->getFloat ();

    if (this->m_hasPuppetMesh) {
	this->updatePuppetAnimation ();
    }

#if !NDEBUG
    std::string str = "Image ";

    if (this->getScene ().getScene ().camera.bloom.enabled->value->getBool () && this->getId () == -1) {
	str += "bloom";
    } else {
	str += this->getImage ().name + " (" + std::to_string (this->getId ()) + ", "
	    + this->getImage ().model->material->filename + ")";
    }

    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif /* DEBUG */

    for (size_t row = 0; row < m_puppetBlendRows; ++row)
        for (int lane = 0; lane < 4; ++lane) m_puppetBlendMap[row][lane] = m_rig.blendMap[row * 4 + lane];
    renderPuppetChannelPrepass ();
    auto cur = this->m_activePasses.begin ();
    size_t passIndex = 0;

    for (const auto end = this->m_activePasses.end (); cur != end; ++cur, ++passIndex) {
	if (*cur == this->m_puppetMeshPass && this->m_puppetClipTargetPass) {
	    this->renderPuppetClipped (*cur);
	} else {
	    (*cur)->render ();
	}
        if (m_puppetChannelMesh && !m_puppetChannelOffscreen && passIndex + 1 == m_basePassCount)
            renderPuppetChannelDirect (m_puppetChannelDirectTarget);
    }

#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */
}

const float& CImage::getBrightness () const { return this->m_image.brightness->value->getFloat (); }

const float& CImage::getUserAlpha () const { return this->m_image.alpha->value->getFloat (); }

const float& CImage::getAlpha () const { return this->m_image.alpha->value->getFloat (); }

const glm::vec3& CImage::getColor () const { return this->m_image.color->value->getVec3 (); }

const glm::vec4& CImage::getColor4 () const { return m_effectiveColor4; }

const glm::vec3& CImage::getCompositeColor () const { return this->m_image.color->value->getVec3 (); }

glm::vec2 CImage::resolveGeometrySize (float sceneWidth, float sceneHeight, glm::vec3& origin) const {
    // puppet canvases are expanded to fit the animated mesh (see loadPuppetMesh)
    if (this->m_hasPuppetMesh && this->m_puppetSize.x > 0.0f && this->m_puppetSize.y > 0.0f) {
	return this->m_puppetSize;
    }

    glm::vec2 size = this->getSize ();

    if ((size.x == 0.0f || size.y == 0.0f) && this->m_texture != nullptr) {
	size.x = static_cast<float> (this->m_texture->getRealWidth ());
	size.y = static_cast<float> (this->m_texture->getRealHeight ());
    } else if (
	(size.x == 0.0f || size.y == 0.0f) && this->getImage ().model->width.has_value ()
	&& this->getImage ().model->height.has_value ()
    ) {
	size.x = static_cast<float> (this->getImage ().model->width.value ());
	size.y = static_cast<float> (this->getImage ().model->height.value ());
    }

    if (this->getImage ().model->fullscreen) {
	size = { sceneWidth, sceneHeight };
	origin = { sceneWidth / 2.0f, sceneHeight / 2.0f, 0.0f };
    }

    return size;
}

void CImage::updateScenePosition (
    const glm::vec3& origin_in, const glm::vec2& size, const glm::vec3& scale, float sceneWidth, float sceneHeight
) {
    glm::vec3 origin = origin_in;

    // note: model cropoffset is deliberately NOT applied anywhere — it's editor metadata;
    // object origins already refer to the cropped canvas center (verified against WE)

    const glm::vec2 scaledSize = size * glm::vec2 (scale);
    this->m_pos.x = origin.x - (scaledSize.x / 2.0f);
    this->m_pos.w = origin.y + (scaledSize.y / 2.0f);
    this->m_pos.z = origin.x + (scaledSize.x / 2.0f);
    this->m_pos.y = origin.y - (scaledSize.y / 2.0f);

    if (this->getImage ().alignment.find ("top") != std::string::npos) {
	this->m_pos.y -= scaledSize.y / 2.0f;
	this->m_pos.w -= scaledSize.y / 2.0f;
    } else if (this->getImage ().alignment.find ("bottom") != std::string::npos) {
	this->m_pos.y += scaledSize.y / 2.0f;
	this->m_pos.w += scaledSize.y / 2.0f;
    }

    if (this->getImage ().alignment.find ("left") != std::string::npos) {
	this->m_pos.x += scaledSize.x / 2.0f;
	this->m_pos.z += scaledSize.x / 2.0f;
    } else if (this->getImage ().alignment.find ("right") != std::string::npos) {
	this->m_pos.x -= scaledSize.x / 2.0f;
	this->m_pos.z -= scaledSize.x / 2.0f;
    }

    this->m_pos.x -= sceneWidth / 2.0f;
    this->m_pos.y = sceneHeight / 2.0f - this->m_pos.y;
    this->m_pos.z -= sceneWidth / 2.0f;
    this->m_pos.w = sceneHeight / 2.0f - this->m_pos.w;
}

void CImage::uploadGeometryBuffers (const glm::vec2& size) {
    GLfloat sceneSpacePosition[] = { this->m_pos.x, this->m_pos.y, 0.0f, this->m_pos.x, this->m_pos.w, 0.0f,
				     this->m_pos.z, this->m_pos.y, 0.0f, this->m_pos.z, this->m_pos.y, 0.0f,
				     this->m_pos.x, this->m_pos.w, 0.0f, this->m_pos.z, this->m_pos.w, 0.0f };

    float width = 1.0f;
    float height = 1.0f;
    if (this->getTexture () != nullptr && !this->getTexture ()->isAnimated ()
	&& (this->getTexture ()->getTextureWidth (0) != this->getTexture ()->getRealWidth ()
	    || this->getTexture ()->getTextureHeight (0) != this->getTexture ()->getRealHeight ())) {
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }

    float x = 0.0f;
    float y = 0.0f;
    GLfloat realWidth = size.x;
    GLfloat realHeight = size.y;
    GLfloat realX = 0.0f;
    GLfloat realY = 0.0f;

    if (this->getImage ().model->passthrough) {
	width = 1.0f;
	height = 1.0f;
	realX = this->m_pos.x;
	realY = this->m_pos.w;
	realWidth = this->m_pos.z;
	realHeight = this->m_pos.y;

	if (this->getImage ().model->fullscreen) {
	    realX = -1.0f;
	    realY = -1.0f;
	    realWidth = 1.0f;
	    realHeight = 1.0f;
	}
    }

    GLfloat texcoordCopy[] = { x, height, x, y, width, height, width, height, x, y, width, y };
    GLfloat copySpacePosition[] = { realX,     realHeight, 0.0f, realX, realY, 0.0f, realWidth, realHeight, 0.0f,
				    realWidth, realHeight, 0.0f, realX, realY, 0.0f, realWidth, realY,      0.0f };

    glBindBuffer (GL_ARRAY_BUFFER, this->m_sceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (sceneSpacePosition), sceneSpacePosition, GL_DYNAMIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_copySpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (copySpacePosition), copySpacePosition, GL_DYNAMIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordCopy);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordCopy), texcoordCopy, GL_DYNAMIC_DRAW);

    this->m_sceneCenter
	= glm::vec3 ((this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f);
    this->m_modelViewProjectionCopy = this->getImage ().model->passthrough
	? this->m_modelViewProjectionScreen
	: glm::ortho<float> (0.0, size.x, 0.0, size.y);
    this->m_modelViewProjectionCopyInverse = glm::inverse (this->m_modelViewProjectionCopy);
    this->m_modelMatrix = glm::ortho<float> (0.0, size.x, 0.0, size.y);
}

CImage::ResolvedTransform CImage::updateGeometryBuffers () {
    auto sceneWidth = static_cast<float> (this->getScene ().getWidth ());
    auto sceneHeight = static_cast<float> (this->getScene ().getHeight ());
    const auto transform = this->resolveTransform (this->getImage ());
    glm::vec3 origin = transform.origin;
    const glm::vec3 scale = transform.scale;
    const glm::vec2 size = this->resolveGeometrySize (sceneWidth, sceneHeight, origin);
    const glm::vec2 previousSize = this->m_size;
    this->m_size = size;
    if (this->m_hasPuppetMesh && size != previousSize) {
	this->m_puppetSize = size;
    }

    this->updateScenePosition (origin, size, scale, sceneWidth, sceneHeight);
    if (this->m_hasPuppetMesh) {
	this->updatePuppetScenePositionBuffer (size, this->m_puppetRawPositions);
    }
    this->uploadGeometryBuffers (size);
    return transform;
}

void CImage::updateScreenSpacePosition () {
    const ResolvedTransform transform = this->updateGeometryBuffers ();

    // Build rotation from angles (already in radians from scene.json — see CParticle.cpp:2119)
    // Negate X and Z rotations to account for Y-flipped coordinate system (CParticle.cpp:2120)
    const float angle = transform.angle;
    glm::mat4 rotModel = glm::mat4 (1.0f);
    if (angle != 0.0f) {
	rotModel = glm::translate (rotModel, this->m_sceneCenter);
	rotModel = glm::rotate (rotModel, -angle, glm::vec3 (0.0f, 0.0f, 1.0f));
	rotModel = glm::translate (rotModel, -this->m_sceneCenter);
    }

    glm::mat4 mvp
	= this->getScene ().getCamera ().getProjection () * this->getScene ().getCamera ().getLookAt () * rotModel;

    // Apply parallax displacement if enabled
    if (this->getScene ().getScene ().camera.parallax.enabled->value->getBool ()
	&& !this->getScene ().getContext ().getApp ().getContext ().settings.mouse.disableparallax) {
	const glm::vec2 depth = this->getImage ().parallaxDepth->value->getVec2 ();
	const glm::vec2* displacement = this->getScene ().getParallaxDisplacement ();
	const float referenceSize = static_cast<float> (this->getScene ().getWidth ());
	// The scene displacement already includes the global amount; zero depth must remain stationary.
	float x = depth.x * displacement->x * referenceSize;
	float y = depth.y * displacement->y * referenceSize;
	mvp = glm::translate (mvp, { x, y, 0.0f });
    }

    this->m_modelViewProjectionScreen = mvp;
    this->m_modelViewProjectionScreenInverse = glm::inverse (mvp);
    if (this->getImage ().model->passthrough) {
	this->m_modelViewProjectionCopy = this->m_modelViewProjectionScreen;
	this->m_modelViewProjectionCopyInverse = this->m_modelViewProjectionScreenInverse;
    }
}

const Image& CImage::getImage () const { return this->m_image; }

glm::vec2 CImage::getSize () const {
    for (const auto& pass : this->getImage ().model->material->passes) {
	if (!pass->usertextures.empty () && this->getImage ().size.x > 0.0f && this->getImage ().size.y > 0.0f) {
	    return this->getImage ().size;
	}
    }

    if (this->m_texture == nullptr) {
	return this->getImage ().size;
    }

    return { this->m_texture->getRealWidth (), this->m_texture->getRealHeight () };
}

GLuint CImage::getSceneSpacePosition () const { return this->m_sceneSpacePosition; }

GLuint CImage::getCopySpacePosition () const { return this->m_copySpacePosition; }

GLuint CImage::getPassSpacePosition () const { return this->m_passSpacePosition; }

GLuint CImage::getTexCoordCopy () const { return this->m_texcoordCopy; }

GLuint CImage::getTexCoordPass () const { return this->m_texcoordPass; }
