#include "ffx_types.h"

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#include <imgui.h>
#include <reshade/reshade.hpp>

typedef int32_t FfxErrorCode;

typedef struct FfxFsr2Interface
{
	void* fpCreateBackendContext;    ///< A callback function to create and initialize the backend context.
	void* fpGetDeviceCapabilities;   ///< A callback function to query device capabilites.
	void* fpDestroyBackendContext;   ///< A callback function to destroy the backendcontext. This also dereferences the device.
	void* fpCreateResource;          ///< A callback function to create a resource.
	void* fpRegisterResource;        ///< A callback function to register an external resource.
	void* fpUnregisterResources;     ///< A callback function to unregister external resource.
	void* fpGetResourceDescription;  ///< A callback function to retrieve a resource description.
	void* fpDestroyResource;         ///< A callback function to destroy a resource.
	void* fpCreatePipeline;          ///< A callback function to create a render or compute pipeline.
	void* fpDestroyPipeline;         ///< A callback function to destroy a render or compute pipeline.
	void* fpScheduleGpuJob;          ///< A callback function to schedule a render job.
	void* fpExecuteGpuJobs;          ///< A callback function to execute all queued render jobs.

	void* scratchBuffer;       ///< A preallocated buffer for memory utilized internally by the backend.
	size_t scratchBufferSize;  ///< Size of the buffer pointed to by <c><i>scratchBuffer</i></c>.
} FfxFsr2Interface;

typedef struct FfxFsr2ContextDescription
{
	uint32_t flags;                 ///< A collection of <c><i>FfxFsr2InitializationFlagBits</i></c>.
	FfxDimensions2D maxRenderSize;  ///< The maximum size that rendering will be performed at.
	FfxDimensions2D displaySize;    ///< The size of the presentation resolution targeted by the upscaling process.
	FfxFsr2Interface callbacks;     ///< A set of pointers to the backend implementation for FSR 2.0.
	FfxDevice device;               ///< The abstracted device which is passed to some callback functions.

	void* fpMessage;  ///< A pointer to a function that can recieve messages from the runtime.
} FfxFsr2ContextDescription;

typedef struct FfxFsr2DispatchDescription
{
	FfxCommandList commandList;              ///< The <c><i>FfxCommandList</i></c> to record FSR2 rendering commands into.
	FfxResource color;                       ///< A <c><i>FfxResource</i></c> containing the color buffer for the current frame (at render resolution).
	FfxResource depth;                       ///< A <c><i>FfxResource</i></c> containing 32bit depth values for the current frame (at render resolution).
	FfxResource motionVectors;               ///< A <c><i>FfxResource</i></c> containing 2-dimensional motion vectors (at render resolution if <c><i>FFX_FSR2_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS</i></c> is not set).
	FfxResource exposure;                    ///< A optional <c><i>FfxResource</i></c> containing a 1x1 exposure value.
	FfxResource reactive;                    ///< A optional <c><i>FfxResource</i></c> containing alpha value of reactive objects in the scene.
	FfxResource transparencyAndComposition;  ///< A optional <c><i>FfxResource</i></c> containing alpha value of special objects in the scene.
	FfxResource output;                      ///< A <c><i>FfxResource</i></c> containing the output color buffer for the current frame (at presentation resolution).
	FfxFloatCoords2D jitterOffset;           ///< The subpixel jitter offset applied to the camera.
	FfxFloatCoords2D motionVectorScale;      ///< The scale factor to apply to motion vectors.
	FfxDimensions2D renderSize;              ///< The resolution that was used for rendering the input resources.
	bool enableSharpening;                   ///< Enable an additional sharpening pass.
	float sharpness;                         ///< The sharpness value between 0 and 1, where 0 is no additional sharpness and 1 is maximum additional sharpness.
	float frameTimeDelta;                    ///< The time elapsed since the last frame (expressed in milliseconds).
	float preExposure;                       ///< The pre exposure value (must be > 0.0f)
	bool reset;                              ///< A boolean value which when set to true, indicates the camera has moved discontinuously.
	float cameraNear;                        ///< The distance to the near plane of the camera.
	float cameraFar;                         ///< The distance to the far plane of the camera.
	float cameraFovAngleVertical;            ///< The camera angle field of view in the vertical direction (expressed in radians).
	float viewSpaceToMetersFactor;           ///< The scale factor to convert view space units to meters

	// EXPERIMENTAL reactive mask generation parameters
	bool enableAutoReactive;      ///< A boolean value to indicate internal reactive autogeneration should be used
	FfxResource colorOpaqueOnly;  ///< A <c><i>FfxResource</i></c> containing the opaque only color buffer for the current frame (at render resolution).
	float autoTcThreshold;        ///< Cutoff value for TC
	float autoTcScale;            ///< A value to scale the transparency and composition mask
	float autoReactiveScale;      ///< A value to scale the reactive mask
	float autoReactiveMax;        ///< A value to clamp the reactive mask
} FfxFsr2DispatchDescription;

static float* fMipBias = nullptr;

HMODULE _hModule;
FfxDimensions2D _displaySize;
bool _forceDisable = false;
bool _registeredAddon = false;

void DrawMenu(reshade::api::effect_runtime*)
{
	ImGui::Text(std::format("Current fMipBias {}", *fMipBias).c_str());
	ImGui::Checkbox("Disable (for testing only)", &_forceDisable);
}

void AdjustBias(FfxFsr2DispatchDescription* dispatchParams)
{
	float renderResolutionX = dispatchParams->renderSize.width;
	float displayResolutionX = _displaySize.width;

	float bias = log2(renderResolutionX / displayResolutionX);
	float clampedBias = std::clamp(bias, -10.0f, 0.0f);

	static bool erroredBefore = false;
	if (bias != clampedBias && !erroredBefore) {
		erroredBefore = true;
		ERROR("Upscaling Fix BAD VALUE : renderResolutionX {} displayResolutionX {} bias {}", renderResolutionX, displayResolutionX, bias);
	}

	*fMipBias = clampedBias;

	if (_forceDisable)
		*fMipBias = 0;
}

FfxErrorCode ffxFsr2ContextCreate_hook(void* context, FfxFsr2ContextDescription* contextDescription);

decltype(&ffxFsr2ContextCreate_hook) ffxFsr2ContextCreate_original;

FfxErrorCode ffxFsr2ContextCreate_hook(void* context, FfxFsr2ContextDescription* contextDescription)
{
	_displaySize = contextDescription->displaySize;
	INFO("Initial displaySize {} {}", _displaySize.width, _displaySize.height);

	if (!_registeredAddon) {
		_registeredAddon = true;

		if (reshade::register_addon(_hModule)) {
			INFO("Registered ReShade addon, adding menu");
			reshade::register_overlay(nullptr, &DrawMenu);
		} else {
			INFO("Failed to register ReShade addon, not adding menu");
		}
	}

	return (ffxFsr2ContextCreate_original)(context, contextDescription);
}

FfxErrorCode ffxFsr2ContextDispatch_hook(void* context, FfxFsr2DispatchDescription* dispatchParams);

decltype(&ffxFsr2ContextDispatch_hook) ffxFsr2ContextDispatch_original;

FfxErrorCode ffxFsr2ContextDispatch_hook(void* context, FfxFsr2DispatchDescription* dispatchParams)
{
	AdjustBias(dispatchParams);
	return (ffxFsr2ContextDispatch_original)(context, dispatchParams);
}

void AddINISetting_fMipBias_hook(void* setting, char* name_section);

decltype(&AddINISetting_fMipBias_hook) AddINISetting_fMipBias_original;

void AddINISetting_fMipBias_hook(void* setting, char* name_section)
{
	fMipBias = reinterpret_cast<float*>(AsAddress(setting) + 8);
	INFO("Found fMipBias at {:X}", AsAddress(fMipBias) - dku::Hook::Module::get().base() + 0x140000000);
	return (AddINISetting_fMipBias_original)(setting, name_section);
}

extern "C" DLLEXPORT const char* NAME = "Upscaling Fix for Starfield";
extern "C" DLLEXPORT const char* DESCRIPTION = "";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID)
{
	if (dwReason == DLL_PROCESS_ATTACH) {
#ifndef NDEBUG
		while (!IsDebuggerPresent()) {
			Sleep(100);
		}
#endif
		_hModule = hModule;

		DKUtil::Logger::Init(Plugin::NAME, std::to_string(Plugin::Version));

		INFO("{} v{} loaded", Plugin::NAME, Plugin::Version);

		dku::Hook::Trampoline::AllocTrampoline(42);

		{
			const auto scan = static_cast<uint8_t*>(dku::Hook::Assembly::search_pattern<"E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 83 C4 28 E9 ?? ?? ?? ?? CC CC CC CC CC 48 83 EC 18">());
			if (!scan) {
				ERROR("Failed to find AddINISetting_fMipBias_hook!")
			}
			AddINISetting_fMipBias_original = dku::Hook::write_call<5>(AsAddress(scan), AddINISetting_fMipBias_hook);
			INFO("Found AddINISetting_fMipBias_hook at {:X}", AsAddress(scan) - dku::Hook::Module::get().base() + 0x140000000);
		}
			
		{
			const auto scan = static_cast<uint8_t*>(dku::Hook::Assembly::search_pattern<"48 8B 49 10 E8 ?? ?? ?? ?? 48 81 C4 ?? ?? ?? ??">());
			if (!scan) {
				ERROR("Failed to find ffxFsr2ContextCreate!")
			}
			ffxFsr2ContextCreate_original = dku::Hook::write_call<5>(AsAddress(scan) + 0x4, ffxFsr2ContextCreate_hook);
			INFO("Found ffxFsr2ContextCreate at {:X}", AsAddress(scan) + 0x4 - dku::Hook::Module::get().base() + 0x140000000);
		}
					
		{
			const auto scan = static_cast<uint8_t*>(dku::Hook::Assembly::search_pattern<"89 9D 20 07 00 00 88 85 38 07 00 00 E8 ?? ?? ?? ??">());
			if (!scan) {
				ERROR("Failed to find ffxFsr2ContextDispatch!")
			}
			ffxFsr2ContextDispatch_original = dku::Hook::write_call<5>(AsAddress(scan) + 0xC, ffxFsr2ContextDispatch_hook);
			INFO("Found ffxFsr2ContextDispatch at {:X}", AsAddress(scan) + 0xC - dku::Hook::Module::get().base() + 0x140000000);
		}

	}
	return TRUE;
}