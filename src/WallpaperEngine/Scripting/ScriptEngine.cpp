#include "ScriptEngine.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "Modules/ColorModule.h"
#include "Modules/MathModule.h"
#include "Modules/ScriptModule.h"
#include "ScriptPropertiesObject.h"
#include "LocalStorageObject.h"
#include "PuppetScriptObject.h"
#include "TextureAnimationObject.h"
#include "LayerPropertyWrites.h"
#include "SceneSettingProperties.h"
#include "ScriptableObject.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/Builtins.generated.h"
#include "quickjs.h"

#include <algorithm>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <future>
#include <optional>
#include <poll.h>
#include <ranges>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace WallpaperEngine::Render::Objects {
class CSound;
}
using namespace WallpaperEngine::Scripting;
using namespace WallpaperEngine::Data::Model;

extern char** environ;
extern float g_Time;
extern float g_TimeLast;

void scriptengine_dump (JSContext* ctx, JSValueConst obj) {
    JSPropertyEnum* props;
    uint32_t len;

    if (JS_GetOwnPropertyNames (ctx, &props, &len, obj, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) {
	return;
    }

    for (uint32_t i = 0; i < len; ++i) {
	const char* name = JS_AtomToCString (ctx, props[i].atom);

	JSValue val = JS_GetProperty (ctx, obj, props[i].atom);

	const char* value_str = JS_ToCString (ctx, val);

	printf ("%s = %s\n", name, value_str ? value_str : "<non-string>");

	JS_FreeCString (ctx, value_str);
	JS_FreeValue (ctx, val);
	JS_FreeCString (ctx, name);
    }

    js_free (ctx, props);
}

JSModuleDef* scriptengine_module_loader (JSContext* ctx, const char* module, void* opaque) {
    const auto* scriptEngine = static_cast<ScriptEngine*> (opaque);

    const auto& modules = scriptEngine->getModules ();
    const auto it = modules.find (module);

    if (it == modules.end ()) {
	return scriptEngine->loadAssetModule (ctx, module);
    }

    return it->second->getDefinition ();
}

JSModuleDef* ScriptEngine::loadAssetModule (JSContext* context, const char* name) const {
    return m_assetModules->load (context, name);
}

JSValue ScriptEngine::dynamicToJs (DynamicValue& value, bool snapshot, bool angle) const {
    if (angle && value.getType () == DynamicValue::Vec3) {
        DynamicValue degrees (value.getVec3 () / kLayerDegreesToRadians);
        return m_adapters.vec3->instantiate (degrees, true);
    }
    switch (value.getType ()) {
	case DynamicValue::Null:
	    return JS_NULL;
	case DynamicValue::String:
	    return JS_NewString (this->m_context, value.getString ().c_str ());
	case DynamicValue::Float:
	    return JS_NewFloat64 (this->m_context, value.getFloat ());
	case DynamicValue::Int:
	    return JS_NewInt32 (this->m_context, value.getInt ());
	case DynamicValue::Boolean:
	    return JS_NewBool (this->m_context, value.getBool ());
	case DynamicValue::Vec2:
	    return snapshot ? this->m_adapters.vec2->instantiate (value, true)
	                    : this->m_adapters.vec2->instantiate (value);
	case DynamicValue::Vec3:
	    return snapshot ? this->m_adapters.vec3->instantiate (value, true)
	                    : this->m_adapters.vec3->instantiate (value);
	case DynamicValue::Vec4:
	    return snapshot ? this->m_adapters.vec4->instantiate (value, true)
	                    : this->m_adapters.vec4->instantiate (value);
	default:
	    return JS_UNDEFINED;
    }
}

ScriptEngine::ScriptEngine (Wallpapers::CScene& scene, Media::MediaSource& mediaSource) :
    m_scene (scene), m_mediaSource (mediaSource) {
    this->m_unregisterMediaUpdateCallback
	= mediaSource.addMetadataListener ([this] (const Media::MediaSource::MediaInfo& info) {
	      this->notifyMediaUpdate (info);
	  });

    this->m_unregisterAlbumArtUpdateCallback
	= mediaSource.addAlbumArtListener ([this] (const Media::MediaSource::MediaInfo& info) {
	      // TODO: SEPARATE THESE INTO THEIR OWN UPDATES SO JS ONLY RECEIVES THE MEANINGFUL UPDATES
	      this->notifyMediaUpdate (info);
	  });

    this->m_runtime = JS_NewRuntime ();

    if (!this->m_runtime) {
	sLog.exception ("ScriptEngine: Failed to create JS runtime");
    }

    // debug leaks on termination
    JS_SetDumpFlags (this->m_runtime, JS_DUMP_LEAKS);

    this->m_context = JS_NewContext (this->m_runtime);

    if (!this->m_context) {
	JS_FreeRuntime (this->m_runtime);
	sLog.exception ("ScriptEngine: Failed to create JS context");
    }
    JS_SetContextOpaque (this->m_context, this);

    this->m_globalThis = JS_GetGlobalObject (this->m_context);

    this->m_adapters = {
	.vec4 = std::unique_ptr<Adapters::VectorAdapter<4>> (new Adapters::VectorAdapter<4> (*this)),
	.vec3 = std::unique_ptr<Adapters::VectorAdapter<3>> (new Adapters::VectorAdapter<3> (*this)),
	.vec2 = std::unique_ptr<Adapters::VectorAdapter<2>> (new Adapters::VectorAdapter<2> (*this)),
	.object
	= std::unique_ptr<Adapters::ScriptableObjectAdapter> (new Adapters::ScriptableObjectAdapter (*this, "ILayer")),
    };

    this->m_engineObject = std::make_unique<EngineObject> (*this, scene);
    this->m_inputObject = std::make_unique<InputObject> (*this, scene);
    this->m_sceneObject = std::make_unique<SceneObject> (*this, scene);
    this->m_consoleObject = std::make_unique<ConsoleObject> (*this, scene);
    this->m_scriptPropertiesObject = std::make_unique<ScriptPropertiesObject> (*this, scene);
    std::string screenKey;
    for (const auto& [name, viewport] : scene.getContext ().getOutput ().getViewports ()) {
        if (!screenKey.empty ()) screenKey += "+";
        screenKey += name;
    }
    m_localStorageObject = std::make_unique<LocalStorageObject> (m_context, scene.getScene ().project,
        screenKey, [this] { return m_runningModule && JS_IsUndefined (m_runningModule->module); });
    JS_SetPropertyStr (m_context, m_globalThis, "localStorage", m_localStorageObject->instance ());

    auto wemath = std::make_unique<Modules::MathModule> (*this);
    auto wecolor = std::make_unique<Modules::ColorModule> (*this);

    this->m_modules.emplace (wemath->getName (), std::move (wemath));
    this->m_modules.emplace (wecolor->getName (), std::move (wecolor));

    m_assetModules = std::make_unique<Modules::ScriptModuleAssets> ([&scene] (const std::string& path) {
        return scene.getScene ().project.assetLocator->readString (path);
    });
    for (const auto& [name, property] : scene.getScene ().project.properties)
        m_propertyListeners.push_back (property->listen ([this] (const DynamicValue&, DynamicValue::UpdateSource) {
            m_userPropertiesDirty = true;
        }));
    JS_SetModuleLoaderFunc (m_runtime, Modules::normalizeScriptModuleName, scriptengine_module_loader, this);
    // setup scene objects and other things
    m_puppetScripts = std::make_unique<PuppetScriptObject> (*this);
    m_textureAnimations = std::make_unique<TextureAnimationObject> (m_context);
    this->installBuiltins ();
    // add engine to the global
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "engine", this->m_engineObject->getInstance (), JS_PROP_C_W_E
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "input", this->m_inputObject->getInstance (), JS_PROP_C_W_E
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "thisScene", this->m_sceneObject->getInstance (), JS_PROP_C_W_E
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "console", this->m_consoleObject->getInstance (), JS_PROP_C_W_E
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "shared", JS_NewObject (this->m_context), JS_PROP_C_W_E
    );
}

ScriptEngine::~ScriptEngine () {
    this->m_unregisterMediaUpdateCallback ();
    this->m_unregisterAlbumArtUpdateCallback ();
    for (const auto& unregister : m_propertyListeners) unregister ();

    for (const auto& module : this->m_scriptModules | std::views::values) {
	JS_FreeValue (this->m_context, module.module);
    }

    JS_FreeValue (this->m_context, this->m_globalThis);

    this->m_adapters.vec4.reset ();
    this->m_adapters.vec3.reset ();
    this->m_adapters.vec2.reset ();
    this->m_adapters.object.reset ();
    this->m_puppetScripts.reset ();
    this->m_textureAnimations.reset ();

    this->m_consoleObject.reset ();
    this->m_engineObject.reset ();
    this->m_inputObject.reset ();
    this->m_sceneObject.reset ();
    this->m_scriptPropertiesObject.reset ();
    this->m_localStorageObject.reset ();
    this->m_modules.clear ();
    this->m_scriptModules.clear ();

    if (this->m_context) {
	JS_FreeContext (this->m_context);
    }
    if (this->m_runtime) {
	JS_FreeRuntime (this->m_runtime);
    }
}

/// Helper to check for and log JS exceptions
static void logJSException (JSContext* ctx, const char* context) {
    JSValue exc = JS_GetException (ctx);
    if (!JS_IsNull (exc) && !JS_IsUndefined (exc)) {
	const char* str = JS_ToCString (ctx, exc);
	if (str) {
	    sLog.error ("ScriptEngine [", context, "]: ", str);
	    JS_FreeCString (ctx, str);
	}
    }
    JS_FreeValue (ctx, exc);
}

void ScriptEngine::installBuiltins () {
    if (this->m_builtinsInstalled || !this->m_context) {
	return;
    }

    JSValue result = JS_Eval (
	this->m_context, SCENE_SCRIPT_BUILTINS, strlen (SCENE_SCRIPT_BUILTINS), "<scene-script-builtins>",
	JS_EVAL_TYPE_GLOBAL
    );
    if (JS_IsException (result)) {
	logJSException (this->m_context, "installBuiltins");
    }
    JS_FreeValue (this->m_context, result);
    this->m_builtinsInstalled = true;
}

JSValue ScriptEngine::call (JSValue module, int argc, JSValue argv[], const char* name) {
    // check if there's an update method and run it
    JSValue function = JS_GetPropertyStr (this->m_context, module, name);
    ScopeGuard guard ([&] () { JS_FreeValue (this->m_context, function); });

    if (!JS_IsFunction (this->m_context, function)) {
	return JS_UNDEFINED;
    }

    return JS_Call (this->m_context, function, module, argc, argv);
}

void ScriptEngine::updateValue (JSValue value, DynamicValue& target, bool angle, DynamicValue::UnderlyingType type) const {
    if (JS_IsException (value) || JS_IsUndefined (value) || JS_IsNull (value)) return;
    LayerWrite result = LayerWrite::Ignore;
    const auto hostType = type == DynamicValue::Null ? target.getType () : type;
    switch (hostType) {
        case DynamicValue::String: {
            std::string text;
            result = convertLayerString (m_context, value, text);
            if (result == LayerWrite::Assign) target.update (text, DynamicValue::Script);
            break;
        }
        case DynamicValue::Boolean: {
            bool boolean;
            result = convertLayerBoolean (value, boolean);
            if (result == LayerWrite::Assign) target.update (boolean, DynamicValue::Script);
            break;
        }
        case DynamicValue::Int: {
            int32_t integer;
            result = convertLayerInt (m_context, value, integer);
            if (result == LayerWrite::Assign) target.update (integer, DynamicValue::Script);
            break;
        }
        case DynamicValue::Float: {
            float number;
            result = convertLayerFloat (m_context, value, number);
            if (result == LayerWrite::Assign) target.update (number, DynamicValue::Script);
            break;
        }
        case DynamicValue::Vec2:
        case DynamicValue::Vec3:
        case DynamicValue::Vec4: {
            const int count = hostType == DynamicValue::Vec2 ? 2 : hostType == DynamicValue::Vec3 ? 3 : 4;
            glm::vec4 vector (0);
            result = convertLayerVector (m_context, value, count, angle, glm::value_ptr (vector));
            if (result == LayerWrite::Assign) {
                if (count == 2) target.update (glm::vec2 (vector), DynamicValue::Script);
                else if (count == 3) target.update (glm::vec3 (vector), DynamicValue::Script);
                else target.update (vector, DynamicValue::Script);
            }
            break;
        }
        default: break;
    }

}

void ScriptEngine::queueScript (const std::string& key, DynamicValue& value, ScriptableObject& object, JSValueConst owner,
                               DynamicValue::UnderlyingType type) {
    // Construction can register a typed property over a generic group value.
    // Evaluate only after every layer and its final properties exist.
    if (!value.getScriptSource ()) {
        m_pendingModules.erase (key);
        std::erase (m_dispatchOrder, key);
        return;
    }
    if (!m_pendingModules.contains (key)) m_dispatchOrder.push_back (key);
    m_pendingModules.insert_or_assign (key, PendingModule { &value, &object });
    m_scriptTypes.insert_or_assign (key, type);
    if (!JS_IsUndefined (owner)) {
        if (const auto found = m_scriptOwners.find (key); found != m_scriptOwners.end ()) JS_FreeValue (m_context, found->second);
        m_scriptOwners.insert_or_assign (key, JS_DupValue (m_context, owner));
    }
}

void ScriptEngine::loadScript (const std::string& key, DynamicValue& currentValue, ScriptableObject& object) {
    const auto& source = currentValue.getScriptSource ();
    if (!source) return;

    auto it = this->m_scriptModules.find (key);

    if (it != this->m_scriptModules.end ()) {
	return;
    }

    JSValue compiledModule = JS_Eval (
	this->m_context, source->c_str (), source->size (), key.c_str (),
	JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY
    );
    if (JS_IsException (compiledModule)) {
	logJSException (this->m_context, "queueScript.compile");
	return;
    }

    auto* moduleDefinition = static_cast<JSModuleDef*> (JS_VALUE_GET_PTR (compiledModule));
    JSValue previousLayer = JS_GetPropertyStr (m_context, m_globalThis, "thisLayer");
    JSValue previousObject = JS_GetPropertyStr (m_context, m_globalThis, "thisObject");
    JSValue layer = m_adapters.object->instantiate (object);
    JS_SetPropertyStr (m_context, m_globalThis, "thisLayer", JS_DupValue (m_context, layer));
    const auto owner = m_scriptOwners.find (key);
    JS_SetPropertyStr (m_context, m_globalThis, "thisObject",
        owner != m_scriptOwners.end () ? JS_DupValue (m_context, owner->second) : JS_DupValue (m_context, layer));
    JS_FreeValue (m_context, layer);
    ScopeGuard layerGuard ([&] {
        JS_SetPropertyStr (m_context, m_globalThis, "thisLayer", previousLayer);
        JS_SetPropertyStr (m_context, m_globalThis, "thisObject", previousObject);
    });

    // Script properties are created while the compiled module is evaluated.
    LoadedModule loadingModule {
	.value = currentValue,
	.module = JS_UNDEFINED,
	.object = object,
        .key = key,
    };
    LoadedModule* previousModule = this->m_runningModule;
    this->m_runningModule = &loadingModule;

    m_evaluatingModuleTopLevel = true;
    ScopeGuard evaluationScope ([this] { m_evaluatingModuleTopLevel = false; });
    JSValue evaluation = JS_EvalFunction (this->m_context, JS_DupValue (this->m_context, compiledModule));
    bool evaluationFailed = JS_IsException (evaluation);
    while (!evaluationFailed && JS_IsPromise (evaluation)
	   && JS_PromiseState (this->m_context, evaluation) == JS_PROMISE_PENDING) {
	JSContext* jobContext = nullptr;
	const int result = JS_ExecutePendingJob (this->m_runtime, &jobContext);
	if (result < 0) {
	    logJSException (jobContext == nullptr ? this->m_context : jobContext, "queueScript.eval");
	    evaluationFailed = true;
	    break;
	}
	if (result == 0) {
	    break;
	}
    }
    this->m_runningModule = previousModule;
    if (evaluationFailed) {
	if (JS_IsException (evaluation)) {
	    logJSException (this->m_context, "queueScript.eval");
	}
	JS_FreeValue (this->m_context, evaluation);
	JS_FreeValue (this->m_context, compiledModule);
	return;
    }
    if (JS_IsPromise (evaluation) && JS_PromiseState (this->m_context, evaluation) == JS_PROMISE_REJECTED) {
	JS_Throw (this->m_context, JS_PromiseResult (this->m_context, evaluation));
	logJSException (this->m_context, "queueScript.eval");
	evaluationFailed = true;
    } else if (JS_IsPromise (evaluation) && JS_PromiseState (this->m_context, evaluation) == JS_PROMISE_PENDING) {
	sLog.error ("ScriptEngine [queueScript.eval]: module evaluation did not finish");
	evaluationFailed = true;
    }
    JS_FreeValue (this->m_context, evaluation);
    if (evaluationFailed) {
	JS_FreeValue (this->m_context, compiledModule);
	return;
    }

    JSValue module = JS_GetModuleNamespace (this->m_context, moduleDefinition);
    JS_FreeValue (this->m_context, compiledModule);
    if (JS_IsException (module)) {
	logJSException (this->m_context, "queueScript.namespace");
	return;
    }

    auto inserted = this->m_scriptModules.emplace (
	key,
	LoadedModule {
	    .value = currentValue,
	    .module = module,
	    .object = object,
            .key = key,
	}
    );

    if (!inserted.second) {
	JS_FreeValue (this->m_context, module);
	return;
    }

}

JSValue ScriptEngine::callOwned (LoadedModule& module, int argc, JSValueConst argv[], const char* name) {
    auto* previousModule = m_runningModule;
    JSValue previousLayer = JS_GetPropertyStr (m_context, m_globalThis, "thisLayer");
    JSValue previousObject = JS_GetPropertyStr (m_context, m_globalThis, "thisObject");
    JSValue layer = m_adapters.object->instantiate (module.object);
    m_runningModule = &module;
    JS_SetPropertyStr (m_context, m_globalThis, "thisLayer", JS_DupValue (m_context, layer));
    const auto owner = m_scriptOwners.find (module.key);
    JS_SetPropertyStr (m_context, m_globalThis, "thisObject",
        owner != m_scriptOwners.end () ? JS_DupValue (m_context, owner->second) : JS_DupValue (m_context, layer));
    JS_FreeValue (m_context, layer);
    ScopeGuard restore ([&] {
        m_runningModule = previousModule;
        JS_SetPropertyStr (m_context, m_globalThis, "thisLayer", previousLayer);
        JS_SetPropertyStr (m_context, m_globalThis, "thisObject", previousObject);
    });
    JSValue result = call (module.module, argc, argv, name);
    if (JS_IsException (result)) logJSException (m_context, name);
    return result;
}

std::unordered_set<std::string> ScriptEngine::initializeModules (size_t first) {
    const std::vector<std::string> dispatchOrder (m_dispatchOrder.begin () + first, m_dispatchOrder.end ());
    for (const auto& key : dispatchOrder) {
        const auto pending = m_pendingModules.find (key);
        if (pending != m_pendingModules.end ())
            loadScript (key, *pending->second.value, *pending->second.object);
    }
    for (const auto& key : dispatchOrder) m_pendingModules.erase (key);
    std::unordered_set<std::string> initialized;
    for (const auto& key : dispatchOrder) {
        const auto found = m_scriptModules.find (key);
        if (found == m_scriptModules.end () || found->second.initialized) continue;
        auto& module = found->second;
        module.initialized = true;
        initialized.insert (key);
        m_pendingInitialNotifications.insert (key);
        JSValue args[] = { dynamicToJs (module.value, true, key.starts_with ("angles_")) };
        JSValue result = callOwned (module, 1, args, "init");
        if (key.starts_with ("animationlayer"))
            storeSceneSetting (m_context, result, module.value, key.ends_with (".visible") ? SceneSettingType::Bool : SceneSettingType::Float);
        else updateValue (result, module.value, key.starts_with ("angles_"), m_scriptTypes.at (key));
        if (JS_HasException (m_context)) logJSException (m_context, "property conversion");
        JS_FreeValue (m_context, result);
        JS_FreeValue (m_context, args[0]);
    }
    return initialized;
}

void ScriptEngine::tick () {
    initializeModules ();
    const auto initialized = std::exchange (m_pendingInitialNotifications, {});
    const auto dispatchOrder = m_dispatchOrder;
    if (m_userPropertiesDirty || !initialized.empty ()) {
        const bool notifyAll = m_userPropertiesDirty;
        m_userPropertiesDirty = false;
        JSValue properties = JS_NewObject (m_context);
        for (const auto& [name, property] : m_scene.getScene ().project.properties) {
            JSValue value;
            if (property->is<PropertyColor> ()) {
                DynamicValue color (property->getVec3 ());
                value = dynamicToJs (color, true);
            } else value = dynamicToJs (*property, true);
            JS_SetPropertyStr (m_context, properties, name.c_str (), value);
        }
        for (const auto& key : dispatchOrder) {
            if (!notifyAll && !initialized.contains (key)) continue;
            const auto found = m_scriptModules.find (key);
            if (found == m_scriptModules.end ()) continue;
            JSValue args[] = { properties };
            JSValue result = callOwned (found->second, 1, args, "applyUserProperties");
            JS_FreeValue (m_context, result);
        }
        JS_FreeValue (m_context, properties);
    }
    if (!initialized.empty ()) notifyMediaUpdate (m_mediaSource.getMediaInfo (), &initialized);
    m_scene.dispatchCursorEvents ();
    m_engineObject->tick ();
    for (const auto& key : dispatchOrder) {
        const auto found = m_scriptModules.find (key);
        if (found == m_scriptModules.end ()) continue;
        auto& module = found->second;
        // A script may retain its initial vector; subsequent writes must not
        // move that baseline along with the rendered property.
        JSValue args[] = { dynamicToJs (module.value, true, key.starts_with ("angles_")) };
        JSValue result = callOwned (module, 1, args, "update");
        if (key.starts_with ("animationlayer"))
            storeSceneSetting (m_context, result, module.value, key.ends_with (".visible") ? SceneSettingType::Bool : SceneSettingType::Float);
        else updateValue (result, module.value, key.starts_with ("angles_"), m_scriptTypes.at (key));
        if (JS_HasException (m_context)) logJSException (m_context, "property conversion");
        JS_FreeValue (m_context, result);
        JS_FreeValue (m_context, args[0]);
    }
}

void ScriptEngine::removeScript (const std::string& key) {
    m_pendingInitialNotifications.erase (key);
    if (const auto found = m_scriptModules.find (key); found != m_scriptModules.end ()) {
        JSValue result = callOwned (found->second, 0, nullptr, "destroy");
        JS_FreeValue (m_context, result);
        JS_FreeValue (m_context, found->second.module);
        m_scriptModules.erase (found);
    }
    m_pendingModules.erase (key);
    std::erase (m_dispatchOrder, key);
    m_scriptTypes.erase (key);
    if (const auto owner = m_scriptOwners.find (key); owner != m_scriptOwners.end ()) {
        JS_FreeValue (m_context, owner->second);
        m_scriptOwners.erase (owner);
    }
}

void ScriptEngine::dispatchCursorEvent (const ScriptableObject& object, const char* event,
                                        const glm::vec3& world, const glm::vec3& local) {
    const auto order = m_dispatchOrder;
    for (const auto& key : order) {
        const auto found = m_scriptModules.find (key);
        if (found == m_scriptModules.end () || &found->second.object != &object) continue;
        JSValue payload = JS_NewObject (m_context);
        DynamicValue worldValue (world), localValue (local);
        JS_SetPropertyStr (m_context, payload, "worldPosition", dynamicToJs (worldValue, true));
        JS_SetPropertyStr (m_context, payload, "localPosition", dynamicToJs (localValue, true));
        JS_SetPropertyStr (m_context, payload, "hitBox", JS_NULL);
        JSValue args[] {payload};
        JSValue result = callOwned (found->second, 1, args, event);
        JS_FreeValue (m_context, result);
        JS_FreeValue (m_context, payload);
    }
}

void ScriptEngine::destroyObjectModules (ScriptableObject& object) {
    const auto order = m_dispatchOrder;
    for (const auto& key : order) {
        const auto found = m_scriptModules.find (key);
        if (found != m_scriptModules.end () && &found->second.object == &object) removeScript (key);
    }
    unregisterObject (object);
}

void ScriptEngine::unregisterObject (ScriptableObject& object) {
    m_engineObject->forgetObject (object);
    m_textureAnimations->forgetInstance (&object);
    std::vector<std::string> keys;
    for (const auto& [key, pending] : m_pendingModules) if (pending.object == &object) keys.push_back (key);
    for (const auto& [key, module] : m_scriptModules) if (&module.object == &object) keys.push_back (key);
    for (const auto& key : keys) {
        // The concrete object's destructor may already have released its resources.
        if (const auto found = m_scriptModules.find (key); found != m_scriptModules.end ()) {
            JS_FreeValue (m_context, found->second.module);
            m_scriptModules.erase (found);
        }
        removeScript (key);
    }
}

void ScriptEngine::callLayerCallback (ScriptableObject& layer, JSValueConst callback, JSValueConst receiver) {
    JSValue previousLayer = JS_GetPropertyStr (m_context, m_globalThis, "thisLayer");
    JSValue previousObject = JS_GetPropertyStr (m_context, m_globalThis, "thisObject");
    JSValue instance = m_adapters.object->instantiate (layer);
    JSValue previousReceiver = m_engineObject->exchangeReceiver (JS_DupValue (m_context, receiver));
    JS_SetPropertyStr (m_context, m_globalThis, "thisLayer", JS_DupValue (m_context, instance));
    JS_SetPropertyStr (m_context, m_globalThis, "thisObject", instance);
    JSValue result = JS_Call (m_context, callback, receiver, 0, nullptr);
    if (JS_IsException (result)) logJSException (m_context, "animation ended");
    JS_FreeValue (m_context, result);
    JS_FreeValue (m_context, m_engineObject->exchangeReceiver (previousReceiver));
    JS_SetPropertyStr (m_context, m_globalThis, "thisLayer", previousLayer);
    JS_SetPropertyStr (m_context, m_globalThis, "thisObject", previousObject);
}

void ScriptEngine::shutdownScripts () {
    for (const auto& key : m_dispatchOrder) {
        const auto found = m_scriptModules.find (key);
        if (found == m_scriptModules.end () || !found->second.initialized) continue;
        JSValue result = callOwned (found->second, 0, nullptr, "destroy");
        JS_FreeValue (m_context, result);
    }
    for (auto& [key, module] : m_scriptModules) JS_FreeValue (m_context, module.module);
    m_scriptModules.clear ();
    m_pendingModules.clear ();
    m_dispatchOrder.clear ();
    for (const auto& [key, owner] : m_scriptOwners) JS_FreeValue (m_context, owner);
    m_scriptOwners.clear ();
    m_scriptTypes.clear ();
    m_pendingInitialNotifications.clear ();
}

void ScriptEngine::notifyMediaUpdate (const Media::MediaSource::MediaInfo& media,
                                     const std::unordered_set<std::string>* initialized) {
    JSContext* ctx = this->m_context;

    DynamicValue primaryColorValue (glm::vec3 (0.12f, 0.12f, 0.12f));
    DynamicValue secondaryColorValue (glm::vec3 (0.0f, 0.0f, 0.0f));
    DynamicValue tertiaryColorValue (glm::vec3 (0.25f, 0.25f, 0.25f));
    DynamicValue highContrastColorValue (glm::vec3 (1.0f, 1.0f, 1.0f));

    // TODO: PROCESS THESE COLORS INSTEAD OF HARDCODING THEM
    JSValue primaryColor = this->m_adapters.vec3->instantiate (primaryColorValue, true);
    JSValue secondaryColor = this->m_adapters.vec3->instantiate (secondaryColorValue, true);
    JSValue tertiaryColor = this->m_adapters.vec3->instantiate (tertiaryColorValue, true);
    JSValue highContrastColor = this->m_adapters.vec3->instantiate (highContrastColorValue, true);

    JSValue propertiesEvent = JS_NewObject (ctx);

    // set properties
    JS_SetPropertyStr (ctx, propertiesEvent, "title", JS_NewString (ctx, media.title.c_str ()));
    JS_SetPropertyStr (ctx, propertiesEvent, "artist", JS_NewString (ctx, media.artist.c_str ()));
    JS_SetPropertyStr (ctx, propertiesEvent, "albumTitle", JS_NewString (ctx, media.album.c_str ()));

    JSValue playbackEvent = JS_NewObject (ctx);

    JS_SetPropertyStr (ctx, playbackEvent, "state", JS_NewInt32 (ctx, media.playbackState));

    JSValue mediaTimelineEvent = JS_NewObject (ctx);

    JS_SetPropertyStr (ctx, mediaTimelineEvent, "position", JS_NewFloat64 (ctx, media.position));
    JS_SetPropertyStr (ctx, mediaTimelineEvent, "duration", JS_NewFloat64 (ctx, media.duration));

    JSValue mediaThumbnailEvent = JS_NewObject (ctx);

    JS_SetPropertyStr (ctx, mediaThumbnailEvent, "hasThumbnail", JS_NewBool (ctx, media.url.has_value ()));
    JS_SetPropertyStr (ctx, mediaThumbnailEvent, "primaryColor", primaryColor);
    JS_SetPropertyStr (ctx, mediaThumbnailEvent, "secondaryColor", secondaryColor);
    JS_SetPropertyStr (ctx, mediaThumbnailEvent, "tertiaryColor", tertiaryColor);
    JS_SetPropertyStr (ctx, mediaThumbnailEvent, "highContrastColor", highContrastColor);

    JSValue propertiesArgs[] = { propertiesEvent };
    JSValue playbackArgs[] = { playbackEvent };
    JSValue mediaTimelineArgs[] = { mediaTimelineEvent };
    JSValue mediaThumbnailArgs[] = { mediaThumbnailEvent };

    // Callbacks may create layers; they receive their own startup event next tick.
    const auto dispatchOrder = m_dispatchOrder;
    for (const auto& key : dispatchOrder) {
        if (initialized && !initialized->contains (key)) continue;
        const auto found = m_scriptModules.find (key);
        if (found == m_scriptModules.end () || !found->second.initialized) continue;
        auto& module = found->second;
	// call all methods
	JSValue result1 = this->callOwned (module, 1, propertiesArgs, "mediaPropertiesChanged");
	JSValue result2 = this->callOwned (module, 1, playbackArgs, "mediaPlaybackChanged");
	JSValue result3 = this->callOwned (module, 1, mediaTimelineArgs, "mediaTimelineChanged");
	JSValue result4 = this->callOwned (module, 1, mediaThumbnailArgs, "mediaThumbnailChanged");

	JS_FreeValue (ctx, result1);
	JS_FreeValue (ctx, result2);
	JS_FreeValue (ctx, result3);
	JS_FreeValue (ctx, result4);
    }

    // free all created objects as we don't keep a ref to them anymore
    JS_FreeValue (ctx, propertiesEvent);
    JS_FreeValue (ctx, playbackEvent);
    JS_FreeValue (ctx, mediaTimelineEvent);
    JS_FreeValue (ctx, mediaThumbnailEvent);
}
