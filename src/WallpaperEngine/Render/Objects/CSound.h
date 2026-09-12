#pragma once

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

using namespace WallpaperEngine;

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

class CSound final : virtual public CObject, public Scripting::ScriptableObject {
public:
    CSound (Wallpapers::CScene& scene, const Sound& sound);
    ~CSound () override;

    void render () override;
    void play () override;
    void stop () override;

protected:
    void load ();

private:
    std::map<int, Audio::AudioStream*> m_audioStreams = {};
    bool m_scriptInitialized = false;

    const Sound& m_sound;
};
} // namespace WallpaperEngine::Render::Objects
