// engine.net - the network's SceneModule contributes the NetworkComponentManager to the scene
// composition, so authored NetworkComponents (and the server's runtime AssignNetworkId) have a home.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.net.replication; // AddNetworkSceneManagers ("net.Network" identity)
import engine.net;

using namespace foundation::core;
namespace scene = foundation::scene;

TEST_CASE("net-subsystem: the scene module injects the NetworkComponentManager")
{
    foundation::net::RegisterReplicationComponents();

    scene::Scene scene;

    // No net managers until the composition installs them (a bare scene is not networked).
    CHECK(scene.FindManagerBySerializationId(u8"net.Network") == nullptr);
    CHECK(scene.FindManagerBySerializationId(u8"net.Transform") == nullptr);

    foundation::net::AddNetworkSceneManagers(scene); // the composition's net module install

    // Now an authored/assigned NetworkComponent (identity) + NetworkedTransform (replicated movement)
    // both have a home in this scene.
    CHECK(scene.FindManagerBySerializationId(u8"net.Network") != nullptr);
    CHECK(scene.FindManagerBySerializationId(u8"net.Transform") != nullptr);
}