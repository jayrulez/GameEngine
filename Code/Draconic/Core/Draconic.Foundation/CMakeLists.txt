# Draconic::Foundation - the foundation module (`Draconic.Foundation`).
#
# One named module composed of partitions (one per subsystem), re-exported by
# FoundationModule.cppm. Built as a static library for now.

add_library(Draconic.Foundation STATIC)
add_library(Draconic::Foundation ALIAS Draconic.Foundation)

target_link_libraries(Draconic.Foundation PUBLIC Draconic::Policy)

# Classic (non-module) translation units: macro-centric subsystems whose
# functions need external linkage reachable from any module's GMF (see
# Debug/Assert.h). Kept minimal.
target_sources(Draconic.Foundation PRIVATE
    Debug/Debug.cpp)

# Module IMPLEMENTATION units (`module draconic.foundation;`): definitions kept out of the
# interface partitions. Listed as regular sources, NOT in the CXX_MODULES file set.
target_sources(Draconic.Foundation PRIVATE
    Reflection/FoundationReflectionImpl.cpp)   # DRACONIC_REFLECT_* bodies (see FoundationReflection.cppm)

# Module interface units. The primary unit (FoundationModule.cppm) must be listed;
# partitions are added here as subsystems land.
target_sources(Draconic.Foundation
    PUBLIC
        FILE_SET core_modules TYPE CXX_MODULES FILES
            FoundationModule.cppm
            Base.cppm
            RefCounted.cppm
            UniquePtr.cppm
            Function.cppm
            Memory/Allocator.cppm
            Memory/LinearAllocator.cppm
            Memory/StackAllocator.cppm
            Memory/PoolAllocator.cppm
            Memory/FrameAllocator.cppm
            Memory/TrackingAllocator.cppm
            Memory/MemoryTag.cppm
            System/System.cppm
            System/Time.cppm
            Containers/Span.cppm
            Containers/Array.cppm
            Containers/FixedArray.cppm
            Containers/RingBuffer.cppm
            Containers/IntrusiveList.cppm
            Hash.cppm
            Containers/HashMap.cppm
            Containers/HashSet.cppm
            Text/String.cppm
            Text/StringHash.cppm
            Text/StringUtil.cppm
            Text/Format.cppm
            Log/Logger.cppm
            Log/ConsoleSink.cppm
            Log/FileSink.cppm
            Log/RingLogSink.cppm
            Math/Math.cppm
            Math/Float2.cppm
            Math/Float3.cppm
            Math/Float4.cppm
            Math/Color.cppm
            Random.cppm
            Guid.cppm
            FixedStepper.cppm
            Math/Float4x4.cppm
            Math/Float3x3.cppm
            Math/Simd.cppm
            Math/SimdVector.cppm
            Math/SimdMatrix.cppm
            Math/Quaternion.cppm
            Math/Transform.cppm
            Math/Easings.cppm
            Math/AABB.cppm
            Math/Plane.cppm
            Math/BoundingVolumes.cppm
            Math/Rectangle.cppm
            Math/ContentFit.cppm
            RTTI/TypeInfo.cppm
            RTTI/TypeRegistry.cppm
            RTTI/Object.cppm
            Reflection/Variant.cppm
            Reflection/Instance.cppm
            Reflection/Reflection.cppm
            Reflection/EnumReflection.cppm
            Reflection/ConstantRegistry.cppm
            Reflection/FoundationReflection.cppm
            IO/Stream.cppm
            IO/BinaryIO.cppm
            IO/Path.cppm
            IO/FileSystem.cppm
            Serialization/ISerializer.cppm
            Serialization/ISerializable.cppm
            Serialization/SerializableRegistry.cppm
            Serialization/Serializer.cppm
            Serialization/BinarySerializer.cppm
            Serialization/Serialize.cppm
            Threading/Atomic.cppm
            Threading/Thread.cppm
            Threading/Mutex.cppm
            Threading/ScopedLock.cppm
            Threading/SpinLock.cppm
            Threading/ConditionVariable.cppm
            Threading/Semaphore.cppm
            Threading/SharedMutex.cppm
            Threading/JobSystem.cppm
            Library/DynamicLibrary.cppm)

# Per-platform backends (classic TUs). Emscripten is POSIX-shaped: it reuses the Linux
# backend (pthread API present; actual thread spawning arrives with the wasm-pthreads
# opt-in - web v1 is single-threaded and the JobSystem's caller-participation covers it).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR EMSCRIPTEN)
    target_sources(Draconic.Foundation PRIVATE
        System/Linux/LinuxSystem.cpp
        Threading/Linux/LinuxThread.cpp)
elseif(WIN32)
    target_sources(Draconic.Foundation PRIVATE
        System/Win32/Win32System.cpp
        Threading/Win32/Win32Thread.cpp)
    target_link_libraries(Draconic.Foundation PUBLIC ws2_32)   # Winsock2 for the UDP socket backend
endif()

# Threading runtime (pthreads on Linux).
find_package(Threads REQUIRED)
target_link_libraries(Draconic.Foundation PUBLIC Threads::Threads)

# Dynamic loader (dlopen/dlsym) for the Library module.
target_link_libraries(Draconic.Foundation PUBLIC ${CMAKE_DL_LIBS})

# Classic headers that cross the module boundary (macros only). Consumers
# include them as "Foundation/<Header>.h"; the engine include root is Code/Draconic.
target_sources(Draconic.Foundation
    PUBLIC
        FILE_SET core_headers TYPE HEADERS
            BASE_DIRS ${CMAKE_SOURCE_DIR}/Code/Draconic
            FILES
                Prelude.h
                Debug/Assert.h
                Math/SimdConfig.h
                System/SystemBackend.h
                Log/Log.h
                Reflection/Reflect.h
                Threading/ThreadBackend.h)

# So `#include "Foundation/Prelude.h"` resolves from module global-module-fragments.
target_include_directories(Draconic.Foundation PUBLIC ${CMAKE_SOURCE_DIR}/Code/Draconic)

