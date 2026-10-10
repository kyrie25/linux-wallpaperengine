#pragma once

#include "ScriptableObject.h"
#include "quickjs.h"

namespace WallpaperEngine::Render::Objects { class CImage; }

namespace WallpaperEngine::Scripting {
class ScriptEngine;

class PuppetScriptObject {
public:
    explicit PuppetScriptObject (ScriptEngine& engine);
    ~PuppetScriptObject ();
    JSValue method (JSValueConst layer, const char* name);
    void finishFrame (Render::Objects::CImage& image);
    JSValue animation (ScriptableObject& image, size_t serial);

private:
    static JSValue layerCall (JSContext*, JSValueConst, int, JSValueConst*, int, JSValue*);
    static JSValue animationCall (JSContext*, JSValueConst, int, JSValueConst*, int, JSValue*);
    friend struct PuppetAnimationHandle;
    ScriptEngine& m_engine;
    JSClassID m_animationClass = 0;
    std::map<std::pair<const void*, size_t>, JSValue> m_instances;
};
}
