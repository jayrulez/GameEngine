<screen mode="overlay">

  <Flex direction="horizontal" justify="space-between" align="start" padding="18">

    <!-- The clock (penalties included), the flash of a missed gate's penalty, the avalanche. -->
    <Flex direction="vertical" align="start" spacing="4">
      <Panel padding="10" style="background: rounded-rect(#10182AB0, radius=10);">
        <Label id="hud-time" text="0:00.00" font-size="30" style="text-color: #FFFFFF;"/>
      </Panel>
      <Label id="hud-penalty" text="+2 s" font-size="24" style="text-color: #FF5A4A;" visibility="hidden"/>
      <!-- How far behind the avalanche is, from the moment it breaks loose (Ridge). -->
      <Panel id="hud-avalanche-panel" padding="10" visibility="hidden" style="background: rounded-rect(#5A1A1AC0, radius=10);">
        <Flex direction="horizontal" align="center" spacing="8">
          <Label text="Avalanche" font-size="18" style="text-color: #FFC8C0;"/>
          <Label id="hud-avalanche" text="" font-size="26" style="text-color: #FFFFFF;"/>
        </Flex>
      </Panel>
    </Flex>

    <!-- A landed trick, named with its points, for a moment; under it the combo, while there is one. -->
    <Flex direction="vertical" align="center" spacing="2">
      <Label id="hud-trick" text="" font-size="30" style="text-color: #FFE38A;" visibility="hidden"/>
      <Label id="hud-combo" text="" font-size="22" style="text-color: #FFFFFF;" visibility="hidden"/>
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
