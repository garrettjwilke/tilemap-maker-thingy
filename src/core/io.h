#pragma once

#include "tilemap_doc.h"

#include <string>

namespace tmm {

// Composite image export
std::string export_composite_png(const TilemapDoc& doc, const std::string& path);

// Mega Drive collision export
std::string export_mde_collision_json(const TilemapDoc& doc, const std::string& path);
std::string export_collision_bin(const TilemapDoc& doc, const std::string& path);

// Map project JSON save & load (MapIo v4 format)
std::string save_map_json(const TilemapDoc& doc, const std::string& path, const std::string& tileset_path_override = "");
std::string load_map_json(TilemapDoc& doc, const std::string& path);

// Map project (.tmproj) save & load
std::string save_map_project(const TilemapDoc& doc, const std::string& path, const std::string& tileset_proj_text = "");
std::string load_map_project(TilemapDoc& doc, const std::string& path, std::string* out_tileset_proj_text = nullptr);

// Tileset PNG export
std::string export_tileset_png(const Tileset& tileset, const std::string& path);

// Tileset terrain save & export
std::string save_terrain_file(Tileset& tileset, const std::string& path, const std::string& tileset_image_name = "");
std::string export_tileset_terrain(const Tileset& tileset, const std::string& path, const std::string& tileset_image_name = "");

// File helpers
bool write_text_file(const std::string& path, const std::string& text);
bool read_text_file(const std::string& path, std::string& text);
bool write_binary_file(const std::string& path, const std::vector<uint8_t>& bytes);
bool read_binary_file(const std::string& path, std::vector<uint8_t>& bytes);

} // namespace tmm
