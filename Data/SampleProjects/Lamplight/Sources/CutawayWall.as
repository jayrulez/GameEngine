// CutawayWall - a wall that gets out of the way: faded while the camera is outside it and the thief
// inside, so a camera above and behind sees into the room. The fade is the renderer's screen-door
// dither (MeshComponent.fade), eased in and out rather than cut, and the wall keeps casting its
// whole shadow, so the room behind it stays as dark as it was.
class CutawayWall
{
    private Entity@ self;

    [null, "The camera"] Entity@ camera;
    [null, "The thief"] Entity@ thief;
    [0.0, "The wall's outward normal, x"] float normalX;
    [0.0, "The wall's outward normal, z"] float normalZ;
    [0.85, "How far it fades while in the way (1 = gone)"] float cutaway;
    [4.0, "How quickly it fades in or out (per second)"] float speed;

    private float m_fade = 0.0f;

    CutawayWall(Entity@ entity) { @self = entity; }

    void onUpdate(double dt)
    {
        if (camera is null || thief is null || !camera.isValid() || !thief.isValid())
        {
            return;
        }
        Float3 at = self.worldPosition();
        Float3 c = camera.worldPosition();
        Float3 t = thief.worldPosition();
        float cameraSide = (c.x - at.x) * normalX + (c.z - at.z) * normalZ;
        float thiefSide = (t.x - at.x) * normalX + (t.z - at.z) * normalZ;
        float want = (cameraSide > 0.0f && thiefSide < 0.0f) ? cutaway : 0.0f;
        float step = speed * float(dt);
        float next = m_fade < want ? (m_fade + step > want ? want : m_fade + step)
                                   : (m_fade - step < want ? want : m_fade - step);
        if (next != m_fade)
        {
            m_fade = next;
            MeshComponent::of(self).fade = m_fade;
        }
    }
}
