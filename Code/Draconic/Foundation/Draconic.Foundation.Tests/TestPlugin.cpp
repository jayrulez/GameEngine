// A tiny shared library loaded by the Library (DynamicLibrary) tests.
// Plain C ABI exports so symbol names are unmangled.

#if defined(_WIN32)
#define DRACONIC_PLUGIN_EXPORT __declspec(dllexport)
#else
#define DRACONIC_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C"
{
    DRACONIC_PLUGIN_EXPORT int DraconicTestAdd(int a, int b) { return a + b; }
    DRACONIC_PLUGIN_EXPORT int DraconicTestAnswer() { return 42; }
}
