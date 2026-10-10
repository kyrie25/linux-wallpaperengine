#pragma once

#include "Adapters/VectorAdapter.h"
#include "ConsoleObject.h"
#include "EngineObject.h"
#include "InputObject.h"
#include "Modules/ScriptModule.h"
#include "Modules/ScriptModuleAssets.h"
#include "SceneObject.h"

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Media/MediaSource.h"

namespace WallpaperEngine::Media {
class MediaSource;
}
extern "C" {
#include "quickjs.h"
}

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Scripting {
class ScriptPropertiesObject;
class LocalStorageObject;
class PuppetScriptObject;
class TextureAnimationObject;
namespace Adapters {
    class ScriptableObjectAdapter;
}
using namespace WallpaperEngine::Data::Model;

class ScriptEngine {
public:
    struct LoadedModule {
	DynamicValue& value;
	JSValue module;
	ScriptableObject& object;
	bool initialized = false;
        std::string key;
    };
    struct JSObjectAdapters {
	std::unique_ptr<Adapters::VectorAdapter<4>> vec4;
	std::unique_ptr<Adapters::VectorAdapter<3>> vec3;
	std::unique_ptr<Adapters::VectorAdapter<2>> vec2;
	std::unique_ptr<Adapters::ScriptableObjectAdapter> object;
    };

    ~ScriptEngine ();
    ScriptEngine (Render::Wallpapers::CScene& scene, Media::MediaSource& mediaSource);
    ScriptEngine (const ScriptEngine&) = delete;
    ScriptEngine& operator= (const ScriptEngine&) = delete;

    JSRuntime* getRuntime () const { return m_runtime; }
    JSContext* getContext () const { return m_context; }
    JSModuleDef* loadAssetModule (JSContext* context, const char* name) const;
    JSValue getGlobalThis () const { return m_globalThis; }
    LoadedModule* getRunningModule () const { return m_runningModule; }
    JSValue dynamicToJs (DynamicValue& value, bool snapshot = false, bool angle = false) const;
    void updateValue (JSValue value, DynamicValue& target, bool angle = false,
                      DynamicValue::UnderlyingType type = DynamicValue::Null) const;
    void shutdownScripts ();
    void removeScript (const std::string& key);
    void callLayerCallback (ScriptableObject& layer, JSValueConst callback, JSValueConst receiver);
    PuppetScriptObject& getPuppetScripts () const { return *m_puppetScripts; }
    SceneObject& getSceneObject () const { return *m_sceneObject; }
    TextureAnimationObject& getTextureAnimations () const { return *m_textureAnimations; }

    /**
     * Evaluate a WallpaperEngine script's update() function.
     *
     * @param key The full JS script text (ES6 module with export function update(value))
     * @param currentValue The current value to pass to update()
     * @return The modified value from update(), or a copy of currentValue on error
     */
    void queueScript (const std::string& key, DynamicValue& currentValue, ScriptableObject& object,
                      JSValueConst owner = JS_UNDEFINED, DynamicValue::UnderlyingType type = DynamicValue::Null);
    void unregisterObject (ScriptableObject& object);
    void dispatchCursorEvent (const ScriptableObject& object, const char* event, const glm::vec3& world, const glm::vec3& local);
    void destroyObjectModules (ScriptableObject& object);
    size_t nextQueueOrder () const { return m_dispatchOrder.size (); }
    std::unordered_set<std::string> initializeModules (size_t first = 0);
    bool isEvaluatingModuleTopLevel () const { return m_evaluatingModuleTopLevel; }

    /**
     * Runs a frame tick in the javascript engine. Dispatches any pending events,
     * timeouts, intervals AND calls any update() functions.
     */
    void tick ();

    const JSObjectAdapters& getAdapters () const { return m_adapters; }
    const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    const std::map<std::string, std::unique_ptr<Modules::ScriptModule>>& getModules () const { return m_modules; }

private:
    JSValue call (JSValue module, int argc, JSValueConst argv[], const char* name);

    void installBuiltins ();

    void notifyMediaUpdate (const Media::MediaSource::MediaInfo& media,
                            const std::unordered_set<std::string>* initialized = nullptr);

    void loadScript (const std::string& key, DynamicValue& value, ScriptableObject& object);
    JSValue callOwned (LoadedModule& module, int argc, JSValueConst argv[], const char* name);

    JSRuntime* m_runtime = nullptr;
    JSContext* m_context = nullptr;
    JSValue m_globalThis;
    Render::Wallpapers::CScene& m_scene;
    std::unique_ptr<EngineObject> m_engineObject;
    std::unique_ptr<InputObject> m_inputObject;
    std::unique_ptr<SceneObject> m_sceneObject;
    std::unique_ptr<ConsoleObject> m_consoleObject;
    std::unique_ptr<ScriptPropertiesObject> m_scriptPropertiesObject;
    std::unique_ptr<LocalStorageObject> m_localStorageObject;
    std::unique_ptr<PuppetScriptObject> m_puppetScripts;
    std::unique_ptr<TextureAnimationObject> m_textureAnimations;

    std::unique_ptr<Modules::ScriptModuleAssets> m_assetModules;
    bool m_userPropertiesDirty = true;
    std::vector<std::function<void ()>> m_propertyListeners;
    std::map<std::string, std::unique_ptr<Modules::ScriptModule>> m_modules = {};
    std::map<std::string, LoadedModule> m_scriptModules = {};

    struct PendingModule { DynamicValue* value; ScriptableObject* object; };
    std::map<std::string, PendingModule> m_pendingModules;
    std::vector<std::string> m_dispatchOrder;
    std::map<std::string, JSValue> m_scriptOwners;
    std::map<std::string, DynamicValue::UnderlyingType> m_scriptTypes;
    LoadedModule* m_runningModule = nullptr;
    std::unordered_set<std::string> m_pendingInitialNotifications;

    bool m_builtinsInstalled = false;
    bool m_evaluatingModuleTopLevel = false;
    Media::MediaSource& m_mediaSource;
    std::function<void ()> m_unregisterMediaUpdateCallback;
    std::function<void ()> m_unregisterAlbumArtUpdateCallback;

    JSObjectAdapters m_adapters;
};
} // namespace WallpaperEngine::Scripting
