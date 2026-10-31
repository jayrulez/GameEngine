# Foundation::Net.Replication - `foundation.net.replication`: the StateReplication foundation: a stable
# NetworkId and authority, and the reflection-driven field codec (marked properties go to :wire, with
# no hand-written per-component net code). Over Core (reflection) and Net (wire).

util_add_engine_library(Net.Replication ALIAS Foundation::Net.Replication)
target_link_libraries(Net.Replication PUBLIC Foundation::Core Foundation::Net Foundation::Scene Foundation::Resource Foundation::Script.Facades Foundation::Policy)

target_sources(Net.Replication
    PUBLIC
        FILE_SET net_replication_modules TYPE CXX_MODULES FILES
            Replication.cppm
    PRIVATE
        ReplicationImpl.cpp
        Interpolation.cpp)

