#include "graphics/shader/meshOutputShader.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::IR::StageOutputKind;

// One vec4 output of the generated shader: a copy of a record attribute, or zero when the
// pixel shader reads a parameter the mesh program never exports.
struct ParameterOutput {
	uint32_t location  = 0;
	uint32_t attribute = UINT32_MAX;
};

std::vector<ParameterOutput> PlanParameterOutputs(
    const ShaderRecompiler::IR::CompiledShaderInfo& program,
    const ShaderPixelInputInfo*                      pixel_info) {
	const auto& outputs   = program.info.outputs;
	const auto  attribute = [&](uint32_t parameter) {
		for (uint32_t index = 0; index < outputs.size(); index++) {
			if (outputs[index].kind == StageOutputKind::Parameter &&
			    outputs[index].index == parameter) {
				return index;
			}
		}
		return UINT32_MAX;
	};
	std::vector<ParameterOutput> plan;
	std::array<bool, 64>         used {};
	if (pixel_info != nullptr && pixel_info->stage.program != nullptr) {
		// The pixel shader decides which location carries which parameter.
		std::vector<uint32_t> active_inputs;
		for (const auto& input: pixel_info->stage.program->info.inputs) {
			if (input.kind == ShaderRecompiler::IR::StageInputKind::Parameter) {
				active_inputs.push_back(input.location);
			}
		}
		for (const auto input: active_inputs) {
			const auto location = ShaderPixelParameterLocation(*pixel_info, active_inputs, input);
			EXIT_IF(location >= used.size());
			if (!used[location]) {
				used[location] = true;
				plan.push_back({location,
				                attribute(ShaderPixelParameterMappedLocation(*pixel_info, input))});
			}
		}
	}
	for (uint32_t index = 0; index < outputs.size(); index++) {
		const auto& output = outputs[index];
		if (output.kind == StageOutputKind::Parameter && output.location < used.size() &&
		    !used[output.location]) {
			used[output.location] = true;
			plan.push_back({output.location, index});
		}
	}
	return plan;
}

} // namespace

std::vector<uint32_t>
BuildMeshOutputVertexShader(const ShaderRecompiler::IR::CompiledShaderInfo& program,
                            const ShaderPixelInputInfo*                      pixel_info) {
	using ShaderRecompiler::Spirv::Builder;

	const auto& outputs   = program.info.outputs;
	const bool  has_layer = std::ranges::any_of(
	    outputs, [](const auto& output) { return output.kind == StageOutputKind::Layer; });

	Builder builder(has_layer ? 0x00010500u : 0x00010300u);
	builder.RequireCapability(spv::CapabilityShader);
	if (has_layer) {
		builder.RequireCapability(spv::CapabilityShaderLayer);
	}
	builder.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);

	const auto void_type     = builder.Type(spv::OpTypeVoid);
	const auto uint_type     = builder.Type(spv::OpTypeInt, 32u, 0u);
	const auto int_type      = builder.Type(spv::OpTypeInt, 32u, 1u);
	const auto float_type    = builder.Type(spv::OpTypeFloat, 32u);
	const auto vec4_type     = builder.Type(spv::OpTypeVector, float_type, 4u);
	const auto function_type = builder.Type(spv::OpTypeFunction, void_type);
	const auto pointer       = [&](spv::StorageClass storage, uint32_t type) {
		return builder.Type(spv::OpTypePointer, storage, type);
	};
	const auto variable = [&](spv::StorageClass storage, uint32_t type) {
		return builder.DefineGlobalVariable(pointer(storage, type), storage);
	};

	std::vector<uint32_t> interfaces;
	std::vector<uint32_t> attributes(outputs.size());
	for (uint32_t index = 0; index < outputs.size(); index++) {
		const auto type   = outputs[index].kind == StageOutputKind::Layer ? uint_type : vec4_type;
		attributes[index] = variable(spv::StorageClassInput, type);
		builder.AddAnnotation(spv::OpDecorate, attributes[index], spv::DecorationLocation, index);
		interfaces.push_back(attributes[index]);
	}

	struct Copy {
		uint32_t type   = 0;
		uint32_t source = 0; // 0: store zero
		uint32_t target = 0;
		bool     member = false;
	};
	std::vector<Copy> copies;
	for (uint32_t index = 0; index < outputs.size(); index++) {
		const auto& output = outputs[index];
		switch (output.kind) {
			case StageOutputKind::Position: {
				const auto per_vertex = builder.DecoratedType(
				    spv::OpTypeStruct,
				    {{spv::OpMemberDecorate, {0u, spv::DecorationBuiltIn, spv::BuiltInPosition}},
				     {spv::OpDecorate, {spv::DecorationBlock}}},
				    vec4_type);
				copies.push_back({vec4_type, attributes[index],
				                  variable(spv::StorageClassOutput, per_vertex), true});
				break;
			}
			case StageOutputKind::Layer: {
				const auto target = variable(spv::StorageClassOutput, uint_type);
				builder.AddAnnotation(spv::OpDecorate, target, spv::DecorationBuiltIn,
				                      spv::BuiltInLayer);
				copies.push_back({uint_type, attributes[index], target});
				break;
			}
			case StageOutputKind::Parameter: break; // planned below
			default: EXIT("unsupported mesh output kind=%u\n", static_cast<uint32_t>(output.kind));
		}
	}
	const auto plan = PlanParameterOutputs(program, pixel_info);
	for (const auto& parameter: plan) {
		const auto target = variable(spv::StorageClassOutput, vec4_type);
		builder.AddAnnotation(spv::OpDecorate, target, spv::DecorationLocation,
		                      parameter.location);
		copies.push_back({vec4_type,
		                  parameter.attribute == UINT32_MAX ? 0u : attributes[parameter.attribute],
		                  target});
	}
	for (const auto& copy: copies) {
		interfaces.push_back(copy.target);
	}

	const auto main = builder.AllocateId();
	builder.AddName(main, "main");
	builder.AddFunction(spv::OpFunction, void_type, main, spv::FunctionControlMaskNone,
	                    function_type);
	builder.AddFunction(spv::OpLabel, builder.AllocateId());
	for (const auto& copy: copies) {
		uint32_t value = 0;
		if (copy.source != 0) {
			value = builder.AllocateId();
			builder.AddFunction(spv::OpLoad, copy.type, value, copy.source);
		} else {
			value = builder.Constant(spv::OpConstantNull, copy.type);
		}
		auto target = copy.target;
		if (copy.member) {
			target = builder.AllocateId();
			builder.AddFunction(spv::OpAccessChain, pointer(spv::StorageClassOutput, copy.type),
			                    target, copy.target,
			                    builder.Constant(spv::OpConstant, int_type, 0u));
		}
		builder.AddFunction(spv::OpStore, target, value);
	}
	builder.AddFunction(spv::OpReturn);
	builder.AddFunction(spv::OpFunctionEnd);
	builder.AddEntryPoint(spv::ExecutionModelVertex, main, "main", interfaces);
	return builder.Build();
}

} // namespace Libs::Graphics
