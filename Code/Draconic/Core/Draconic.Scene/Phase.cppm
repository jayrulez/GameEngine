/// Draconic::Scene - the `:phase` partition.
///
/// ScenePhase: the ordered slots a scene runs each update. Order is the contract -
/// systems that depend on each other rely on it (input/physics readback before
/// gameplay; gameplay before async work; everything before the transform recompute;
/// render/spatial extraction after). TransformUpdate is internal (the Scene drives
/// the hierarchy itself); systems register into the others.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.scene:phase;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::scene
{

    enum class ScenePhase : u8
    {
        Initialize,      // run pending component initialization (deferred from Add)
        PreUpdate,       // physics readback, input application
        Update,          // main gameplay / AI (sequential; cross-component reads safe)
        AsyncUpdate,     // parallel per-system; a system may touch only its own data
        PostUpdate,      // animation, constraints, late logic
        TransformUpdate, // (internal) dirty transform propagation - Scene-driven
        PostTransform,   // render extraction, spatial index update (final transforms ready)
        Cleanup,         // deferred destruction settles here
        Count,
    };

} // namespace draconic::scene
