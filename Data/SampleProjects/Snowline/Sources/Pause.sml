<screen mode="modal" transition="fade" default-focus="resume-btn">

  <!-- The pause menu (Snowline.as): the run holds still under it. Escape, Start or B resumes. -->
  <Flex direction="vertical" justify="center" align="center" padding="32">
    <Panel padding="32" style="background: rounded-rect(#10182AE6, radius=16);">
      <Flex direction="vertical" align="center" spacing="10">
        <Label text="Paused" font-size="48" style="text-color: #FFFFFF;"/>
        <Spacer spacer-height="12"/>
        <Flex direction="vertical" spacing="10" width="300">
          <Button id="resume-btn" text="Resume" height="50" font-size="24" style="background: state-list(normal=rounded-rect(#1C2740, radius=10, border=#5A6E96, border-width=2), hover=rounded-rect(#26345A, radius=10, border=#B8C8E8, border-width=3), pressed=rounded-rect(#304070, radius=10, border=#FFD966, border-width=4), focused=rounded-rect(#1C2740, radius=10, border=#FFD966, border-width=4)); text-color: #FFFFFF;"/>
          <Button id="restart-btn" text="Restart run" height="50" font-size="24" style="background: state-list(normal=rounded-rect(#1C2740, radius=10, border=#5A6E96, border-width=2), hover=rounded-rect(#26345A, radius=10, border=#B8C8E8, border-width=3), pressed=rounded-rect(#304070, radius=10, border=#FFD966, border-width=4), focused=rounded-rect(#1C2740, radius=10, border=#FFD966, border-width=4)); text-color: #FFFFFF;"/>
          <Button id="courses-btn" text="Courses" height="50" font-size="24" style="background: state-list(normal=rounded-rect(#1C2740, radius=10, border=#5A6E96, border-width=2), hover=rounded-rect(#26345A, radius=10, border=#B8C8E8, border-width=3), pressed=rounded-rect(#304070, radius=10, border=#FFD966, border-width=4), focused=rounded-rect(#1C2740, radius=10, border=#FFD966, border-width=4)); text-color: #FFFFFF;"/>
        </Flex>
        <Spacer spacer-height="8"/>
        <Label text="Escape, Start or B to ride on" font-size="15" style="text-color: #8FA3C8;"/>
      </Flex>
    </Panel>
  </Flex>

</screen>
