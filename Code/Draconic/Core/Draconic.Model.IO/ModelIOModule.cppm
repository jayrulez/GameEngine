/// Model IO - loader abstraction and registry.
/// Format-specific loaders (GLTF, FBX) register here.
/// Callers use loadModel(path, model) which selects the right loader by extension.

module;
#include "Draconic.Foundation/Prelude.h"

#include <cstring>
#include <vector>

export module draconic.model.io;

import draconic.foundation;
import draconic.model;

using namespace draconic::foundation;

export namespace draconic::model::io
{

    /// Abstract base for format-specific model loaders.
    class ModelLoader
    {
    public:
        virtual ~ModelLoader() = default;

        /// Check if this loader supports the given file extension (e.g. ".gltf").
        [[nodiscard]] virtual bool supportsExtension(StringView ext) const = 0;

        /// Load a model from a file path.
        virtual ModelLoadResult load(StringView path, Model& model) = 0;
    };

    /// Register a model loader. Does not take ownership.
    void registerLoader(ModelLoader* loader);

    /// Unregister a model loader.
    void unregisterLoader(ModelLoader* loader);

    /// Load a model from a file, selecting the appropriate loader by extension.
    /// Returns UnsupportedFormat if no loader is registered for the extension.
    ModelLoadResult loadModel(StringView path, Model& model);

    /// Check if any loaders are registered.
    [[nodiscard]] bool hasLoaders();

    // ---- Implementation ----

    namespace detail
    {
        inline Array<ModelLoader*>& loaders()
        {
            static Array<ModelLoader*> s;
            return s;
        }

        inline StringView getExtension(StringView path)
        {
            for (usize i = path.Size(); i > 0; --i)
            {
                if (path.Data()[i - 1] == '.')
                    return StringView(path.Data() + i - 1, path.Size() - i + 1);
                if (path.Data()[i - 1] == '/' || path.Data()[i - 1] == '\\')
                    break;
            }
            return {};
        }
    }

    inline void registerLoader(ModelLoader* loader)
    {
        if (!loader)
            return;
        auto& v = detail::loaders();
        for (auto* l : v)
            if (l == loader)
                return;
        v.PushBack(loader);
    }

    inline void unregisterLoader(ModelLoader* loader)
    {
        auto& v = detail::loaders();
        for (usize i = 0; i < v.Size(); ++i)
        {
            if (v[i] == loader)
            {
                v.RemoveAt(i);
                return;
            }
        }
    }

    inline ModelLoadResult loadModel(StringView path, Model& model)
    {
        StringView ext = detail::getExtension(path);
        for (auto* loader : detail::loaders())
        {
            if (loader->supportsExtension(ext))
                return loader->load(path, model);
        }
        return ModelLoadResult::UnsupportedFormat;
    }

    inline bool hasLoaders() { return !detail::loaders().IsEmpty(); }

} // namespace draconic::model::io
