#include "ScriptableObjectAdapter.h"
#include "WallpaperEngine/Scripting/LayerPropertyWrites.h"

#include <cstring>
#include <utility>

#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"
#include "WallpaperEngine/Scripting/PuppetScriptObject.h"
#include "WallpaperEngine/Scripting/EffectThisObject.h"
#include "WallpaperEngine/Scripting/TextureAnimationObject.h"
#include "WallpaperEngine/Render/Objects/CImage.h"

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::Scripting::Adapters;

#define SCRIPTABLE_OPAQUE_MAGIC 0xdeadbeef

struct OpaqueScriptableObjectAdapter {
    unsigned int magic;
    ScriptableObjectAdapter& adapter;
    std::shared_ptr<WallpaperEngine::Scripting::ScriptableObject::Lifetime> lifetime;
};

WallpaperEngine::Scripting::ScriptableObject* ScriptableObjectAdapter::objectOf (JSValueConst value) {
    JSClassID classId = 0;
    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (value, &classId));
    return container && container->magic == SCRIPTABLE_OPAQUE_MAGIC ? container->lifetime->object : nullptr;
}

static JSValue layer_effect (JSContext* ctx, JSValueConst value, int argc, JSValueConst* argv, int count) {
    const auto* effects = WallpaperEngine::Scripting::layerEffects (ScriptableObjectAdapter::objectOf (value));
    if (!effects) return JS_NULL;
    if (count) return JS_NewInt64 (ctx, effects->size ());
    if (!argc) return JS_NULL;
    int32_t index = -1;
    if (JS_IsNumber (argv[0])) {
        double number = 0;
        if (JS_ToFloat64 (ctx, &number, argv[0]) < 0) return JS_EXCEPTION;
        if (number >= 0 && number < effects->size () && std::trunc (number) == number) index = int32_t (number);
    } else if (JS_IsString (argv[0])) {
        const char* name = JS_ToCString (ctx, argv[0]);
        if (!name) return JS_EXCEPTION;
        for (size_t i = 0; i < effects->size (); ++i) if ((*effects)[i]->name == name) index = int32_t (i);
        JS_FreeCString (ctx, name);
    }
    return index < 0 ? JS_NULL : WallpaperEngine::Scripting::createEffectThisObject (ctx, value, index);
}

static JSValue layer_texture_animation (JSContext* ctx, JSValueConst value, int, JSValueConst*) {
    auto* layer = ScriptableObjectAdapter::objectOf (value);
    if (!layer || !layer->is<WallpaperEngine::Render::Objects::CImage> ()) return JS_NULL;
    return layer->getScene ().getScriptEngine ().getTextureAnimations ().instantiate (layer,
        layer->as<WallpaperEngine::Render::Objects::CImage> ()->getTextureAnimation ());
}

JSValue scriptableobject_play (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (this_val, &classId));
    if (!container || container->magic != SCRIPTABLE_OPAQUE_MAGIC || !container->lifetime->object) {
	return JS_EXCEPTION;
    }
    container->lifetime->object->play ();
    return JS_UNDEFINED;
}

JSValue scriptableobject_stop (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (this_val, &classId));
    if (!container || container->magic != SCRIPTABLE_OPAQUE_MAGIC || !container->lifetime->object) {
	return JS_EXCEPTION;
    }
    container->lifetime->object->stop ();
    return JS_UNDEFINED;
}

JSValue scriptableobject_property_get (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    JSClassID classId = 0;

    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (obj_val, &classId));

    if (!container || container->magic != SCRIPTABLE_OPAQUE_MAGIC || !container->lifetime->object) {
	return JS_EXCEPTION;
    }

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return JS_EXCEPTION;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

    JSValue puppet = container->adapter.getEngine ().getPuppetScripts ().method (obj_val, name);
    if (!JS_IsUndefined (puppet)) return puppet;
    if (std::strcmp (name, "getTextureAnimation") == 0) return JS_NewCFunction (ctx, layer_texture_animation, name, 0);
    if (std::strcmp (name, "getEffect") == 0 || std::strcmp (name, "getEffectCount") == 0)
        return JS_NewCFunctionMagic (ctx, layer_effect, name, 1, JS_CFUNC_generic_magic, std::strcmp (name, "getEffectCount") == 0);

    if (std::strcmp (name, "play") == 0) {
	return JS_NewCFunction (ctx, scriptableobject_play, "play", 0);
    }
    if (std::strcmp (name, "stop") == 0) {
	return JS_NewCFunction (ctx, scriptableobject_stop, "stop", 0);
    }

    JSPropertyDescriptor descriptor {};
    if (JS_GetOwnProperty (ctx, &descriptor, obj_val, atom) > 0) {
        JSValue result = JS_DupValue (ctx, descriptor.value);
        JS_FreeValue (ctx, descriptor.value);
        JS_FreeValue (ctx, descriptor.getter);
        JS_FreeValue (ctx, descriptor.setter);
        return result;
    }
    if (std::strcmp (name, "solid") == 0) return JS_NewBool (ctx, container->lifetime->object->isSolid ());
    if (std::strcmp (name, "disablepropagation") == 0) return JS_NewBool (ctx, container->lifetime->object->disablesCursorPropagation ());
    if (std::strcmp (name, "name") == 0)
        return JS_NewString (ctx, container->lifetime->object->getScriptName ().c_str ());
    if (std::strcmp (name, "id") == 0)
        return JS_NewInt32 (ctx, container->lifetime->object->getId ());
    const auto alias = WallpaperEngine::Scripting::layerPropertyKey (name);
    const auto key = alias.value_or (name);
    const auto& properties = container->lifetime->object->getProperties ();
    const auto property = properties.find (std::string (key));
    if (property == properties.end ()) return JS_UNDEFINED;
    return container->adapter.getEngine ().dynamicToJs (
        property->second.value, true, std::strcmp (name, "angles") == 0);
}

int scriptableobject_property_set (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
) {
    JSClassID classId = 0;

    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (obj_val, &classId));

    if (!container || container->magic != SCRIPTABLE_OPAQUE_MAGIC || !container->lifetime->object) {
	return -1;
    }

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return -1;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });
    if (std::strcmp (name, "solid") == 0 || std::strcmp (name, "disablepropagation") == 0) {
        if (JS_IsBool (val)) {
            if (std::strcmp (name, "solid") == 0) container->lifetime->object->setSolid (JS_ToBool (ctx, val));
            else container->lifetime->object->setDisablePropagation (JS_ToBool (ctx, val));
        }
        return 1;
    }
    if (std::strcmp (name, "name") == 0) {
        std::string value;
        const auto converted = WallpaperEngine::Scripting::convertLayerString (ctx, val, value);
        if (converted == WallpaperEngine::Scripting::LayerWrite::Assign)
            container->lifetime->object->setScriptName (std::move (value));
        return converted == WallpaperEngine::Scripting::LayerWrite::Exception ? -1 : 1;
    }
    const auto& properties = container->lifetime->object->getProperties ();
    const auto alias = WallpaperEngine::Scripting::layerPropertyKey (name);
    const auto property = properties.find (std::string (alias.value_or (name)));
    if (property == properties.end ())
        return WallpaperEngine::Scripting::defineLayerExpando (ctx, receiver, atom, val, flags);
    try {
        container->adapter.getEngine ().updateValue (val, property->second.value, std::strcmp (name, "angles") == 0,
            property->second.type);
        return JS_HasException (ctx) ? -1 : 1;
    } catch (const std::exception& error) {
        JS_ThrowTypeError (ctx, "%s", error.what ());
        return -1;
    }
}

static void scriptableobject_finalizer (JSRuntime*, JSValueConst value) {
    JSClassID classId = 0;
    delete static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (value, &classId));
}

ScriptableObjectAdapter::ScriptableObjectAdapter (ScriptEngine& engine, std::string name) :
    ObjectAdapter (engine), m_instances (engine.getContext ()),
    m_exoticMethods (
	{
	    .get_property = scriptableobject_property_get,
	    .set_property = scriptableobject_property_set,
	}
    ),
    m_name (std::move (name)) {
    this->registerType (
	{
	    .class_name = m_name.c_str (),
	    .finalizer = scriptableobject_finalizer,
	    .exotic = &m_exoticMethods,
	}
    );
}

JSValue ScriptableObjectAdapter::instantiate (ScriptableObject& object) {
    JSValue cached = m_instances.find (&object);
    if (!JS_IsUndefined (cached)) return cached;
    JSValue result = this->ObjectAdapter::instantiate (object);
    JS_SetOpaque (
	result,
	new OpaqueScriptableObjectAdapter { .magic = SCRIPTABLE_OPAQUE_MAGIC, .adapter = *this, .lifetime = object.getLifetime () }
    );

    m_instances.retain (&object, result);
    return result;
}

JSValue ScriptableObjectAdapter::instantiate (DynamicValue& value) {
    throw std::runtime_error ("Cannot create a ScriptableObject instance from a DynamicValue");
}
