<!-- The HUD (PlatformerGame.as fills it): lives and coins top left (the gem beside them once
     found), the level and its clock in the middle, the score top right with the gains that float
     up under it. Over the level, taking none of its input. Styled by SkyHopperSheet.css. Back
     (Escape, East) is the game's own Pause action, not the screen stack's. -->
<Screen blocks="false" back="none">
  <Panel class="hud-screen">
    <!-- Lives and coins: the run's stakes, each an icon and a count. -->
    <Panel class="hud hud-row">
      <Image id="hud-heart" class="icon icon-30" src="{c1e3a2f2-ed4e-4765-886f-d5fd8d2776fd}"/>
      <Label id="hud-lives" class="hud-value lilita hud-number" text="x 3"/>
      <Image id="hud-coin" class="icon icon-28 hud-gap" src="{8286b06a-7c3d-4a38-a303-c8e96991d0b9}"/>
      <Label id="hud-coins" class="hud-coins lilita hud-number" text="0 / 0"/>
      <Image id="hud-gem" class="icon icon-26 hud-gem" src="{1cb0b016-e4eb-4605-ab11-4a30fc6aa89c}"/>
    </Panel>
    <!-- The level and its clock. -->
    <Panel class="hud hud-row">
      <Label id="hud-level" class="hud-level" text="1  Grassy Hills"/>
      <Image class="icon icon-24 hud-gap" src="{61312858-e1db-4148-952a-29b440b6cd66}"/>
      <Label id="hud-time" class="hud-value lilita hud-clock" text="0:00"/>
    </Panel>
    <!-- The score, and the gains that float up from it. -->
    <Panel class="hud-right">
      <Panel class="hud hud-score-card">
        <Label id="hud-score" class="hud-score lilita" text="0"/>
      </Panel>
      <Label id="hud-popup" class="popup lilita" text="+100"/>
      <Label id="hud-life-popup" class="popup-life lilita" text="1UP"/>
    </Panel>
  </Panel>
</Screen>
