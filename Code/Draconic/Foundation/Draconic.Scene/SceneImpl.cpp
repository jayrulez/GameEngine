// Draconic Scene - draconic.scene:scene implementation unit.
//
// Out-of-line definitions for Scene's non-trivial member functions (sec 3.2 / sec 10.6).
// The class declaration + trivial inline accessors stay in Scene.cppm; the substantial
// method bodies live here so the interface partition stays small.

module;
#include "Draconic.Foundation/Prelude.h"
#include <type_traits>

module draconic.scene;

import draconic.foundation;

using namespace draconic::foundation;

namespace draconic::scene
{
    EntityHandle Scene::CreateEntity(StringView name)
    {
        // The RNG is deterministic and loading a scene does NOT advance it past the loaded
        // entities, so a fresh id can reproduce a loaded one - re-roll until free (the same
        // collision the content DB re-rolls; three entities sharing a guid corrupts saves).
        Guid id = Guid::Generate(m_rng);
        while (FindEntity(id).IsAssigned())
        {
            id = Guid::Generate(m_rng);
        }
        return CreateEntityInternal(id, name);
    }

    EntityHandle Scene::CreateEntity(const Guid& id, StringView name)
    {
        return CreateEntityInternal(id, name);
    }

    void Scene::DestroyEntity(EntityHandle entity)
    {
        if (!IsValid(entity))
        {
            return;
        }
        if (m_isUpdating)
        {
            m_pendingDestroys.PushBack(entity);
            return;
        }
        DestroyEntityImmediate(entity);
    }

    bool Scene::IsValid(EntityHandle entity) const noexcept
    {
        if (!entity.IsAssigned() || entity.index >= m_entities.Size())
        {
            return false;
        }
        const EntitySlot& slot = m_entities[entity.index];
        return slot.alive && slot.generation == entity.generation;
    }

    Guid Scene::GetEntityId(EntityHandle entity) const
    {
        return IsValid(entity) ? m_entities[entity.index].persistentId : Guid{};
    }

    EntityHandle Scene::FindEntity(const Guid& id)
    {
        EntityHandle* found = m_idMap.Find(id);
        if (found == nullptr)
        {
            return EntityHandle::Invalid();
        }
        if (IsValid(*found))
        {
            return *found;
        }
        m_idMap.Remove(id);
        return EntityHandle::Invalid();
    }

    StringView Scene::GetEntityName(EntityHandle entity) const
    {
        return IsValid(entity) ? m_entities[entity.index].name.AsView() : StringView{};
    }

    void Scene::SetEntityName(EntityHandle entity, StringView name)
    {
        if (!IsValid(entity))
        {
            return;
        }
        m_entities[entity.index].name = String(name);
        ++m_revision;
    }

    bool Scene::IsActive(EntityHandle entity) const noexcept
    {
        return IsValid(entity) && m_entities[entity.index].active;
    }

    void Scene::SetActive(EntityHandle entity, bool active)
    {
        if (!IsValid(entity))
        {
            return;
        }
        if (m_entities[entity.index].active == active)
        {
            return;
        }
        m_entities[entity.index].active = active;
        ++m_revision;
        for (SceneSystem* s : m_sortedSystems)
        {
            s->OnEntityActiveChanged(entity, active);
        }
    }

    void Scene::AddPrefabInstance(UniquePtr<PrefabInstanceState> state)
    {
        if (state)
        {
            m_prefabInstances.PushBack(static_cast<UniquePtr<PrefabInstanceState>&&>(state));
        }
    }

    Scene::PrefabInstanceState* Scene::FindPrefabInstanceByRoot(const Guid& rootEntityId)
    {
        for (auto& s : m_prefabInstances)
        {
            if (s->rootEntityId == rootEntityId)
            {
                return s.Get();
            }
        }
        return nullptr;
    }

    void Scene::RemovePrefabInstance(const Guid& rootEntityId)
    {
        for (usize i = 0; i < m_prefabInstances.Size(); ++i)
        {
            if (m_prefabInstances[i]->rootEntityId == rootEntityId)
            {
                m_prefabInstances.RemoveAt(i);
                return;
            }
        }
    }

    usize Scene::PrefabInstanceCount() const noexcept { return m_prefabInstances.Size(); }

    void Scene::AddPendingPrefabInstance(UniquePtr<PendingPrefabInstance> pending)
    {
        if (pending)
        {
            m_pendingPrefabs.PushBack(static_cast<UniquePtr<PendingPrefabInstance>&&>(pending));
        }
    }

    Array<UniquePtr<Scene::PendingPrefabInstance>> Scene::TakePendingPrefabInstances()
    {
        Array<UniquePtr<PendingPrefabInstance>> out =
            static_cast<Array<UniquePtr<PendingPrefabInstance>>&&>(m_pendingPrefabs);
        m_pendingPrefabs = Array<UniquePtr<PendingPrefabInstance>>{};
        return out;
    }

    usize Scene::PendingPrefabInstanceCount() const noexcept { return m_pendingPrefabs.Size(); }

    void Scene::ForEachPendingPrefabInstance(const Function<void(PendingPrefabInstance&)>& fn)
    {
        for (const UniquePtr<PendingPrefabInstance>& p : m_pendingPrefabs)
        {
            if (p)
            {
                fn(*p);
            }
        }
    }

    void Scene::SetLocalTransform(EntityHandle entity, const Transform& transform)
    {
        if (!IsValid(entity))
        {
            return;
        }
        m_transforms[entity.index].local = transform;
        MarkDirty(entity);
    }

    Transform Scene::GetLocalTransform(EntityHandle entity) const
    {
        return IsValid(entity) ? m_transforms[entity.index].local : Transform{};
    }

    void Scene::SetLocalPosition(EntityHandle entity, Float3 position)
    {
        if (!IsValid(entity))
        {
            return;
        }
        m_transforms[entity.index].local.position = position;
        MarkDirty(entity);
    }

    Float4x4 Scene::GetWorldMatrix(EntityHandle entity) const
    {
        return IsValid(entity) ? m_transforms[entity.index].worldMatrix : Float4x4::Identity();
    }

    Float4x4 Scene::GetPrevWorldMatrix(EntityHandle entity) const
    {
        return IsValid(entity) ? m_transforms[entity.index].prevWorldMatrix : Float4x4::Identity();
    }

    Float3 Scene::GetWorldPosition(EntityHandle entity) const
    {
        const Float4x4 w = GetWorldMatrix(entity);
        return Float3{w.m[3][0], w.m[3][1], w.m[3][2]};
    }

    bool Scene::IsTransformUpdatedThisFrame(EntityHandle entity) const
    {
        return IsValid(entity) && m_transforms[entity.index].updatedThisFrame;
    }

    EntityHandle Scene::GetParent(EntityHandle entity) const
    {
        return IsValid(entity) ? m_transforms[entity.index].parent : EntityHandle::Invalid();
    }

    EntityHandle Scene::GetFirstChild(EntityHandle entity) const
    {
        return IsValid(entity) ? m_transforms[entity.index].firstChild : EntityHandle::Invalid();
    }

    EntityHandle Scene::GetNextSibling(EntityHandle entity) const
    {
        return IsValid(entity) ? m_transforms[entity.index].nextSibling : EntityHandle::Invalid();
    }

    u32 Scene::GetChildCount(EntityHandle entity) const
    {
        if (!IsValid(entity))
        {
            return 0;
        }
        u32 count = 0;
        EntityHandle child = m_transforms[entity.index].firstChild;
        while (child.IsAssigned() && IsValid(child))
        {
            ++count;
            child = m_transforms[child.index].nextSibling;
        }
        return count;
    }

    EntityHandle Scene::FindEntityByName(StringView name) const
    {
        for (u32 i = 0; i < m_entities.Size(); ++i)
        {
            if (m_entities[i].alive && m_entities[i].name.AsView() == name)
            {
                return EntityHandle{i, m_entities[i].generation};
            }
        }
        return EntityHandle::Invalid();
    }

    EntityHandle Scene::FindChildByName(EntityHandle parent, StringView name) const
    {
        EntityHandle child = parent.IsAssigned() ? GetFirstChild(parent) : GetFirstRoot();
        while (child.IsAssigned() && IsValid(child))
        {
            if (GetEntityName(child) == name)
            {
                return child;
            }
            child = GetNextSibling(child);
        }
        return EntityHandle::Invalid();
    }

    EntityHandle Scene::FindEntityByPath(StringView path) const
    {
        EntityHandle current = EntityHandle::Invalid(); // start at the roots
        bool matchedAny = false;
        usize begin = 0;
        for (usize i = 0; i <= path.Size(); ++i)
        {
            if (i != path.Size() && path[i] != u8'/')
            {
                continue;
            }
            const StringView segment = path.SubStr(begin, i - begin);
            begin = i + 1;
            if (segment.IsEmpty())
            {
                continue;
            } // tolerate // and leading/trailing /
            current = FindChildByName(current, segment);
            if (!current.IsAssigned())
            {
                return EntityHandle::Invalid();
            }
            matchedAny = true;
        }
        return matchedAny ? current : EntityHandle::Invalid();
    }

    void Scene::SetParent(EntityHandle child, EntityHandle parent)
    {
        if (!IsValid(child))
        {
            return;
        }
        if (parent.IsAssigned() && !IsValid(parent))
        {
            return;
        }
        if (child == parent)
        {
            return;
        }
        if (parent.IsAssigned() && IsDescendantOf(parent, child))
        {
            return;
        } // cycle guard

        RemoveFromParent(child);
        m_transforms[child.index].parent = parent;
        if (parent.IsAssigned())
        {
            TransformData& p = m_transforms[parent.index];
            AppendToList(child, p.firstChild, p.lastChild);
        }
        else
        {
            AppendToList(child, m_firstRoot, m_lastRoot);
        }
        MarkDirty(child);
        ++m_revision;
    }

    void Scene::SetParent(EntityHandle child, EntityHandle parent, bool keepWorldTransform)
    {
        if (!keepWorldTransform)
        {
            SetParent(child, parent);
            return;
        }
        if (!IsValid(child))
        {
            return;
        }
        const Float4x4 childWorld = ComposeWorldMatrix(child);
        const u64 before = m_revision;
        SetParent(child, parent);
        if (m_revision != before)
        {
            ApplyWorldAsLocal(child, childWorld);
        }
    }

    void Scene::MoveBefore(EntityHandle child, EntityHandle sibling, bool keepWorldTransform)
    {
        if (!keepWorldTransform)
        {
            MoveBefore(child, sibling);
            return;
        }
        if (!IsValid(child))
        {
            return;
        }
        const Float4x4 childWorld = ComposeWorldMatrix(child);
        const u64 before = m_revision;
        MoveBefore(child, sibling);
        if (m_revision != before)
        {
            ApplyWorldAsLocal(child, childWorld);
        }
    }

    Float4x4 Scene::ComposeWorldMatrix(EntityHandle entity) const
    {
        Float4x4 world = Float4x4::Identity();
        for (EntityHandle e = entity; IsValid(e); e = m_transforms[e.index].parent)
        {
            world = world * m_transforms[e.index].local.ToMatrix();
        }
        return world;
    }

    void Scene::MoveBefore(EntityHandle child, EntityHandle sibling)
    {
        if (!IsValid(child) || !IsValid(sibling))
        {
            return;
        }
        if (child == sibling)
        {
            return;
        }
        if (m_transforms[sibling.index].prevSibling == child)
        {
            return;
        } // already there
        const EntityHandle parent = m_transforms[sibling.index].parent;
        if (parent.IsAssigned() && IsDescendantOf(parent, child))
        {
            return;
        } // cycle guard

        RemoveFromParent(child);
        TransformData& c = m_transforms[child.index];
        TransformData& s = m_transforms[sibling.index];
        c.parent = parent;
        c.nextSibling = sibling;
        c.prevSibling = s.prevSibling;
        if (s.prevSibling.IsAssigned())
        {
            m_transforms[s.prevSibling.index].nextSibling = child;
        }
        else if (parent.IsAssigned())
        {
            m_transforms[parent.index].firstChild = child;
        }
        else
        {
            m_firstRoot = child;
        }
        s.prevSibling = child;
        MarkDirty(child);
        ++m_revision;
    }

    void Scene::UpdateTransforms()
    {
        const u32 count = static_cast<u32>(m_transforms.Size());
        if (count == 0)
        {
            return;
        }

        for (u32 idx : m_transformsUpdatedThisFrame)
        {
            if (idx >= m_transforms.Size())
            {
                continue;
            }
            TransformData& d = m_transforms[idx];
            d.updatedThisFrame = false;
            if (!d.dirty && m_entities[idx].alive)
            {
                d.prevWorldMatrix = d.worldMatrix;
            }
        }
        m_transformsUpdatedThisFrame.Clear();

        // Recurse from every dirty-subtree TOP: a dirty entity whose parent is clean (or who
        // has none). MarkDirty propagates down, so interior dirty nodes always have a dirty
        // parent - but a freshly REPARENTED entity under a clean parent is a top that the old
        // roots-only scan missed (its world matrix stayed stale until something moved the
        // parent; pasted/duplicated children rendered at the origin).
        for (u32 i = 0; i < count; ++i)
        {
            const TransformData& d = m_transforms[i];
            if (!d.dirty || !m_entities[i].alive)
            {
                continue;
            }
            if (!d.parent.IsAssigned())
            {
                UpdateTransformRecursive(i, Float4x4::Identity());
            }
            else if (!m_transforms[d.parent.index].dirty)
            {
                // The parent is clean, so its cached world matrix is current.
                UpdateTransformRecursive(i, m_transforms[d.parent.index].worldMatrix);
            }
        }
    }

    Span<const u32> Scene::TransformsUpdatedThisFrame() const noexcept
    {
        return {m_transformsUpdatedThisFrame.Data(), m_transformsUpdatedThisFrame.Size()};
    }

    ComponentManagerBase* Scene::FindManagerBySerializationId(StringView typeId)
    {
        for (SceneSystem* s : m_sortedSystems)
        {
            if (ComponentManagerBase* m = s->AsComponentManager())
            {
                if (m->IsSerializable() && m->SerializationTypeId() == typeId)
                {
                    return m;
                }
            }
        }
        return nullptr;
    }

    namespace
    {
        // Inline resolve context for a component ref - fits Variant's 24-byte inline storage. The
        // manager pointer is scene-owned and pool-stable; the component itself is NEVER cached here
        // (the sparse-set pool swap-removes), only re-looked-up per access.
        struct ComponentRefCtx
        {
            Scene* scene;
            EntityHandle entity;
            ComponentManagerBase* manager;
        };

        // The resolver a RESOLVE-mode Variant calls each deref: the CURRENT component address, or
        // null (component absent / entity stale - the manager's generation check handles staleness).
        void* ResolveComponent(const Variant& value)
        {
            const auto* ctx = static_cast<const ComponentRefCtx*>(value.ResolveContext());
            return (ctx->manager != nullptr)
                       ? ctx->manager->GetComponentInstance(ctx->entity).Pointer()
                       : nullptr;
        }
    }

    Variant Scene::MakeComponentRef(EntityHandle entity, const TypeInfo& componentType)
    {
        ComponentManagerBase* manager = FindManagerByComponentType(componentType);
        if (manager == nullptr)
        {
            return Variant{};
        }
        return Variant::Resolving(&componentType, &ResolveComponent,
                                  ComponentRefCtx{this, entity, manager});
    }

    void Scene::SetFixedTiming(f32 step, u32 maxSteps) noexcept
    {
        m_stepper.step = step;
        m_stepper.maxSteps = maxSteps;
    }

    u32 Scene::AdvanceTime(f32 scaledDelta)
    {
        const u32 steps = m_stepper.Advance(scaledDelta);
        for (u32 i = 0; i < steps; ++i)
        {
            FixedUpdate(m_stepper.step);
        }
        m_fixedAlpha = m_stepper.Alpha();
        return steps;
    }

    void Scene::Start()
    {
        if (m_started)
        {
            return;
        }
        // Authored locals -> world matrices BEFORE systems hear OnSceneStarted: world
        // matrices are Identity until the first update, and start callbacks that sample
        // them (physics body building, spawn points) must see the authored layout.
        UpdateTransforms();
        m_started = true;
        m_simulationEnabled = true;
        for (SceneSystem* s : m_sortedSystems)
        {
            s->OnSceneStarted();
        }
    }

    void Scene::Stop()
    {
        if (!m_started)
        {
            return;
        }
        for (SceneSystem* s : m_sortedSystems)
        {
            s->OnSceneStopped();
        }
        m_started = false;
    }

    void Scene::Update(f32 deltaTime)
    {
        m_isUpdating = true;
        InitializePendingComponents(); // ScenePhase::Initialize
        RunPhase(ScenePhase::PreUpdate, deltaTime);
        RunPhase(ScenePhase::Update, deltaTime);
        RunPhase(ScenePhase::AsyncUpdate, deltaTime);
        RunPhase(ScenePhase::PostUpdate, deltaTime);
        // Deliver this frame's events at the tick top level (no VM call active - the script bridge's
        // handlers run here safely), before transforms so a handler that moves an entity is reflected.
        m_events.Drain();
        UpdateTransforms(); // ScenePhase::TransformUpdate (internal)
        RunPhase(ScenePhase::PostTransform, deltaTime);
        m_isUpdating = false;
        ProcessPendingDestroys(); // ScenePhase::Cleanup
    }

    void Scene::FixedUpdate(f32 fixedDeltaTime)
    {
        for (SceneSystem* s : m_sortedSystems)
        {
            if (s->IsSimulationOnly() && !m_simulationEnabled)
            {
                continue;
            }
            s->OnFixedUpdate(fixedDeltaTime);
        }
    }

    void Scene::InitializePendingComponents()
    {
        for (SceneSystem* s : m_sortedSystems)
        {
            if (ComponentManagerBase* mgr = s->AsComponentManager())
            {
                mgr->InitializePendingComponents();
            }
        }
    }

    EntityHandle Scene::CreateEntityInternal(const Guid& id, StringView name)
    {
        u32 index;
        if (!m_freeList.IsEmpty())
        {
            index = m_freeList.Back();
            m_freeList.PopBack();
        }
        else
        {
            index = static_cast<u32>(m_entities.Size());
            m_entities.PushBack(EntitySlot{});
            m_transforms.PushBack(TransformData{});
        }

        EntitySlot& slot = m_entities[index];
        ++slot.generation;
        slot.alive = true;
        slot.active = true;
        slot.persistentId = id;
        slot.name = name.IsEmpty() ? String{} : String(name);

        m_transforms[index] = TransformData{}; // identity local, no links, not dirty

        ++m_aliveCount;
        ++m_revision;

        const EntityHandle handle{index, slot.generation};
        m_idMap.InsertOrAssign(id, handle);
        AppendToList(handle, m_firstRoot, m_lastRoot); // new entities start as roots
        return handle;
    }

    void Scene::DestroyEntityImmediate(EntityHandle entity)
    {
        const u32 index = entity.index;

        // Destroy the subtree first (snapshot the next sibling before each child dies).
        EntityHandle child = m_transforms[index].firstChild;
        while (child.IsAssigned())
        {
            const EntityHandle nextSibling =
                IsValid(child) ? m_transforms[child.index].nextSibling : EntityHandle::Invalid();
            DestroyEntityImmediate(child);
            child = nextSibling;
        }

        RemoveFromParent(entity);
        for (SceneSystem* s : m_sortedSystems)
        {
            s->OnEntityDestroyed(entity);
        } // managers free components

        EntitySlot& slot = m_entities[index];
        m_idMap.Remove(slot.persistentId);
        slot.alive = false;
        slot.active = false;
        slot.name = String{};
        slot.persistentId = Guid{};
        m_freeList.PushBack(index);
        --m_aliveCount;
        ++m_revision;

        m_transforms[index] = TransformData{};
    }

    void Scene::MarkDirty(EntityHandle entity)
    {
        if (!entity.IsAssigned())
        {
            return;
        }
        TransformData& d = m_transforms[entity.index];
        if (d.dirty)
        {
            return;
        }
        d.dirty = true;
        EntityHandle child = d.firstChild;
        while (child.IsAssigned() && IsValid(child))
        {
            const EntityHandle next = m_transforms[child.index].nextSibling;
            MarkDirty(child);
            child = next;
        }
        if (d.parent.IsAssigned())
        {
            MarkDirty(d.parent);
        }
    }

    void Scene::ApplyWorldAsLocal(EntityHandle child, const Float4x4& childWorld)
    {
        const EntityHandle parent = m_transforms[child.index].parent;
        const Float4x4 localMat =
            parent.IsAssigned() ? childWorld * Inverse(ComposeWorldMatrix(parent)) : childWorld;
        SetLocalTransform(child, Transform::FromMatrix(localMat));
    }

    void Scene::UpdateTransformRecursive(u32 index, const Float4x4& parentWorld)
    {
        {
            TransformData& d = m_transforms[index];
            d.prevWorldMatrix = d.worldMatrix;
            d.worldMatrix = d.local.ToMatrix() * parentWorld;
            d.dirty = false;
            d.updatedThisFrame = true;
        }
        m_transformsUpdatedThisFrame.PushBack(index);

        const Float4x4 myWorld = m_transforms[index].worldMatrix;
        EntityHandle child = m_transforms[index].firstChild;
        while (child.IsAssigned() && IsValid(child))
        {
            const u32 ci = child.index;
            UpdateTransformRecursive(ci, myWorld);
            child = m_transforms[ci].nextSibling;
        }
    }

    void Scene::AppendToList(EntityHandle entity, EntityHandle& head, EntityHandle& tail)
    {
        TransformData& e = m_transforms[entity.index];
        e.nextSibling = EntityHandle::Invalid();
        e.prevSibling = tail;
        if (!head.IsAssigned())
        {
            head = entity;
            tail = entity;
            return;
        }
        m_transforms[tail.index].nextSibling = entity;
        tail = entity;
    }

    void Scene::RemoveFromParent(EntityHandle child)
    {
        TransformData& c = m_transforms[child.index];
        const EntityHandle parent = c.parent;
        const EntityHandle prev = c.prevSibling;
        const EntityHandle next = c.nextSibling;
        if (prev.IsAssigned())
        {
            m_transforms[prev.index].nextSibling = next;
        }
        if (next.IsAssigned())
        {
            m_transforms[next.index].prevSibling = prev;
        }
        if (parent.IsAssigned())
        {
            TransformData& p = m_transforms[parent.index];
            if (p.firstChild == child)
            {
                p.firstChild = next;
            }
            if (p.lastChild == child)
            {
                p.lastChild = prev;
            }
        }
        else
        {
            if (m_firstRoot == child)
            {
                m_firstRoot = next;
            }
            if (m_lastRoot == child)
            {
                m_lastRoot = prev;
            }
        }
        c.parent = EntityHandle::Invalid();
        c.nextSibling = EntityHandle::Invalid();
        c.prevSibling = EntityHandle::Invalid();
    }

    bool Scene::IsDescendantOf(EntityHandle entity, EntityHandle ancestor) const
    {
        EntityHandle cur = entity;
        while (cur.IsAssigned() && IsValid(cur))
        {
            if (cur == ancestor)
            {
                return true;
            }
            cur = m_transforms[cur.index].parent;
        }
        return false;
    }

    void Scene::RunPhase(ScenePhase phase, f32 deltaTime)
    {
        for (SceneSystem* s : m_sortedSystems)
        {
            if (s->IsSimulationOnly() && !m_simulationEnabled)
            {
                continue;
            }
            s->OnUpdate(phase, deltaTime);
        }
    }

    void Scene::InsertSortedSystem(SceneSystem* system)
    {
        m_sortedSystems.PushBack(system);
        usize i = m_sortedSystems.Size() - 1;
        while (i > 0 && m_sortedSystems[i - 1]->UpdateOrder() > system->UpdateOrder())
        {
            m_sortedSystems[i] = m_sortedSystems[i - 1];
            m_sortedSystems[i - 1] = system;
            --i;
        }
    }

    void Scene::ProcessPendingDestroys()
    {
        if (m_pendingDestroys.IsEmpty())
        {
            return;
        }
        Array<EntityHandle> batch = Move(m_pendingDestroys);
        m_pendingDestroys = Array<EntityHandle>{};
        for (EntityHandle e : batch)
        {
            if (IsValid(e))
            {
                DestroyEntityImmediate(e);
            }
        }
    }
}
