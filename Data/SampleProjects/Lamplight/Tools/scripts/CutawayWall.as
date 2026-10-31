// CutawayWall: a wall piece that gets out of the camera's way. It fades while the line from the
// camera to the thief crosses it, measured along the floor, and comes back when the line no longer
// does. The piece counts as `margin` wider on either side so that the thief's shoulders and the
// ground a step ahead stay visible. The fade uses the renderer's screen-door dither
// (MeshComponent.fade), eased in and out. The piece keeps casting its whole shadow, so the area
// behind it stays as dark as before.
class CutawayWall
{
    private Entity@ self;

    [null, "The camera"] Entity@ camera;
    [null, "The thief"] Entity@ thief;
    [0.0, "The piece's face direction (its normal), x"] float normalX;
    [1.0, "The piece's face direction (its normal), z"] float normalZ;
    [1.0, "Half the piece's length along its face (m)"] float halfLength;
    [1.0, "How much wider than the piece the line may pass and still fade it (m)"] float margin;
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
        float want = inTheWay() ? cutaway : 0.0f;
        float step = speed * float(dt);
        float next = m_fade < want ? (m_fade + step > want ? want : m_fade + step)
                                   : (m_fade - step < want ? want : m_fade - step);
        if (next != m_fade)
        {
            m_fade = next;
            MeshComponent::of(self).fade = m_fade;
        }
    }

    // Whether the line from the camera to the thief crosses the piece, in the plane of the floor.
    // It does when the two ends lie on opposite sides of the face and the crossing point lies within
    // the widened length of the piece.
    private bool inTheWay()
    {
        Float3 at = self.worldPosition();
        Float3 c = camera.worldPosition();
        Float3 t = thief.worldPosition();
        float cameraSide = (c.x - at.x) * normalX + (c.z - at.z) * normalZ;
        float thiefSide = (t.x - at.x) * normalX + (t.z - at.z) * normalZ;
        if ((cameraSide > 0.0f) == (thiefSide > 0.0f))
        {
            return false;
        }
        float k = cameraSide / (cameraSide - thiefSide); // how far along the line it crosses
        float x = c.x + (t.x - c.x) * k - at.x;
        float z = c.z + (t.z - c.z) * k - at.z;
        float along = x * -normalZ + z * normalX; // distance along the face
        return along >= -(halfLength + margin) && along <= halfLength + margin;
    }
}
