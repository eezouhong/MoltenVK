#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace melonx::air {
// Add only reassociation/contraction to the Mesa non-exact marker. Retain the
// original AIR type graph, metadata, offsets, and NaN/Inf behavior.
bool relaxMathPermissions(const void* bytes, size_t size, std::vector<uint8_t>& output,
                          std::string& error, size_t* adjusted = nullptr);
}
