// Subscriber: a house that takes the paper. The script sits on the delivery zone in front of the
// porch (a static trigger), whose child "Marker" floats above to show the house wants a paper. The
// first paper to arrive ("PaperArrived", from Paper.as) is a delivery: "Delivered" goes out with
// the points, the marker disappears, and the zone switches itself off. It takes no more papers,
// and since an inactive entity has no trigger, the bike's auto-aim (an overlap query on the zone
// group) no longer finds it, so the arc and ring stop pointing at a porch already served. A
// delivery plays the paper slapping the porch and a chime, sparkles off the porch, and taps the
// pad.

Guid kLandSound = Guid("5ac330ab-7693-43a0-85ed-7b89d8a484e9");
Guid kDeliveredSound = Guid("0357e902-2dcc-4003-bd4e-340c6f8fb268");
Guid kFxSparkle = Guid("e362b75b-f227-4a33-b093-26e485cd3cd0");

class Subscriber
{
    private Entity@ self;

    [100, "Points for this delivery"] int value;

    private bool m_delivered = false;

    Subscriber(Entity@ entity) { @self = entity; }

    void onPaperArrived(int unused)
    {
        if (m_delivered)
        {
            return;
        }
        m_delivered = true;
        Audio::playOneShot(kLandSound, AudioBus::Effects, 0.9f, Random::range(0.95f, 1.05f));
        Audio::playOneShot(kDeliveredSound, AudioBus::Effects, 0.8f);
        Input::rumble(0.1f, 0.45f, 0.1f); // a delivery's tap
        ScenePrefabs::of(self.scene).spawn(kFxSparkle, self.position() + Float3(0.0f, 0.3f, 0.0f));
        self.scene.events.emit("Delivered", value);
        Entity@ marker = self.findChildByName("Marker");
        if (marker !is null && marker.isValid())
        {
            marker.setActive(false);
        }
        self.setActive(false);
    }
}
