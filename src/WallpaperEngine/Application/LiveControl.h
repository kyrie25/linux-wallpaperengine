#pragma once

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Render/WallpaperState.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace WallpaperEngine::Application {
struct LiveControl {
    std::optional<int> fps;
    std::optional<int> volume;
    std::optional<Render::WallpaperState::TextureUVsScaling> scaling;
    std::optional<glm::vec2> alignment;

    static LiveControl parse (const Data::JSON::JSON& json) {
        if (!json.is_object ()) throw std::invalid_argument ("control must be an object");
        LiveControl result;
        for (const auto& [key, value] : json.items ()) {
            if (key == "fps" || key == "volume") {
                if (!value.is_number_integer ()) throw std::invalid_argument (key + " must be an integer");
                const auto number = value.get<int64_t> ();
                if (key == "fps") result.fps = std::clamp<int64_t> (number, 1, 240);
                else result.volume = std::clamp<int64_t> (number, 0, 128);
            } else if (key == "scaling") {
                using Scaling = Render::WallpaperState::TextureUVsScaling;
                if (value == "fill") result.scaling = Scaling::ZoomFillUVs;
                else if (value == "fit") result.scaling = Scaling::ZoomFitUVs;
                else if (value == "stretch") result.scaling = Scaling::StretchUVs;
                else if (value == "default") result.scaling = Scaling::DefaultUVs;
                else throw std::invalid_argument ("invalid scaling");
            } else if (key == "alignment") {
                if (!value.is_array () || value.size () != 2 || !value[0].is_number () || !value[1].is_number ())
                    throw std::invalid_argument ("alignment must contain two numbers");
                const glm::vec2 alignment (value[0].get<float> (), value[1].get<float> ());
                if (!std::isfinite (alignment.x) || !std::isfinite (alignment.y))
                    throw std::invalid_argument ("alignment must be finite");
                result.alignment = glm::clamp (alignment, glm::vec2 (0), glm::vec2 (1));
            } else {
                throw std::invalid_argument ("unsupported live setting: " + key);
            }
        }
        return result;
    }
};
}
