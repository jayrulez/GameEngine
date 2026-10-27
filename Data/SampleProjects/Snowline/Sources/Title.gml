<!-- The title (Snowline.as fills it from the save): a card per course, its picture (thumbnails.py,
     UI/Thumbs) with its name and the best medal and time won on it, or what unlocks it. A card is one
     button holding its picture and labels; a locked card is disabled and dims. Quit, under the cards,
     ends the game. Styled by SnowlineSheet.css. Back (Escape, East) is the game's own
     Pause action (Snowline.as), not the screen stack's. -->
<Screen class="fades" nav-default="course-0" back="none">
  <Panel class="centre">
    <Panel class="card title-card">
      <Label class="title-name" text="Snowline"/>
      <Label class="title-lead" text="Down the mountain against the clock"/>
      <Panel class="courses">
        <Button id="course-0" class="slate course">
          <Image class="course-picture" src="{80e6e15f-9ae9-49eb-8997-be7cece959e1}"/>
          <Label class="course-name" text="Meadow"/>
          <Label id="course-0-best" class="course-best" text=""/>
        </Button>
        <Button id="course-1" class="slate course">
          <Image class="course-picture" src="{eb3096c4-08bc-4198-8ed2-cb657ab510fb}"/>
          <Label class="course-name" text="Forest"/>
          <Label id="course-1-best" class="course-best" text=""/>
        </Button>
        <Button id="course-2" class="slate course">
          <Image class="course-picture" src="{5885a767-b7c1-4bc9-8878-661ac3c5bcf3}"/>
          <Label class="course-name" text="Ridge"/>
          <Label id="course-2-best" class="course-best" text=""/>
        </Button>
      </Panel>
      <Button id="quit-btn" class="slate quit" text="Quit"/>
      <Label class="title-hint" text="Arrows or stick to choose, Enter or A to ride. In a run, Escape or Start to pause."/>
    </Panel>
  </Panel>
</Screen>
