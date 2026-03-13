#pragma once
#include <asset_loader.h>


namespace assets {


	struct Vertex_f32_PNCV {

		float position[3];
		float normal[3];
		float color[3];
		float uv[2];
	};
	struct Vertex_P32N8C8V16 {

		float position[3];
		uint8_t normal[3];
		uint8_t color[3];
		float uv[2];
	};



	enum class VertexFormat : uint32_t
	{
		Unknown = 0,
		PNCV_F32, //everything at 32 bits
		P32N8C8V16, //position at 32 bits, normal at 8 bits, color at 8 bits, uvs at 16 bits float
		Dynamic // Added dynamically parsed vertex format
	};

	enum class VertexAttributeFormat : uint32_t {
		Unknown = 0,
		Float32x2,
		Float32x3,
		Float32x4,
		Uint8x4_UNorm,
		Uint16x2_Float
	};

	enum class VertexAttributeSemantic : uint32_t {
		Position = 0,
		Normal,
		Color,
		TexCoord0,
		TexCoord1,
		TexCoord2,
		TexCoord3,
		Tangent,
		Joints,
		Weights
	};

	struct VertexAttribute {
		VertexAttributeSemantic semantic;
		VertexAttributeFormat format;
		uint32_t offset;
		uint32_t binding; // Usually 0, but good for multi-buffer
	};

	struct MeshBounds {
		
		float origin[3];
		float radius;
		float extents[3];
	};


	struct MeshInfo {
		uint64_t vertexBuferSize;
		uint64_t indexBuferSize;
		uint32_t vertexStride; // Added stride for dynamic format
		MeshBounds bounds;
		VertexFormat vertexFormat;
		std::vector<VertexAttribute> attributes; // Dynamic attributes if VertexFormat == Dynamic
		char indexSize;
		CompressionMode compressionMode;
		std::string originalFile;
	};

	MeshInfo read_mesh_info(AssetFile* file);

	void unpack_mesh(MeshInfo* info, const char* sourcebuffer, size_t sourceSize, char* vertexBufer, char* indexBuffer);

	AssetFile pack_mesh(MeshInfo* info, char* vertexData, char* indexData);

	MeshBounds calculateBounds(Vertex_f32_PNCV* vertices, size_t count);
}