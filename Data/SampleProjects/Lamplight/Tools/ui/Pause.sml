<screen mode="modal" transition="fade" default-focus="resume-btn">

  <!-- The pause menu over a level that holds still (Lamplight.as). -->
  <Flex direction="vertical" justify="center" align="center">
    <Panel padding="24" style="background: rounded-rect(#0C0907E8, radius=16);">
      <Flex direction="vertical" align="stretch" spacing="10" width="300">
        <Label text="Paused" font-size="36" style="text-color: #F0D8A8;"/>
        <Button id="resume-btn" text="Resume" height="44" font-size="22" style="background: state-list(normal=rounded-rect(#1E1610, radius=10, border=#6A5030, border-width=2), hover=rounded-rect(#2C2016, radius=10, border=#D8B070, border-width=3), pressed=rounded-rect(#3A2A1A, radius=10, border=#F0C070, border-width=4), focused=rounded-rect(#1E1610, radius=10, border=#F0C070, border-width=4), disabled=rounded-rect(#100C08, radius=10, border=#3A2C1E, border-width=2)); text-color: #F0E0C0;"/>
        <Button id="restart-btn" text="Start the level over" height="44" font-size="22" style="background: state-list(normal=rounded-rect(#1E1610, radius=10, border=#6A5030, border-width=2), hover=rounded-rect(#2C2016, radius=10, border=#D8B070, border-width=3), pressed=rounded-rect(#3A2A1A, radius=10, border=#F0C070, border-width=4), focused=rounded-rect(#1E1610, radius=10, border=#F0C070, border-width=4), disabled=rounded-rect(#100C08, radius=10, border=#3A2C1E, border-width=2)); text-color: #F0E0C0;"/>
        <Button id="title-btn" text="Leave to the title" height="44" font-size="22" style="background: state-list(normal=rounded-rect(#1E1610, radius=10, border=#6A5030, border-width=2), hover=rounded-rect(#2C2016, radius=10, border=#D8B070, border-width=3), pressed=rounded-rect(#3A2A1A, radius=10, border=#F0C070, border-width=4), focused=rounded-rect(#1E1610, radius=10, border=#F0C070, border-width=4), disabled=rounded-rect(#100C08, radius=10, border=#3A2C1E, border-width=2)); text-color: #F0E0C0;"/>
      </Flex>
    </Panel>
  </Flex>

</screen>
