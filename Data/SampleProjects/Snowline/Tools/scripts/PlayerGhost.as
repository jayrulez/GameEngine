// PlayerGhost: the player's own best run, ridden again beside them. The script sits on the ghost's
// entity, whose first child is the rider model in its own see-through colour. Tools/course.py
// places it.
//
// Every run is recorded from the rider's first frame ("RunStarted") at `sampleRate` samples a
// second; each sample is the rider's position and facing (x, y, z, yaw). When a run sets the
// course's best ("NewBest" from Snowline.as), its recording is saved as the course's ghost
// ("ghost.<scene>", a list of numbers in the save) and becomes the ghost for the runs after it.
// The ghost rides the saved run, interpolated between its samples; with none saved it stays
// hidden.

const int kSampleSize = 4; // x, y, z, yaw

class PlayerGhost
{
    private Entity@ self;

    [10.0, "Samples recorded each second"] float sampleRate;

    private Entity@ m_rider;
    private Entity@ m_model;
    private array<float> m_recording; // this run, recorded as it is ridden
    private array<float> m_ghost;     // the best run, played back
    private bool m_started = false;
    private bool m_recordingOn = false;
    private float m_time = 0.0f;
    private float m_nextSample = 0.0f;

    PlayerGhost(Entity@ entity) { @self = entity; }

    void onStart()
    {
        @m_rider = self.scene.find("Rider");
        @m_model = self.firstChild();
        m_ghost = Save::getFloats(key());
        show(m_ghost.length() >= kSampleSize * 2);
    }

    void onRunStarted(int unused)
    {
        m_started = true;
        m_recordingOn = true;
    }

    // A new run: start a fresh recording, and ride the best run from the top.
    void onRunRestart(int unused)
    {
        m_recording.resize(0);
        m_recordingOn = true;
        m_time = 0.0f;
        m_nextSample = 0.0f;
        show(m_ghost.length() >= kSampleSize * 2);
    }

    // The run is over: stop recording (the rider coasts on past the line).
    void onRunFinished(int unused)
    {
        m_recordingOn = false;
    }

    // This run is the course's best, so it becomes the ghost from now on and is saved.
    void onNewBest(int hundredths)
    {
        m_ghost = m_recording;
        Save::setFloats(key(), m_ghost);
        Save::flush();
    }

    void onUpdate(double dt)
    {
        if (!m_started || m_rider is null || !m_rider.isValid())
        {
            return;
        }
        m_time += float(dt);
        if (m_recordingOn && m_time >= m_nextSample)
        {
            record();
            m_nextSample += 1.0f / sampleRate;
        }
        play();
    }

    private void record()
    {
        // The rider is a root entity, so position() holds this frame's pose; worldPosition() still
        // holds last frame's.
        Float3 at = m_rider.position();
        Float3 forward = Quaternion::RotateVector(m_rider.rotation(), Float3(0.0f, 0.0f, 1.0f));
        m_recording.insertLast(at.x);
        m_recording.insertLast(at.y);
        m_recording.insertLast(at.z);
        m_recording.insertLast(Math::Atan2(forward.x, forward.z));
    }

    // Places the ghost where the saved run was at this run's time, interpolating between two
    // samples. Once the saved run is over it stays at its last sample.
    private void play()
    {
        int samples = int(m_ghost.length()) / kSampleSize;
        if (samples < 2)
        {
            return;
        }
        float at = m_time * sampleRate;
        int i = int(at);
        if (i >= samples - 1)
        {
            i = samples - 2;
            at = float(samples - 1);
        }
        float k = at - float(i);
        int a = i * kSampleSize;
        int b = a + kSampleSize;
        Float3 p = Float3(m_ghost[a] + (m_ghost[b] - m_ghost[a]) * k,
                          m_ghost[a + 1] + (m_ghost[b + 1] - m_ghost[a + 1]) * k,
                          m_ghost[a + 2] + (m_ghost[b + 2] - m_ghost[a + 2]) * k);
        float turn = m_ghost[b + 3] - m_ghost[a + 3];
        while (turn > 3.14159f) { turn -= 6.28318f; }
        while (turn < -3.14159f) { turn += 6.28318f; }
        float yaw = m_ghost[a + 3] + turn * k;
        self.setPosition(p);
        self.setRotationEuler(0.0f, Math::RadiansToDegrees(yaw), 0.0f);
    }

    private void show(bool visible)
    {
        if (m_model !is null && m_model.isValid())
        {
            m_model.setActive(visible);
        }
    }

    private string key()
    {
        return "ghost." + self.scene.name();
    }
}
