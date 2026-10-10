#pragma once

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Data/Model/EffectConditions.h"

namespace WallpaperEngine::Data::Parsers {
using JSON = WallpaperEngine::Data::JSON::JSON;
using namespace WallpaperEngine::Data::Model;

class EffectParser {
public:
    static EffectUniquePtr load (const Project& project, const std::string& filename);
    static ComboMap parseConditionCombos (const JSON& it);
    static EffectConditions parseConditions (const JSON& it);

private:
    static EffectUniquePtr parse (const JSON& it, const Project& project);
    static std::vector<std::string> parseDependencies (const JSON& it);
    static std::vector<EffectPassUniquePtr> parseEffectPasses (const JSON& it, const Project& project);
    static std::map<int, std::string> parseBinds (const JSON& it);
    static std::vector<FBOUniquePtr> parseFBOs (const JSON& it);
};
} // namespace WallpaperEngine::Data::Parsers
