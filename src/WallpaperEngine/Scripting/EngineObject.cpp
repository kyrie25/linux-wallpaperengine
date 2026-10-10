#include "EngineObject.h"
#include "ScriptEngine.h"
#include "Adapters/ScriptableObjectAdapter.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include <algorithm>
#include <vector>

using namespace WallpaperEngine::Scripting;

extern float g_Time;
extern float g_TimeLast;
extern float g_Daytime;

static uint32_t EngineInstanceId = 0;
std::map<uint32_t, EngineObject&> engineInstances;

JSValue engine_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_EXCEPTION; }

JSValue engine_open_user_shortcut (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_UNDEFINED;
}

JSValue engine_get_frametime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewFloat64 (ctx, g_Time - g_TimeLast);
}

JSValue engine_get_runtime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewFloat64 (ctx, g_Time);
}

JSValue engine_get_daytime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewFloat64 (ctx, g_Daytime);
}

static EngineObject* engine_owner (JSValueConst value) {
    JSClassID id = 0;
    return static_cast<EngineObject*> (JS_GetAnyOpaque (value, &id));
}

static JSValue engine_screen_resolution (JSContext* ctx, JSValueConst value, int, JSValueConst*) {
    const auto* owner = engine_owner (value);
    if (!owner) return JS_ThrowTypeError (ctx, "Invalid engine receiver");
    const auto& output = owner->getScene ().getContext ().getOutput ();
    DynamicValue size (glm::vec2 (output.getFullWidth (), output.getFullHeight ()));
    return owner->getEngine ().dynamicToJs (size, true);
}

static JSValue engine_landscape (JSContext* ctx, JSValueConst value, int, JSValueConst*) {
    const auto* owner = engine_owner (value);
    if (!owner) return JS_ThrowTypeError (ctx, "Invalid engine receiver");
    const auto& output = owner->getScene ().getContext ().getOutput ();
    return JS_NewBool (ctx, output.getFullWidth () >= output.getFullHeight ());
}

namespace {
JSValue engine_cancel_timer (JSContext* ctx, JSValueConst, int, JSValueConst*, int magic, JSValueConst* data) {
    const auto it = engineInstances.find (magic);
    if (it == engineInstances.end ()) {
	return JS_FALSE;
    }
    uint32_t id = 0;
    if (JS_ToUint32 (ctx, &id, data[0]) < 0) {
	return JS_EXCEPTION;
    }
    return JS_NewBool (ctx, it->second.clearTimer (id));
}

JSValue engine_set_timer (JSContext* ctx, int argc, JSValueConst* argv, int magic, bool interval) {
    const auto it = engineInstances.find (magic);
    if (it == engineInstances.end ()) {
	return JS_ThrowTypeError (ctx, "timer owner no longer exists");
    }
    const auto* module = it->second.getEngine ().getRunningModule ();
    if (module && JS_IsUndefined (module->module)) {
	return JS_ThrowSyntaxError (ctx, "timers cannot be scheduled from global scope");
    }
    if (argc < (interval ? 2 : 1) || !JS_IsFunction (ctx, argv[0])) {
	return JS_NULL;
    }
    double delay = 0;
    if (argc > 1 && JS_IsNumber (argv[1])) {
	if (JS_ToFloat64 (ctx, &delay, argv[1]) < 0) {
	    return JS_EXCEPTION;
	}
    } else if (interval) {
	return JS_NULL;
    }
    if (interval && delay <= 0) {
	return JS_NULL;
    }
    const uint32_t id = it->second.reserveTimer (argv[0], static_cast<float> (delay / 1000), interval);
    JSValue data[] = { JS_NewUint32 (ctx, id) };
    JSValue cancel = JS_NewCFunctionData (ctx, engine_cancel_timer, 0, magic, 1, data);
    JS_FreeValue (ctx, data[0]);
    if (JS_IsException (cancel)) {
	it->second.clearTimer (id);
    }
    return cancel;
}
}

JSValue engine_set_interval (JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    return engine_set_timer (ctx, argc, argv, magic, true);
}

JSValue engine_set_timeout (JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    return engine_set_timer (ctx, argc, argv, magic, false);
}

JSValue engine_register_audio_buffers (JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic);
    if (it == engineInstances.end ()) {
	return JS_ThrowTypeError (ctx, "audio buffer owner no longer exists");
    }
    const auto* module = it->second.getEngine ().getRunningModule ();
    if (!module || !JS_IsUndefined (module->module)) {
	return JS_ThrowTypeError (ctx, "registerAudioBuffers can only be called from global scope");
    }
    uint32_t resolution = 16;
    if (argc > 0 && !JS_IsUndefined (argv[0]) && JS_ToUint32 (ctx, &resolution, argv[0]) < 0) {
	return JS_EXCEPTION;
    }
    if (resolution != 16 && resolution != 32 && resolution != 64) {
	return JS_ThrowRangeError (ctx, "Resolution must be either 16, 32 or 64");
    }
    return it->second.registerAudioBuffers (resolution);
}

EngineObject::EngineObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_instanceId (++EngineInstanceId), m_classId (0) {
    this->m_definition = { .class_name = "IEngine" };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_classId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_classId, &this->m_definition);
    this->m_instance = JS_NewObjectClass (this->m_engine.getContext (), this->m_classId);

    JS_DupValue (this->m_engine.getContext (), this->m_instance);
    engineInstances.emplace (this->m_instanceId, *this);

    // set properties
    JS_SetOpaque (this->m_instance, this);
    JSAtom sizeAtom = JS_NewAtom (m_engine.getContext (), "screenResolution");
    JS_DefinePropertyGetSet (m_engine.getContext (), m_instance, sizeAtom,
        JS_NewCFunction (m_engine.getContext (), engine_screen_resolution, "get", 0), JS_UNDEFINED,
        JS_PROP_ENUMERABLE);
    JS_FreeAtom (m_engine.getContext (), sizeAtom);
    JS_DefinePropertyValueStr (m_engine.getContext (), m_instance, "isLandscape",
        JS_NewCFunction (m_engine.getContext (), engine_landscape, "isLandscape", 0), JS_PROP_C_W_E);
    JSAtom timeAtom = JS_NewAtom (m_engine.getContext (), "time");
    JS_DefinePropertyGetSet (m_engine.getContext (), m_instance, timeAtom,
        JS_NewCFunction (m_engine.getContext (), engine_get_runtime, "get", 0), JS_UNDEFINED,
        JS_PROP_ENUMERABLE);
    JS_FreeAtom (m_engine.getContext (), timeAtom);
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "frametime"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_frametime, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "runtime"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_runtime, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "timeOfDay"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_daytime, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_16",
	JS_NewInt32 (this->m_engine.getContext (), 16), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_32",
	JS_NewInt32 (this->m_engine.getContext (), 32), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_64",
	JS_NewInt32 (this->m_engine.getContext (), 64), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "setInterval",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_set_interval, "setInterval", 2, JS_CFUNC_generic_magic,
	    this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "setTimeout",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_set_timeout, "setTimeout", 2, JS_CFUNC_generic_magic,
	    this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "openUserShortcut",
	JS_NewCFunction (this->m_engine.getContext (), engine_open_user_shortcut, "openUserShortcut", 0),
	JS_PROP_ENUMERABLE
    );
    // TODO: ADD THE REST OF THE DEFINITION!
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "registerAudioBuffers",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_register_audio_buffers, "registerAudioBuffers", 1,
	    JS_CFUNC_generic_magic, this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
}

EngineObject::~EngineObject () {
    engineInstances.erase (this->m_instanceId);
    for (const auto& [id, timer] : this->m_timers) {
	JS_FreeValue (this->m_engine.getContext (), timer.callback);
	JS_FreeValue (this->m_engine.getContext (), timer.receiver);
	JS_FreeValue (this->m_engine.getContext (), timer.layer);
        JS_FreeValue (this->m_engine.getContext (), timer.object);
    }
    for (const auto& registration : this->m_audioBuffers) {
	for (JSValue array : registration.arrays) {
	    JS_FreeValue (this->m_engine.getContext (), array);
	}
    }
    JS_FreeValue (this->m_engine.getContext (), this->m_instance);
}

uint32_t EngineObject::reserveTimer (JSValue function, float duration, bool interval) {
    const auto id = ++this->m_nextTimerId;
    JSContext* ctx = this->m_engine.getContext ();
    const auto* module = this->m_engine.getRunningModule ();
    this->m_timers.emplace (
	id,
	Timer { JS_DupValue (ctx, function), JS_DupValue (ctx, module ? module->module : this->m_activeReceiver),
                JS_GetPropertyStr (ctx, this->m_engine.getGlobalThis (), "thisLayer"),
                JS_GetPropertyStr (ctx, this->m_engine.getGlobalThis (), "thisObject"), duration, duration, interval }
    );
    return id;
}

bool EngineObject::clearTimer (uint32_t id) {
    const auto it = this->m_timers.find (id);
    if (it == this->m_timers.end ()) {
	return false;
    }
    JS_FreeValue (this->getEngine ().getContext (), it->second.callback);
    JS_FreeValue (this->getEngine ().getContext (), it->second.receiver);
    JS_FreeValue (this->getEngine ().getContext (), it->second.layer);
    JS_FreeValue (this->getEngine ().getContext (), it->second.object);
    this->m_timers.erase (it);
    return true;
}

JSValue EngineObject::registerAudioBuffers (uint32_t resolution) {
    JSContext* ctx = this->m_engine.getContext ();
    JSValue result = JS_NewObject (ctx);
    if (JS_IsException (result)) {
	return result;
    }
    AudioBuffers registration { resolution, { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED } };
    JSValue layer = JS_GetPropertyStr (ctx, m_engine.getGlobalThis (), "thisLayer");
    registration.owner = Adapters::ScriptableObjectAdapter::objectOf (layer);
    JS_FreeValue (ctx, layer);
    constexpr std::array<const char*, 3> names { "left", "right", "average" };
    for (size_t channel = 0; channel < names.size (); channel++) {
	JSValue length = JS_NewUint32 (ctx, resolution);
	JSValue array = JS_NewTypedArray (ctx, 1, &length, JS_TYPED_ARRAY_FLOAT32);
	JS_FreeValue (ctx, length);
	if (JS_IsException (array)) {
	    for (JSValue stored : registration.arrays) {
		JS_FreeValue (ctx, stored);
	    }
	    JS_FreeValue (ctx, result);
	    return array;
	}
	registration.arrays[channel] = JS_DupValue (ctx, array);
	if (JS_DefinePropertyValueStr (ctx, result, names[channel], array, JS_PROP_ENUMERABLE) < 0) {
	    for (JSValue stored : registration.arrays) {
		JS_FreeValue (ctx, stored);
	    }
	    JS_FreeValue (ctx, result);
	    return JS_EXCEPTION;
	}
    }
    this->m_audioBuffers.push_back (registration);
    return result;
}

void EngineObject::forgetObject (ScriptableObject& object) {
    std::vector<uint32_t> ids;
    for (const auto& [id, timer] : m_timers)
        if (Adapters::ScriptableObjectAdapter::objectOf (timer.layer) == &object) ids.push_back (id);
    for (auto id : ids) clearTimer (id);
    std::erase_if (m_audioBuffers, [&] (const auto& registration) {
        if (registration.owner != &object) return false;
        for (auto array : registration.arrays) JS_FreeValue (m_engine.getContext (), array);
        return true;
    });
}

void EngineObject::tick () {
    JSContext* ctx = this->m_engine.getContext ();
    const auto& recorder = this->m_scene.getAudioContext ().getRecorder ();
    for (const auto& registration : this->m_audioBuffers) {
	const float* source = registration.resolution == 16 ? recorder.audio16
	    : registration.resolution == 32                 ? recorder.audio32
							    : recorder.audio64;
	for (size_t channel = 0; channel < registration.arrays.size (); channel++) {
	    size_t offset = 0, length = 0;
	    JSValue buffer = JS_GetTypedArrayBuffer (ctx, registration.arrays[channel], &offset, &length, nullptr);
	    if (JS_IsException (buffer)) {
		JS_FreeValue (ctx, JS_GetException (ctx));
		continue;
	    }
	    size_t bufferLength = 0;
	    uint8_t* bytes = JS_GetArrayBuffer (ctx, &bufferLength, buffer);
	    if (bytes && length == registration.resolution * sizeof (float) && offset <= bufferLength
		&& length <= bufferLength - offset) {
		std::copy_n (
		    source + channel * registration.resolution, registration.resolution,
		    reinterpret_cast<float*> (bytes + offset)
		);
	    }
	    JS_FreeValue (ctx, buffer);
	}
    }
    // Snapshot due IDs before invoking JS: callbacks may cancel themselves,
    // cancel another due timer, or create a timer for the following frame.
    std::vector<uint32_t> due;
    for (auto& [id, timer] : this->m_timers) {
	timer.remaining -= std::max (g_Time - g_TimeLast, 0.0f);
	if (!(timer.remaining > 0)) {
	    due.push_back (id);
	}
    }
    for (uint32_t id : due) {
	const auto it = this->m_timers.find (id);
	if (it == this->m_timers.end ()) {
	    continue;
	}
	JSValue callback = JS_DupValue (ctx, it->second.callback);
	JSValue receiver = JS_DupValue (ctx, it->second.receiver);
	JSValue layer = JS_DupValue (ctx, it->second.layer);
        JSValue object = JS_DupValue (ctx, it->second.object);
	JSValue previousReceiver = this->m_activeReceiver;
	this->m_activeReceiver = receiver;
	JSValue previousLayer = JS_GetPropertyStr (ctx, this->m_engine.getGlobalThis (), "thisLayer");
        JSValue previousObject = JS_GetPropertyStr (ctx, this->m_engine.getGlobalThis (), "thisObject");
	JS_SetPropertyStr (ctx, this->m_engine.getGlobalThis (), "thisLayer", layer);
        JS_SetPropertyStr (ctx, this->m_engine.getGlobalThis (), "thisObject", object);
	WallpaperEngine::Data::Utils::ScopeGuard guard ([&] {
	    this->m_activeReceiver = previousReceiver;
	    JS_SetPropertyStr (ctx, this->m_engine.getGlobalThis (), "thisLayer", previousLayer);
            JS_SetPropertyStr (ctx, this->m_engine.getGlobalThis (), "thisObject", previousObject);
	    JS_FreeValue (ctx, receiver);
	});
	if (it->second.interval) {
	    it->second.remaining = it->second.duration;
	} else {
	    this->clearTimer (id);
	}
	JSValue result = JS_Call (ctx, callback, receiver, 0, nullptr);
	if (JS_IsException (result)) {
	    JSValue error = JS_GetException (ctx);
	    const char* message = JS_ToCString (ctx, error);
	    sLog.error ("SceneScript timer: ", message ? message : "unknown exception");
	    if (message) {
		JS_FreeCString (ctx, message);
	    }
	    JS_FreeValue (ctx, error);
	}
	JS_FreeValue (ctx, result);
	JS_FreeValue (ctx, callback);
    }
}
