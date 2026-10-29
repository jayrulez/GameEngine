# Networking

> Status: CURRENT
> Verified: 2026-08-12 @ 9c9046f8
> Track: [[networking-track]] / [[game-instance-track]]

Reflection-driven, server-authoritative networking, shipped end to end (P0-P2) and wired into
`DefaultApplication` per game instance. A game sets a role, marks component fields `Replicated`,
and gets authoritative, fog-of-war-filtered, interpolated replication with prefab net-spawn +
late-join - no hand-written per-component net code. First target is real-time strategy; the
`IReplicationModel` seam keeps other genres open.

## Modules

- **`foundation.net`** - transport: reliable-UDP + TCP + loopback/sim + the wire codec. Raw
  sockets live in `Core/System` (the platform backend pattern - `System.cppm` + per-OS dirs),
  not here; `foundation.net` is the reliability/session layer above them. Hosts are a
  dotted-quad literal OR a DNS name everywhere a host is taken (`ResolveEndpoint`,
  `TcpSocket::Connect`, `Net.connect`, `HttpFetch`): `Core::ResolveHostIPv4` parses a literal
  without a lookup and otherwise blocks on the platform resolver (getaddrinfo, first A
  record) - resolve at the connect edge, never per frame. IPv4 only; IPv6 and TLS deferred.
- **`foundation.http`** - the sibling HTTP/1.1 layer over the TCP wrappers: incremental message
  parser, a pump-model `HttpServer` (one request per connection, `Connection: close`) with
  Server-Sent Events streams (`SseStream`, ref-counted, held past the response), and a blocking
  localhost `HttpFetch` client (no DNS, no TLS - the localhost trust domain). Consumers: the MCP
  streamable-HTTP host, the WebSocket upgrade path (a WS connection begins as an HTTP request -
  this server is the web-networking server's front door), loopback tooling/tests.
- **`foundation.net.manager`** - `NetworkManager` (a session endpoint) + `INetworkController` +
  the `Net` script-facade binding. A GameInstance owns its OWN networking and goes online at
  RUNTIME via the facade (no app-owned socket). The endpoint + role lifecycle + `INetworkController`
  impl live on `engine.runtime::NetworkController` (`engine.gameinstance`), which the GameInstance
  COMPOSES and forwards to (networking-extraction.md P1; GameInstance no longer inherits
  `INetworkController`). Later phases move the per-frame drive onto the standard lanes.
- **`foundation.net.replication`** - `StateReplication` + the `IReplicationModel` seam.
- **`engine.net`** - the net scene-integration: `NetworkSubsystem` (registers the reflected components)
  plus `NetworkSceneSystem` and the engine-level `AddNetworkSceneManagers` installer (the net
  `SceneModule` - [[scene-composition]]) that installs the `NetworkComponentManager` + the scene system.
  `NetworkSceneSystem::OnFixedUpdate` drives the endpoint's `UpdateReplication` on the per-scene FIXED
  lane (deterministic, physics-lockstep - networking-extraction.md P2); the per-instance
  `NetworkController` points it at the live endpoint only for the endpoint's current replicated scene.
  The subsystem also OWNS the per-frame TRANSPORT pump: `NetworkSubsystem::PostUpdate` visits every live
  endpoint (via an app-provided source) and drives `UpdateTransport` (socket recv/send) on the Context
  lane (networking-extraction.md P3) - replacing the app's former `OnFixedUpdate` `DriveNetwork` fan-out
  (deleted, including the editor's embedded mirror). `NetworkManager::Update` is split into
  `UpdateTransport` (per-frame) and `UpdateReplication` (per-scene fixed lane).

## Roles + startup

`NetworkStartup` carries the role (`NetworkRole` None/Server/Client) + preset; the app's role-preset
startup is a thin call into the instance's controller. Per-instance: each `GameInstance` has its own
endpoint and goes online at runtime via the `Net` facade (`Net.startServer` / `Net.connect`), so a
single process can host multiple instances. The prefab net-spawn resolver is injected once as a factory
onto each instance's `NetworkController` (`Network().SetSpawnResolverFactory` - networking-extraction.md
P4); the controller makes one per endpoint and installs it, so reconnect keeps it. The app provides only
the content-DB-backed factory (`MakeSpawnResolver`); it no longer wires the endpoint itself.

## Replication (`StateReplication`)

Mark component fields `Replicated` (a reflection attribute) and the model does the rest:
- the server builds a snapshot and a PER-PEER delta (reflection-driven field codec);
- **prefab net-spawn** replicates spawned entities; a joining client gets a full-snapshot
  **late-join** sync (first-class for this target - reconnect brings full state);
- **a joiner may wait for its map** (`NetworkManager::SetWaitForReady`, `Net.waitForReady(true)` on
  the host): a client that loads the host's map after connecting hears no state, only RPCs (the
  host's welcome naming the map), until it says it is ready (`SendReady`, `Net.ready()`, a byte on
  the replication channel, the one thing a client sends there); the host then forgets its
  baseline, so its first delta is the full snapshot. Off by default: state flows at once;
- clients **interpolate** replicated state for smoothness;
- **per-peer relevancy / fog-of-war** filters what each client sees - interest management is a
  SECURITY requirement here (a client never receives what it should not see), not just bandwidth.

No client prediction / reconciliation / lag compensation and no lockstep determinism are needed
for the current target (server-authoritative), which is what shipped.

## The model seam (`IReplicationModel`)

`StateReplication` is one model behind the `IReplicationModel` seam - the escape hatch that keeps
the design multi-genre without disturbing what shipped. A twitch-shooter unreliable-snapshot+ack
model, or a large-scale-RTS lockstep `CommandReplication`, can slot BESIDE it. Target-specific
choices (e.g. the reliable-ordered delta baseline) are POLICY of the current send path, not baked
into the wire format or the seam. A future lockstep `CommandReplication` would re-raise the
determinism caveat (lockstep needs bit-exact simulation - not promised by state replication).

## Script facade

The `Net` facade (registered by `RegisterNetScriptFacade`, resolved per script context through
the instance's `NetworkManager`): `Net.isServer()` / `isClient()` / `peerCount()` /
`startServer(...)` / `connect(...)`, `Net.canHost()` (false in a browser, which cannot listen, so a
title offers Host only where it works; `NetworkManager::CanHost`), plus SEND-side RPC (`Net.rpc(...)` / `rpcNumber(...)` /
`rpcText(...)`), and the RECEIVE side, `Net.on(name, fn)` (P3 slice 1, built 2026-10-14): the
handler is called with the sender's peer id, then the number or the text, if any. The sends write
a one-byte kind (`ScriptRpcKind`) after the RPC id so the receiver knows what follows. Handlers
live in the instance's `NetScriptBinding`, so a script registers them before it hosts or joins and
every endpoint the controller opens gets them (`InstallScriptRpcHandlers`); AngelScript gets the
handler types `NetHandler`, `NetNumberHandler` and `NetTextHandler` through the script layer's
delegate shapes (`RegisterDelegateShape`). `Net.spawn(prefab, x, y, z)` (P3 slice 2, built
2026-10-14), host only: spawns the prefab in the replicated scene through the scene's own
`PrefabSpawnSystem` (as `scene.spawn`) and assigns its NetworkId with the prefab, so each client's
spawn handler builds its copy and a late joiner gets it in the snapshot; null on a client.
Players (P3 slice 3, built 2026-10-14): a script names a player by an id, 0 the host and a client
the PeerId the host gave it, which the session's `Welcome` control message (sent reliably as the
host accepts) tells the client (`NetSession::LocalPeer`). `NetworkComponent.owner` is that id,
set by `Net.spawnFor(player, prefab, x, y, z)` (`spawn` is the host's) and sent in the spawn
record after the prefab, so every peer knows whose an entity is (`Net.isMine(entity)`,
`NetworkComponent.of(e).owner`). `Net.localPlayer()`; `Net.rpcTo` / `rpcNumberTo` / `rpcTextTo`,
host to one player; `Net.onJoin(fn(player))` / `Net.onLeave(fn(player))` through
`NetworkManager::SetPlayerEvents` (the host hears clients come and go; a client hears its own
join once welcomed, and the host leave as 0). On a client a `Net.on` handler's sender is 0, the
host. Orders are RPCs the host checks itself (it knows the sender); who leaves and what happens to
their entities is the game's call in its `onLeave`.
Script behaviour properties replicate when marked (Masquerade P1, built 2026-10-14):
`ScriptPropertyDesc::replicated`, from AngelScript's `replicated` metadata word or a Luau class's
`replicated` list (the cook refuses an unknown word or name, and an entity or asset property).
Replication reaches them through `scene::IReplicatedStateSource`, a component manager's own state
beside reflected `net.replicated` fields: `ScriptComponentManager` writes each networked entity's
replicated properties (live from the script object on the host), and a client writes them into
its behaviour (an override, plus the setter when the instance runs). The blob is compared with each
peer's last, so only a change is sent.
`NetworkComponent.of(entity).authority` exposes
ownership to script. Certified on AngelScript + Luau (see `Integration.ScriptFacades`; Wren was
retired 2026-08-19).

## Deferred

- **P3 - commands/orders layer** (script-facing: receive-RPC-into-script, spawn-a-networked-entity
  from script, ownership + orders, demo content): slices 1 to 3 (`Net.on`, `Net.spawn`, players and owners) built 2026-10-14, the demo slice is
  progress for Masquerade (`Specs/masquerade.md`, P0). Plan: `Documentation/Plans/networking-commands.md`.
- Backlog (web WebSocket client, replication refinements, transport/deployment, matchmaking/
  services hooks): `Documentation/Backlog/networking-followups.md`.
- **Beef port / unification with the legacy Sedulous.Net**: RULED 2026-09-09 - Raptor's stack is
  the foundation and the new Sedulous ports it directly; the DNS gap on our side is closed
  (above). `Documentation/Plans/net-unification.md`.

---

Design rationale (roll-your-own transport + the escape hatch, sockets-in-Core, HTTP-as-sibling,
the genre-tuning analysis, the Traktor references) is in
`Documentation/Archive/networking-design-history.md`.
