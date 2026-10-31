# Foundation::Core - the foundation module (`foundation.core`).
#
# One named module composed of partitions (one per subsystem), re-exported by
# CoreModule.cppm. Built as a static library.

util_add_engine_library(Core ALIAS Foundation::Core)
target_link_libraries(Core PUBLIC Foundation::Policy)

# Classic (non-module) translation units: macro-centric subsystems whose
# functions need external linkage reachable from any module's GMF (see
# Debug/Assert.h). Kept minimal.
target_sources(Core PRIVATE
    Debug/Debug.cpp)

# Module implementation units (`module foundation.core;`): definitions kept out of the
# interface partitions. Listed as regular sources, not in the CXX_MODULES file set.
target_sources(Core PRIVATE
    RefCountedImpl.cpp                   # MakeRef handshake slot (one per process, not per library)
    RTTI/TypeInfoImpl.cpp                # TypeOf<T>() slot table (one TypeInfo per process)
    Threading/JobSystemImpl.cpp          # global JobSystem pointer and worker slot (one per process)
    Threading/ThreadImpl.cpp             # the calling thread's name (one per process)
    Text/TextImpl.cpp                    # float to_chars (STL tables must not reach consumers)
    Reflection/CoreReflectionImpl.cpp)   # REFLECT_* bodies (see CoreReflection.cppm)

# Module interface units: the primary unit (CoreModule.cppm) and every partition.
target_sources(Core
    PUBLIC
        FILE_SET core_modules TYPE CXX_MODULES FILES
            CoreModule.cppm
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
            Math/Curve.cppm
            Math/Gradient.cppm
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
            Reflection/CoreReflection.cppm
            IO/Stream.cppm
            IO/BinaryIO.cppm
            IO/Path.cppm
            IO/FileSystem.cppm
            Serialization/ISerializer.cppm
            Serialization/ISerializable.cppm
            Serialization/SerializableRegistry.cppm
            Serialization/Serializer.cppm
            Serialization/BinarySerializer.cppm
            Serialization/SchemaRecorder.cppm
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

# Per-platform backends (classic TUs). Emscripten is POSIX-shaped, so it reuses the Linux
# backend (the pthread API is present). The web build is single-threaded: spawning threads
# needs the wasm-pthreads opt-in, and the JobSystem's calling thread runs the jobs itself.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR EMSCRIPTEN)
    target_sources(Core PRIVATE
        System/Linux/LinuxSystem.cpp
        Threading/Linux/LinuxThread.cpp)
elseif(WIN32)
    target_sources(Core PRIVATE
        System/Win32/Win32System.cpp
        Threading/Win32/Win32Thread.cpp)
    target_link_libraries(Core PUBLIC ws2_32 bcrypt)   # Winsock2 for the UDP socket backend; BCrypt for FillEntropy
endif()

# Threading runtime (pthreads on Linux).
find_package(Threads REQUIRED)
target_link_libraries(Core PUBLIC Threads::Threads)

# Dynamic loader (dlopen/dlsym) for the Library module.
target_link_libraries(Core PUBLIC ${CMAKE_DL_LIBS})

# Classic headers that cross the module boundary (macros only). Consumers
# include them as "Core/<Header>.h"; the engine include root is Code.
target_sources(Core
    PUBLIC
        FILE_SET core_headers TYPE HEADERS
            BASE_DIRS ${CMAKE_SOURCE_DIR}/Code
            FILES
                Prelude.h
                Debug/Assert.h
                Math/SimdConfig.h
                System/SystemBackend.h
                Log/Log.h
                Reflection/Reflect.h
                Threading/ThreadBackend.h)

# So `#include "Core/Prelude.h"` resolves from module global-module-fragments.
target_include_directories(Core PUBLIC ${CMAKE_SOURCE_DIR}/Code)

