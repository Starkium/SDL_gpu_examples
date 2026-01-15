#include "Common.h"

#include <SDL3/SDL_openxr.h>

typedef struct Swapchain {
	XrSwapchain swapchain;
	SDL_GPUTexture **images;
	XrExtent2Di size;
	SDL_GPUTextureFormat format;
} Swapchain;

static XrInstance instance = NULL;
static XrSystemId systemId = 0;
static XrSession session = NULL;
static bool doXrFrameLoop = false;
static XrSpace localSpace = NULL;

static Swapchain *swapchains = NULL;
static XrView *views = NULL;
static Uint32 viewCount = 0;

static int Init(Context* context)
{
	XrResult result;

	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_ENABLE_BOOLEAN, true);
	SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_INSTANCE_POINTER, &instance);
	SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_SYSTEM_ID_POINTER, &systemId);

	context->Device = SDL_CreateGPUDeviceWithProperties(props);
	if (!context->Device)
	{
		SDL_Log("SDL_CreateXRGPUDeviceWithProperties failed, reason %s", SDL_GetError());
		return -1;
	}

	SDL_Log("DEBUG: XR instance = %p, systemId = %llu", (void*)instance, (unsigned long long)systemId);

	XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
	XR_ERR_RET(SDL_CreateGPUXRSession(context->Device, &sessionCreateInfo, &session), -1);

	context->Window = SDL_CreateWindow(context->ExampleName, 640, 480, SDL_WINDOW_RESIZABLE);
	if (context->Window == NULL)
	{
		SDL_Log("CreateWindow failed: %s", SDL_GetError());
		return -1;
	}

	if (!SDL_ClaimWindowForGPUDevice(context->Device, context->Window))
	{
		SDL_Log("GPUClaimWindow failed");
		return -1;
	}

	return 0;
}

static int HandleStateChangedEvent(Context* context, XrEventDataSessionStateChanged* event)
{
	switch(event->state)
	{
		case XR_SESSION_STATE_READY:
		{
			XrSessionBeginInfo sessionBeginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
			sessionBeginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			XR_ERR_RET(xrBeginSession(session, &sessionBeginInfo), -1);

			SDL_Log("Begun OpenXR session");
			
			XR_ERR_RET(xrEnumerateViewConfigurationViews(
				instance, 
				systemId, 
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 
				0, 
				&viewCount, 
				NULL), -1);

			XrViewConfigurationView *view_configuration_views = SDL_stack_alloc(XrViewConfigurationView, viewCount);
			for (Uint32 i = 0; i < viewCount; i++) view_configuration_views[i] = (XrViewConfigurationView){XR_TYPE_VIEW_CONFIGURATION_VIEW};
			
			XR_ERR_RET(xrEnumerateViewConfigurationViews(
				instance, 
				systemId, 
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 
				viewCount, 
				&viewCount, 
				view_configuration_views), -1);

			if(viewCount > 0) {
				swapchains = SDL_calloc(viewCount, sizeof(Swapchain));
				views = SDL_calloc(viewCount, sizeof(XrView));

				for(Uint32 i = 0; i < viewCount; i++)
				{
					views[i] = (XrView){XR_TYPE_VIEW, .pose = {.orientation = IDENTITY_QUAT}}; // Init the orientation to an identity, so it's always a valid quaternion

					Swapchain *swapchain = &swapchains[i];

					XrViewConfigurationView view = view_configuration_views[i];

					SDL_Log(
						"%d max width: %d, max height: %d, max sample count: %d, rec width: %d, rec height: %d, rec sample count: %d",
						i, 
						view.maxImageRectWidth, 
						view.maxImageRectHeight, 
						view.maxSwapchainSampleCount, 
						view.recommendedImageRectWidth, 
						view.recommendedImageRectHeight, 
						view.recommendedSwapchainSampleCount);

					XrSwapchainCreateInfo swapchainCreateInfo = {
						.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
						.width = view.recommendedImageRectWidth,
						.height = view.recommendedImageRectHeight,
						.mipCount = 1,
						.sampleCount = 1,
						.faceCount = 1,
						.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, /* sure..? not sure if anything else is needed. */
						.arraySize = 1};

					swapchain->size = (XrExtent2Di){swapchainCreateInfo.width, swapchainCreateInfo.height};

					XR_ERR_RET(SDL_CreateGPUXRSwapchain(
						context->Device, 
						session, 
						&swapchainCreateInfo, 
						&swapchain->format, 
						&swapchain->swapchain, 
						&swapchain->images), -1);
				}
			}

			SDL_stack_free(view_configuration_views);

			doXrFrameLoop = true;

			XR_ERR_RET(xrCreateReferenceSpace(session, &(XrReferenceSpaceCreateInfo){
				.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
				.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
				.poseInReferenceSpace = IDENTITY_POSE,
			}, &localSpace), -1);

			break;
		}
		case XR_SESSION_STATE_STOPPING:
		{
			doXrFrameLoop = false;

			XR_ERR_RET(xrEndSession(session), -1);

			SDL_Log("Ended OpenXR session");

			break;
		}
		case XR_SESSION_STATE_EXITING:
		{
			/* TODO: handle this gracefully */

			SDL_Log("Session is exiting");

			return -1;
		}
	}

	return 0;
}

static int HandleXrEvent(Context* context) 
{
	XrResult result;
	XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
	XR_ERR_RET(result = xrPollEvent(instance, &event), -1);
	if(result == XR_SUCCESS) {
		switch(event.type)
		{
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: 
			{
				XrEventDataSessionStateChanged *stateChangedEvent = ((XrEventDataSessionStateChanged *)&event);

				if(HandleStateChangedEvent(context, stateChangedEvent) != 0) return -1;

				break;
			}
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			{
				XrEventDataInstanceLossPending instanceLossPending = *((XrEventDataInstanceLossPending *)&event);

				/* TODO: handle this gracefully, probably by re-initializing the GPU device/instance */

				SDL_Log("Instance loss pending, bailing out..");

				return -1;

				break;
			}
		}
	}

	return 0;
}

static int Update(Context* context)
{
	XrResult result;

	if(HandleXrEvent(context) != 0) return -1;

	return 0;
}

static int RenderView(Context *context, SDL_GPUCommandBuffer *cmdbuf, SDL_GPUTexture *texture, XrView view)
{
	SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(cmdbuf, &(SDL_GPUColorTargetInfo){
		.texture = texture,
		.clear_color = {0.5, 1, 0.5, 1},
		.load_op = SDL_GPU_LOADOP_CLEAR,
		.store_op = SDL_GPU_STOREOP_STORE,
	}, 1, NULL);

	SDL_EndGPURenderPass(render_pass);

	return 0;
}

static int RenderDesktopView(Context *context, SDL_GPUCommandBuffer *cmdbuf)
{
	SDL_GPUTexture *swapchainTexture;
	SDL_AcquireGPUSwapchainTexture(cmdbuf, context->Window, &swapchainTexture, NULL, NULL);

	XrView view = {XR_TYPE_VIEW, .pose = IDENTITY_POSE};
	if(viewCount > 0) view = views[0];

	// Render the desktop view with the first view
	return RenderView(context, cmdbuf, swapchainTexture, view);
}

static int Draw(Context* context)
{
	SDL_GPUCommandBuffer* cmdbuf = SDL_AcquireGPUCommandBuffer(context->Device);
	if (cmdbuf == NULL)
	{
		SDL_Log("AcquireGPUCommandBuffer failed: %s", SDL_GetError());
		return -1;
	}

	Uint32 viewCountOutput = 0;
	if(doXrFrameLoop) 
	{
		XrFrameWaitInfo frameWaitInfo = {XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState frameState = {XR_TYPE_FRAME_STATE};
		// Wait for the next frame
		XR_ERR_RET(xrWaitFrame(session, &frameWaitInfo, &frameState), -1);

		// Begin a new frame
		XrFrameBeginInfo frameBeginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
		XR_ERR_RET(xrBeginFrame(session, &frameBeginInfo), -1);

		// If we need to render, fill out the projection views for each eye
		// Use alloca for MSVC compatibility (doesn't support C99 VLAs)
		XrCompositionLayerProjectionView *projectionViews = (XrCompositionLayerProjectionView *)SDL_stack_alloc(XrCompositionLayerProjectionView, viewCount);

		if(frameState.shouldRender)
		{
			XrViewState viewState = {XR_TYPE_VIEW_STATE};
			XR_ERR_RET(xrLocateViews(session, &(XrViewLocateInfo){
				.type = XR_TYPE_VIEW_LOCATE_INFO,
				.displayTime = frameState.predictedDisplayTime,
				.space = localSpace,
				.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			}, &viewState, viewCount, &viewCountOutput, views), -1);

			for(Uint32 i = 0; i < viewCountOutput; i++) {
				Swapchain swapchain = swapchains[i];

				Uint32 swapchainIndex;
				XR_ERR_RET(xrAcquireSwapchainImage(swapchain.swapchain, NULL, &swapchainIndex), -1);

				XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				waitInfo.timeout = XR_INFINITE_DURATION; /* spec says the runtime *must* never block indefinitely, so this is always safe! but maybe we *should* tune this? not sure */
				XR_ERR_RET(xrWaitSwapchainImage(swapchain.swapchain, &waitInfo), -1);

				/* we got the texture we're going to render with! */
				SDL_GPUTexture *swapchain_texture = swapchain.images[swapchainIndex];

				if(RenderView(context, cmdbuf, swapchain_texture, views[i]) != 0) return -1;
			}

			// Always render desktop view
			RenderDesktopView(context, cmdbuf);

			SDL_SubmitGPUCommandBuffer(cmdbuf);

			for(Uint32 i = 0; i < viewCountOutput; i++) {
				Swapchain swapchain = swapchains[i];

				XR_ERR_RET(xrReleaseSwapchainImage(swapchains[i].swapchain, NULL), -1);

				projectionViews[i] = (XrCompositionLayerProjectionView){XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
					.fov = views[i].fov,
					.pose = views[i].pose,
					.subImage = {
						.swapchain = swapchain.swapchain,
						.imageArrayIndex = 0,
						.imageRect = {.offset = {0}, .extent = swapchain.size},
					}
				};
			}
		} 
		// Even if we shouldn't be rendering to the VR views, lets still render the desktop view
		else {
			if(RenderDesktopView(context, cmdbuf) != 0) return -1;

			SDL_SubmitGPUCommandBuffer(cmdbuf);
		}

		XrFrameEndInfo frameEndInfo = {XR_TYPE_FRAME_END_INFO};
		frameEndInfo.displayTime = frameState.predictedDisplayTime;
		frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

		frameEndInfo.layerCount = frameState.shouldRender ? 1 : 0;

		const XrCompositionLayerBaseHeader *projectionLayers[1];
		projectionLayers[0] = (const XrCompositionLayerBaseHeader*) &(XrCompositionLayerProjection){
			.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
			.space = localSpace,
			.viewCount = viewCountOutput,
			.views = projectionViews,
		};
		frameEndInfo.layers = projectionLayers;

		XR_ERR_RET(xrEndFrame(session, &frameEndInfo), -1);
		SDL_stack_free(projectionViews);
	} 
	// If we aren't in the OpenXR frame loop, let's still render to the desktop view in our own frame loop
	else {
		if(RenderDesktopView(context, cmdbuf) != 0) return -1;

		SDL_SubmitGPUCommandBuffer(cmdbuf);
	}

	return 0;
}

static void Quit(Context* context)
{
	if(swapchains) {
		for(Uint32 i = 0; i < viewCount; i++) {
			Swapchain swapchain = swapchains[i];

			SDL_DestroyGPUXRSwapchain(context->Device, swapchain.swapchain, swapchain.images);
		}

		SDL_free(swapchains);
	}
	if (views) SDL_free(views);
	if (localSpace) xrDestroySpace(localSpace);
	if (session) xrDestroySession(session);
	if (instance) xrDestroyInstance(instance);

	CommonQuit(context);
}

Example BasicVr_Example = { "BasicVr", Init, Update, Draw, Quit };
