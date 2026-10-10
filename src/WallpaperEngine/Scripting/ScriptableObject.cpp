#include "ScriptableObject.h"

#include "ScriptEngine.h"
#include "EffectThisObject.h"
#include "Adapters/ScriptableObjectAdapter.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"

#include <ranges>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Scripting;

ScriptableObject::ScriptableObject (Wallpapers::CScene& scene, const Object& object) : CObject (scene, object), m_solid (object.solid), m_disablePropagation (object.disablePropagation), m_scriptName (object.name) {
    // register common dynamic values
    this->registerProperty ("origin", *object.origin->value);
    this->registerProperty ("scale", *object.groupScale->value);
    this->registerProperty ("angles", *object.groupAngles->value);
    this->registerProperty ("visible", *object.groupVisible->value);
    if (const auto* effects = layerEffects (this)) {
        JSContext* ctx = scene.getScriptEngine ().getContext ();
        JSValue layer = scene.getScriptEngine ().getAdapters ().object->instantiate (*this);
        for (size_t index = 0; index < effects->size (); ++index) {
            JSValue owner = createEffectThisObject (ctx, layer, index);
            scene.getScriptEngine ().queueScript ("effect" + std::to_string (getId ()) + "[" + std::to_string (index) + "].visible",
                *(*effects)[index]->visible->value, *this, owner);
            JS_FreeValue (ctx, owner);
        }
        JS_FreeValue (ctx, layer);
    }
}

ScriptableObject::~ScriptableObject () {
    m_lifetime->object = nullptr;
    getScene ().getScriptEngine ().unregisterObject (*this);
    getScene ().getScriptEngine ().getAdapters ().object->forgetInstance (*this);
}

DynamicValue& ScriptableObject::getProperty (const std::string& name) {
    const auto it = this->m_properties.find (name);

    if (it == this->m_properties.end ()) {
	sLog.exception ("Property '" + name + "' not found on object '" + this->getObject ().name + "'");
    }

    return it->second.value;
}

const std::map<std::string, ScriptableObject::PropertyEntry>& ScriptableObject::getProperties () const {
    return this->m_properties;
}

void ScriptableObject::registerProperty (const std::string& name, DynamicValue& value, DynamicValue::UnderlyingType type) {
    this->m_properties.erase (name);
    auto inserted = this->m_properties.emplace (
	name, PropertyEntry { .key = name + "_" + std::to_string (this->getId ()), .value = value, .type = type }
    );

    if (!inserted.second) {
	return;
    }

    this->getScene ().getScriptEngine ().queueScript (inserted.first->second.key, inserted.first->second.value, *this,
        JS_UNDEFINED, type);
}
