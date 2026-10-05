<screen mode="overlay">

  <Flex direction="horizontal" justify="space-between" align="start" padding="18">

    <!-- The clock (penalties included) and the flash of a missed gate's penalty. -->
    <Flex direction="vertical" align="start" spacing="4">
      <Panel padding="10" style="background: rounded-rect(#10182AB0, radius=10);">
        <Label id="hud-time" text="0:00.00" font-size="30" style="text-color: #FFFFFF;"/>
      </Panel>
      <Label id="hud-penalty" text="+2 s" font-size="24" style="text-color: #FF5A4A;" visibility="hidden"/>
    </Flex>

    <!-- Gates passed of the course's, and gems taken of its gems. -->
    <Flex direction="vertical" align="end" spacing="8">
      <Panel padding="10" style="background: rounded-rect(#10182AB0, radius=10);">
        <Flex direction="horizontal" align="center" spacing="8">
          <Label text="Gates" font-size="18" style="text-color: #B8C8E8;"/>
          <Label id="hud-gates" text="0 / 0" font-size="26" style="text-color: #FFFFFF;"/>
        </Flex>
      </Panel>
      <Panel padding="10" style="background: rounded-rect(#10182AB0, radius=10);">
        <Flex direction="horizontal" align="center" spacing="8">
          <Label text="Gems" font-size="18" style="text-color: #7FDFFF;"/>
          <Label id="hud-gems" text="0 / 0" font-size="26" style="text-color: #FFFFFF;"/>
        </Flex>
      </Panel>
    </Flex>

  </Flex>

</screen>
