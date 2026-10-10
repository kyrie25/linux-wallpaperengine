#pragma once

#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Data/Model/DynamicValue.h"

extern "C" {
#include "quickjs.h"
}

namespace WallpaperEngine::Scripting {
// Shader-bound scripts receive their material constants as thisObject while
// thisLayer remains the containing image layer.
JSValue createMaterialThisObject (JSContext* context, JSValueConst layer,
                                  const std::map<std::string, const Data::Model::UserSetting*>& constants,
                                  const std::map<std::string, Data::Model::DynamicValue::UnderlyingType>& types);
}
