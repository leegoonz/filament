/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gltfio/MaterialProvider.h>

#include <filament/MaterialInstance.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>

#include <math/mat4.h>

#include <utils/Log.h>

#if GLTFIO_LITE
#include "gltfresources_lite.h"
#else
#include "gltfresources.h"
#endif

using namespace filament;
using namespace filament::math;
using namespace gltfio;
using namespace utils;

namespace {

using CullingMode = MaterialInstance::CullingMode;

class UbershaderLoader : public MaterialProvider {
public:
    UbershaderLoader(filament::Engine* engine);
    ~UbershaderLoader() {}

    MaterialInstance* createMaterialInstance(MaterialKey* config, UvMap* uvmap,
            const char* label) override;

    size_t getMaterialsCount() const noexcept override;
    const Material* const* getMaterials() const noexcept override;
    void destroyMaterials() override;

    bool needsDummyData(VertexAttribute attrib) const noexcept override {
        switch (attrib) {
            case VertexAttribute::UV0:
            case VertexAttribute::UV1:
            case VertexAttribute::COLOR:
                return true;
            default:
                return false;
        }
    }

    Material* getMaterial(const MaterialKey& config) const;

    enum ShadingMode {
        UNLIT = 0,
        LIT = 1,
        SPECULAR_GLOSSINESS = 2,
    };

    mutable Material* mMaterials[12] = {};
    Texture* mDummyTexture = nullptr;

    Engine* mEngine;
};

#if GLTFIO_LITE

#define CREATE_MATERIAL(name) Material::Builder() \
    .package(GLTFRESOURCES_LITE_ ## name ## _DATA, GLTFRESOURCES_LITE_ ## name ## _SIZE) \
    .build(*mEngine);

#else

#define CREATE_MATERIAL(name) Material::Builder() \
    .package(GLTFRESOURCES_ ## name ## _DATA, GLTFRESOURCES_ ## name ## _SIZE) \
    .build(*mEngine);

#endif

#define MATINDEX(shading, alpha, sheen, transmit, volume) (volume ? 11 : (transmit ? 10 : (sheen ? 9 : (int(shading) + 3 * int(alpha)))))

UbershaderLoader::UbershaderLoader(Engine* engine) : mEngine(engine) {
    unsigned char texels[4] = {};
    mDummyTexture = Texture::Builder()
            .width(1).height(1)
            .format(Texture::InternalFormat::RGBA8)
            .build(*mEngine);
    Texture::PixelBufferDescriptor pbd(texels, sizeof(texels), Texture::Format::RGBA,
            Texture::Type::UBYTE);
    mDummyTexture->setImage(*mEngine, 0, std::move(pbd));
}

size_t UbershaderLoader::getMaterialsCount() const noexcept {
    return sizeof(mMaterials) / sizeof(mMaterials[0]);
}

const Material* const* UbershaderLoader::getMaterials() const noexcept {
    return &mMaterials[0];
}

void UbershaderLoader::destroyMaterials() {
    for (auto& material : mMaterials) {
        mEngine->destroy(material);
        material = nullptr;
    }
    mEngine->destroy(mDummyTexture);
}

Material* UbershaderLoader::getMaterial(const MaterialKey& config) const {
    const ShadingMode shading = config.unlit ? UNLIT :
            (config.useSpecularGlossiness ? SPECULAR_GLOSSINESS : LIT);
    const int matindex = MATINDEX(shading, config.alphaMode, config.hasSheen, config.hasTransmission, config.hasVolume);
    if (mMaterials[matindex] != nullptr) {
        return mMaterials[matindex];
    }
    switch (matindex) {
        #if !GLTFIO_LITE || defined(GLTFRESOURCES_LITE_LIT_OPAQUE_DATA)
        case MATINDEX(LIT, AlphaMode::OPAQUE, false, false, false): mMaterials[matindex] = CREATE_MATERIAL(LIT_OPAQUE); break;
        #endif

        #if !GLTFIO_LITE || defined(GLTFRESOURCES_LITE_LIT_FADE_DATA)
        case MATINDEX(LIT, AlphaMode::BLEND, false, false, false): mMaterials[matindex] = CREATE_MATERIAL(LIT_FADE); break;
        #endif

        #if !GLTFIO_LITE
        case MATINDEX(LIT, AlphaMode::MASK, false, false, false): mMaterials[matindex] = CREATE_MATERIAL(LIT_MASKED); break;
        case MATINDEX(UNLIT, AlphaMode::OPAQUE, false, false, false): mMaterials[matindex] = CREATE_MATERIAL(UNLIT_OPAQUE); break;
        case MATINDEX(UNLIT, AlphaMode::MASK, false, false, false): mMaterials[matindex] = CREATE_MATERIAL(UNLIT_MASKED); break;
        case MATINDEX(UNLIT, AlphaMode::BLEND, false, false, false): mMaterials[matindex] = CREATE_MATERIAL(UNLIT_FADE); break;
        case MATINDEX(SPECULAR_GLOSSINESS, AlphaMode::OPAQUE, false, false, false): mMaterials[matindex] = CREATE_MATERIAL(SPECULARGLOSSINESS_OPAQUE); break;
        case MATINDEX(SPECULAR_GLOSSINESS, AlphaMode::MASK, false, false, false): mMaterials[matindex] = CREATE_MATERIAL(SPECULARGLOSSINESS_MASKED); break;
        case MATINDEX(SPECULAR_GLOSSINESS, AlphaMode::BLEND, false, false, false): mMaterials[matindex] = CREATE_MATERIAL(SPECULARGLOSSINESS_FADE); break;
        case MATINDEX(0, 0, false, true, false): mMaterials[matindex] = CREATE_MATERIAL(LIT_TRANSMISSION); break;
        case MATINDEX(0, 0, true, false, false): mMaterials[matindex] = CREATE_MATERIAL(LIT_SHEEN); break;
        case MATINDEX(0, 0, false, false, true): mMaterials[matindex] = CREATE_MATERIAL(LIT_VOLUME); break;
        #endif
    }
    if (mMaterials[matindex] == nullptr) {
        slog.w << "Unsupported glTF material configuration; falling back to LIT_OPAQUE." << io::endl;
        MaterialKey litOpaque = config;
        litOpaque.alphaMode = AlphaMode::OPAQUE;
        litOpaque.hasTransmission = false;
        litOpaque.hasVolume = false;
        litOpaque.hasSheen = false;
        litOpaque.useSpecularGlossiness = false;
        litOpaque.unlit = false;
        return getMaterial(litOpaque);
    }
    return mMaterials[matindex];
}

MaterialInstance* UbershaderLoader::createMaterialInstance(MaterialKey* config, UvMap* uvmap,
        const char* label) {
    // Diagnostics are not supported with LOAD_UBERSHADERS, please use GENERATE_SHADERS instead.
    if (config->enableDiagnostics) {
        return nullptr;
    }

    if (config->hasVolume && config->hasSheen) {
        slog.w << "Volume and sheen are not supported together in ubershader mode,"
                  " removing sheen (" << label << ")." << io::endl;
        config->hasSheen = false;
    }

    if (config->hasTransmission && config->hasSheen) {
        slog.w << "Transmission and sheen are not supported together in ubershader mode,"
                  " removing sheen (" << label << ")." << io::endl;
        config->hasSheen = false;
    }

    const bool clearCoatConflict = config->hasVolume || config->hasTransmission || config->hasSheen;

    // Due to sampler overload, disable transmission if necessary and print a friendly warning.
    if (config->hasClearCoat && clearCoatConflict) {
        slog.w << "Volume, transmission and sheen are not supported in ubershader mode for clearcoat"
                  " materials (" << label << ")." << io::endl;
        config->hasVolume = false;
        config->hasTransmission = false;
        config->hasSheen = false;
    }

    constrainMaterial(config, uvmap);
    auto getUvIndex = [uvmap](uint8_t srcIndex, bool hasTexture) -> int {
        return hasTexture ? int(uvmap->at(srcIndex)) - 1 : -1;
    };
    Material* material = getMaterial(*config);
    MaterialInstance* mi = material->createInstance(label);
    mi->setParameter("baseColorIndex",
            getUvIndex(config->baseColorUV, config->hasBaseColorTexture));
    mi->setParameter("normalIndex", getUvIndex(config->normalUV, config->hasNormalTexture));
    mi->setParameter("metallicRoughnessIndex",
            getUvIndex(config->metallicRoughnessUV, config->hasMetallicRoughnessTexture));
    mi->setParameter("aoIndex", getUvIndex(config->aoUV, config->hasOcclusionTexture));
    mi->setParameter("emissiveIndex", getUvIndex(config->emissiveUV, config->hasEmissiveTexture));

    mi->setDoubleSided(config->doubleSided);
    mi->setCullingMode(config->doubleSided ? CullingMode::NONE : CullingMode::BACK);
    mi->setTransparencyMode(config->doubleSided ?
            MaterialInstance::TransparencyMode::TWO_PASSES_TWO_SIDES :
            MaterialInstance::TransparencyMode::DEFAULT);

    #if !GLTFIO_LITE

    // Initially, assume that the clear coat texture can be honored.  This is changed to false when
    // running into a sampler count limitation. TODO: check if these constraints can now be relaxed.
    bool clearCoatNeedsTexture = true;

    mat3f identity;
    mi->setParameter("baseColorUvMatrix", identity);
    mi->setParameter("metallicRoughnessUvMatrix", identity);
    mi->setParameter("normalUvMatrix", identity);
    mi->setParameter("occlusionUvMatrix", identity);
    mi->setParameter("emissiveUvMatrix", identity);

    if (config->hasClearCoat) {
        mi->setParameter("clearCoatIndex",
                getUvIndex(config->clearCoatUV, config->hasClearCoatTexture));
        mi->setParameter("clearCoatRoughnessIndex",
                getUvIndex(config->clearCoatRoughnessUV, config->hasClearCoatRoughnessTexture));
        mi->setParameter("clearCoatNormalIndex",
                getUvIndex(config->clearCoatNormalUV, config->hasClearCoatNormalTexture));
        mi->setParameter("clearCoatUvMatrix", identity);
        mi->setParameter("clearCoatRoughnessUvMatrix", identity);
        mi->setParameter("clearCoatNormalUvMatrix", identity);
    } else {
        if (config->hasSheen) {
            clearCoatNeedsTexture = false;
            mi->setParameter("sheenColorIndex",
                    getUvIndex(config->sheenColorUV, config->hasSheenColorTexture));
            mi->setParameter("sheenRoughnessIndex",
                    getUvIndex(config->sheenRoughnessUV, config->hasSheenRoughnessTexture));
            mi->setParameter("sheenColorUvMatrix", identity);
            mi->setParameter("sheenRoughnessUvMatrix", identity);

        }
        if (config->hasVolume) {
            clearCoatNeedsTexture = false;
            mi->setParameter("volumeThicknessUvMatrix", identity);
            mi->setParameter("volumeThicknessIndex",
                    getUvIndex(config->transmissionUV, config->hasVolumeThicknessTexture));
        }
        if (config->hasTransmission) {
            clearCoatNeedsTexture = false;
            mi->setParameter("transmissionUvMatrix", identity);
            mi->setParameter("transmissionIndex",
                    getUvIndex(config->transmissionUV, config->hasTransmissionTexture));
        }
    }
    #else

    // In the GLTFIO_LITE configuration we do not support UV matrices, clear coat, sheen, specular
    // glossiness, or transmission. For more details, see `gltflite.mat.in`. To configure a custom
    // set of features, create your own MaterialProvider class, perhaps using UbershaderLoader as a
    // starting point.
    const bool clearCoatNeedsTexture = false;

    #endif

    TextureSampler sampler;
    mi->setParameter("normalMap", mDummyTexture, sampler);
    mi->setParameter("baseColorMap", mDummyTexture, sampler);
    mi->setParameter("metallicRoughnessMap", mDummyTexture, sampler);
    mi->setParameter("occlusionMap", mDummyTexture, sampler);
    mi->setParameter("emissiveMap", mDummyTexture, sampler);
    if (clearCoatNeedsTexture) {
        mi->setParameter("clearCoatMap", mDummyTexture, sampler);
        mi->setParameter("clearCoatRoughnessMap", mDummyTexture, sampler);
        mi->setParameter("clearCoatNormalMap", mDummyTexture, sampler);
    }
    if (!config->hasClearCoat) {
        if (config->hasTransmission) {
            mi->setParameter("transmissionMap", mDummyTexture, sampler);
        }
        if (config->hasSheen) {
            mi->setParameter("sheenColorMap", mDummyTexture, sampler);
            mi->setParameter("sheenRoughnessMap", mDummyTexture, sampler);
        }
    }

    if (mi->getMaterial()->hasParameter("ior")) {
        mi->setParameter("ior", 1.5f);
    }
    if (mi->getMaterial()->hasParameter("reflectance")) {
        mi->setParameter("reflectance", 0.5f);
    }

    return mi;
}

} // anonymous namespace

namespace gltfio {

MaterialProvider* createUbershaderLoader(filament::Engine* engine) {
    return new UbershaderLoader(engine);
}

} // namespace gltfio
