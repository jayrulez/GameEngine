// draconic.net.manager - the Net facade PROVEN end-to-end on both script backends.
//
// ManagerTests.cpp checks the facade TYPE registers; this drives a real Wren / AngelScript
// VM: a server manager installs its net.runtime service into the context, then a script calls
// Net.isServer()/isClient()/peerCount() and we read the results back. This is the acceptance test
// for the extensibility hook (RegisterExtraFacadeName -> Wren prelude; registry -> AngelScript).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.net;
import draconic.net.manager;
import draconic.script;
import draconic.script.wren;
import draconic.script.angelscript;

using namespace draconic::foundation;
using namespace draconic::script;
namespace net = draconic::net;

namespace
{
    // A server endpoint over the sim network + a controller the Net facade resolves through. The
    // endpoint is already live here, so the runtime role hooks are no-ops (they are exercised
    // per-instance in the runtime tests); NetEndpoint() hands the facade the live server.
    struct ServerFixture final : net::INetworkController
    {
        net::SimDatagramNetwork network{net::SimConditions{}};
        net::NetworkManager server{*network.CreateSocket()};
        net::NetScriptBinding binding;
        ServerFixture()
        {
            server.StartServer(/*dedicated=*/true);
            binding.controller = this;
        }

        bool StartServer(u16, bool) override { return true; }
        bool Connect(StringView, u16) override { return false; }
        void StopNetworking() override {}
        [[nodiscard]] net::NetworkManager* NetEndpoint() const override
        {
            return const_cast<net::NetworkManager*>(&server);
        }
    };
}

TEST_CASE("net-facade: Wren reads the live session through the Net facade")
{
    RegisterFoundationTypes();
    net::RegisterNetScriptFacade();

    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    ServerFixture fx;
    net::InstallNetScriptService(*ctx, fx.binding);

    // Top-level Wren; the reflected static facade is imported by the (extensible) behavior prelude,
    // but a bare "main" module reaches the class directly since RegisterReflectedTypes emitted it.
    const Status status = ctx->Load(u8"var IsServer = Net.isServer()\n"
                                    u8"var IsClient = Net.isClient()\n"
                                    u8"var Peers = Net.peerCount()\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(ctx->GetGlobal(u8"IsServer").Get<bool>() == true);
    CHECK(ctx->GetGlobal(u8"IsClient").Get<bool>() == false);
    CHECK(ctx->GetGlobal(u8"Peers").Get<f64>() == 0.0); // Wren numbers are doubles
}

TEST_CASE("net-facade: AngelScript reads the live session through the Net facade")
{
    RegisterFoundationTypes();
    net::RegisterNetScriptFacade();

    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    ServerFixture fx;
    net::InstallNetScriptService(*ctx, fx.binding);

    // AngelScript: statics live in the type's namespace (Net::isServer()); logic in main().
    const Status status = ctx->Load(u8"bool IsServer;\n"
                                    u8"bool IsClient;\n"
                                    u8"int Peers;\n"
                                    u8"void main() {\n"
                                    u8"  IsServer = Net::isServer();\n"
                                    u8"  IsClient = Net::isClient();\n"
                                    u8"  Peers = Net::peerCount();\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(ctx->GetGlobal(u8"IsServer").Get<bool>() == true);
    CHECK(ctx->GetGlobal(u8"IsClient").Get<bool>() == false);
    CHECK(ctx->GetGlobal(u8"Peers").Get<f64>() ==
          0.0); // the AS backend unifies integer globals to f64
}
