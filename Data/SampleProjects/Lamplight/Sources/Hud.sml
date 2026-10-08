<screen mode="overlay">

  <Flex direction="vertical" justify="space-between" align="stretch" padding="18" spacing="10">

    <!-- A word at the top for a moment: a checkpoint reached. -->
    <Flex direction="horizontal" justify="center">
      <Label id="hud-note" text="Checkpoint" font-size="28" visibility="hidden" style="text-color: #F0E0C0;"/>
    </Flex>

    <Flex direction="vertical" justify="end" align="stretch" spacing="10">

      <!-- What the thief can do here (put out a lamp, pick a lock), set by whatever is in reach. -->
      <Flex direction="horizontal" justify="center">
        <Panel id="hud-prompt-panel" padding="10" visibility="hidden" style="background: rounded-rect(#0A0E18C0, radius=10);">
          <Flex direction="vertical" align="center" spacing="6">
            <Label id="hud-prompt" text="" font-size="22" style="text-color: #F0E0C0;"/>
            <ProgressBar id="hud-pick" value="0" width="220" height="8" visibility="hidden"/>
          </Flex>
        </Panel>
      </Flex>

      <!-- The light meter, bottom left: how lit the thief is, the one thing to read before moving. -->
      <Flex direction="horizontal" justify="start">
        <Panel padding="12" style="background: rounded-rect(#0A0E18C0, radius=10);">
          <Flex direction="vertical" align="start" spacing="6" width="240">
            <Label id="hud-light-label" text="Hidden" font-size="20" style="text-color: #D8C8A8;"/>
            <ProgressBar id="hud-light" value="0" width="240" height="12"/>
          </Flex>
        </Panel>
      </Flex>

    </Flex>
  </Flex>

</screen>
