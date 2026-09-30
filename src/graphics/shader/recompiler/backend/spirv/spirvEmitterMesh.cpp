#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t MeshArray(EmitterState& state, spv::StorageClass storage, uint32_t type, uint32_t count) {
	const auto array = state.builder.Type(spv::OpTypeArray, type, ConstantU32(state, count));
	return state.builder.DefineGlobalVariable(TypePointer(state, storage, array), storage);
}

uint32_t MeshElement(EmitterState& state, uint32_t variable, spv::StorageClass storage,
                     uint32_t type, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePointer(state, storage, type), pointer,
	                          variable, index);
	return pointer;
}

uint32_t MeshLoad(EmitterState& state, uint32_t variable, spv::StorageClass storage, uint32_t type,
                  uint32_t index) {
	const auto pointer = MeshElement(state, variable, storage, type, index);
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, type, value, pointer);
	return value;
}

uint32_t MeshOutputType(EmitterState& state, IR::StageOutputKind kind) {
	return kind == IR::StageOutputKind::Layer ? TypeU32(state) : TypeF32Vector(state, 4);
}

// Legacy: where one workgroup of the compute encoding writes its vertices and indices.
struct ComputeOutput {
	uint32_t vertex_address = 0;
	uint32_t index_address  = 0;
	uint32_t vertex_base    = 0;
	uint32_t primitive_base = 0;
};

uint32_t ConstantU64Scalar(EmitterState& state, uint64_t value) {
	return state.builder.Constant(spv::OpConstant, TypeScalarU64(state),
	                              static_cast<uint32_t>(value),
	                              static_cast<uint32_t>(value >> 32u));
}

uint32_t MeshDrawDword(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePushConstantElementPointer(state), pointer,
	                          state.push_constant_variable, ConstantU32(state, 0),
	                          ConstantU32(state, index));
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	return value;
}

uint32_t MeshDrawAddress(EmitterState& state, uint32_t index) {
	const auto type = TypeScalarU64(state);
	const auto low  = Unary(state, spv::OpUConvert, type, MeshDrawDword(state, index));
	const auto high = Unary(state, spv::OpUConvert, type, MeshDrawDword(state, index + 1u));
	return Binary(state, spv::OpBitwiseOr, type, low,
	              Binary(state, spv::OpShiftLeftLogical, type, high, ConstantU64Scalar(state, 32)));
}

void StoreOutputDword(EmitterState& state, uint32_t base, uint32_t dword, uint32_t value) {
	const auto type   = TypeScalarU64(state);
	const auto offset = Binary(state, spv::OpShiftLeftLogical, type,
	                           Unary(state, spv::OpUConvert, type, dword),
	                           ConstantU64Scalar(state, 2));
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer,
	                          Binary(state, spv::OpIAdd, type, base, offset));
	constexpr uint32_t alignment = sizeof(uint32_t);
	state.builder.AddFunction(spv::OpStore, pointer, value, spv::MemoryAccessAlignedMask,
	                          alignment);
}

ComputeOutput PrepareComputeOutput(EmitterState& state) {
	const auto& mesh     = state.input_info.vertex->mesh;
	const auto  group    = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 0);
	const auto  instance = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 1);
	const auto  groups   = MeshDrawDword(state, IR::PushData::MeshComputeGroupCount);
	const auto  slot =
	    EmitAddU32(state, EmitBinaryU32(state, spv::OpIMul, instance, groups), group);
	return {
	    .vertex_address = MeshDrawAddress(state, IR::PushData::MeshComputeVertexAddress),
	    .index_address  = MeshDrawAddress(state, IR::PushData::MeshComputeIndexAddress),
	    .vertex_base =
	        EmitBinaryU32(state, spv::OpIMul, slot, ConstantU32(state, mesh.max_vertices)),
	    .primitive_base =
	        EmitBinaryU32(state, spv::OpIMul, slot, ConstantU32(state, mesh.max_primitives)),
	};
}

} // namespace

void DefineMeshOutputs(EmitterState& state) {
	const auto& mesh = state.input_info.vertex->mesh;
	for (auto& output: state.outputs) {
		if (output.kind != IR::StageOutputKind::Position &&
		    output.kind != IR::StageOutputKind::Parameter &&
		    output.kind != IR::StageOutputKind::Layer) {
			EXIT("unsupported mesh output kind=%u\n", static_cast<uint32_t>(output.kind));
		}
		const auto type    = MeshOutputType(state, output.kind);
		// Only Layer is read by another invocation, through the primitive's provoking vertex.
		const bool shared = output.kind == IR::StageOutputKind::Layer;
		output.mesh_data_variable =
		    MeshArray(state, shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, type,
		              shared ? mesh.max_vertices : state.lane_count);
		if (mesh.via_compute) {
			continue;
		}
		output.variable_id = MeshArray(
		    state, spv::StorageClassOutput, type,
		    output.kind == IR::StageOutputKind::Layer ? mesh.max_primitives : mesh.max_vertices);
		state.interface_variables.push_back(output.variable_id);
		state.builder.AddName(output.variable_id, output.debug_name.c_str());
		if (output.kind == IR::StageOutputKind::Parameter) {
			state.builder.AddAnnotation(spv::OpDecorate, output.variable_id,
			                            spv::DecorationLocation, output.location);
		} else {
			state.builder.AddAnnotation(spv::OpDecorate, output.variable_id, spv::DecorationBuiltIn,
			                            output.kind == IR::StageOutputKind::Layer
			                                ? spv::BuiltInLayer
			                                : spv::BuiltInPosition);
		}
		if (output.kind == IR::StageOutputKind::Layer) {
			state.builder.AddAnnotation(spv::OpDecorate, output.variable_id,
			                            spv::DecorationPerPrimitiveEXT); // PerPrimitiveEXT
		}
	}
	state.mesh_allocation = MeshArray(state, spv::StorageClassWorkgroup, TypeU32(state), 2);
	state.mesh_primitive_data =
	    MeshArray(state, spv::StorageClassPrivate, TypeU32(state), state.lane_count);
	if (mesh.via_compute) {
		return;
	}
	state.mesh_primitives =
	    MeshArray(state, spv::StorageClassOutput, TypeU32Vector(state, 3), mesh.max_primitives);
	state.mesh_cull =
	    MeshArray(state, spv::StorageClassOutput, TypeBool(state), mesh.max_primitives);
	state.interface_variables.push_back(state.mesh_primitives);
	state.interface_variables.push_back(state.mesh_cull);
	state.builder.AddAnnotation(
	    spv::OpDecorate, state.mesh_primitives, spv::DecorationBuiltIn,
	    spv::BuiltInPrimitiveTriangleIndicesEXT); // PrimitiveTriangleIndicesEXT
	state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationBuiltIn,
	                            spv::BuiltInCullPrimitiveEXT); // CullPrimitiveEXT
	state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationPerPrimitiveEXT);
}

uint32_t MeshOutputPointer(EmitterState& state, IR::StageOutputKind kind, uint32_t index) {
	const auto output = std::ranges::find_if(state.outputs, [=](const OutputBinding& binding) {
		return binding.kind == kind && binding.index == index;
	});
	if (output == state.outputs.end()) {
		EXIT("mesh export has no output binding: kind=%u index=%u\n", static_cast<uint32_t>(kind),
		     index);
	}
	const bool shared = kind == IR::StageOutputKind::Layer;
	return MeshElement(
	    state, output->mesh_data_variable,
	    shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, MeshOutputType(state, kind),
	    shared ? EmitLocalInvocationIndex(state) : ConstantU32(state, state.lane_half));
}

uint32_t MeshPrimitivePointer(EmitterState& state) {
	return MeshElement(state, state.mesh_primitive_data, spv::StorageClassPrivate, TypeU32(state),
	                   ConstantU32(state, state.lane_half));
}

void EmitMeshAllocate(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state = ctx.state;
	const auto first = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpIEqual, TypeBool(state), first,
	                          EmitLocalInvocationIndex(state), ConstantU32(state, 0));
	EmitIfCondition(state, first, [&] {
		const auto allocation = ctx.Arg(inst, 0);
		for (uint32_t field = 0; field < 2; field++) {
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), value, allocation,
			                          ConstantU32(state, field * 12u),
			                          ConstantU32(state, field == 0 ? 10u : 11u));
			const auto pointer =
			    MeshElement(state, state.mesh_allocation, spv::StorageClassWorkgroup,
			                TypeU32(state), ConstantU32(state, field));
			state.builder.AddFunction(spv::OpStore, pointer, value);
		}
	});
}

void EmitMeshEntryPoint(EmitterState& state) {
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.builder.AllocateId());
	state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state), state.builder.AllocateId(),
	                          state.mesh_guest_func);
	// All guest waves finish before the uniform Vulkan allocation and output stores.
	EmitBarrier(state);
	const auto vertices   = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 0));
	const auto primitives = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 1));
	const bool    via_compute = MeshViaCompute(state);
	ComputeOutput compute;
	if (via_compute) {
		compute = PrepareComputeOutput(state);
	} else {
		state.builder.AddFunction(spv::OpSetMeshOutputsEXT, vertices,
		                          primitives); // OpSetMeshOutputsEXT
	}
	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half      = half;
		const auto index     = EmitLocalInvocationIndex(state);
		const auto is_vertex = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_vertex, index, vertices);
		EmitIfCondition(state, is_vertex, [&] {
			if (via_compute) {
				const auto record = EmitBinaryU32(
				    state, spv::OpIMul, EmitAddU32(state, compute.vertex_base, index),
				    ConstantU32(state, static_cast<uint32_t>(state.outputs.size()) *
				                           ShaderMeshInputInfo::ComputeOutputDwords));
				uint32_t dword = 0;
				for (const auto& output: state.outputs) {
					const auto at = [&](uint32_t component) {
						return EmitAddU32(state, record, ConstantU32(state, dword + component));
					};
					if (output.kind == IR::StageOutputKind::Layer) {
						StoreOutputDword(state, compute.vertex_address, at(0),
						                 MeshLoad(state, output.mesh_data_variable,
						                          spv::StorageClassWorkgroup, TypeU32(state),
						                          index));
					} else {
						const auto value =
						    MeshLoad(state, output.mesh_data_variable, spv::StorageClassPrivate,
						             TypeF32Vector(state, 4), ConstantU32(state, half));
						for (uint32_t component = 0; component < 4; component++) {
							const auto f32 = state.builder.AllocateId();
							state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), f32,
							                          value, component);
							StoreOutputDword(state, compute.vertex_address, at(component),
							                 EmitBitcastF32ToU32(state, f32));
						}
					}
					dword += ShaderMeshInputInfo::ComputeOutputDwords;
				}
				return;
			}
			for (const auto& output: state.outputs) {
				if (output.kind == IR::StageOutputKind::Layer) {
					continue;
				}
				const auto type  = MeshOutputType(state, output.kind);
				const auto value =
				    MeshLoad(state, output.mesh_data_variable, spv::StorageClassPrivate, type,
				             ConstantU32(state, half));
				const auto pointer =
				    MeshElement(state, output.variable_id, spv::StorageClassOutput, type, index);
				state.builder.AddFunction(spv::OpStore, pointer, value);
			}
		});
		if (via_compute) {
			// Every primitive slot of the workgroup gets a triple: the lane's own primitive when
			// it is allocated and visible, otherwise the degenerate triangle (base, base, base).
			// Nothing has to be cleared before the dispatch.
			const auto& mesh    = state.input_info.vertex->mesh;
			const auto  threads = mesh.threads_num[0];
			const auto  packed  = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                               TypeU32(state), ConstantU32(state, half));
			const auto  null_bit =
			    EmitBinaryU32(state, spv::OpBitwiseAnd, packed, ConstantU32(state, 0x80000000u));
			const auto visible = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpIEqual, TypeBool(state), visible, null_bit,
			                          ConstantU32(state, 0));
			const auto allocated = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpULessThan, TypeBool(state), allocated, index,
			                          primitives);
			const auto live = EmitLogicalAndBool(state, allocated, visible);
			uint32_t   corner[3] {};
			for (uint32_t component = 0; component < 3; component++) {
				const auto vertex = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), vertex, packed,
				                          ConstantU32(state, component * 10u),
				                          ConstantU32(state, 10));
				corner[component] =
				    Select(state, TypeU32(state), live,
				           EmitAddU32(state, compute.vertex_base, vertex), compute.vertex_base);
			}
			for (uint32_t pass = 0; pass * threads < mesh.max_primitives; pass++) {
				const auto slot = pass == 0 ? index
				                            : EmitAddU32(state, index,
				                                         ConstantU32(state, pass * threads));
				const auto write = [&] {
					const auto first = EmitBinaryU32(
					    state, spv::OpIMul, EmitAddU32(state, compute.primitive_base, slot),
					    ConstantU32(state, 3));
					for (uint32_t component = 0; component < 3; component++) {
						StoreOutputDword(state, compute.index_address,
						                 EmitAddU32(state, first, ConstantU32(state, component)),
						                 pass == 0 ? corner[component] : compute.vertex_base);
					}
				};
				if ((pass + 1u) * threads <= mesh.max_primitives) {
					write();
				} else {
					const auto in_range = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpULessThan, TypeBool(state), in_range, slot,
					                          ConstantU32(state, mesh.max_primitives));
					EmitIfCondition(state, in_range, write);
				}
			}
			continue;
		}
		const auto is_primitive = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_primitive, index,
		                          primitives);
		EmitIfCondition(state, is_primitive, [&] {
			const auto packed = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, half));
			uint32_t   vertex[3] {};
			for (uint32_t component = 0; component < 3; component++) {
				vertex[component] = state.builder.AllocateId();
				state.builder.AddFunction(
				    spv::OpBitFieldUExtract, TypeU32(state), vertex[component], packed,
				    ConstantU32(state, component * 10u), ConstantU32(state, 10));
			}
			const auto triangle = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 3), triangle,
			                          vertex[0], vertex[1], vertex[2]);
			const auto triangle_pointer =
			    MeshElement(state, state.mesh_primitives, spv::StorageClassOutput,
			                TypeU32Vector(state, 3), index);
			state.builder.AddFunction(spv::OpStore, triangle_pointer, triangle);
			const auto null_bit =
			    EmitBinaryU32(state, spv::OpBitwiseAnd, packed, ConstantU32(state, 0x80000000u));
			const auto culled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), culled, null_bit,
			                          ConstantU32(state, 0));
			state.builder.AddFunction(spv::OpStore,
			                          MeshElement(state, state.mesh_cull, spv::StorageClassOutput,
			                                      TypeBool(state), index),
			                          culled);
			for (const auto& output: state.outputs) {
				if (output.kind != IR::StageOutputKind::Layer) {
					continue;
				}
				const auto layer = MeshLoad(state, output.mesh_data_variable,
				                            spv::StorageClassWorkgroup, TypeU32(state),
				                            vertex[state.input_info.vertex->mesh.provoking_vertex]);
				const auto pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput,
				                                 TypeU32(state), index);
				state.builder.AddFunction(spv::OpStore, pointer, layer);
			}
		});
	}
	state.lane_half = 0;
	state.builder.AddFunction(spv::OpReturn);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
