#pragma once

#include "ObjectAdapter.h"
#include "ObjectInstanceCache.h"

namespace WallpaperEngine::Scripting::Adapters {
class ScriptableObjectAdapter : public ObjectAdapter {
public:
    explicit ScriptableObjectAdapter (ScriptEngine& engine, std::string name);

    JSValue instantiate (ScriptableObject& object) override;
    static ScriptableObject* objectOf (JSValueConst value);
    JSValue instantiate (Data::Model::DynamicValue& value) override;
    void forgetInstance (const ScriptableObject& object) { m_instances.forget (&object); }

private:
    ObjectInstanceCache m_instances;
    JSClassExoticMethods m_exoticMethods;
    std::string m_name;
};
}
