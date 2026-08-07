// Single translation unit for the stb_truetype implementation.
// Module units cannot define this macro (static symbol collisions), so the
// implementation lives in a regular .cpp linked into Draconic::FontsTTF.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
