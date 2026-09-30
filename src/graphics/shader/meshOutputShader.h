#ifndef EMULATOR_SRC_GRAPHICS_SHADER_MESHOUTPUTSHADER_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_MESHOUTPUTSHADER_H_

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

struct ShaderPixelInputInfo;

namespace ShaderRecompiler::IR {
struct CompiledShaderInfo;
}

// Legacy: vertex shader that replays the vertex records written by the compute encoding of a
// mesh program. Attribute N is output N of the program. Parameter locations follow the pixel
// shader when one is given; a parameter it reads but the program never exports reads as zero.
std::vector<uint32_t>
BuildMeshOutputVertexShader(const ShaderRecompiler::IR::CompiledShaderInfo& program,
                            const ShaderPixelInputInfo*                      pixel_info);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_MESHOUTPUTSHADER_H_
