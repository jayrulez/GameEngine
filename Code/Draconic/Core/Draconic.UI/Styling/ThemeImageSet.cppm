// Draconic UI - :theme_image_set partition
//
// Generic container for theme images, keyed by "styleClass:propertyName". Pass to a textured-theme
// factory to build a fully image-skinned StyleSheet. Ported from the ThemeImageEntry/ThemeImageSet
// portion of Sedulous.UI/src/Styling/TexturedTheme.bf (the TexturedTheme::Create factory is deferred
// until the control style classes exist).
//
// Divergences (language): Beef Dictionary<String, ...> with manual key/value ~delete becomes
// HashMap<String, ...> (RAII); the Beef tuple `(ControlState, String)` state entry becomes a struct;
// GetImages()/GetStateGroups() enumerators become const-ref accessors to the backing maps.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:theme_image_set;

import draconic.foundation;  // HashMap, Array, String, StringView, Optional
import draconic.image; // ImageData, NineSlice
import :control_state;

using namespace draconic::foundation;
namespace image = draconic::image;

export namespace draconic::ui
{
    /// Entry for a single image in a ThemeImageSet.
    struct ThemeImageEntry
    {
        const image::ImageData* Image = nullptr;
        image::NineSlice Slices{};
        bool IsNineSlice = false;
    };

    /// A (state, internal image key) pair within a state group.
    struct ThemeStateEntry
    {
        ControlState State = ControlState::Normal;
        String Key{};
    };

    /// Generic container for theme images, keyed by style property + style class.
    class ThemeImageSet
    {
    public:
        /// Add a single image for a drawable key. Uses 9-slice if slices are non-zero. Null is ignored.
        void AddImage(StringView drawableKey, const image::ImageData* image,
                      image::NineSlice slices = {})
        {
            if (image == nullptr)
            {
                return;
            }
            ThemeImageEntry entry;
            entry.Image = image;
            entry.Slices = slices;
            entry.IsNineSlice = slices.IsValid();
            m_images.InsertOrAssign(String(drawableKey), entry);
        }

        /// Add state-variant images for a drawable key (creates a StateListDrawable).
        void AddStateImages(StringView drawableKey, const image::ImageData* normal,
                            const image::ImageData* hover = nullptr,
                            const image::ImageData* pressed = nullptr,
                            const image::ImageData* disabled = nullptr,
                            const image::ImageData* focused = nullptr, image::NineSlice slices = {})
        {
            Array<ThemeStateEntry> group;

            auto addState = [&](ControlState state, const image::ImageData* img, StringView suffix)
            {
                if (img == nullptr)
                {
                    return;
                }
                String internalKey(drawableKey);
                internalKey += u8"_";
                internalKey += suffix;
                AddImage(internalKey.AsView(), img, slices);
                group.PushBack(ThemeStateEntry{state, Move(internalKey)});
            };

            addState(ControlState::Normal, normal, u8"Normal");
            addState(ControlState::Hover, hover, u8"Hover");
            addState(ControlState::Pressed, pressed, u8"Pressed");
            addState(ControlState::Disabled, disabled, u8"Disabled");
            addState(ControlState::Focused, focused, u8"Focused");

            m_stateGroups.InsertOrAssign(String(drawableKey), Move(group));
        }

        /// All image entries (keyed "styleClass:propertyName").
        [[nodiscard]] const HashMap<String, ThemeImageEntry>& GetImages() const { return m_images; }

        /// All state groups (keyed "styleClass:propertyName").
        [[nodiscard]] const HashMap<String, Array<ThemeStateEntry>>& GetStateGroups() const
        {
            return m_stateGroups;
        }

        /// Get a single image entry by key.
        [[nodiscard]] Optional<ThemeImageEntry> GetEntry(StringView key) const
        {
            if (const ThemeImageEntry* entry = m_images.Find(String(key)))
            {
                return *entry;
            }
            return {};
        }

    private:
        HashMap<String, ThemeImageEntry> m_images;
        HashMap<String, Array<ThemeStateEntry>> m_stateGroups;
    };
}
