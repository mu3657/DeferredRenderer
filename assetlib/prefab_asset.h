#pragma once
#include <asset_loader.h>
#include <unordered_map>

namespace assets {

	struct PrefabInfo {
		//points to matrix array in the blob
		std::unordered_map<uint64_t, int> node_matrices;
		std::unordered_map<uint64_t, std::string> node_names;

		std::unordered_map<uint64_t, uint64_t> node_parents;

		struct NodeMesh {
			std::string material_path;
			std::string mesh_path;
		};

		struct NodeLight {
			float color[3]{1.f, 1.f, 1.f};
			float intensity{1.f};
			int type{0}; // 0 = point, 1 = directional, 2 = spot, 3 = rect area
			float range{0.f};
			float width{1.f};
			float height{1.f};
		};

		std::unordered_map<uint64_t, NodeMesh> node_meshes;
		std::unordered_map<uint64_t, NodeLight> node_lights;

		std::vector<std::array<float,16>> matrices;
	};


	PrefabInfo read_prefab_info(AssetFile* file);
	AssetFile pack_prefab(const PrefabInfo& info);
}
