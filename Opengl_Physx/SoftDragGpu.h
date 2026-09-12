#pragma once
#include <PxPhysicsAPI.h>
#include <cudamanager/PxCudaContext.h>
#include <cudamanager/PxCudaContextManager.h>
#include <vector>
#include <stdexcept>

class SoftDragGpu
{
public:
	SoftDragGpu(physx::PxCudaContextManager& cuda, const std::vector<physx::PxU32>& ids, const std::vector<physx::PxVec4>& offsets)
		: cuda(cuda), count(static_cast<physx::PxU32>(ids.size()))
	{
		if (ids.empty() || ids.size() != offsets.size()) throw std::invalid_argument("Invalid soft drag patch.");
		try
		{
			physx::PxScopedCudaLock lock(cuda);
			auto* context = cuda.getCudaContext();
			Check(context->moduleLoadDataEx(&module, Kernel(), 0, nullptr, nullptr));
			Check(context->moduleGetFunction(&function, module, "soft_drag"));
			Check(context->memAlloc(&idsD, ids.size() * sizeof(physx::PxU32)));
			Check(context->memAlloc(&offsetsD, offsets.size() * sizeof(physx::PxVec4)));
			Check(context->memcpyHtoD(idsD, ids.data(), ids.size() * sizeof(physx::PxU32)));
			Check(context->memcpyHtoD(offsetsD, offsets.data(), offsets.size() * sizeof(physx::PxVec4)));
		}
		catch (...) { Release(); throw; }
	}

	~SoftDragGpu() { Release(); }
	SoftDragGpu(const SoftDragGpu&) = delete;
	SoftDragGpu& operator=(const SoftDragGpu&) = delete;

	void Apply(physx::PxDeformableVolume& actor, float x, float y, float z, float deltaTime)
	{
		auto positions = reinterpret_cast<CUdeviceptr>(actor.getSimPositionInvMassBufferD());
		auto velocities = reinterpret_cast<CUdeviceptr>(actor.getSimVelocityBufferD());
		void* parameters[] = { &positions,&velocities,&idsD,&offsetsD,&count,&x,&y,&z,&deltaTime };
		{
			physx::PxScopedCudaLock lock(cuda);
			auto* context = cuda.getCudaContext();
			Check(context->launchKernel(function, (count + 63) / 64, 1, 1, 64, 1, 1, 0, nullptr, parameters, nullptr, __FILE__, __LINE__));
			Check(context->streamSynchronize(nullptr));
		}
		actor.markDirty(physx::PxDeformableVolumeDataFlag::eSIM_VELOCITY);
		actor.setWakeCounter(0.4f);
	}

private:
	physx::PxCudaContextManager& cuda;
	physx::PxU32 count = 0;
	CUmodule module = nullptr;
	CUfunction function = nullptr;
	CUdeviceptr idsD = 0, offsetsD = 0;

	static void Check(int result)
	{
		if (result != 0) throw std::runtime_error("GPU soft drag operation failed.");
	}

	void Release()
	{
		physx::PxScopedCudaLock lock(cuda);
		auto* context = cuda.getCudaContext();
		if (idsD) context->memFree(idsD);
		if (offsetsD) context->memFree(offsetsD);
		if (module) context->moduleUnload(module);
		idsD = offsetsD = 0; module = nullptr;
	}

	static const char* Kernel()
	{
		return R"PTX(

.version 8.7
.target sm_75
.address_size 64


.visible .entry soft_drag(
	.param .u64 soft_drag_param_0,
	.param .u64 soft_drag_param_1,
	.param .u64 soft_drag_param_2,
	.param .u64 soft_drag_param_3,
	.param .u32 soft_drag_param_4,
	.param .f32 soft_drag_param_5,
	.param .f32 soft_drag_param_6,
	.param .f32 soft_drag_param_7,
	.param .f32 soft_drag_param_8
)
{
	.reg .pred 	%p<6>;
	.reg .f32 	%f<97>;
	.reg .b32 	%r<7>;
	.reg .b64 	%rd<17>;


	ld.param.u64 	%rd3, [soft_drag_param_0];
	ld.param.u64 	%rd4, [soft_drag_param_1];
	ld.param.u64 	%rd5, [soft_drag_param_2];
	ld.param.u64 	%rd6, [soft_drag_param_3];
	ld.param.u32 	%r2, [soft_drag_param_4];
	ld.param.f32 	%f34, [soft_drag_param_5];
	ld.param.f32 	%f35, [soft_drag_param_6];
	ld.param.f32 	%f36, [soft_drag_param_7];
	ld.param.f32 	%f37, [soft_drag_param_8];
	mov.u32 	%r3, %ctaid.x;
	mov.u32 	%r4, %ntid.x;
	mov.u32 	%r5, %tid.x;
	mad.lo.s32 	%r1, %r3, %r4, %r5;
	setp.ge.u32 	%p1, %r1, %r2;
	@%p1 bra 	$L__BB0_9;

	cvta.to.global.u64 	%rd7, %rd4;
	cvt.u64.u32 	%rd1, %r1;
	cvta.to.global.u64 	%rd8, %rd5;
	mul.wide.u32 	%rd9, %r1, 4;
	add.s64 	%rd10, %rd8, %rd9;
	ld.global.u32 	%r6, [%rd10];
	cvta.to.global.u64 	%rd11, %rd3;
	mul.wide.u32 	%rd12, %r6, 16;
	add.s64 	%rd13, %rd11, %rd12;
	ld.global.v4.f32 	{%f38, %f39, %f40, %f41}, [%rd13];
	add.s64 	%rd2, %rd7, %rd12;
	ld.global.v4.f32 	{%f42, %f43, %f44, %f45}, [%rd2];
	setp.le.f32 	%p2, %f41, 0f00000000;
	@%p2 bra 	$L__BB0_9;

	cvta.to.global.u64 	%rd14, %rd6;
	shl.b64 	%rd15, %rd1, 4;
	add.s64 	%rd16, %rd14, %rd15;
	ld.global.v4.f32 	{%f47, %f48, %f49, %f50}, [%rd16];
	add.f32 	%f51, %f47, %f34;
	sub.f32 	%f16, %f51, %f38;
	add.f32 	%f52, %f48, %f35;
	mov.f32 	%f53, 0f3D23D70A;
	max.f32 	%f54, %f52, %f53;
	sub.f32 	%f17, %f54, %f39;
	add.f32 	%f55, %f49, %f36;
	sub.f32 	%f18, %f55, %f40;
	mul.f32 	%f56, %f17, %f17;
	fma.rn.f32 	%f57, %f16, %f16, %f56;
	fma.rn.f32 	%f58, %f18, %f18, %f57;
	sqrt.rn.f32 	%f19, %f58;
	setp.leu.f32 	%p3, %f19, 0f3F400000;
	mov.f32 	%f95, 0f3F800000;
	mov.f32 	%f94, %f95;
	@%p3 bra 	$L__BB0_4;

	mov.f32 	%f59, 0f3F400000;
	div.rn.f32 	%f94, %f59, %f19;

$L__BB0_4:
	mul.f32 	%f61, %f50, 0f42C80000;
	fma.rn.f32 	%f62, %f61, %f37, 0f3F800000;
	mul.f32 	%f63, %f50, 0f463B8000;
	mul.f32 	%f64, %f63, %f37;
	fma.rn.f32 	%f65, %f64, %f37, %f62;
	mul.f32 	%f66, %f63, %f16;
	mul.f32 	%f67, %f66, %f94;
	add.f32 	%f68, %f61, %f64;
	mul.f32 	%f69, %f42, %f68;
	sub.f32 	%f70, %f67, %f69;
	div.rn.f32 	%f22, %f70, %f65;
	mul.f32 	%f71, %f63, %f17;
	mul.f32 	%f72, %f71, %f94;
	mul.f32 	%f73, %f43, %f68;
	sub.f32 	%f74, %f72, %f73;
	div.rn.f32 	%f23, %f74, %f65;
	mul.f32 	%f75, %f63, %f18;
	mul.f32 	%f76, %f75, %f94;
	mul.f32 	%f77, %f44, %f68;
	sub.f32 	%f78, %f76, %f77;
	div.rn.f32 	%f24, %f78, %f65;
	mul.f32 	%f79, %f23, %f23;
	fma.rn.f32 	%f80, %f22, %f22, %f79;
	fma.rn.f32 	%f81, %f24, %f24, %f80;
	sqrt.rn.f32 	%f25, %f81;
	setp.leu.f32 	%p4, %f25, 0f44960000;
	@%p4 bra 	$L__BB0_6;

	mov.f32 	%f82, 0f44960000;
	div.rn.f32 	%f95, %f82, %f25;

$L__BB0_6:
	mul.f32 	%f84, %f22, %f95;
	fma.rn.f32 	%f28, %f84, %f37, %f42;
	mul.f32 	%f85, %f23, %f95;
	fma.rn.f32 	%f29, %f85, %f37, %f43;
	mul.f32 	%f86, %f24, %f95;
	fma.rn.f32 	%f30, %f86, %f37, %f44;
	mul.f32 	%f87, %f29, %f29;
	fma.rn.f32 	%f88, %f28, %f28, %f87;
	fma.rn.f32 	%f89, %f30, %f30, %f88;
	sqrt.rn.f32 	%f31, %f89;
	setp.leu.f32 	%p5, %f31, 0f41400000;
	mov.f32 	%f96, 0f3F800000;
	@%p5 bra 	$L__BB0_8;

	mov.f32 	%f90, 0f41400000;
	div.rn.f32 	%f96, %f90, %f31;

$L__BB0_8:
	mul.f32 	%f91, %f30, %f96;
	mul.f32 	%f92, %f29, %f96;
	mul.f32 	%f93, %f28, %f96;
	st.global.v4.f32 	[%rd2], {%f93, %f92, %f91, %f45};

$L__BB0_9:
	ret;

}

)PTX";
	}
};
