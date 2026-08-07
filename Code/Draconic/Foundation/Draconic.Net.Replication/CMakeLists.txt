# Draconic::Net.Replication - `draconic.net.replication`: the StateReplication foundation - a stable
# NetworkId + authority, and the REFLECTION-DRIVEN field codec (marked properties -> :wire, no
# hand-written per-component net code). Over Foundation (reflection) + Net (wire). Snapshot/delta/spawn/
# relevancy stack on top in later slices. See docs/design/networking.md §5, §7 P2.

add_library(Draconic.Net.Replication STATIC)
add_library(Draconic::Net.Replication ALIAS Draconic.Net.Replication)

target_link_libraries(Draconic.Net.Replication PUBLIC Draconic::Foundation Draconic::Net Draconic::Scene Draconic::Resource Draconic::Script.Facades Draconic::Policy)

target_sources(Draconic.Net.Replication
    PUBLIC
        FILE_SET net_replication_modules TYPE CXX_MODULES FILES
            Replication.cppm
    PRIVATE
        ReplicationImpl.cpp
        Interpolation.cpp)

