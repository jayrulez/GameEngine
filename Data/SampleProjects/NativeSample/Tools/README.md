# NativeSample's authoring tools

NativeSample's content is built through the engine editor's MCP tools. Open the project with
`Tools.Editor Data/SampleProjects/NativeSample --mcp-port 7405` (the port `mcp.py` uses unless
`MCP_PORT` says otherwise), build its plugin first (Project > Build Project Plugins, then Project >
Reload Project Plugins) so the editor knows the Spin component and the note asset type, then run
the script. None of this is game content.

- `mcp.py`: a small MCP client (`python3 mcp.py http <tool> '<json args>'`, or `--list`).
- `scenegen.py`: scene XML built from the running editor's `component_schema`, so a component
  record starts from the engine's own defaults; the plugin's Spin component is in the schema while
  the plugin is loaded.
- `spinners.py`: the content. The Cube and Plane primitives, a note (Notes/Welcome, the Pipeline
  part's asset type), and the scene Spinners (Scenes/Spinners), made the project's default: a
  floor, a row of five cubes that each spin at their own rate about their own axis, and a carousel
  whose hub turns four arms, each carrying a cube that spins as well, lit by a sun under the default
  sky. Every cube is the Cube primitive on the default material, told apart by its tint. A rerun
  writes the scene over itself and makes nothing twice.
