// Draconic::SceneEditor - the `draconic.scene.editor` module (tooling).
//
// The scene SAVE / cook path: capture a live Scene into a content-DB instance (a
// SceneDocument primary + the serialized world in the "scene" data stream), using the
// same bidirectional SerializeScene the runtime loads with. For scenes the cooked file
// IS the authored form, so "save" is the editor-side operation (the mirror of the
// runtime LoadScene). Never linked by the runtime.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.scene.editor;

import draconic.foundation;
import draconic.content;
import draconic.scene;
import draconic.scene.resource;
import draconic.xml.serialization;

using namespace draconic::foundation;

export namespace draconic::scene
{

    // Captures `scene` into `instance`: writes the SceneDocument primary (name) so the
    // instance materializes + is discoverable, then the serialized world into the "scene"
    // data stream. Round-trips with LoadScene.
    inline Status SaveScene(Scene& scene, draconic::content::Instance& instance)
    {
        SceneDocument doc;
        doc.name = String(scene.Name());
        const Status wrote = instance.WriteObject(doc);
        if (!wrote.IsOk())
        {
            return wrote;
        }

        // Sources are TEXT (docs/design/text-scenes.md): diffable, mergeable, hand-editable.
        // Export staging transcodes to the binary wire for the player.
        draconic::xml::XmlSerializer ser;
        SerializeScene(ser, scene, nullptr, ScenePrefabMode::Referenced, true,
                       detail::SceneStreamEncoding::Text);
        if (!ser.IsOk())
        {
            return ser.GetStatus();
        }
        String text;
        ser.GetOutput(text);
        return instance.WriteData(
            u8"scene", Span<const byte>{reinterpret_cast<const byte*>(text.CStr()), text.Size()});
    }

    // The prefab twin: PrefabDocument primary + the world serialized EXPANDED - any nested
    // prefab instances flatten into plain entities (nesting is P4; expansion degrades it to
    // baked members instead of silently dropping them), and the stream doubles as the spawn
    // payload AND the edit page's load stream (both read plain entity records).
    inline Status SavePrefab(Scene& scene, draconic::content::Instance& instance)
    {
        // A prefab is a single-rooted subtree; refuse a multi-root layout instead of saving a
        // template whose apply/capture path would silently drop the sibling roots.
        usize rootCount = 0;
        for (EntityHandle r = scene.GetFirstRoot(); r.IsAssigned(); r = scene.GetNextSibling(r))
        {
            ++rootCount;
        }
        if (rootCount > 1)
        {
            return Status{ErrorCode::InvalidArgument};
        }

        // Self-nesting guard: a prefab whose edit scene contains an instance of ITSELF would
        // reference itself forever.
        bool selfReference = false;
        scene.ForEachPrefabInstance(
            [&](Scene::PrefabInstanceState& state)
            {
                if (state.prefabId == instance.Id())
                {
                    selfReference = true;
                }
            });
        if (selfReference)
        {
            return Status{ErrorCode::InvalidArgument};
        }

        PrefabDocument doc;
        doc.name = String(scene.Name());
        const Status wrote = instance.WriteObject(doc);
        if (!wrote.IsOk())
        {
            return wrote;
        }

        // Referenced + no settings (P4): nested instances persist as ref+delta RECORDS - the
        // flat forest SpawnPrefab replays - instead of flattening into plain entities. TEXT
        // like scene saves (export transcodes).
        draconic::xml::XmlSerializer ser;
        SerializeScene(ser, scene, nullptr, ScenePrefabMode::Referenced, /*includeSettings=*/false,
                       detail::SceneStreamEncoding::Text);
        if (!ser.IsOk())
        {
            return ser.GetStatus();
        }
        String text;
        ser.GetOutput(text);
        return instance.WriteData(
            u8"scene", Span<const byte>{reinterpret_cast<const byte*>(text.CStr()), text.Size()});
    }

} // namespace draconic::scene
