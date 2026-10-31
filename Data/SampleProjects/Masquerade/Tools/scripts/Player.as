// Player: a guest that someone plays. It sits on a guest's entity like any other (a navigation
// agent and an Instance Member of a costume), so it looks like a guest. It moves where the stick
// points, relative to where the camera looks, by driving its agent at that velocity, with no path
// to wait for; the crowd makes way round it and it round the crowd, as guests do for each other.
// It walks at a guest's stroll by default. Sprint is faster but makes you stand out (Ball.as);
// holding Dance dances; Tag tells the ball, and Ball.as decides who is in reach. It shoulders past
// guests at half pace while pressed against one (`crowd`), so it cannot plough through a group,
// and standing guests step out of its way (Guest.as). It wears its costume on a child, one of six
// looks that Ball.as picks between, so the clip goes to each look. Its `activity` values are the
// same as a guest's.
//
// Over the network the host moves every hunter. A client's own hunter sends its stick, sprint,
// dance and tags to the host (Net "input" and "tag"), the host's Ball passes them to the hunter
// ("Input"), and the transform and activity come back replicated. A hunter spawned for a joining
// player finds the camera, the crowd and the Ball by name, and on that player's own client it
// tells the camera to follow it.

const int kIdle = 0; // clip indices, in the instanced skinning order of the Guests entity
const int kWalk = 1;
const int kTalk = 2;
const int kDance = 3;

class Player
{
    private Entity@ self;

    [null, "The camera whose facing makes forward"] Entity@ camera;
    [null, "The crowd: its children's children are the guests it pushes past"] Entity@ crowd;
    [0.55, "Nearer than this to a guest (m) is pressing against it"] float pressDistance;
    [0.5, "Its pace while pressing against a guest (of the full)"] float pressPace;
    [1.0, "Walking speed (m/s): a guest's, the Walk clip's"] float walkSpeed;
    [3.0, "Sprinting speed (m/s)"] float sprintSpeed;
    [0, "What the player is doing (0 idle, 1 walk, 2 talk, 3 dance): the clip it plays", replicated] int activity;
    [false, "Sprinting now (a hunter sees it)", replicated] bool sprinting;

    private float m_yaw = 0.0f;
    private bool m_moving = false;
    private float m_pressCheck = 0.0f;
    private bool m_pressed = false;
    private Float4 m_remote = Float4(0.0f, 0.0f, 0.0f, 0.0f); // host: a remote player's input
    private float m_remoteAge = 99.0f;
    private int m_sent = -1;          // client: the input last sent to the host
    private bool m_ringShown = false;  // the ring under this hunter is shown (it is this peer's own)
    private float m_sendIn = 0.0f;
    private bool m_handedOver = false; // client: the agent is stopped so the host's transforms rule

    Player(Entity@ entity) { @self = entity; }

    void onStart()
    {
        // A hunter spawned for a joining player has no scene references set, so it finds what it
        // needs by name.
        if (camera is null || !camera.isValid())
        {
            @camera = self.scene.find("Camera");
        }
        if (crowd is null || !crowd.isValid())
        {
            @crowd = self.scene.find("Crowd");
        }
        Entity@ ball = self.scene.find("Ball");
        if (ball !is null && ball.isValid())
        {
            ball.send("Hunter", self); // on every peer, so the seats can find it
        }
        if (mine() && camera !is null && camera.isValid())
        {
            camera.send("Follow", self);
        }
    }

    // The owning player: 0 for the host, otherwise the id the host gave a client. mine() is true
    // when this peer plays it.
    private int owner()
    {
        NetworkComponent@ net = NetworkComponent::of(self);
        return net is null ? 0 : int(net.owner);
    }

    private bool mine()
    {
        int me = Net::localPlayer();
        return owner() == (me < 0 ? 0 : me);
    }

    // Host only: a remote player's stick (x, z), sprint and dance, passed on by Ball.as.
    void onInput(Float4 input)
    {
        m_remote = input;
        m_remoteAge = 0.0f;
    }

    void onUpdate(double dt)
    {
        float d = float(dt);
        // Show the ring on the floor under your own guest (maps.py hides it on every hunter) once
        // this hunter's owner is known; a joiner's hunter becomes its own as the host's state
        // arrives.
        bool own = mine();
        if (own != m_ringShown)
        {
            Entity@ ring = self.findChildByName("Ring");
            if (ring !is null && ring.isValid())
            {
                DecalComponent@ decal = DecalComponent::of(ring);
                if (decal !is null)
                {
                    decal.visible = own;
                    m_ringShown = own;
                }
            }
        }
        // Read the player's input: the stick as a direction on the floor, sprint, dance and tag.
        float dx = 0.0f;
        float dz = 0.0f;
        bool sprint = false;
        bool dance = false;
        bool tag = false;
        if (mine())
        {
            Float2 stick = Input::value2D("Move");
            float amount = Math::Sqrt(stick.x * stick.x + stick.y * stick.y);
            if (amount >= 0.1f && camera !is null && camera.isValid())
            {
                if (amount > 1.0f)
                {
                    stick = Float2(stick.x / amount, stick.y / amount);
                }
                // The camera's facing projected onto the floor: forward, and right of it.
                Float3 look = Quaternion::RotateVector(camera.rotation(), Float3(0.0f, 0.0f, -1.0f));
                float flat = Math::Max(Math::Sqrt(look.x * look.x + look.z * look.z), 0.0001f);
                Float3 forward = Float3(look.x / flat, 0.0f, look.z / flat);
                Float3 right = Float3(-forward.z, 0.0f, forward.x);
                dx = forward.x * stick.y + right.x * stick.x;
                dz = forward.z * stick.y + right.z * stick.x;
            }
            sprint = Input::isDown("Sprint");
            dance = Input::isDown("Dance");
            tag = Input::wasPressed("Tag");
        }
        else if (!Net::isClient())
        {
            m_remoteAge += d;
            if (m_remoteAge < 0.5f) // a quiet client stands still
            {
                dx = m_remote.x;
                dz = m_remote.y;
                sprint = m_remote.z > 0.5f;
                dance = m_remote.w > 0.5f;
            }
        }

        if (Net::isClient())
        {
            if (!m_handedOver)
            {
                NavAgentComponent::of(self).stop(); // it walked since the scene loaded; the host moves it now
                m_handedOver = true;
            }
            // The host moves every hunter. A client sends its own player's input and plays what the
            // host replicates back.
            if (mine())
            {
                send(dx, dz, sprint, dance, d);
                if (tag)
                {
                    Net::rpc("tag");
                }
            }
            playClip();
            return;
        }

        NavAgentComponent@ agent = NavAgentComponent::of(self);
        float amount = Math::Sqrt(dx * dx + dz * dz);
        sprinting = false;
        if (amount >= 0.1f)
        {
            sprinting = sprint;
            float speed = sprinting ? sprintSpeed : walkSpeed;
            m_pressCheck -= d;
            if (m_pressCheck <= 0.0f)
            {
                m_pressCheck = 0.1f;
                m_pressed = pressing();
            }
            if (m_pressed)
            {
                speed *= pressPace;
            }
            agent.move(dx / amount * Math::Min(amount, 1.0f) * speed, dz / amount * Math::Min(amount, 1.0f) * speed);
            m_moving = true;
            face(dx, dz, d);
        }
        else
        {
            agent.move(0.0f, 0.0f);
            m_moving = false;
        }
        if (tag)
        {
            self.scene.events.emit("Tag", owner()); // Ball.as finds the seat
        }
        activity = m_moving ? kWalk : (dance ? kDance : kIdle);
        playClip();
    }

    // Client only: sends the player's input to the host, packed in one number (the stick to a
    // hundredth, sprint and dance as bits), whenever it changes and five times a second anyway.
    private void send(float dx, float dz, bool sprint, bool dance, float d)
    {
        int ix = int(Math::Clamp((dx + 1.0f) * 100.0f + 0.5f, 0.0f, 200.0f));
        int iz = int(Math::Clamp((dz + 1.0f) * 100.0f + 0.5f, 0.0f, 200.0f));
        int packed = (ix * 201 + iz) * 4 + (sprint ? 1 : 0) + (dance ? 2 : 0);
        m_sendIn -= d;
        if (packed != m_sent || m_sendIn <= 0.0f)
        {
            Net::rpcNumber("input", double(packed));
            m_sent = packed;
            m_sendIn = 0.2f;
        }
    }

    // Plays the clip for its replicated activity on each of its looks; only the one it wears is
    // drawn.
    private void playClip()
    {
        Entity@ child = self.firstChild();
        while (child !is null && child.isValid())
        {
            InstanceMemberComponent@ look = InstanceMemberComponent::of(child);
            if (look !is null)
            {
                look.clip = uint(activity);
            }
            @child = child.nextSibling();
        }
    }

    // Whether it is pressed against a guest: any member of the crowd within pressDistance.
    private bool pressing()
    {
        if (crowd is null || !crowd.isValid())
        {
            return false;
        }
        Float3 p = self.worldPosition();
        float near = pressDistance * pressDistance;
        Entity@ group = crowd.firstChild();
        while (group !is null && group.isValid())
        {
            Entity@ guest = group.firstChild();
            while (guest !is null && guest.isValid())
            {
                Float3 q = guest.worldPosition();
                float dx = q.x - p.x;
                float dz = q.z - p.z;
                if (dx * dx + dz * dz < near)
                {
                    return true;
                }
                @guest = guest.nextSibling();
            }
            @group = group.nextSibling();
        }
        return false;
    }

    // Turns to face where it goes (the model faces +Z), the short way round.
    private void face(float dx, float dz, float d)
    {
        float want = Math::Atan2(dx, dz);
        float delta = want - m_yaw;
        while (delta > 3.14159265f) delta -= 6.2831853f;
        while (delta < -3.14159265f) delta += 6.2831853f;
        m_yaw += delta * Math::Min(10.0f * d, 1.0f);
        self.setRotationEuler(0.0f, m_yaw * 57.2957795f, 0.0f);
    }
}
