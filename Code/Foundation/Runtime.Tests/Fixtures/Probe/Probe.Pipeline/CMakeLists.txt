# Probe.Pipeline: a fixture plugin part (Runtime.Tests links the probe packages through the registry).
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PUBLIC Foundation::Runtime Foundation::Policy)
target_sources(${PLUGIN_PART} PRIVATE ProbePipeline.cpp)
