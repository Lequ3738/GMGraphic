#pragma once

#include "../main.h"

struct atlas_image;
struct texture_atlas;

uint texture_atlas_create(uint size, texture_atlas::atlas_kind kind);
bool texture_atlas_add_sprite(uint id, std::string& gmspr_file);