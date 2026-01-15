/*
 * SpinningCubeVr - A VR example that renders a spinning colored cube
 * with proper stereoscopic projection using OpenXR.
 */

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

// Cube rendering resources
static SDL_GPUGraphicsPipeline *XrPipeline = NULL;
static SDL_GPUGraphicsPipeline *DesktopPipeline = NULL;
static SDL_GPUBuffer *VertexBuffer = NULL;
static SDL_GPUBuffer *IndexBuffer = NULL;
static float Time = 0.0f;

// Multiple cubes configuration
#define NUM_CUBES 7
static Vector3 CubePositions[NUM_CUBES] = {
	{ 0.0f, 0.0f, -2.0f },    // Center
	{ -1.5f, 0.5f, -2.5f },   // Left-up
	{ 1.5f, 0.3f, -2.5f },    // Right-up
	{ 0.0f, 1.2f, -3.0f },    // Above-far
	{ -1.0f, -0.5f, -1.5f },  // Left-down-close
	{ 1.0f, -0.4f, -1.8f },   // Right-down
	{ 0.0f, -0.8f, -2.5f },   // Below-far
};
static float CubeScales[NUM_CUBES] = { 1.0f, 0.7f, 0.7f, 0.5f, 0.6f, 0.6f, 0.4f };
static float CubeSpeedMultipliers[NUM_CUBES] = { 1.0f, 1.5f, -1.2f, 2.0f, -0.8f, 1.3f, -2.5f };

// Shader handles for deferred pipeline creation
static SDL_GPUShader* VertexShader = NULL;
static SDL_GPUShader* FragmentShader = NULL;

// Helper: Create projection matrix from XR FOV
static Matrix4x4 Matrix4x4_CreateProjectionFov(XrFovf fov, float nearZ, float farZ)
{
	float tanLeft = SDL_tanf(fov.angleLeft);
	float tanRight = SDL_tanf(fov.angleRight);
	float tanUp = SDL_tanf(fov.angleUp);
	float tanDown = SDL_tanf(fov.angleDown);

	float tanWidth = tanRight - tanLeft;
	float tanHeight = tanUp - tanDown;

	return (Matrix4x4){
		2.0f / tanWidth, 0, 0, 0,
		0, 2.0f / tanHeight, 0, 0,
		(tanRight + tanLeft) / tanWidth, (tanUp + tanDown) / tanHeight, -farZ / (farZ - nearZ), -1,
		0, 0, -(farZ * nearZ) / (farZ - nearZ), 0
	};
}

// Helper: Create view matrix from XR pose (eye pose in world space -> view matrix)
static Matrix4x4 Matrix4x4_CreateFromXrPose(XrPosef pose)
{
	// XR gives us the eye pose in world space. View matrix = inverse of eye transform.
	// This matches the pattern in Matrix4x4_CreateLookAt from Common.c
	
	// Extract quaternion components
	float x = pose.orientation.x;
	float y = pose.orientation.y;
	float z = pose.orientation.z;
	float w = pose.orientation.w;

	// Build rotation matrix from quaternion - these are the camera's basis vectors
	// Right vector (X axis of camera)
	Vector3 right = {
		1.0f - 2.0f * (y * y + z * z),
		2.0f * (x * y + w * z),
		2.0f * (x * z - w * y)
	};
	
	// Up vector (Y axis of camera)
	Vector3 up = {
		2.0f * (x * y - w * z),
		1.0f - 2.0f * (x * x + z * z),
		2.0f * (y * z + w * x)
	};
	
	// Forward vector (Z axis of camera) - note: OpenXR uses -Z forward
	Vector3 forward = {
		2.0f * (x * z + w * y),
		2.0f * (y * z - w * x),
		1.0f - 2.0f * (x * x + y * y)
	};

	// Camera position
	Vector3 pos = { pose.position.x, pose.position.y, pose.position.z };

	// View matrix follows same structure as CreateLookAt:
	// Basis vectors as columns (transposed), translation as -dot products
	return (Matrix4x4){
		right.x,   up.x,   forward.x,   0,
		right.y,   up.y,   forward.y,   0,
		right.z,   up.z,   forward.z,   0,
		-Vector3_Dot(right, pos), -Vector3_Dot(up, pos), -Vector3_Dot(forward, pos), 1
	};
}

// Helper: Create rotation around Y axis
static Matrix4x4 Matrix4x4_CreateRotationY(float radians)
{
	return (Matrix4x4){
		SDL_cosf(radians), 0, -SDL_sinf(radians), 0,
		0, 1, 0, 0,
		SDL_sinf(radians), 0, SDL_cosf(radians), 0,
		0, 0, 0, 1
	};
}

// Helper: Create rotation around X axis
static Matrix4x4 Matrix4x4_CreateRotationX(float radians)
{
	return (Matrix4x4){
		1, 0, 0, 0,
		0, SDL_cosf(radians), SDL_sinf(radians), 0,
		0, -SDL_sinf(radians), SDL_cosf(radians), 0,
		0, 0, 0, 1
	};
}

// Helper: Create scale matrix
static Matrix4x4 Matrix4x4_CreateScale(float scale)
{
	return (Matrix4x4){
		scale, 0, 0, 0,
		0, scale, 0, 0,
		0, 0, scale, 0,
		0, 0, 0, 1
	};
}

// Helper: Create a graphics pipeline for a specific format
static SDL_GPUGraphicsPipeline* CreateCubePipeline(Context* context, SDL_GPUTextureFormat colorFormat)
{
	if (!VertexShader || !FragmentShader) {
		SDL_Log("Shaders not loaded!");
		return NULL;
	}

	SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {
		.target_info = {
			.num_color_targets = 1,
			.color_target_descriptions = (SDL_GPUColorTargetDescription[]){{
				.format = colorFormat
			}},
			.has_depth_stencil_target = false
		},
		.depth_stencil_state = (SDL_GPUDepthStencilState){
			.enable_depth_test = false,
			.enable_depth_write = false
		},
		.rasterizer_state = (SDL_GPURasterizerState){
			.cull_mode = SDL_GPU_CULLMODE_BACK,
			.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
			.fill_mode = SDL_GPU_FILLMODE_FILL
		},
		.vertex_input_state = (SDL_GPUVertexInputState){
			.num_vertex_buffers = 1,
			.vertex_buffer_descriptions = (SDL_GPUVertexBufferDescription[]){{
				.slot = 0,
				.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
				.instance_step_rate = 0,
				.pitch = sizeof(PositionColorVertex)
			}},
			.num_vertex_attributes = 2,
			.vertex_attributes = (SDL_GPUVertexAttribute[]){{
				.buffer_slot = 0,
				.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
				.location = 0,
				.offset = 0
			}, {
				.buffer_slot = 0,
				.format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,
				.location = 1,
				.offset = sizeof(float) * 3
			}}
		},
		.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
		.vertex_shader = VertexShader,
		.fragment_shader = FragmentShader
	};

	SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(context->Device, &pipelineCreateInfo);
	if (!pipeline) {
		SDL_Log("Failed to create pipeline for format %d: %s", colorFormat, SDL_GetError());
	}
	return pipeline;
}

static int Init(Context* context)
{
	XrResult result;

	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_ENABLE, true);
	SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_INSTANCE_OUT, &instance);
	SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_SYSTEM_ID_OUT, &systemId);

	context->Device = SDL_CreateGPUDeviceWithProperties(props);
	if (!context->Device)
	{
		SDL_Log("SDL_CreateGPUDeviceWithProperties failed: %s", SDL_GetError());
		return -1;
	}

	SDL_Log("XR Device created successfully!");

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

	// Initialize asset loader for shaders
	InitializeAssetLoader();

	// Load shaders (pipeline will be created lazily when we know the format)
	VertexShader = LoadShader(context->Device, "PositionColorTransform.vert", 0, 1, 0, 0);
	if (VertexShader == NULL)
	{
		SDL_Log("Failed to create vertex shader!");
		return -1;
	}

	FragmentShader = LoadShader(context->Device, "SolidColor.frag", 0, 0, 0, 0);
	if (FragmentShader == NULL)
	{
		SDL_Log("Failed to create fragment shader!");
		return -1;
	}

	// Create desktop pipeline immediately (we know the window format)
	SDL_GPUTextureFormat windowFormat = SDL_GetGPUSwapchainTextureFormat(context->Device, context->Window);
	DesktopPipeline = CreateCubePipeline(context, windowFormat);
	if (DesktopPipeline == NULL)
	{
		SDL_Log("Failed to create desktop pipeline!");
		return -1;
	}

	// Create vertex buffer with colored cube vertices
	VertexBuffer = SDL_CreateGPUBuffer(
		context->Device,
		&(SDL_GPUBufferCreateInfo){
			.usage = SDL_GPU_BUFFERUSAGE_VERTEX,
			.size = sizeof(PositionColorVertex) * 24
		}
	);

	// Create index buffer
	IndexBuffer = SDL_CreateGPUBuffer(
		context->Device,
		&(SDL_GPUBufferCreateInfo){
			.usage = SDL_GPU_BUFFERUSAGE_INDEX,
			.size = sizeof(Uint16) * 36
		}
	);

	// Upload cube data
	SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(
		context->Device,
		&(SDL_GPUTransferBufferCreateInfo){
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = sizeof(PositionColorVertex) * 24 + sizeof(Uint16) * 36
		}
	);

	PositionColorVertex* vertexData = SDL_MapGPUTransferBuffer(context->Device, transferBuffer, false);

	// Cube vertices (0.5m cube, centered at origin - translation applied via model matrix)
	float s = 0.25f; // half-size

	// Front face (red) - facing -Z
	vertexData[0] = (PositionColorVertex){ -s, -s, -s, 255, 0, 0, 255 };
	vertexData[1] = (PositionColorVertex){  s, -s, -s, 255, 0, 0, 255 };
	vertexData[2] = (PositionColorVertex){  s,  s, -s, 255, 0, 0, 255 };
	vertexData[3] = (PositionColorVertex){ -s,  s, -s, 255, 0, 0, 255 };

	// Back face (green) - facing +Z
	vertexData[4] = (PositionColorVertex){  s, -s,  s, 0, 255, 0, 255 };
	vertexData[5] = (PositionColorVertex){ -s, -s,  s, 0, 255, 0, 255 };
	vertexData[6] = (PositionColorVertex){ -s,  s,  s, 0, 255, 0, 255 };
	vertexData[7] = (PositionColorVertex){  s,  s,  s, 0, 255, 0, 255 };

	// Left face (blue) - facing -X
	vertexData[8]  = (PositionColorVertex){ -s, -s,  s, 0, 0, 255, 255 };
	vertexData[9]  = (PositionColorVertex){ -s, -s, -s, 0, 0, 255, 255 };
	vertexData[10] = (PositionColorVertex){ -s,  s, -s, 0, 0, 255, 255 };
	vertexData[11] = (PositionColorVertex){ -s,  s,  s, 0, 0, 255, 255 };

	// Right face (yellow) - facing +X
	vertexData[12] = (PositionColorVertex){ s, -s, -s, 255, 255, 0, 255 };
	vertexData[13] = (PositionColorVertex){ s, -s,  s, 255, 255, 0, 255 };
	vertexData[14] = (PositionColorVertex){ s,  s,  s, 255, 255, 0, 255 };
	vertexData[15] = (PositionColorVertex){ s,  s, -s, 255, 255, 0, 255 };

	// Top face (magenta) - facing +Y
	vertexData[16] = (PositionColorVertex){ -s, s, -s, 255, 0, 255, 255 };
	vertexData[17] = (PositionColorVertex){  s, s, -s, 255, 0, 255, 255 };
	vertexData[18] = (PositionColorVertex){  s, s,  s, 255, 0, 255, 255 };
	vertexData[19] = (PositionColorVertex){ -s, s,  s, 255, 0, 255, 255 };

	// Bottom face (cyan) - facing -Y
	vertexData[20] = (PositionColorVertex){ -s, -s,  s, 0, 255, 255, 255 };
	vertexData[21] = (PositionColorVertex){  s, -s,  s, 0, 255, 255, 255 };
	vertexData[22] = (PositionColorVertex){  s, -s, -s, 0, 255, 255, 255 };
	vertexData[23] = (PositionColorVertex){ -s, -s, -s, 0, 255, 255, 255 };

	// Indices
	Uint16* indexData = (Uint16*)(vertexData + 24);
	Uint16 indices[] = {
		0, 1, 2, 0, 2, 3,       // front
		4, 5, 6, 4, 6, 7,       // back
		8, 9, 10, 8, 10, 11,    // left
		12, 13, 14, 12, 14, 15, // right
		16, 17, 18, 16, 18, 19, // top
		20, 21, 22, 20, 22, 23  // bottom
	};
	SDL_memcpy(indexData, indices, sizeof(indices));

	SDL_UnmapGPUTransferBuffer(context->Device, transferBuffer);

	// Upload to GPU
	SDL_GPUCommandBuffer* uploadCmdbuf = SDL_AcquireGPUCommandBuffer(context->Device);
	SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(uploadCmdbuf);

	SDL_UploadToGPUBuffer(
		copyPass,
		&(SDL_GPUTransferBufferLocation){ .transfer_buffer = transferBuffer, .offset = 0 },
		&(SDL_GPUBufferRegion){ .buffer = VertexBuffer, .offset = 0, .size = sizeof(PositionColorVertex) * 24 },
		false
	);

	SDL_UploadToGPUBuffer(
		copyPass,
		&(SDL_GPUTransferBufferLocation){ .transfer_buffer = transferBuffer, .offset = sizeof(PositionColorVertex) * 24 },
		&(SDL_GPUBufferRegion){ .buffer = IndexBuffer, .offset = 0, .size = sizeof(Uint16) * 36 },
		false
	);

	SDL_EndGPUCopyPass(copyPass);
	SDL_SubmitGPUCommandBuffer(uploadCmdbuf);
	SDL_ReleaseGPUTransferBuffer(context->Device, transferBuffer);

	SDL_Log("SpinningCubeVr initialized! Put on your headset to see a spinning cube.");

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

			SDL_Log("OpenXR session started!");

			XR_ERR_RET(xrEnumerateViewConfigurationViews(
				instance,
				systemId,
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
				0,
				&viewCount,
				NULL), -1);

			XrViewConfigurationView *viewConfigViews = SDL_stack_alloc(XrViewConfigurationView, viewCount);
			for (Uint32 i = 0; i < viewCount; i++) viewConfigViews[i] = (XrViewConfigurationView){XR_TYPE_VIEW_CONFIGURATION_VIEW};

			XR_ERR_RET(xrEnumerateViewConfigurationViews(
				instance,
				systemId,
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
				viewCount,
				&viewCount,
				viewConfigViews), -1);

			if(viewCount > 0) {
				swapchains = SDL_calloc(viewCount, sizeof(Swapchain));
				views = SDL_calloc(viewCount, sizeof(XrView));

				for(Uint32 i = 0; i < viewCount; i++)
				{
					views[i] = (XrView){XR_TYPE_VIEW, .pose = {.orientation = IDENTITY_QUAT}};

					Swapchain *swapchain = &swapchains[i];
					XrViewConfigurationView viewConfig = viewConfigViews[i];

					SDL_Log("Eye %d: %dx%d", i, viewConfig.recommendedImageRectWidth, viewConfig.recommendedImageRectHeight);

					XrSwapchainCreateInfo swapchainCreateInfo = {
						.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
						.width = viewConfig.recommendedImageRectWidth,
						.height = viewConfig.recommendedImageRectHeight,
						.mipCount = 1,
						.sampleCount = 1,
						.faceCount = 1,
						.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
						.arraySize = 1
					};

					swapchain->size = (XrExtent2Di){swapchainCreateInfo.width, swapchainCreateInfo.height};

					XR_ERR_RET(SDL_CreateGPUXRSwapchain(
						context->Device,
						session,
						&swapchainCreateInfo,
						&swapchain->format,
						&swapchain->swapchain,
						&swapchain->images), -1);

					// Create XR pipeline on first swapchain (they all use the same format)
					if (i == 0 && XrPipeline == NULL) {
						SDL_Log("Creating XR pipeline for format: %d", swapchain->format);
						XrPipeline = CreateCubePipeline(context, swapchain->format);
						if (XrPipeline == NULL) {
							SDL_Log("Failed to create XR pipeline!");
							return -1;
						}
					}
				}
			}

			SDL_stack_free(viewConfigViews);
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
			SDL_Log("OpenXR session ended");
			break;
		}
		case XR_SESSION_STATE_EXITING:
		{
			SDL_Log("Session exiting");
			return -1;
		}
		default:
			break;
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
				SDL_Log("Instance loss pending");
				return -1;
			}
			default:
				break;
		}
	}

	return 0;
}

static int Update(Context* context)
{
	if(HandleXrEvent(context) != 0) return -1;

	// Update time for animations
	Time += context->DeltaTime;

	return 0;
}

static int RenderAllCubes(Context* context, SDL_GPUCommandBuffer* cmdbuf, SDL_GPUTexture* texture, SDL_GPUGraphicsPipeline* pipeline, XrView view, int width, int height)
{
	// Create view and projection matrices from XR data
	Matrix4x4 viewMatrix = Matrix4x4_CreateFromXrPose(view.pose);
	Matrix4x4 projMatrix = Matrix4x4_CreateProjectionFov(view.fov, 0.05f, 100.0f);

	SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(cmdbuf, &(SDL_GPUColorTargetInfo){
		.texture = texture,
		.clear_color = {0.05f, 0.05f, 0.15f, 1.0f},
		.load_op = SDL_GPU_LOADOP_CLEAR,
		.store_op = SDL_GPU_STOREOP_STORE,
	}, 1, NULL);

	SDL_BindGPUGraphicsPipeline(renderPass, pipeline);
	SDL_SetGPUViewport(renderPass, &(SDL_GPUViewport){0, 0, (float)width, (float)height, 0, 1});
	SDL_SetGPUScissor(renderPass, &(SDL_Rect){0, 0, width, height});
	SDL_BindGPUVertexBuffers(renderPass, 0, &(SDL_GPUBufferBinding){VertexBuffer, 0}, 1);
	SDL_BindGPUIndexBuffer(renderPass, &(SDL_GPUBufferBinding){IndexBuffer, 0}, SDL_GPU_INDEXELEMENTSIZE_16BIT);

	// Render each cube with its own transform
	for (int i = 0; i < NUM_CUBES; i++) {
		float rotation = Time * CubeSpeedMultipliers[i];
		float scale = CubeScales[i];
		Vector3 pos = CubePositions[i];

		// Build model matrix: Scale * RotationX * RotationY * Translation
		Matrix4x4 scaleMatrix = Matrix4x4_CreateScale(scale);
		Matrix4x4 rotY = Matrix4x4_CreateRotationY(rotation);
		Matrix4x4 rotX = Matrix4x4_CreateRotationX(rotation * 0.7f);
		Matrix4x4 translationMatrix = Matrix4x4_CreateTranslation(pos.x, pos.y, pos.z);
		
		// Combine transforms
		Matrix4x4 scaleRot = Matrix4x4_Multiply(scaleMatrix, rotY);
		Matrix4x4 fullRot = Matrix4x4_Multiply(scaleRot, rotX);
		Matrix4x4 modelMatrix = Matrix4x4_Multiply(fullRot, translationMatrix);

		// Final MVP
		Matrix4x4 modelView = Matrix4x4_Multiply(modelMatrix, viewMatrix);
		Matrix4x4 mvp = Matrix4x4_Multiply(modelView, projMatrix);

		SDL_PushGPUVertexUniformData(cmdbuf, 0, &mvp, sizeof(mvp));
		SDL_DrawGPUIndexedPrimitives(renderPass, 36, 1, 0, 0, 0);
	}

	SDL_EndGPURenderPass(renderPass);

	return 0;
}

static int RenderDesktopView(Context* context, SDL_GPUCommandBuffer* cmdbuf)
{
	SDL_GPUTexture* swapchainTexture;
	SDL_AcquireGPUSwapchainTexture(cmdbuf, context->Window, &swapchainTexture, NULL, NULL);

	if (swapchainTexture == NULL) return 0;

	// Use first eye view for desktop mirror, or identity if not available
	XrView view = {XR_TYPE_VIEW, .pose = IDENTITY_POSE, .fov = {-0.8f, 0.8f, 0.8f, -0.8f}};
	if(viewCount > 0 && views) view = views[0];

	int w, h;
	SDL_GetWindowSize(context->Window, &w, &h);

	return RenderAllCubes(context, cmdbuf, swapchainTexture, DesktopPipeline, view, w, h);
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
		XR_ERR_RET(xrWaitFrame(session, &frameWaitInfo, &frameState), -1);

		XrFrameBeginInfo frameBeginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
		XR_ERR_RET(xrBeginFrame(session, &frameBeginInfo), -1);

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
				waitInfo.timeout = XR_INFINITE_DURATION;
				XR_ERR_RET(xrWaitSwapchainImage(swapchain.swapchain, &waitInfo), -1);

				SDL_GPUTexture *texture = swapchain.images[swapchainIndex];
				if(RenderAllCubes(context, cmdbuf, texture, XrPipeline, views[i], swapchain.size.width, swapchain.size.height) != 0) return -1;
			}

			// Also render desktop mirror
			RenderDesktopView(context, cmdbuf);

			SDL_SubmitGPUCommandBuffer(cmdbuf);

			for(Uint32 i = 0; i < viewCountOutput; i++) {
				Swapchain swapchain = swapchains[i];

				XR_ERR_RET(xrReleaseSwapchainImage(swapchain.swapchain, NULL), -1);

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
		else {
			RenderDesktopView(context, cmdbuf);
			SDL_SubmitGPUCommandBuffer(cmdbuf);
		}

		XrFrameEndInfo frameEndInfo = {XR_TYPE_FRAME_END_INFO};
		frameEndInfo.displayTime = frameState.predictedDisplayTime;
		frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		frameEndInfo.layerCount = frameState.shouldRender ? 1 : 0;

		const XrCompositionLayerBaseHeader *projectionLayers[1];
		projectionLayers[0] = (const XrCompositionLayerBaseHeader*)&(XrCompositionLayerProjection){
			.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
			.space = localSpace,
			.viewCount = viewCountOutput,
			.views = projectionViews,
		};
		frameEndInfo.layers = projectionLayers;

		XR_ERR_RET(xrEndFrame(session, &frameEndInfo), -1);
		SDL_stack_free(projectionViews);
	}
	else {
		RenderDesktopView(context, cmdbuf);
		SDL_SubmitGPUCommandBuffer(cmdbuf);
	}

	return 0;
}

static void Quit(Context* context)
{
	if (XrPipeline) SDL_ReleaseGPUGraphicsPipeline(context->Device, XrPipeline);
	if (DesktopPipeline) SDL_ReleaseGPUGraphicsPipeline(context->Device, DesktopPipeline);
	if (VertexShader) SDL_ReleaseGPUShader(context->Device, VertexShader);
	if (FragmentShader) SDL_ReleaseGPUShader(context->Device, FragmentShader);
	if (VertexBuffer) SDL_ReleaseGPUBuffer(context->Device, VertexBuffer);
	if (IndexBuffer) SDL_ReleaseGPUBuffer(context->Device, IndexBuffer);

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

Example SpinningCubeVr_Example = { "SpinningCubeVr", Init, Update, Draw, Quit };
