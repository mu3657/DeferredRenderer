#include "mesh_asset.h"
#include "json.hpp"
#include "lz4.h"


assets::VertexFormat parse_format(const char* f) {

	if (strcmp(f, "PNCV_F32") == 0)
	{
		return assets::VertexFormat::PNCV_F32;
	}
	else if (strcmp(f, "P32N8C8V16") == 0)
	{
		return assets::VertexFormat::P32N8C8V16;
	}
	else if (strcmp(f, "Dynamic") == 0) 
	{
		return assets::VertexFormat::Dynamic;
	}
	else
	{
		return assets::VertexFormat::Unknown;
	}
}

assets::VertexAttributeFormat parse_attribute_format(const std::string& f) {
	if (f == "Float32x2") return assets::VertexAttributeFormat::Float32x2;
	if (f == "Float32x3") return assets::VertexAttributeFormat::Float32x3;
	if (f == "Float32x4") return assets::VertexAttributeFormat::Float32x4;
	if (f == "Uint8x4_UNorm") return assets::VertexAttributeFormat::Uint8x4_UNorm;
	if (f == "Uint16x2_Float") return assets::VertexAttributeFormat::Uint16x2_Float;
	return assets::VertexAttributeFormat::Unknown;
}

assets::VertexAttributeSemantic parse_attribute_semantic(const std::string& s) {
	if (s == "Position") return assets::VertexAttributeSemantic::Position;
	if (s == "Normal") return assets::VertexAttributeSemantic::Normal;
	if (s == "Color") return assets::VertexAttributeSemantic::Color;
	if (s == "TexCoord0") return assets::VertexAttributeSemantic::TexCoord0;
	if (s == "TexCoord1") return assets::VertexAttributeSemantic::TexCoord1;
	if (s == "TexCoord2") return assets::VertexAttributeSemantic::TexCoord2;
	if (s == "TexCoord3") return assets::VertexAttributeSemantic::TexCoord3;
	if (s == "Tangent") return assets::VertexAttributeSemantic::Tangent;
	if (s == "Joints") return assets::VertexAttributeSemantic::Joints;
	if (s == "Weights") return assets::VertexAttributeSemantic::Weights;
	return assets::VertexAttributeSemantic::Position;
}

std::string format_attribute_format(assets::VertexAttributeFormat f) {
	if (f == assets::VertexAttributeFormat::Float32x2) return "Float32x2";
	if (f == assets::VertexAttributeFormat::Float32x3) return "Float32x3";
	if (f == assets::VertexAttributeFormat::Float32x4) return "Float32x4";
	if (f == assets::VertexAttributeFormat::Uint8x4_UNorm) return "Uint8x4_UNorm";
	if (f == assets::VertexAttributeFormat::Uint16x2_Float) return "Uint16x2_Float";
	return "Unknown";
}

std::string format_attribute_semantic(assets::VertexAttributeSemantic s) {
	if (s == assets::VertexAttributeSemantic::Position) return "Position";
	if (s == assets::VertexAttributeSemantic::Normal) return "Normal";
	if (s == assets::VertexAttributeSemantic::Color) return "Color";
	if (s == assets::VertexAttributeSemantic::TexCoord0) return "TexCoord0";
	if (s == assets::VertexAttributeSemantic::TexCoord1) return "TexCoord1";
	if (s == assets::VertexAttributeSemantic::TexCoord2) return "TexCoord2";
	if (s == assets::VertexAttributeSemantic::TexCoord3) return "TexCoord3";
	if (s == assets::VertexAttributeSemantic::Tangent) return "Tangent";
	if (s == assets::VertexAttributeSemantic::Joints) return "Joints";
	if (s == assets::VertexAttributeSemantic::Weights) return "Weights";
	return "Position";
}

assets::MeshInfo assets::read_mesh_info(AssetFile* file)
{
	MeshInfo info;

	nlohmann::json metadata = nlohmann::json::parse(file->json);

	
	info.vertexBuferSize = metadata["vertex_buffer_size"];		
	info.indexBuferSize = metadata["index_buffer_size"];
	info.indexSize = (uint8_t) metadata["index_size"];
	info.originalFile = metadata["original_file"];

	std::string compressionString = metadata["compression"];
	info.compressionMode = parse_compression(compressionString.c_str());

	std::vector<float> boundsData;
	boundsData.reserve(7);
	boundsData = metadata["bounds"].get<std::vector<float>>();

	info.bounds.origin[0] = boundsData[0];
	info.bounds.origin[1] = boundsData[1];
	info.bounds.origin[2] = boundsData[2];
		
	info.bounds.radius = boundsData[3];
	
	info.bounds.extents[0] = boundsData[4];
	info.bounds.extents[1] = boundsData[5];
	info.bounds.extents[2] = boundsData[6];

	std::string vertexFormat = metadata["vertex_format"];
	info.vertexFormat = parse_format(vertexFormat.c_str());

	if (info.vertexFormat == VertexFormat::Dynamic) {
		info.vertexStride = metadata["vertex_stride"];

		auto attributesArray = metadata["attributes"];
		info.attributes.reserve(attributesArray.size());
		for (auto& attr : attributesArray) {
			VertexAttribute a;
			a.semantic = parse_attribute_semantic(attr["semantic"]);
			a.format = parse_attribute_format(attr["format"]);
			a.offset = attr["offset"];
			a.binding = attr["binding"];
			info.attributes.push_back(a);
		}
	} else {
		info.vertexStride = 0;
	}

    return info;
}

void assets::unpack_mesh(MeshInfo* info, const char* sourcebuffer, size_t sourceSize, char* vertexBufer, char* indexBuffer)
{
	//decompressing into temporal vector. TODO: streaming decompress directly on the buffers
	std::vector<char> decompressedBuffer;
	decompressedBuffer.resize(info->vertexBuferSize + info->indexBuferSize);

	LZ4_decompress_safe(sourcebuffer, decompressedBuffer.data(), static_cast<int>(sourceSize), static_cast<int>(decompressedBuffer.size()));

	//copy vertex buffer
	memcpy(vertexBufer, decompressedBuffer.data(), info->vertexBuferSize);

	//copy index buffer
	memcpy(indexBuffer, decompressedBuffer.data() + info->vertexBuferSize, info->indexBuferSize);
}

assets::AssetFile assets::pack_mesh(MeshInfo* info, char* vertexData, char* indexData)
{
    AssetFile file;
	file.type[0] = 'M';
	file.type[1] = 'E';
	file.type[2] = 'S';
	file.type[3] = 'H';
	file.version = 1;

	nlohmann::json metadata;
	if (info->vertexFormat == VertexFormat::P32N8C8V16) {
		metadata["vertex_format"] = "P32N8C8V16";
	}
	else if (info->vertexFormat == VertexFormat::PNCV_F32)
	{
		metadata["vertex_format"] = "PNCV_F32";
	}
	else if (info->vertexFormat == VertexFormat::Dynamic)
	{
		metadata["vertex_format"] = "Dynamic";
		metadata["vertex_stride"] = info->vertexStride;

		nlohmann::json attributesArray = nlohmann::json::array();
		for (const auto& attr : info->attributes) {
			nlohmann::json jAttr;
			jAttr["semantic"] = format_attribute_semantic(attr.semantic);
			jAttr["format"] = format_attribute_format(attr.format);
			jAttr["offset"] = attr.offset;
			jAttr["binding"] = attr.binding;
			attributesArray.push_back(jAttr);
		}
		metadata["attributes"] = attributesArray;
	}
	metadata["vertex_buffer_size"] = info->vertexBuferSize;
	metadata["index_buffer_size"] = info->indexBuferSize;
	metadata["index_size"] = info->indexSize;
	metadata["original_file"] = info->originalFile;

	std::vector<float> boundsData;
	boundsData.resize(7);

	boundsData[0] = info->bounds.origin[0];
	boundsData[1] = info->bounds.origin[1];
	boundsData[2] = info->bounds.origin[2];

	boundsData[3] = info->bounds.radius;

	boundsData[4] = info->bounds.extents[0];
	boundsData[5] = info->bounds.extents[1];
	boundsData[6] = info->bounds.extents[2];

	metadata["bounds"] = boundsData;

	size_t fullsize = info->vertexBuferSize + info->indexBuferSize;

	std::vector<char> merged_buffer;
	merged_buffer.resize(fullsize);

	//copy vertex buffer
	memcpy(merged_buffer.data(), vertexData, info->vertexBuferSize);

	//copy index buffer
	memcpy(merged_buffer.data() + info->vertexBuferSize, indexData, info->indexBuferSize);


	//compress buffer and copy it into the file struct
	size_t compressStaging = LZ4_compressBound(static_cast<int>(fullsize));

	file.binaryBlob.resize(compressStaging);

	int compressedSize = LZ4_compress_default(merged_buffer.data(), file.binaryBlob.data(), static_cast<int>(merged_buffer.size()), static_cast<int>(compressStaging));
	file.binaryBlob.resize(compressedSize);

	metadata["compression"] = "LZ4";

	file.json = metadata.dump();

	return file;
}

assets::MeshBounds assets::calculateBounds(Vertex_f32_PNCV* vertices, size_t count)
{
	MeshBounds bounds;

	float min[3] = { std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max() };
	float max[3] = { std::numeric_limits<float>::min(),std::numeric_limits<float>::min(),std::numeric_limits<float>::min() };

	for (int i = 0; i < count; i++) {
		min[0] = std::min(min[0], vertices[i].position[0]);
		min[1] = std::min(min[1], vertices[i].position[1]);
		min[2] = std::min(min[2], vertices[i].position[2]);

		max[0] = std::max(max[0], vertices[i].position[0]);
		max[1] = std::max(max[1], vertices[i].position[1]);
		max[2] = std::max(max[2], vertices[i].position[2]);
	}

	bounds.extents[0] = (max[0] - min[0]) / 2.0f;
	bounds.extents[1] = (max[1] - min[1]) / 2.0f;
	bounds.extents[2] = (max[2] - min[2]) / 2.0f;

	bounds.origin[0] = bounds.extents[0] + min[0];
	bounds.origin[1] = bounds.extents[1] + min[1];
	bounds.origin[2] = bounds.extents[2] + min[2];

	//go through the vertices again to calculate the exact bounding sphere radius
	float r2 = 0;
	for (int i = 0; i < count; i++) {

		float offset[3];
		offset[0] = vertices[i].position[0] - bounds.origin[0];
		offset[1] = vertices[i].position[1] - bounds.origin[1];
		offset[2] = vertices[i].position[2] - bounds.origin[2];

		//pithagoras
		float distance = offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2];
		r2 = std::max(r2, distance);
	}

	bounds.radius = std::sqrt(r2);

	return bounds;
}
