#pragma once
#include "quickjs.h"

#include <array>
#include <map>
#include <vector>
#include <utility>

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}
namespace WallpaperEngine::Scripting {
class ScriptEngine;
class ScriptableObject;
class EngineObject {
public:
    EngineObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene);
    ~EngineObject ();

    const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    JSValue getInstance () const { return m_instance; }
    ScriptEngine& getEngine () const { return m_engine; }
    uint32_t getInstanceId () const { return m_instanceId; }
    uint32_t reserveTimer (JSValue function, float duration, bool interval);
    bool clearTimer (uint32_t id);
    void forgetObject (ScriptableObject& object);
    JSValue registerAudioBuffers (uint32_t resolution);
    JSValue exchangeReceiver (JSValue value) { return std::exchange (m_activeReceiver, value); }

    void tick ();

protected:
    struct Timer {
	JSValue callback;
	JSValue receiver;
	JSValue layer;
        JSValue object;
	float duration;
	float remaining;
	bool interval;
    };

    uint32_t m_nextTimerId = 0;
    std::map<uint32_t, Timer> m_timers;
    JSValue m_activeReceiver = JS_UNDEFINED;
    struct AudioBuffers {
	uint32_t resolution;
	std::array<JSValue, 3> arrays;
        ScriptableObject* owner = nullptr;
    };
    std::vector<AudioBuffers> m_audioBuffers;
    Render::Wallpapers::CScene& m_scene;
    ScriptEngine& m_engine;

    uint32_t m_instanceId;
    JSClassID m_classId;
    JSClassDef m_definition;
    JSValue m_instance;
};
}
