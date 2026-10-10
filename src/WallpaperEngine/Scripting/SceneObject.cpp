#include "SceneObject.h"

#include <cstring>
#include <iomanip>
#include <sstream>
#include "SceneCameraTransforms.h"
#include "WallpaperEngine/Data/JSON.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "ScriptEngine.h"
#include "ScriptableObject.h"
#include "SceneSettingProperties.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

using namespace WallpaperEngine::Scripting;

SceneObject* get_opaque (JSContext* context, JSValueConst value) {
    auto* engine = static_cast<ScriptEngine*> (JS_GetContextOpaque (context));
    auto* owner = engine ? static_cast<SceneObject*> (JS_GetOpaque (value,
        JS_GetClassID (engine->getSceneObject ().getInstance ()))) : nullptr;
    if (!owner) JS_ThrowTypeError (context, "IScene receiver required");
    return owner;
}

static JSValue get_scene_time (JSContext* ctx, JSValueConst value, int, JSValueConst*) {
    auto* owner = get_opaque (ctx, value);
    return owner ? JS_NewFloat64 (ctx, owner->getScene ().getTime ()) : JS_EXCEPTION;
}

static JSValue get_scene_dt (JSContext* ctx, JSValueConst value, int, JSValueConst*) {
    auto* owner = get_opaque (ctx, value);
    return owner ? JS_NewFloat64 (ctx, owner->getScene ().getDeltaTime ()) : JS_EXCEPTION;
}

static JSValue get_scene_fps (JSContext* ctx, JSValueConst value, int, JSValueConst*) {
    auto* owner = get_opaque (ctx, value);
    return owner ? JS_NewFloat64 (ctx, owner->getScene ().getFps ()) : JS_EXCEPTION;
}

static JSValue scene_get_setting (JSContext* ctx, JSValueConst receiver, int, JSValueConst*, int magic) {
    auto* owner = get_opaque (ctx, receiver);
    if (!owner) return JS_EXCEPTION;
    const auto& property = sceneSettingProperties ()[magic];
    const auto& setting = property.setting (owner->getScene ().getScene ());
    if (!setting || !setting->value) return JS_UNDEFINED;
    if (property.type == SceneSettingType::Vec3)
        return owner->getEngine ().dynamicToJs (*setting->value, true);
    return sceneSettingScalarValue (ctx, *setting->value, property.type);
}

static JSValue scene_set_setting (JSContext* ctx, JSValueConst receiver, int argc, JSValueConst* argv, int magic) {
    const auto& property = sceneSettingProperties ()[magic];
    auto* owner = get_opaque (ctx, receiver);
    if (!owner) return JS_EXCEPTION;
    const auto& setting = property.setting (owner->getScene ().getScene ());
    if (!setting || !setting->value) return JS_UNDEFINED;
    return storeSceneSetting (ctx, argc ? argv[0] : JS_UNDEFINED, *setting->value, property.type)
        ? JS_UNDEFINED : JS_EXCEPTION;
}

JSValue get_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 1) {
	return JS_UNDEFINED;
    }

    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    JSValue layer = argv[0];

    if (JS_IsNumber (layer)) {
	int id = 0;

	JS_ToInt32 (ctx, &id, layer);

	auto* object = container->getScene ().getObject (id);

	if (object == nullptr) {
	    return JS_UNDEFINED;
	}

	if (!object->is<ScriptableObject> ()) {
	    return JS_UNDEFINED;
	}

	// TODO: REMOVE THIS CONST_CAST?
	return container->getEngine ().getAdapters ().object->instantiate (
	    const_cast<ScriptableObject&> (*object->as<ScriptableObject> ())
	);
    } else if (JS_IsString (layer)) {
	// find by name, this is harder
	const char* result = JS_ToCString (ctx, layer);

	if (result == nullptr) {
	    return JS_UNDEFINED;
	}

	ScopeGuard guard ([=] { JS_FreeCString (ctx, result); });

	for (auto object : container->getScene ().getObjectsByRenderOrder ()) {
	    if (!object->is<ScriptableObject> () || object->as<ScriptableObject> ()->getScriptName () != result) {
		continue;
	    }

	    if (!object->is<ScriptableObject> ()) {
		continue;
	    }

	    return container->getEngine ().getAdapters ().object->instantiate (*object->as<ScriptableObject> ());
	}
    }

    return JS_UNDEFINED;
}

static const WallpaperEngine::Render::CObject* resolve_scene_layer (
    JSContext* ctx, SceneObject& owner, JSValueConst value) {
    if (auto* layer = WallpaperEngine::Scripting::Adapters::ScriptableObjectAdapter::objectOf (value))
        return layer;
    if (JS_IsNumber (value)) {
        int index = 0;
        if (JS_ToInt32 (ctx, &index, value) < 0) return nullptr;
        const auto& order = owner.getScene ().getScriptLayers ();
        return index >= 0 && static_cast<size_t> (index) < order.size () ? order[index] : nullptr;
    }
    if (JS_IsString (value)) {
        const char* name = JS_ToCString (ctx, value);
        if (!name) return nullptr;
        const std::string requested (name);
        JS_FreeCString (ctx, name);
        // Match the name that script writes to `layer.name` update, as getLayer does.
        for (const auto* object : owner.getScene ().getScriptLayers ()) {
            const auto* scriptable = object->is<ScriptableObject> () ? object->as<ScriptableObject> () : nullptr;
            if ((scriptable ? scriptable->getScriptName () : object->getObject ().name) == requested)
                return object;
        }
    }
    return nullptr;
}

JSValue scene_set_camera_transforms (JSContext* ctx, JSValueConst thisValue,
                                     int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, thisValue);
    if (!owner) return JS_EXCEPTION;
    if (owner->getEngine ().isEvaluatingModuleTopLevel ())
        return JS_ThrowTypeError (ctx, "setCameraTransforms cannot be used in global scope");
    if (argc == 0 || !JS_IsObject (argv[0])) return JS_FALSE;
    const auto values = readSceneCameraTransforms (ctx, argv[0]);
    if (!values) return JS_EXCEPTION;
    owner->getMutableScene ().getCamera ().setTransforms (
            values->eye ? &*values->eye : nullptr, values->center ? &*values->center : nullptr,
            values->up ? &*values->up : nullptr, values->zoom ? &*values->zoom : nullptr);
    return JS_TRUE;
}

JSValue scene_enumerate_layers (JSContext* ctx, JSValueConst thisValue, int, JSValueConst*) {
    auto* owner = get_opaque (ctx, thisValue);
    if (!owner) return JS_EXCEPTION;
    if (owner->getEngine ().isEvaluatingModuleTopLevel ())
        return JS_ThrowTypeError (ctx, "enumerateLayers cannot be used in global scope");
    JSValue layers = JS_NewArray (ctx);
    if (JS_IsException (layers)) return layers;
    uint32_t index = 0;
    for (const auto* object : owner->getScene ().getScriptLayers ()) {
        if (!object || !object->is<ScriptableObject> ()) continue;
        JSValue layer = owner->getEngine ().getAdapters ().object->instantiate (
            const_cast<ScriptableObject&> (*object->as<ScriptableObject> ()));
        if (JS_IsException (layer) || JS_SetPropertyUint32 (ctx, layers, index++, layer) < 0) {
            JS_FreeValue (ctx, layers);
            return JS_EXCEPTION;
        }
    }
    return layers;
}

JSValue scene_get_initial_layer_config (JSContext* ctx, JSValueConst thisValue,
                                       int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, thisValue);
    if (!owner) return JS_EXCEPTION;
    if (owner->getEngine ().isEvaluatingModuleTopLevel ())
        return JS_ThrowTypeError (ctx, "getInitialLayerConfig cannot be used in global scope");
    const auto* layer = argc > 0 ? resolve_scene_layer (ctx, *owner, argv[0]) : nullptr;
    if (!layer || layer->getObject ().initialConfiguration.empty ()) return JS_NULL;
    const auto& json = layer->getObject ().initialConfiguration;
    // Parsing a new object on every call detaches nested effects, settings and
    // arrays from both the authored snapshot and other callers' copies.
    return JS_ParseJSON (ctx, json.data (), json.size (), "initial layer configuration");
}

static std::string vector_config_string (JSContext* ctx, JSValueConst value, int dimensions) {
    std::ostringstream encoded;
    encoded.imbue (std::locale::classic ());
    encoded << std::setprecision (std::numeric_limits<double>::max_digits10);
    const bool array = JS_IsArray (value) == 1;
    const char* keys[] = {"x", "y", "z", "w"};
    for (int component = 0; component < dimensions; ++component) {
        JSValue part = array ? JS_GetPropertyUint32 (ctx, value, component)
                             : JS_GetPropertyStr (ctx, value, keys[component]);
        if (JS_IsException (part)) throw std::invalid_argument ("Cannot read layer vector component");
        double number = 0.0;
        const bool valid = JS_IsNumber (part) && JS_ToFloat64 (ctx, &number, part) == 0
            && std::isfinite (number);
        JS_FreeValue (ctx, part);
        if (!valid) throw std::invalid_argument ("Layer vector requires finite numeric components");
        if (component) encoded << ' ';
        encoded << number;
    }
    return encoded.str ();
}

static JSValue shallow_layer_config (JSContext* ctx, ScriptEngine& engine, JSValueConst source,
                                     int valueDimensions = 0) {
    JSPropertyEnum* names = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames (ctx, &names, &count, source,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return JS_EXCEPTION;
    ScopeGuard namesGuard ([&] {
        for (uint32_t index = 0; index < count; ++index) JS_FreeAtom (ctx, names[index].atom);
        js_free (ctx, names);
    });
    JSValue result = JS_NewObject (ctx);
    if (JS_IsException (result)) return JS_EXCEPTION;
    try {
        for (uint32_t index = 0; index < count; ++index) {
            const char* key = JS_AtomToCString (ctx, names[index].atom);
            if (!key) throw std::invalid_argument ("Invalid layer configuration key");
            const std::string fieldName (key);
            JS_FreeCString (ctx, key);
            JSValue field = JS_GetProperty (ctx, source, names[index].atom);
            if (JS_IsException (field)) throw std::invalid_argument ("Invalid layer configuration value");
            ScopeGuard fieldGuard ([&] { JS_FreeValue (ctx, field); });
            int dimensions = 0;
            if (valueDimensions && fieldName == "value") dimensions = valueDimensions;
            else if (fieldName == "origin" || fieldName == "angles" || fieldName == "scale"
                || fieldName == "color" || fieldName == "backgroundcolor") dimensions = 3;
            else if (fieldName == "size" || fieldName == "parallaxDepth") dimensions = 2;
            if (dimensions && JS_IsObject (field)) {
                int setting = 0;
                if (!valueDimensions) {
                    JSAtom valueAtom = JS_NewAtom (ctx, "value");
                    if (valueAtom == JS_ATOM_NULL) throw std::invalid_argument ("Cannot inspect layer vector");
                    setting = JS_GetOwnProperty (ctx, nullptr, field, valueAtom);
                    JS_FreeAtom (ctx, valueAtom);
                    if (setting < 0) throw std::invalid_argument ("Cannot inspect layer vector");
                }
                if (setting) {
                    JSValue wrapper = shallow_layer_config (ctx, engine, field, dimensions);
                    if (JS_IsException (wrapper)) throw std::invalid_argument ("Cannot copy layer vector setting");
                    JS_FreeValue (ctx, field);
                    field = wrapper;
                } else {
                    const std::string encoded = vector_config_string (ctx, field, dimensions);
                    JS_FreeValue (ctx, field);
                    field = JS_NewString (ctx, encoded.c_str ());
                    if (JS_IsException (field)) throw std::invalid_argument ("Cannot encode layer vector");
                }
            }
            const int written = JS_SetProperty (ctx, result, names[index].atom, JS_DupValue (ctx, field));
            if (written < 0) throw std::invalid_argument ("Cannot write layer configuration value");
        }
    } catch (...) {
        JS_FreeValue (ctx, result);
        throw;
    }
    return result;
}

JSValue scene_create_layer (JSContext* ctx, JSValueConst receiver, int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, receiver);
    if (!owner) return JS_EXCEPTION;
    if (owner->getEngine ().isEvaluatingModuleTopLevel ())
        return JS_ThrowTypeError (ctx, "createLayer cannot be called from global scope");
    if (argc != 1 || (!JS_IsObject (argv[0]) && !JS_IsString (argv[0])))
        return JS_ThrowTypeError (ctx, "createLayer expects a configuration object or image asset path");
    std::string configuration;
    if (JS_IsString (argv[0])) {
        const char* path = JS_ToCString (ctx, argv[0]);
        if (!path) return JS_EXCEPTION;
        configuration = WallpaperEngine::Data::JSON::JSON {{"image", path}}.dump ();
        JS_FreeCString (ctx, path);
    } else {
        JSValue normalized;
        try { normalized = shallow_layer_config (ctx, owner->getEngine (), argv[0]); }
        catch (const std::exception& error) { return JS_ThrowTypeError (ctx, "%s", error.what ()); }
        if (JS_IsException (normalized)) return JS_EXCEPTION;
        JSValue json = JS_JSONStringify (ctx, normalized, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue (ctx, normalized);
        if (JS_IsException (json)) return JS_EXCEPTION;
        const char* text = JS_ToCString (ctx, json);
        if (text) configuration = text;
        JS_FreeValue (ctx, json);
        if (!text) return JS_EXCEPTION;
        JS_FreeCString (ctx, text);
    }
    try {
        const auto first = owner->getEngine ().nextQueueOrder ();
        auto* object = owner->getMutableScene ().createScriptLayer (configuration);
        owner->getEngine ().initializeModules (first);
        return owner->getEngine ().getAdapters ().object->instantiate (*object->as<ScriptableObject> ());
    } catch (const std::exception& error) { return JS_ThrowTypeError (ctx, "createLayer: %s", error.what ()); }
}

JSValue scene_get_layer_index (JSContext* ctx, JSValueConst receiver, int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, receiver);
    if (!owner) return JS_EXCEPTION;
    const auto* object = argc ? resolve_scene_layer (ctx, *owner, argv[0]) : nullptr;
    const auto layers = owner->getScene ().getScriptLayers ();
    const auto found = std::find (layers.begin (), layers.end (), object);
    return JS_NewInt32 (ctx, found == layers.end () ? -1 : int (found - layers.begin ()));
}

JSValue scene_sort_layer (JSContext* ctx, JSValueConst receiver, int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, receiver);
    if (!owner) return JS_EXCEPTION;
    if (argc != 2) return JS_ThrowTypeError (ctx, "sortLayer requires layer and index");
    int32_t index;
    if (JS_ToInt32 (ctx, &index, argv[1]) < 0) return JS_EXCEPTION;
    return JS_NewBool (ctx, owner->getMutableScene ().sortScriptLayer (resolve_scene_layer (ctx, *owner, argv[0]), index));
}

JSValue scene_destroy_layer (JSContext* ctx, JSValueConst receiver, int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, receiver);
    if (!owner) return JS_EXCEPTION;
    return JS_NewBool (ctx, argc && owner->getMutableScene ().destroyScriptLayer (resolve_scene_layer (ctx, *owner, argv[0])));
}

SceneObject::SceneObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_classId (0) {
    this->m_definition = { .class_name = "IScene" };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_classId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_classId, &this->m_definition);
    this->m_instance = JS_NewObjectClass (this->m_engine.getContext (), this->m_classId);

    JS_DupValue (this->m_engine.getContext (), this->m_instance);

    // set properties
    JS_SetOpaque (this->m_instance, this);
    for (const auto* name : { "time", "currentTime", "dt", "fps" }) {
        JSAtom atom = JS_NewAtom (m_engine.getContext (), name);
        JSCFunction* getter = std::strcmp (name, "dt") == 0 ? get_scene_dt
            : std::strcmp (name, "fps") == 0 ? get_scene_fps : get_scene_time;
        JS_DefinePropertyGetSet (m_engine.getContext (), m_instance, atom,
            JS_NewCFunction (m_engine.getContext (), getter, "get", 0), JS_UNDEFINED, JS_PROP_ENUMERABLE);
        JS_FreeAtom (m_engine.getContext (), atom);
    }
    const auto& settings = sceneSettingProperties ();
    for (size_t index = 0; index < settings.size (); ++index) {
        JSContext* ctx = m_engine.getContext ();
        JSAtom atom = JS_NewAtom (ctx, settings[index].name);
        JS_DefinePropertyGetSet (ctx, m_instance, atom,
            JS_NewCFunctionMagic (ctx, scene_get_setting, "get", 0, JS_CFUNC_generic_magic, int (index)),
            JS_NewCFunctionMagic (ctx, scene_set_setting, "set", 1, JS_CFUNC_generic_magic, int (index)),
            JS_PROP_ENUMERABLE);
        JS_FreeAtom (ctx, atom);
    }
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "getLayer",
	JS_NewCFunction (this->m_engine.getContext (), get_layer, "getLayer", 1), JS_PROP_ENUMERABLE
    );
    for (const auto& [name, function] : {
        std::pair<const char*, JSCFunction*> {"setCameraTransforms", scene_set_camera_transforms},
        {"enumerateLayers", scene_enumerate_layers}, {"getInitialLayerConfig", scene_get_initial_layer_config},
        {"createLayer", scene_create_layer}, {"getLayerIndex", scene_get_layer_index},
        {"sortLayer", scene_sort_layer}, {"destroyLayer", scene_destroy_layer}})
        JS_SetPropertyStr (m_engine.getContext (), m_instance, name,
            JS_NewCFunction (m_engine.getContext (), function, name, 1));
}

SceneObject::~SceneObject () { JS_FreeValue (this->m_engine.getContext (), this->m_instance); }
