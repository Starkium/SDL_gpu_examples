/*
 * VrGallery - A VR example showcasing multiple objects in a 3D scene
 * Features:
 * - Skybox with cubemap texture
 * - Multiple spinning cubes at different positions
 * - Textured floor plane
 * - Proper stereoscopic VR rendering
 */

#include "Common.h"

#include <SDL3/SDL_openxr.h>

typedef struct Swapchain {
	XrSwapchain swapchain;
	SDL_GPUTexture **images;
	XrExtent2Di size;
	SDL_GPUTextureFormat format;
} Swapchain;

// XR state
static XrInstance instance = NULL;
static XrSystemId systemId = 0;
static XrSession session = NULL;
static bool doXrFrameLoop = false;
static XrSpace localSpace = NULL;
static Swapchain *swapchains = NULL;
static XrView *views = NULL;
static Uint32 viewCount = 0;

// Skybox resources
static SDL_GPUGraphicsPipeline *SkyboxPipeline = NULL;
static SDL_GPUGraphicsPipeline *SkyboxPipelineDesktop = NULL;
static SDL_GPUBuffer *SkyboxVertexBuffer = NULL;
static SDL_GPUBuffer *SkyboxIndexBuffer = NULL;
static SDL_GPUTexture *SkyboxTexture = NULL;
static SDL_GPUSampler *SkyboxSampler = NULL;

// Cube resources
static SDL_GPUGraphicsPipeline *CubePipeline = NULL;
static SDL_GPUGraphicsPipeline *CubePipelineDesktop = NULL;
static SDL_GPUBuffer *CubeVertexBuffer = NULL;
static SDL_GPUBuffer *CubeIndexBuffer = NULL;

// Floor resources (textured quad)
static SDL_GPUGraphicsPipeline *FloorPipeline = NULL;
static SDL_GPUGraphicsPipeline *FloorPipelineDesktop = NULL;
static SDL_GPUBuffer *FloorVertexBuffer = NULL;
static SDL_GPUBuffer *FloorIndexBuffer = NULL;
static SDL_GPUTexture *FloorTexture = NULL;
static SDL_GPUSampler *FloorSampler = NULL;

// Shader handles
static SDL_GPUShader *ColorVertexShader = NULL;
static SDL_GPUShader *ColorFragmentShader = NULL;
static SDL_GPUShader *SkyboxVertexShader = NULL;
static SDL_GPUShader *SkyboxFragmentShader = NULL;
static SDL_GPUShader *TexturedVertexShader = NULL;
static SDL_GPUShader *TexturedFragmentShader = NULL;

// Animation state
static float Time = 0.0f;

// Cube positions in world space
#define NUM_CUBES 5
static Vector3 CubePositions[NUM_CUBES] = {
	{ 0.0f, 0.0f, -2.0f },   // Center, in front
	{ -1.5f, 0.5f, -2.5f },  // Left
	{ 1.5f, 0.3f, -2.5f },   // Right
	{ 0.0f, 1.2f, -3.0f },   // Above
	{ 0.0f, -0.5f, -1.5f },  // Below, closer
};

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

// Helper: Create view matrix from XR pose
static Matrix4x4 Matrix4x4_CreateFromXrPose(XrPosef pose)
{
	float x = pose.orientation.x;
	float y = pose.orientation.y;
	float z = pose.orientation.z;
	float w = pose.orientation.w;

	Vector3 right = {
		1.0f - 2.0f * (y * y + z * z),
		2.0f * (x * y + w * z),
		2.0f * (x * z - w * y)
	};
	
	Vector3 up = {
		2.0f * (x * y - w * z),
		1.0f - 2.0f * (x * x + z * z),
		2.0f * (y * z + w * x)
	};
	
	Vector3 forward = {
		2.0f * (x * z + w * y),
		2.0f * (y * z - w * x),
		1.0f - 2.0f * (x * x + y * y)
	};

	Vector3 pos = { pose.position.x, pose.position.y, pose.position.z };

	return (Matrix4x4){
		right.x,   up.x,   forward.x,   0,
		right.y,   up.y,   forward.y,   0,
		right.z,   up.z,   forward.z,   0,
		-Vector3_Dot(right, pos), -Vector3_Dot(up, pos), -Vector3_Dot(forward, pos), 1
	};
}

static Matrix4x4 Matrix4x4_CreateRotationY(float radians)
{
	return (Matrix4x4){
		SDL_cosf(radians), 0, -SDL_sinf(radians), 0,
		0, 1, 0, 0,
		SDL_sinf(radians), 0, SDL_cosf(radians), 0,
		0, 0, 0, 1
	};
}

static Matrix4x4 Matrix4x4_CreateRotationX(float radians)
{
	return (Matrix4x4){
		1, 0, 0, 0,
		0, SDL_cosf(radians), SDL_sinf(radians), 0,
		0, -SDL_sinf(radians), SDL_cosf(radians), 0,
		0, 0, 0, 1
	};
}

static Matrix4x4 Matrix4x4_CreateScale(float sx, float sy, float sz)
{
	return (Matrix4x4){
		sx, 0, 0, 0,
		0, sy, 0, 0,
		0, 0, sz, 0,
		0, 0, 0, 1
	};
}

// Helper: Create pipeline for a specific format
static SDL_GPUGraphicsPipeline* CreateColorPipeline(Context* context, SDL_GPUTextureFormat colorFormat)
{
	SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {
		.target_info = {
			.num_color_targets = 1,
			.color_target_descriptions = (SDL_GPUColorTargetDescription[]){{
				.format = colorFormat
			}},
			.has_depth_stencil_target = false
		},
		.rasterizer_state = {
			.cull_mode = SDL_GPU_CULLMODE_BACK,
			.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
			.fill_mode = SDL_GPU_FILLMODE_FILL
		},
		.vertex_input_state = {
			.num_vertex_buffers = 1,
			.vertex_buffer_descriptions = (SDL_GPUVertexBufferDescription[]){{
				.slot = 0,
				.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
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
		.vertex_shader = ColorVertexShader,
		.fragment_shader = ColorFragmentShader
	};

	return SDL_CreateGPUGraphicsPipeline(context->Device, &pipelineCreateInfo);
}

static SDL_GPUGraphicsPipeline* CreateSkyboxPipeline(Context* context, SDL_GPUTextureFormat colorFormat)
{
	SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {
		.target_info = {
			.num_color_targets = 1,
			.color_target_descriptions = (SDL_GPUColorTargetDescription[]){{
				.format = colorFormat
			}},
		},
		.rasterizer_state = {
			.cull_mode = SDL_GPU_CULLMODE_NONE,
			.fill_mode = SDL_GPU_FILLMODE_FILL
		},
		.vertex_input_state = {
			.num_vertex_buffers = 1,
			.vertex_buffer_descriptions = (SDL_GPUVertexBufferDescription[]){{
				.slot = 0,
				.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
				.pitch = sizeof(PositionVertex)
			}},
			.num_vertex_attributes = 1,
			.vertex_attributes = (SDL_GPUVertexAttribute[]){{
				.buffer_slot = 0,
				.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
				.location = 0,
				.offset = 0
			}}
		},
		.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
		.vertex_shader = SkyboxVertexShader,
		.fragment_shader = SkyboxFragmentShader
	};

	return SDL_CreateGPUGraphicsPipeline(context->Device, &pipelineCreateInfo);
}

static SDL_GPUGraphicsPipeline* CreateTexturedPipeline(Context* context, SDL_GPUTextureFormat colorFormat)
{
	SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {
		.target_info = {
			.num_color_targets = 1,
			.color_target_descriptions = (SDL_GPUColorTargetDescription[]){{
				.format = colorFormat
			}},
		},
		.rasterizer_state = {
			.cull_mode = SDL_GPU_CULLMODE_NONE,
			.fill_mode = SDL_GPU_FILLMODE_FILL
		},
		.vertex_input_state = {
			.num_vertex_buffers = 1,
			.vertex_buffer_descriptions = (SDL_GPUVertexBufferDescription[]){{
				.slot = 0,
				.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
				.pitch = sizeof(PositionTextureVertex)
			}},
			.num_vertex_attributes = 2,
			.vertex_attributes = (SDL_GPUVertexAttribute[]){{
				.buffer_slot = 0,
				.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
				.location = 0,
				.offset = 0
			}, {
				.buffer_slot = 0,
				.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
				.location = 1,
				.offset = sizeof(float) * 3
			}}
		},
		.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
		.vertex_shader = TexturedVertexShader,
		.fragment_shader = TexturedFragmentShader
	};

	return SDL_CreateGPUGraphicsPipeline(context->Device, &pipelineCreateInfo);
}

static int CreateCubeBuffers(Context* context)
{
	CubeVertexBuffer = SDL_CreateGPUBuffer(context->Device, &(SDL_GPUBufferCreateInfo){
		.usage = SDL_GPU_BUFFERUSAGE_VERTEX,
		.size = sizeof(PositionColorVertex) * 24
	});

	CubeIndexBuffer = SDL_CreateGPUBuffer(context->Device, &(SDL_GPUBufferCreateInfo){
		.usage = SDL_GPU_BUFFERUSAGE_INDEX,
		.size = sizeof(Uint16) * 36
	});

	SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(context->Device, &(SDL_GPUTransferBufferCreateInfo){
		.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
		.size = sizeof(PositionColorVertex) * 24 + sizeof(Uint16) * 36
	});

	PositionColorVertex* vertexData = SDL_MapGPUTransferBuffer(context->Device, transferBuffer, false);

	float s = 0.15f;

	// Front face (red)
	vertexData[0] = (PositionColorVertex){ -s, -s, -s, 255, 80, 80, 255 };
	vertexData[1] = (PositionColorVertex){  s, -s, -s, 255, 80, 80, 255 };
	vertexData[2] = (PositionColorVertex){  s,  s, -s, 255, 80, 80, 255 };
	vertexData[3] = (PositionColorVertex){ -s,  s, -s, 255, 80, 80, 255 };

	// Back face (green)
	vertexData[4] = (PositionColorVertex){  s, -s,  s, 80, 255, 80, 255 };
	vertexData[5] = (PositionColorVertex){ -s, -s,  s, 80, 255, 80, 255 };
	vertexData[6] = (PositionColorVertex){ -s,  s,  s, 80, 255, 80, 255 };
	vertexData[7] = (PositionColorVertex){  s,  s,  s, 80, 255, 80, 255 };

	// Left face (blue)
	vertexData[8]  = (PositionColorVertex){ -s, -s,  s, 80, 80, 255, 255 };
	vertexData[9]  = (PositionColorVertex){ -s, -s, -s, 80, 80, 255, 255 };
	vertexData[10] = (PositionColorVertex){ -s,  s, -s, 80, 80, 255, 255 };
	vertexData[11] = (PositionColorVertex){ -s,  s,  s, 80, 80, 255, 255 };

	// Right face (yellow)
	vertexData[12] = (PositionColorVertex){ s, -s, -s, 255, 255, 80, 255 };
	vertexData[13] = (PositionColorVertex){ s, -s,  s, 255, 255, 80, 255 };
	vertexData[14] = (PositionColorVertex){ s,  s,  s, 255, 255, 80, 255 };
	vertexData[15] = (PositionColorVertex){ s,  s, -s, 255, 255, 80, 255 };

	// Top face (magenta)
	vertexData[16] = (PositionColorVertex){ -s, s, -s, 255, 80, 255, 255 };
	vertexData[17] = (PositionColorVertex){  s, s, -s, 255, 80, 255, 255 };
	vertexData[18] = (PositionColorVertex){  s, s,  s, 255, 80, 255, 255 };
	vertexData[19] = (PositionColorVertex){ -s, s,  s, 255, 80, 255, 255 };

	// Bottom face (cyan)
	vertexData[20] = (PositionColorVertex){ -s, -s,  s, 80, 255, 255, 255 };
	vertexData[21] = (PositionColorVertex){  s, -s,  s, 80, 255, 255, 255 };
	vertexData[22] = (PositionColorVertex){  s, -s, -s, 80, 255, 255, 255 };
	vertexData[23] = (PositionColorVertex){ -s, -s, -s, 80, 255, 255, 255 };

	Uint16* indexData = (Uint16*)(vertexData + 24);
	Uint16 indices[] = {
		0, 1, 2, 0, 2, 3,
		4, 5, 6, 4, 6, 7,
		8, 9, 10, 8, 10, 11,
		12, 13, 14, 12, 14, 15,
		16, 17, 18, 16, 18, 19,
		20, 21, 22, 20, 22, 23
	};
	SDL_memcpy(indexData, indices, sizeof(indices));

	SDL_UnmapGPUTransferBuffer(context->Device, transferBuffer);

	SDL_GPUCommandBuffer* cmdbuf = SDL_AcquireGPUCommandBuffer(context->Device);
	SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdbuf);

	SDL_UploadToGPUBuffer(copyPass,
		&(SDL_GPUTransferBufferLocation){ .transfer_buffer = transferBuffer, .offset = 0 },
		&(SDL_GPUBufferRegion){ .buffer = CubeVertexBuffer, .size = sizeof(PositionColorVertex) * 24 },
		false);

	SDL_UploadToGPUBuffer(copyPass,
		&(SDL_GPUTransferBufferLocation){ .transfer_buffer = transferBuffer, .offset = sizeof(PositionColorVertex) * 24 },
		&(SDL_GPUBufferRegion){ .buffer = CubeIndexBuffer, .size = sizeof(Uint16) * 36 },
		false);

	SDL_EndGPUCopyPass(copyPass);
	SDL_SubmitGPUCommandBuffer(cmdbuf);
	SDL_ReleaseGPUTransferBuffer(context->Device, transferBuffer);

	return 0;
}

static int CreateSkyboxBuffers(Context* context)
{
	SkyboxVertexBuffer = SDL_CreateGPUBuffer(context->Device, &(SDL_GPUBufferCreateInfo){
		.usage = SDL_GPU_BUFFERUSAGE_VERTEX,
		.size = sizeof(PositionVertex) * 24
	});

	SkyboxIndexBuffer = SDL_CreateGPUBuffer(context->Device, &(SDL_GPUBufferCreateInfo){
		.usage = SDL_GPU_BUFFERUSAGE_INDEX,
		.size = sizeof(Uint16) * 36
	});

	SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(context->Device, &(SDL_GPUTransferBufferCreateInfo){
		.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
		.size = sizeof(PositionVertex) * 24 + sizeof(Uint16) * 36
	});

	PositionVertex* vertexData = SDL_MapGPUTransferBuffer(context->Device, transferBuffer, false);

	// Large skybox cube
	float s = 50.0f;

	// +X face
	vertexData[0] = (PositionVertex){ s, -s, -s }; vertexData[1] = (PositionVertex){ s, -s, s };
	vertexData[2] = (PositionVertex){ s, s, s }; vertexData[3] = (PositionVertex){ s, s, -s };
	// -X face
	vertexData[4] = (PositionVertex){ -s, -s, s }; vertexData[5] = (PositionVertex){ -s, -s, -s };
	vertexData[6] = (PositionVertex){ -s, s, -s }; vertexData[7] = (PositionVertex){ -s, s, s };
	// +Y face
	vertexData[8] = (PositionVertex){ -s, s, -s }; vertexData[9] = (PositionVertex){ s, s, -s };
	vertexData[10] = (PositionVertex){ s, s, s }; vertexData[11] = (PositionVertex){ -s, s, s };
	// -Y face
	vertexData[12] = (PositionVertex){ -s, -s, s }; vertexData[13] = (PositionVertex){ s, -s, s };
	vertexData[14] = (PositionVertex){ s, -s, -s }; vertexData[15] = (PositionVertex){ -s, -s, -s };
	// +Z face
	vertexData[16] = (PositionVertex){ s, -s, s }; vertexData[17] = (PositionVertex){ -s, -s, s };
	vertexData[18] = (PositionVertex){ -s, s, s }; vertexData[19] = (PositionVertex){ s, s, s };
	// -Z face
	vertexData[20] = (PositionVertex){ -s, -s, -s }; vertexData[21] = (PositionVertex){ s, -s, -s };
	vertexData[22] = (PositionVertex){ s, s, -s }; vertexData[23] = (PositionVertex){ -s, s, -s };

	Uint16* indexData = (Uint16*)(vertexData + 24);
	Uint16 indices[] = {
		0, 1, 2, 0, 2, 3,
		4, 5, 6, 4, 6, 7,
		8, 9, 10, 8, 10, 11,
		12, 13, 14, 12, 14, 15,
		16, 17, 18, 16, 18, 19,
		20, 21, 22, 20, 22, 23
	};
	SDL_memcpy(indexData, indices, sizeof(indices));

	SDL_UnmapGPUTransferBuffer(context->Device, transferBuffer);

	SDL_GPUCommandBuffer* cmdbuf = SDL_AcquireGPUCommandBuffer(context->Device);
	SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdbuf);

	SDL_UploadToGPUBuffer(copyPass,
		&(SDL_GPUTransferBufferLocation){ .transfer_buffer = transferBuffer },
		&(SDL_GPUBufferRegion){ .buffer = SkyboxVertexBuffer, .size = sizeof(PositionVertex) * 24 },
		false);

	SDL_UploadToGPUBuffer(copyPass,
		&(SDL_GPUTransferBufferLocation){ .transfer_buffer = transferBuffer, .offset = sizeof(PositionVertex) * 24 },
		&(SDL_GPUBufferRegion){ .buffer = SkyboxIndexBuffer, .size = sizeof(Uint16) * 36 },
		false);

	SDL_EndGPUCopyPass(copyPass);
	SDL_SubmitGPUCommandBuffer(cmdbuf);
	SDL_ReleaseGPUTransferBuffer(context->Device, transferBuffer);

	// Create cubemap texture
	SkyboxTexture = SDL_CreateGPUTexture(context->Device, &(SDL_GPUTextureCreateInfo){
		.type = SDL_GPU_TEXTURETYPE_CUBE,
		.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		.width = 64,
		.height = 64,
		.layer_count_or_depth = 6,
		.num_levels = 1,
		.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER
	});

	SkyboxSampler = SDL_CreateGPUSampler(context->Device, &(SDL_GPUSamplerCreateInfo){
		.min_filter = SDL_GPU_FILTER_LINEAR,
		.mag_filter = SDL_GPU_FILTER_LINEAR,
		.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
		.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
		.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE
	});

	// Load cubemap faces
	const char* faceNames[] = { "cube0.bmp", "cube1.bmp", "cube2.bmp", "cube3.bmp", "cube4.bmp", "cube5.bmp" };
	
	cmdbuf = SDL_AcquireGPUCommandBuffer(context->Device);
	copyPass = SDL_BeginGPUCopyPass(cmdbuf);

	for (int i = 0; i < 6; i++) {
		SDL_Surface* surface = LoadImage(faceNames[i], 4);
		if (surface) {
			SDL_GPUTransferBuffer* texTransfer = SDL_CreateGPUTransferBuffer(context->Device, &(SDL_GPUTransferBufferCreateInfo){
				.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
				.size = surface->w * surface->h * 4
			});
			
			void* data = SDL_MapGPUTransferBuffer(context->Device, texTransfer, false);
			SDL_memcpy(data, surface->pixels, surface->w * surface->h * 4);
			SDL_UnmapGPUTransferBuffer(context->Device, texTransfer);

			SDL_UploadToGPUTexture(copyPass,
				&(SDL_GPUTextureTransferInfo){ .transfer_buffer = texTransfer },
				&(SDL_GPUTextureRegion){ .texture = SkyboxTexture, .layer = i, .w = 64, .h = 64, .d = 1 },
				false);

			SDL_ReleaseGPUTransferBuffer(context->Device, texTransfer);
			SDL_DestroySurface(surface);
		}
	}

	SDL_EndGPUCopyPass(copyPass);
	SDL_SubmitGPUCommandBuffer(cmdbuf);

	return 0;
}

static int CreateFloorBuffers(Context* context)
{
	FloorVertexBuffer = SDL_CreateGPUBuffer(context->Device, &(SDL_GPUBufferCreateInfo){
		.usage = SDL_GPU_BUFFERUSAGE_VERTEX,
		.size = sizeof(PositionTextureVertex) * 4
	});

	FloorIndexBuffer = SDL_CreateGPUBuffer(context->Device, &(SDL_GPUBufferCreateInfo){
		.usage = SDL_GPU_BUFFERUSAGE_INDEX,
		.size = sizeof(Uint16) * 6
	});

	SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(context->Device, &(SDL_GPUTransferBufferCreateInfo){
		.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
		.size = sizeof(PositionTextureVertex) * 4 + sizeof(Uint16) * 6
	});

	PositionTextureVertex* vertexData = SDL_MapGPUTransferBuffer(context->Device, transferBuffer, false);

	float floorSize = 5.0f;
	float floorY = -1.0f;
	float uvScale = 5.0f;

	vertexData[0] = (PositionTextureVertex){ -floorSize, floorY, -floorSize, 0, 0 };
	vertexData[1] = (PositionTextureVertex){  floorSize, floorY, -floorSize, uvScale, 0 };
	vertexData[2] = (PositionTextureVertex){  floorSize, floorY,  floorSize, uvScale, uvScale };
	vertexData[3] = (PositionTextureVertex){ -floorSize, floorY,  floorSize, 0, uvScale };

	Uint16* indexData = (Uint16*)(vertexData + 4);
	Uint16 indices[] = { 0, 2, 1, 0, 3, 2 };
	SDL_memcpy(indexData, indices, sizeof(indices));

	SDL_UnmapGPUTransferBuffer(context->Device, transferBuffer);

	SDL_GPUCommandBuffer* cmdbuf = SDL_AcquireGPUCommandBuffer(context->Device);
	SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdbuf);

	SDL_UploadToGPUBuffer(copyPass,
		&(SDL_GPUTransferBufferLocation){ .transfer_buffer = transferBuffer },
		&(SDL_GPUBufferRegion){ .buffer = FloorVertexBuffer, .size = sizeof(PositionTextureVertex) * 4 },
		false);

	SDL_UploadToGPUBuffer(copyPass,
		&(SDL_GPUTransferBufferLocation){ .transfer_buffer = transferBuffer, .offset = sizeof(PositionTextureVertex) * 4 },
		&(SDL_GPUBufferRegion){ .buffer = FloorIndexBuffer, .size = sizeof(Uint16) * 6 },
		false);

	SDL_EndGPUCopyPass(copyPass);
	SDL_SubmitGPUCommandBuffer(cmdbuf);
	SDL_ReleaseGPUTransferBuffer(context->Device, transferBuffer);

	// Load floor texture
	SDL_Surface* surface = LoadImage("ravioli.bmp", 4);
	if (surface) {
		FloorTexture = SDL_CreateGPUTexture(context->Device, &(SDL_GPUTextureCreateInfo){
			.type = SDL_GPU_TEXTURETYPE_2D,
			.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
			.width = surface->w,
			.height = surface->h,
			.layer_count_or_depth = 1,
			.num_levels = 1,
			.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER
		});

		SDL_GPUTransferBuffer* texTransfer = SDL_CreateGPUTransferBuffer(context->Device, &(SDL_GPUTransferBufferCreateInfo){
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = surface->w * surface->h * 4
		});
		
		void* data = SDL_MapGPUTransferBuffer(context->Device, texTransfer, false);
		SDL_memcpy(data, surface->pixels, surface->w * surface->h * 4);
		SDL_UnmapGPUTransferBuffer(context->Device, texTransfer);

		cmdbuf = SDL_AcquireGPUCommandBuffer(context->Device);
		copyPass = SDL_BeginGPUCopyPass(cmdbuf);
		SDL_UploadToGPUTexture(copyPass,
			&(SDL_GPUTextureTransferInfo){ .transfer_buffer = texTransfer },
			&(SDL_GPUTextureRegion){ .texture = FloorTexture, .w = surface->w, .h = surface->h, .d = 1 },
			false);
		SDL_EndGPUCopyPass(copyPass);
		SDL_SubmitGPUCommandBuffer(cmdbuf);

		SDL_ReleaseGPUTransferBuffer(context->Device, texTransfer);
		SDL_DestroySurface(surface);
	}

	FloorSampler = SDL_CreateGPUSampler(context->Device, &(SDL_GPUSamplerCreateInfo){
		.min_filter = SDL_GPU_FILTER_LINEAR,
		.mag_filter = SDL_GPU_FILTER_LINEAR,
		.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
		.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT
	});

	return 0;
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
	if (!context->Device) {
		SDL_Log("SDL_CreateGPUDeviceWithProperties failed: %s", SDL_GetError());
		return -1;
	}

	SDL_Log("VrGallery: XR Device created!");

	XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
	XR_ERR_RET(SDL_CreateGPUXRSession(context->Device, &sessionCreateInfo, &session), -1);

	context->Window = SDL_CreateWindow(context->ExampleName, 800, 600, SDL_WINDOW_RESIZABLE);
	if (!context->Window) {
		SDL_Log("CreateWindow failed: %s", SDL_GetError());
		return -1;
	}

	if (!SDL_ClaimWindowForGPUDevice(context->Device, context->Window)) {
		SDL_Log("ClaimWindow failed");
		return -1;
	}

	InitializeAssetLoader();

	// Load all shaders
	ColorVertexShader = LoadShader(context->Device, "PositionColorTransform.vert", 0, 1, 0, 0);
	ColorFragmentShader = LoadShader(context->Device, "SolidColor.frag", 0, 0, 0, 0);
	SkyboxVertexShader = LoadShader(context->Device, "Skybox.vert", 0, 1, 0, 0);
	SkyboxFragmentShader = LoadShader(context->Device, "Skybox.frag", 1, 0, 0, 0);
	TexturedVertexShader = LoadShader(context->Device, "TexturedQuadWithMatrix.vert", 0, 1, 0, 0);
	TexturedFragmentShader = LoadShader(context->Device, "TexturedQuad.frag", 1, 0, 0, 0);

	if (!ColorVertexShader || !ColorFragmentShader || !SkyboxVertexShader || 
	    !SkyboxFragmentShader || !TexturedVertexShader || !TexturedFragmentShader) {
		SDL_Log("Failed to load shaders!");
		return -1;
	}

	// Create desktop pipelines
	SDL_GPUTextureFormat windowFormat = SDL_GetGPUSwapchainTextureFormat(context->Device, context->Window);
	CubePipelineDesktop = CreateColorPipeline(context, windowFormat);
	SkyboxPipelineDesktop = CreateSkyboxPipeline(context, windowFormat);
	FloorPipelineDesktop = CreateTexturedPipeline(context, windowFormat);

	// Create GPU buffers
	CreateCubeBuffers(context);
	CreateSkyboxBuffers(context);
	CreateFloorBuffers(context);

	SDL_Log("VrGallery initialized! Put on your headset.");
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

			XR_ERR_RET(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, NULL), -1);

			XrViewConfigurationView *viewConfigViews = SDL_stack_alloc(XrViewConfigurationView, viewCount);
			for (Uint32 i = 0; i < viewCount; i++) viewConfigViews[i] = (XrViewConfigurationView){XR_TYPE_VIEW_CONFIGURATION_VIEW};

			XR_ERR_RET(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, viewConfigViews), -1);

			if(viewCount > 0) {
				swapchains = SDL_calloc(viewCount, sizeof(Swapchain));
				views = SDL_calloc(viewCount, sizeof(XrView));

				for(Uint32 i = 0; i < viewCount; i++) {
					views[i] = (XrView){XR_TYPE_VIEW, .pose = {.orientation = IDENTITY_QUAT}};

					Swapchain *swapchain = &swapchains[i];
					XrViewConfigurationView viewConfig = viewConfigViews[i];

					XrSwapchainCreateInfo swapchainCreateInfo = {
						.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
						.width = viewConfig.recommendedImageRectWidth,
						.height = viewConfig.recommendedImageRectHeight,
						.mipCount = 1, .sampleCount = 1, .faceCount = 1,
						.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
						.arraySize = 1
					};

					swapchain->size = (XrExtent2Di){swapchainCreateInfo.width, swapchainCreateInfo.height};

					XR_ERR_RET(SDL_CreateGPUXRSwapchain(context->Device, session, &swapchainCreateInfo,
						&swapchain->format, &swapchain->swapchain, &swapchain->images), -1);

					if (i == 0 && !CubePipeline) {
						SDL_Log("Creating XR pipelines for format: %d", swapchain->format);
						CubePipeline = CreateColorPipeline(context, swapchain->format);
						SkyboxPipeline = CreateSkyboxPipeline(context, swapchain->format);
						FloorPipeline = CreateTexturedPipeline(context, swapchain->format);
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
			doXrFrameLoop = false;
			XR_ERR_RET(xrEndSession(session), -1);
			break;
		case XR_SESSION_STATE_EXITING:
			return -1;
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
		switch(event.type) {
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
				if(HandleStateChangedEvent(context, (XrEventDataSessionStateChanged*)&event) != 0) return -1;
				break;
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				return -1;
			default:
				break;
		}
	}
	return 0;
}

static int Update(Context* context)
{
	if(HandleXrEvent(context) != 0) return -1;
	Time += context->DeltaTime;
	return 0;
}

static void RenderScene(Context* context, SDL_GPUCommandBuffer* cmdbuf, SDL_GPUTexture* texture,
                        SDL_GPUGraphicsPipeline* cubePipe, SDL_GPUGraphicsPipeline* skyboxPipe, SDL_GPUGraphicsPipeline* floorPipe,
                        XrView view, int width, int height)
{
	Matrix4x4 viewMatrix = Matrix4x4_CreateFromXrPose(view.pose);
	Matrix4x4 projMatrix = Matrix4x4_CreateProjectionFov(view.fov, 0.05f, 100.0f);

	SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(cmdbuf, &(SDL_GPUColorTargetInfo){
		.texture = texture,
		.clear_color = {0.1f, 0.1f, 0.15f, 1.0f},
		.load_op = SDL_GPU_LOADOP_CLEAR,
		.store_op = SDL_GPU_STOREOP_STORE,
	}, 1, NULL);

	SDL_SetGPUViewport(renderPass, &(SDL_GPUViewport){0, 0, (float)width, (float)height, 0, 1});
	SDL_SetGPUScissor(renderPass, &(SDL_Rect){0, 0, width, height});

	// Draw skybox (no translation, only rotation from view)
	if (skyboxPipe && SkyboxTexture) {
		SDL_BindGPUGraphicsPipeline(renderPass, skyboxPipe);
		SDL_BindGPUVertexBuffers(renderPass, 0, &(SDL_GPUBufferBinding){SkyboxVertexBuffer, 0}, 1);
		SDL_BindGPUIndexBuffer(renderPass, &(SDL_GPUBufferBinding){SkyboxIndexBuffer, 0}, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		SDL_BindGPUFragmentSamplers(renderPass, 0, &(SDL_GPUTextureSamplerBinding){SkyboxTexture, SkyboxSampler}, 1);
		
		// Skybox view matrix without translation
		Matrix4x4 skyboxView = viewMatrix;
		skyboxView.m41 = 0; skyboxView.m42 = 0; skyboxView.m43 = 0;
		Matrix4x4 skyboxMVP = Matrix4x4_Multiply(skyboxView, projMatrix);
		SDL_PushGPUVertexUniformData(cmdbuf, 0, &skyboxMVP, sizeof(skyboxMVP));
		SDL_DrawGPUIndexedPrimitives(renderPass, 36, 1, 0, 0, 0);
	}

	// Draw floor
	if (floorPipe && FloorTexture) {
		SDL_BindGPUGraphicsPipeline(renderPass, floorPipe);
		SDL_BindGPUVertexBuffers(renderPass, 0, &(SDL_GPUBufferBinding){FloorVertexBuffer, 0}, 1);
		SDL_BindGPUIndexBuffer(renderPass, &(SDL_GPUBufferBinding){FloorIndexBuffer, 0}, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		SDL_BindGPUFragmentSamplers(renderPass, 0, &(SDL_GPUTextureSamplerBinding){FloorTexture, FloorSampler}, 1);
		
		Matrix4x4 floorMVP = Matrix4x4_Multiply(viewMatrix, projMatrix);
		SDL_PushGPUVertexUniformData(cmdbuf, 0, &floorMVP, sizeof(floorMVP));
		SDL_DrawGPUIndexedPrimitives(renderPass, 6, 1, 0, 0, 0);
	}

	// Draw cubes
	if (cubePipe) {
		SDL_BindGPUGraphicsPipeline(renderPass, cubePipe);
		SDL_BindGPUVertexBuffers(renderPass, 0, &(SDL_GPUBufferBinding){CubeVertexBuffer, 0}, 1);
		SDL_BindGPUIndexBuffer(renderPass, &(SDL_GPUBufferBinding){CubeIndexBuffer, 0}, SDL_GPU_INDEXELEMENTSIZE_16BIT);

		for (int i = 0; i < NUM_CUBES; i++) {
			float rotSpeed = 0.5f + i * 0.3f;
			float angle = Time * rotSpeed;
			
			Matrix4x4 rotX = Matrix4x4_CreateRotationX(angle * 0.7f);
			Matrix4x4 rotY = Matrix4x4_CreateRotationY(angle);
			Matrix4x4 rotation = Matrix4x4_Multiply(rotX, rotY);
			Matrix4x4 translation = Matrix4x4_CreateTranslation(CubePositions[i].x, CubePositions[i].y, CubePositions[i].z);
			Matrix4x4 model = Matrix4x4_Multiply(rotation, translation);
			Matrix4x4 modelView = Matrix4x4_Multiply(model, viewMatrix);
			Matrix4x4 mvp = Matrix4x4_Multiply(modelView, projMatrix);

			SDL_PushGPUVertexUniformData(cmdbuf, 0, &mvp, sizeof(mvp));
			SDL_DrawGPUIndexedPrimitives(renderPass, 36, 1, 0, 0, 0);
		}
	}

	SDL_EndGPURenderPass(renderPass);
}

static int RenderDesktopView(Context* context, SDL_GPUCommandBuffer* cmdbuf)
{
	SDL_GPUTexture* swapchainTexture;
	SDL_AcquireGPUSwapchainTexture(cmdbuf, context->Window, &swapchainTexture, NULL, NULL);
	if (!swapchainTexture) return 0;

	XrView view = {XR_TYPE_VIEW, .pose = IDENTITY_POSE, .fov = {-0.8f, 0.8f, 0.8f, -0.8f}};
	if(viewCount > 0 && views) view = views[0];

	int w, h;
	SDL_GetWindowSize(context->Window, &w, &h);

	RenderScene(context, cmdbuf, swapchainTexture, CubePipelineDesktop, SkyboxPipelineDesktop, FloorPipelineDesktop, view, w, h);
	return 0;
}

static int Draw(Context* context)
{
	SDL_GPUCommandBuffer* cmdbuf = SDL_AcquireGPUCommandBuffer(context->Device);
	if (!cmdbuf) return -1;

	Uint32 viewCountOutput = 0;
	if(doXrFrameLoop)
	{
		XrFrameWaitInfo frameWaitInfo = {XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState frameState = {XR_TYPE_FRAME_STATE};
		XR_ERR_RET(xrWaitFrame(session, &frameWaitInfo, &frameState), -1);
		XR_ERR_RET(xrBeginFrame(session, &(XrFrameBeginInfo){XR_TYPE_FRAME_BEGIN_INFO}), -1);

		XrCompositionLayerProjectionView *projectionViews = SDL_stack_alloc(XrCompositionLayerProjectionView, viewCount);

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
				XR_ERR_RET(xrWaitSwapchainImage(swapchain.swapchain, &(XrSwapchainImageWaitInfo){XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION}), -1);

				RenderScene(context, cmdbuf, swapchain.images[swapchainIndex], CubePipeline, SkyboxPipeline, FloorPipeline, views[i], swapchain.size.width, swapchain.size.height);
			}

			RenderDesktopView(context, cmdbuf);
			SDL_SubmitGPUCommandBuffer(cmdbuf);

			for(Uint32 i = 0; i < viewCountOutput; i++) {
				XR_ERR_RET(xrReleaseSwapchainImage(swapchains[i].swapchain, NULL), -1);
				projectionViews[i] = (XrCompositionLayerProjectionView){
					.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
					.fov = views[i].fov,
					.pose = views[i].pose,
					.subImage = { .swapchain = swapchains[i].swapchain, .imageRect = {.extent = swapchains[i].size} }
				};
			}
		} else {
			RenderDesktopView(context, cmdbuf);
			SDL_SubmitGPUCommandBuffer(cmdbuf);
		}

		const XrCompositionLayerBaseHeader *layers[1] = {
			(const XrCompositionLayerBaseHeader*)&(XrCompositionLayerProjection){
				.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
				.space = localSpace,
				.viewCount = viewCountOutput,
				.views = projectionViews,
			}
		};

		XR_ERR_RET(xrEndFrame(session, &(XrFrameEndInfo){
			.type = XR_TYPE_FRAME_END_INFO,
			.displayTime = frameState.predictedDisplayTime,
			.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
			.layerCount = frameState.shouldRender ? 1 : 0,
			.layers = layers
		}), -1);

		SDL_stack_free(projectionViews);
	} else {
		RenderDesktopView(context, cmdbuf);
		SDL_SubmitGPUCommandBuffer(cmdbuf);
	}

	return 0;
}

static void Quit(Context* context)
{
	if (CubePipeline) SDL_ReleaseGPUGraphicsPipeline(context->Device, CubePipeline);
	if (CubePipelineDesktop) SDL_ReleaseGPUGraphicsPipeline(context->Device, CubePipelineDesktop);
	if (SkyboxPipeline) SDL_ReleaseGPUGraphicsPipeline(context->Device, SkyboxPipeline);
	if (SkyboxPipelineDesktop) SDL_ReleaseGPUGraphicsPipeline(context->Device, SkyboxPipelineDesktop);
	if (FloorPipeline) SDL_ReleaseGPUGraphicsPipeline(context->Device, FloorPipeline);
	if (FloorPipelineDesktop) SDL_ReleaseGPUGraphicsPipeline(context->Device, FloorPipelineDesktop);

	if (ColorVertexShader) SDL_ReleaseGPUShader(context->Device, ColorVertexShader);
	if (ColorFragmentShader) SDL_ReleaseGPUShader(context->Device, ColorFragmentShader);
	if (SkyboxVertexShader) SDL_ReleaseGPUShader(context->Device, SkyboxVertexShader);
	if (SkyboxFragmentShader) SDL_ReleaseGPUShader(context->Device, SkyboxFragmentShader);
	if (TexturedVertexShader) SDL_ReleaseGPUShader(context->Device, TexturedVertexShader);
	if (TexturedFragmentShader) SDL_ReleaseGPUShader(context->Device, TexturedFragmentShader);

	if (CubeVertexBuffer) SDL_ReleaseGPUBuffer(context->Device, CubeVertexBuffer);
	if (CubeIndexBuffer) SDL_ReleaseGPUBuffer(context->Device, CubeIndexBuffer);
	if (SkyboxVertexBuffer) SDL_ReleaseGPUBuffer(context->Device, SkyboxVertexBuffer);
	if (SkyboxIndexBuffer) SDL_ReleaseGPUBuffer(context->Device, SkyboxIndexBuffer);
	if (FloorVertexBuffer) SDL_ReleaseGPUBuffer(context->Device, FloorVertexBuffer);
	if (FloorIndexBuffer) SDL_ReleaseGPUBuffer(context->Device, FloorIndexBuffer);

	if (SkyboxTexture) SDL_ReleaseGPUTexture(context->Device, SkyboxTexture);
	if (SkyboxSampler) SDL_ReleaseGPUSampler(context->Device, SkyboxSampler);
	if (FloorTexture) SDL_ReleaseGPUTexture(context->Device, FloorTexture);
	if (FloorSampler) SDL_ReleaseGPUSampler(context->Device, FloorSampler);

	if(swapchains) {
		for(Uint32 i = 0; i < viewCount; i++) {
			SDL_DestroyGPUXRSwapchain(context->Device, swapchains[i].swapchain, swapchains[i].images);
		}
		SDL_free(swapchains);
	}
	if (views) SDL_free(views);
	if (localSpace) xrDestroySpace(localSpace);
	if (session) xrDestroySession(session);
	if (instance) xrDestroyInstance(instance);

	CommonQuit(context);
}

Example VrGallery_Example = { "VrGallery", Init, Update, Draw, Quit };
