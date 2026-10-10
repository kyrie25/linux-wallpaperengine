#include "PuppetScriptObject.h"

#include "ScriptEngine.h"
#include "Adapters/ScriptableObjectAdapter.h"
#include "LayerPropertyWrites.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Render/Objects/CImage.h"
#include <cmath>
#include <cstring>
#include <glm/gtc/type_ptr.hpp>

using namespace WallpaperEngine::Scripting;
using namespace WallpaperEngine::Render::Objects;
using WallpaperEngine::Data::Utils::ScopeGuard;

namespace WallpaperEngine::Scripting {
struct PuppetAnimationHandle {
    PuppetScriptObject& host;
    std::shared_ptr<ScriptableObject::Lifetime> lifetime;
    size_t serial;
};
}

namespace {
enum Method {
    Count, Get, Create, Single, Destroy, BoneCount, BoneIndex, BoneParent,
    GetOrigin, SetOrigin, GetAngles, SetAngles, GetWorld, SetWorld, GetLocal, SetLocal, Impulse, Reset
};
enum AnimationMethod {
    Play, Pause, Stop, Playing, CurrentFrame, Seek, Ended, Fps, FrameCount, Duration, Name, Visible, Rate, Blend,
    SetVisible, SetRate, SetBlend
};
constexpr uint32_t paused = 0x20000000, stopped = 0x40000000, frameSet = 0x2000000;
const char* methods[] = { "getAnimationLayerCount", "getAnimationLayer", "createAnimationLayer", "playSingleAnimation",
    "destroyAnimationLayer", "getBoneCount", "getBoneIndex", "getBoneParentIndex", "getLocalBoneOrigin",
    "setLocalBoneOrigin", "getLocalBoneAngles", "setLocalBoneAngles", "getBoneTransform", "setBoneTransform",
    "getLocalBoneTransform", "setLocalBoneTransform", "applyBonePhysicsImpulse", "resetBonePhysicsSimulation" };

Data::JSON::JSON jsonArgument (JSContext* ctx, JSValueConst value, bool parseString) {
    if (JS_IsString (value) && !parseString) {
        const char* text = JS_ToCString (ctx, value);
        if (!text) return {};
        Data::JSON::JSON result = std::string (text);
        JS_FreeCString (ctx, text);
        return result;
    }
    JSValue encoded = JS_IsString (value) ? JS_DupValue (ctx, value)
        : JS_JSONStringify (ctx, value, JS_UNDEFINED, JS_UNDEFINED);
    ScopeGuard release ([&] { JS_FreeValue (ctx, encoded); });
    if (JS_IsException (encoded) || JS_IsUndefined (encoded)) return {};
    const char* text = JS_ToCString (ctx, encoded);
    if (!text) return {};
    auto result = Data::JSON::JSON::parse (text, nullptr, false);
    JS_FreeCString (ctx, text);
    return result;
}

glm::vec3 readVector (JSContext* ctx, JSValueConst value) {
    float result[3] {};
    convertLayerVector (ctx, value, 3, false, result);
    return glm::make_vec3 (result);
}

glm::vec3 boneAngles (const glm::mat4& local) {
    const float* m = glm::value_ptr (local);
    const float z = std::atan2 (m[1], m[0]);
    const float y = std::atan2 (-m[2], std::hypot (m[6], m[10]));
    const float x = std::atan2 (std::sin (z) * m[8] - std::cos (z) * m[9],
                              std::cos (z) * m[5] - std::sin (z) * m[4]);
    return { x, y, z };
}

JSValue matrixValue (JSContext* ctx, const glm::mat4& matrix) {
    JSValue global = JS_GetGlobalObject (ctx);
    JSValue constructor = JS_GetPropertyStr (ctx, global, "Mat4");
    JSValue result = JS_CallConstructor (ctx, constructor, 0, nullptr);
    JS_FreeValue (ctx, constructor);
    JS_FreeValue (ctx, global);
    if (JS_IsException (result)) return result;
    JSValue values = JS_GetPropertyStr (ctx, result, "m");
    for (uint32_t i = 0; i < 16; ++i) JS_SetPropertyUint32 (ctx, values, i, JS_NewFloat64 (ctx, glm::value_ptr (matrix)[i]));
    JS_FreeValue (ctx, values);
    return result;
}

bool readMatrix (JSContext* ctx, JSValueConst value, glm::mat4& matrix) {
    JSValue values = JS_GetPropertyStr (ctx, value, "m");
    ScopeGuard release ([&] { JS_FreeValue (ctx, values); });
    if (JS_IsException (values)) return false;
    if (!JS_IsObject (values)) return false;
    for (uint32_t i = 0; i < 16; ++i) {
        JSValue part = JS_GetPropertyUint32 (ctx, values, i);
        double number = 0;
        const bool valid = JS_IsNumber (part) && JS_ToFloat64 (ctx, &number, part) == 0 && std::isfinite (number);
        JS_FreeValue (ctx, part);
        if (!valid) return false;
        glm::value_ptr (matrix)[i] = float (number);
    }
    return true;
}
}

PuppetScriptObject::PuppetScriptObject (ScriptEngine& engine) : m_engine (engine) {
    JS_NewClassID (engine.getRuntime (), &m_animationClass);
    JSClassDef definition { .class_name = "AnimationLayer", .finalizer = [] (JSRuntime*, JSValue value) {
        delete static_cast<PuppetAnimationHandle*> (JS_GetOpaque (value, JS_GetClassID (value)));
    } };
    JS_NewClass (engine.getRuntime (), m_animationClass, &definition);
}

PuppetScriptObject::~PuppetScriptObject () {
    for (const auto& [key, value] : m_instances) JS_FreeValue (m_engine.getContext (), value);
}

JSValue PuppetScriptObject::method (JSValueConst layer, const char* name) {
    for (int method = Count; method <= Reset; ++method) {
        if (std::strcmp (name, methods[method]) == 0) {
            JSValue data = JS_DupValue (m_engine.getContext (), layer);
            JSValue result = JS_NewCFunctionData (m_engine.getContext (), layerCall, 1, method, 1, &data);
            JS_FreeValue (m_engine.getContext (), data);
            return result;
        }
    }
    return JS_UNDEFINED;
}

JSValue PuppetScriptObject::layerCall (JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int method, JSValue* data) {
    auto* object = Adapters::ScriptableObjectAdapter::objectOf (data[0]);
    if (!object || !object->is<CImage> ()) return method == Count || method == BoneCount ? JS_NewInt32 (ctx, 0) : JS_NULL;
    auto& image = *object->as<CImage> ();
    auto& rig = image.getRig ();
    auto& host = image.getScene ().getScriptEngine ().getPuppetScripts ();
    if (method == Count) return JS_NewInt64 (ctx, rig.getLayerCount ());
    if (method == BoneCount) return JS_NewInt64 (ctx, rig.bones.size ());
    if (!argc) return JS_NULL;
    std::optional<size_t> serial;
    int32_t index = -1;
    std::string name;
    if (JS_IsNumber (argv[0])) JS_ToInt32 (ctx, &index, argv[0]);
    if (JS_IsString (argv[0])) {
        const char* text = JS_ToCString (ctx, argv[0]);
        if (!text) return JS_EXCEPTION;
        name = text;
        JS_FreeCString (ctx, text);
    }
    if (method == Create || method == Single) {
        const auto animation = jsonArgument (ctx, argv[0], false);
        const auto config = argc > 1 ? jsonArgument (ctx, argv[1], true) : Data::JSON::JSON ();
        if (JS_HasException (ctx)) return JS_EXCEPTION;
        serial = rig.createLayer (animation, config, method == Single, image.getScene ().getScene ().project);
        if (serial) image.registerAnimationLayerProperties (*serial);
        return serial ? host.animation (image, *serial) : JS_NULL;
    }
    if (method == Get || method == Destroy) {
        serial = JS_IsNumber (argv[0]) ? rig.getLayerAt (index) : rig.findLayerByName (name);
        if (method == Get) return serial ? host.animation (image, *serial) : JS_NULL;
        if (JS_IsObject (argv[0])) {
            auto* handle = static_cast<PuppetAnimationHandle*> (JS_GetOpaque (argv[0], host.m_animationClass));
            if (handle && handle->lifetime == image.getLifetime ()) serial = handle->serial;
        }
        return JS_NewBool (ctx, JS_IsString (argv[0]) ? rig.destroyLayersByName (name) : serial && rig.destroyLayer (*serial));
    }
    if (method == BoneIndex) return JS_NewInt32 (ctx, name.empty () ? -1 : rig.findBone (name));
    if (JS_IsString (argv[0])) index = name.empty () && (method == Impulse || method == Reset) ? 0 : rig.findBone (name);
    if (method == BoneParent) return JS_NewInt32 (ctx, index >= 0 && size_t (index) < rig.bones.size () ? rig.bones[index].parent : -1);
    if (index < 0 || size_t (index) >= rig.bones.size ()) return JS_UNDEFINED;
    if (method == Impulse) {
        if (argc >= 3) rig.applyBonePhysicsImpulse (index, readVector (ctx, argv[1]), readVector (ctx, argv[2]));
    } else if (method == Reset) rig.resetBonePhysics (index);
    else if (rig.hasPose ()) {
        auto local = rig.getLocalBoneTransform (index);
        if (method == GetWorld) return matrixValue (ctx, rig.getBoneTransform (index));
        if (method == GetLocal) return matrixValue (ctx, local);
        if (method == GetOrigin || method == GetAngles) {
            DynamicValue value (method == GetOrigin ? glm::vec3 (local[3]) : boneAngles (local));
            return host.m_engine.dynamicToJs (value, true);
        }
        if (argc >= 2) {
            if (method == SetOrigin) local[3] = glm::vec4 (readVector (ctx, argv[1]), local[3].w);
            else if (method == SetAngles) {
                const auto origin = local[3];
                local = glm::mat4_cast (glm::quat (readVector (ctx, argv[1])));
                local[3] = origin;
            } else if (!readMatrix (ctx, argv[1], local)) return JS_HasException (ctx) ? JS_EXCEPTION : JS_UNDEFINED;
            if (JS_HasException (ctx)) return JS_EXCEPTION;
            if (method == SetWorld) rig.setBoneTransform (index, local, image.puppetWorld ());
            else rig.setLocalBoneTransform (index, local, image.puppetWorld ());
        }
    }
    return JS_HasException (ctx) ? JS_EXCEPTION : JS_UNDEFINED;
}

JSValue PuppetScriptObject::animation (ScriptableObject& image, size_t serial) {
    const auto key = std::pair<const void*, size_t> { image.getLifetime ().get (), serial };
    JSContext* ctx = m_engine.getContext ();
    if (const auto found = m_instances.find (key); found != m_instances.end ()) return JS_DupValue (ctx, found->second);
    JSValue value = JS_NewObjectClass (ctx, m_animationClass);
    JS_SetOpaque (value, new PuppetAnimationHandle { *this, image.getLifetime (), serial });
    JSValue classId = JS_NewInt32 (ctx, m_animationClass);
    const char* functions[] = { "play", "pause", "stop", "isPlaying", "getFrame", "setFrame", "addEndedCallback" };
    for (int method = Play; method <= Ended; ++method)
        JS_SetPropertyStr (ctx, value, functions[method], JS_NewCFunctionData (ctx, animationCall, 1, method, 1, &classId));
    const char* properties[] = { "fps", "frameCount", "duration", "name", "visible", "rate", "blend" };
    for (int method = Fps; method <= Blend; ++method) {
        JSAtom atom = JS_NewAtom (ctx, properties[method - Fps]);
        JS_DefinePropertyGetSet (ctx, value, atom,
            JS_NewCFunctionData (ctx, animationCall, 0, method, 1, &classId),
            method < Visible ? JS_UNDEFINED : JS_NewCFunctionData (ctx, animationCall, 1, method + SetVisible - Visible, 1, &classId),
            JS_PROP_ENUMERABLE);
        JS_FreeAtom (ctx, atom);
    }
    JS_SetPropertyStr (ctx, value, "__endedCallbacks", JS_NewArray (ctx));
    JS_FreeValue (ctx, classId);
    m_instances.emplace (key, JS_DupValue (ctx, value));
    return value;
}

JSValue PuppetScriptObject::animationCall (JSContext* ctx, JSValueConst receiver, int argc, JSValueConst* argv, int method, JSValue* data) {
    int32_t classId = 0;
    if (JS_ToInt32 (ctx, &classId, data[0]) < 0) return JS_EXCEPTION;
    auto* handle = static_cast<PuppetAnimationHandle*> (JS_GetOpaque2 (ctx, receiver, classId));
    if (!handle) return JS_EXCEPTION;
    if (!handle || !handle->lifetime->object) return JS_UNDEFINED;
    auto& rig = handle->lifetime->object->as<CImage> ()->getRig ();
    auto* clock = rig.findLayer (handle->serial);
    if (!clock) return method == Playing ? JS_FALSE : JS_UNDEFINED;
    const auto& layer = *clock->layer;
    switch (method) {
        case Play: if (clock->flags & stopped) clock->time = 0; clock->flags &= ~(paused | stopped); break;
        case Pause: clock->flags |= paused; break;
        case Stop: clock->time = 0; clock->flags = (clock->flags | paused) & 0x3fffffff; break;
        case Playing: return JS_NewBool (ctx, !(clock->flags & (paused | stopped)));
        case CurrentFrame: return JS_NewFloat64 (ctx, clock->time * clock->clip.fps);
        case Seek: if (argc && JS_IsNumber (argv[0])) {
            double frame = 0;
            if (JS_ToFloat64 (ctx, &frame, argv[0]) < 0) return JS_EXCEPTION;
            if (std::isfinite (frame)) { clock->time = float (frame) / clock->clip.fps; clock->flags |= frameSet; }
        } break;
        case Ended: if (argc && JS_IsFunction (ctx, argv[0])) {
            JSValue callbacks = JS_GetPropertyStr (ctx, receiver, "__endedCallbacks");
            JSValue length = JS_GetPropertyStr (ctx, callbacks, "length");
            uint32_t count = 0;
            JS_ToUint32 (ctx, &count, length);
            JS_SetPropertyUint32 (ctx, callbacks, count, JS_DupValue (ctx, argv[0]));
            JS_FreeValue (ctx, length); JS_FreeValue (ctx, callbacks);
        } break;
        case Fps: return JS_NewFloat64 (ctx, clock->clip.fps);
        case FrameCount: return JS_NewUint32 (ctx, clock->clip.frameCount);
        case Duration: return JS_NewFloat64 (ctx, clock->clip.frameCount / clock->clip.fps);
        case Name: return JS_NewString (ctx, layer.name.c_str ());
        case Visible: return handle->host.m_engine.dynamicToJs (*layer.visible->value, true);
        case Rate: return JS_NewFloat64 (ctx, layer.rate->value->getFloat ());
        case Blend: return JS_NewFloat64 (ctx, layer.blend->value->getFloat ());
        case SetVisible:
            if (argc) handle->host.m_engine.updateValue (argv[0], *layer.visible->value);
            break;
        case SetRate: case SetBlend:
            if (argc) {
                float value = 0;
                if (convertLayerFloat (ctx, argv[0], value) == LayerWrite::Assign)
                    (method == SetRate ? layer.rate : layer.blend)->value->update (value, DynamicValue::Script);
            }
            break;
    }
    return JS_HasException (ctx) ? JS_EXCEPTION : JS_UNDEFINED;
}

void PuppetScriptObject::finishFrame (CImage& image) {
    image.getRig ().finishEndedLayers ([&] (size_t serial) {
        const auto found = m_instances.find ({ image.getLifetime ().get (), serial });
        if (found == m_instances.end ()) return;
        JSContext* ctx = m_engine.getContext ();
        JSValue callbacks = JS_GetPropertyStr (ctx, found->second, "__endedCallbacks");
        JSValue length = JS_GetPropertyStr (ctx, callbacks, "length");
        uint32_t count = 0;
        JS_ToUint32 (ctx, &count, length);
        JS_FreeValue (ctx, length);
        for (uint32_t i = 0; i < count; ++i) {
            JSValue callback = JS_GetPropertyUint32 (ctx, callbacks, i);
            m_engine.callLayerCallback (image, callback, found->second);
            JS_FreeValue (ctx, callback);
        }
        JS_FreeValue (ctx, callbacks);
    });
    for (const auto& removed : image.getRig ().takeRemovedLayers ()) {
        const auto prefix = "animationlayer" + std::to_string (image.getId ()) + "[" + std::to_string (removed.serial) + "].";
        for (const auto* name : { "visible", "rate", "blend" }) m_engine.removeScript (prefix + name);
        const auto found = m_instances.find ({ image.getLifetime ().get (), removed.serial });
        if (found != m_instances.end ()) { JS_FreeValue (m_engine.getContext (), found->second); m_instances.erase (found); }
    }
}
