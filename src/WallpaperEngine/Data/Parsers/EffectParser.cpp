#include "EffectParser.h"
#include "MaterialParser.h"

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Effect.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/FileSystem/Container.h"

#include <bit>
#include <cmath>
#include <limits>

using WallpaperEngine::Data::JSON::JSON;

namespace {
int32_t nativeConditionNumber (const JSON& value) {
    if (value.is_number_unsigned ())
        return std::bit_cast<int32_t> (static_cast<uint32_t> (value.get<uint64_t> ()));
    if (value.is_number_integer ())
        return std::bit_cast<int32_t> (static_cast<uint32_t> (value.get<int64_t> ()));
    if (value.is_number_float ()) {
        const auto number = value.get<double> ();
        // Native CVTTSD2SI returns the indefinite signed value on overflow.
        return std::isfinite (number) && number >= -2147483648.0 && number < 2147483648.0
            ? static_cast<int32_t> (number) : std::numeric_limits<int32_t>::min ();
    }
    return 0;
}

}

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Model;

EffectUniquePtr EffectParser::load (const Project& project, const std::string& filename) {
    const auto effectJson = WallpaperEngine::Data::JSON::parseAuthoringJson (project.assetLocator->readString (filename), filename);

    return parse (effectJson, project);
}

ComboMap EffectParser::parseConditionCombos (const JSON& it) {
    ComboMap result;
    if (it.is_object ())
        for (const auto& [name, value] : it.items ()) result.emplace (name, nativeConditionNumber (value));
    return result;
}

EffectConditions EffectParser::parseConditions (const JSON& it) {
    EffectConditions result;
    if (!it.is_array ()) return result;
    for (const auto& record : it) {
        if (!record.is_object ()) continue;
        for (const auto& [name, value] : record.items ()) {
            if (!value.is_number () && !value.is_object ()) continue;
            EffectCondition condition {.combo = name};
            if (value.is_number ()) {
                condition.value = nativeConditionNumber (value);
            } else {
                const auto expected = value.optional ("value");
                condition.value = expected ? nativeConditionNumber (*expected) : 0;
                const auto operation = value.optional ("op");
                if (operation && operation->is_string ()) {
                    if (*operation == "ge") condition.comparison = EffectConditionComparison::GreaterEqual;
                    else if (*operation == "gt") condition.comparison = EffectConditionComparison::Greater;
                    else if (*operation == "le") condition.comparison = EffectConditionComparison::LessEqual;
                    else if (*operation == "lt") condition.comparison = EffectConditionComparison::Less;
                }
            }
            result.push_back (std::move (condition));
        }
    }
    return result;
}

EffectUniquePtr EffectParser::parse (const JSON& it, const Project& project) {
    const auto dependencies = it.optional ("dependencies");
    const auto fbos = it.optional ("fbos");

    return std::make_unique<Effect> (Effect {
	.name = it.optional<std::string> ("name", ""),
	.description = it.optional<std::string> ("description", ""),
	.group = it.optional<std::string> ("group", ""),
	.preview = it.optional<std::string> ("preview", ""),
	.dependencies = dependencies.has_value () ? parseDependencies (*dependencies) : std::vector<std::string> {},
	.passes = parseEffectPasses (it.require ("passes", "Effect file must have passes"), project),
	.fbos = fbos.has_value () ? parseFBOs (*fbos) : std::vector<FBOUniquePtr> {},
    });
}

std::vector<std::string> EffectParser::parseDependencies (const JSON& it) {
    std::vector<std::string> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	result.push_back (cur);
    }

    return result;
}

std::vector<EffectPassUniquePtr> EffectParser::parseEffectPasses (const JSON& it, const Project& project) {
    std::vector<EffectPassUniquePtr> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	const auto binds = cur.optional ("bind");
	const auto command = cur.optional ("command");
	const auto material = cur.optional ("material");

	// TODO: CAN TARGET BE SET IF MATERIAL IS SET?

	result.push_back (
	    std::make_unique<EffectPass> (EffectPass {
		.conditions = cur.contains ("conditions") ? parseConditions (cur["conditions"]) : EffectConditions {},
		.material = material.has_value () ? MaterialParser::load (project, *material)
						  : std::optional<MaterialUniquePtr> {},
		.binds = binds.has_value () ? parseBinds (binds.value ()) : std::map<int, std::string> {},
		.command = command.has_value () ? (command.value () == "copy" ? Command_Copy : Command_Swap)
						: std::optional<PassCommandType> {},
		.source = command.has_value ()
		    ? cur.require<std::string> ("source", "Effect command must have a source")
		    : cur.optional<std::string> ("source"),
		.target = command.has_value ()
		    ? cur.require<std::string> ("target", "Effect command must have a target")
		    : cur.optional<std::string> ("target"),
	    })
	);
    }

    return result;
}

std::map<int, std::string> EffectParser::parseBinds (const JSON& it) {
    std::map<int, std::string> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	result.emplace (
	    cur.require ("index", "Texture binds must have an index"),
	    cur.require ("name", "Texture bind must name the FBO that should be used")
	);
    }

    return result;
}

std::vector<FBOUniquePtr> EffectParser::parseFBOs (const JSON& it) {
    std::vector<FBOUniquePtr> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	result.push_back (
	    std::make_unique<FBO> (FBO {
		.conditions = cur.contains ("conditions") ? parseConditions (cur["conditions"]) : EffectConditions {},
		.name = cur.require<std::string> ("name", "FBO must have a name"),
		.format = cur.optional<std::string> ("format", "rgba8888"),
		.scale = cur.optional ("scale", 1.0f),
		.unique = cur.optional ("unique", false),
	    })
	);
    }

    return result;
}