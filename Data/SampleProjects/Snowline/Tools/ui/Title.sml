<screen mode="modal" transition="fade" default-focus="course-0">

  <!-- The title (Snowline.as fills it from the save): a card per course, its picture
       (thumbnails.py) with its name and the best medal and time won on it, or what unlocks it.
       A card is its picture and labels under a see-through button of the same size, whose frame
       lights when it is focused or hovered; a locked card's button is disabled and dims it. -->
  <Flex direction="vertical" justify="center" align="center" padding="32">

    <Panel padding="24" style="background: rounded-rect(#10182AE6, radius=16);">
      <Flex direction="vertical" align="center" spacing="6">
        <Label text="Snowline" font-size="72" style="text-color: #FFFFFF;"/>
        <Label text="Down the mountain against the clock" font-size="20" style="text-color: #B8C8E8;"/>
        <Spacer spacer-height="16"/>

        <Flex direction="horizontal" align="start" spacing="24">

          <Panel width="320" height="270">
            <Flex direction="vertical" align="start" spacing="0">
              <ImageView source="{80e6e15f-9ae9-49eb-8997-be7cece959e1}" width="320" height="180"/>
              <Panel width="320" height="90" padding="10" style="background: rounded-rect(#1C2740, radius=6);">
                <Flex direction="vertical" align="start" spacing="2">
                  <Label text="Meadow" font-size="22" style="text-color: #FFFFFF;"/>
                  <Label id="course-0-best" text="" font-size="14" style="text-color: #E0E8F8;"/>
                </Flex>
              </Panel>
            </Flex>
            <Button id="course-0" text="" width="320" height="270" style="background: state-list(normal=rounded-rect(#00000000, radius=8, border=#5A6E96, border-width=2), hover=rounded-rect(#FFFFFF14, radius=8, border=#B8C8E8, border-width=3), pressed=rounded-rect(#FFFFFF24, radius=8, border=#FFD966, border-width=4), focused=rounded-rect(#00000000, radius=8, border=#FFD966, border-width=5), disabled=rounded-rect(#0A1020B0, radius=8, border=#3A4A6A, border-width=2));"/>
          </Panel>

          <Panel width="320" height="270">
            <Flex direction="vertical" align="start" spacing="0">
              <ImageView source="{eb3096c4-08bc-4198-8ed2-cb657ab510fb}" width="320" height="180"/>
              <Panel width="320" height="90" padding="10" style="background: rounded-rect(#1C2740, radius=6);">
                <Flex direction="vertical" align="start" spacing="2">
                  <Label text="Forest" font-size="22" style="text-color: #FFFFFF;"/>
                  <Label id="course-1-best" text="" font-size="14" style="text-color: #E0E8F8;"/>
                </Flex>
              </Panel>
            </Flex>
            <Button id="course-1" text="" width="320" height="270" style="background: state-list(normal=rounded-rect(#00000000, radius=8, border=#5A6E96, border-width=2), hover=rounded-rect(#FFFFFF14, radius=8, border=#B8C8E8, border-width=3), pressed=rounded-rect(#FFFFFF24, radius=8, border=#FFD966, border-width=4), focused=rounded-rect(#00000000, radius=8, border=#FFD966, border-width=5), disabled=rounded-rect(#0A1020B0, radius=8, border=#3A4A6A, border-width=2));"/>
          </Panel>

        </Flex>

        <Spacer spacer-height="12"/>
        <Label text="Arrows or stick to choose, Enter or A to ride. In a run, Escape or Start for this." font-size="15" style="text-color: #8FA3C8;"/>
      </Flex>
    </Panel>

  </Flex>

</screen>
