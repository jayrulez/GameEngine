<!--
  The entity picker (Documentation/Specs/editor-gui.md, ruling 25): an entity slot's pick, the scene
  as its hierarchy shows it, the same tree in a picker's frame. The search over the scene (a match
  kept with what holds it), the tree with the scene its top row, the foot's hint and None. Code
  fills it.
-->
<Panel class="dialog picker entity-picker">
    <Panel class="picker-search">
        <Panel class="search"><Image class="search-icon" src="Editor/Icons/search.svg"/><TextField id="entity-picker-search" class="search-field" placeholder="Search the scene"/></Panel>
    </Panel>
    <TreeView id="entity-picker-tree" class="entity-picker-tree tree"/>
    <Label id="entity-picker-state" class="entity-picker-state"/>
    <Panel class="picker-foot"><Label id="entity-picker-hint" class="picker-hint"/><Spacer/><Button id="entity-picker-none" class="ghost" text="None"/></Panel>
</Panel>
