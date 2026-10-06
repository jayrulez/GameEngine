<screen mode="modal" transition="fade" default-focus="course-0">

  <!-- The title (Snowline.as fills it from the save): a button per course, the best medal and time
       won on it beside it, or what unlocks it. -->
  <Flex direction="vertical" justify="center" align="center" padding="32">

    <Panel padding="28" style="background: rounded-rect(#10182AE6, radius=16);">
      <Flex direction="vertical" align="center" spacing="6">
        <Label text="Snowline" font-size="72" style="text-color: #FFFFFF;"/>
        <Label text="Down the mountain against the clock" font-size="20" style="text-color: #B8C8E8;"/>
        <Spacer spacer-height="18"/>

        <Flex direction="horizontal" align="center" spacing="18" width="560">
          <Button id="course-0" text="Meadow" width="200" height="50" font-size="24"/>
          <Label id="course-0-best" text="" font-size="19" style="text-color: #E0E8F8;"/>
        </Flex>
        <Flex direction="horizontal" align="center" spacing="18" width="560">
          <Button id="course-1" text="Forest" width="200" height="50" font-size="24"/>
          <Label id="course-1-best" text="" font-size="19" style="text-color: #E0E8F8;"/>
        </Flex>

        <Spacer spacer-height="14"/>
        <Label text="Arrows or stick to choose, Enter or A to ride. In a run, Escape or Start for this." font-size="15" style="text-color: #8FA3C8;"/>
      </Flex>
    </Panel>

  </Flex>

</screen>
