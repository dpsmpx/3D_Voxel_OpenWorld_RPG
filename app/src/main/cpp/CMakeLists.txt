cmake_minimum_required(VERSION 3.20)
project(voxel_rpg CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -ffast-math -fno-exceptions -fno-rtti -flto")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -DDEBUG")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-flto")

add_compile_definitions(GLM_FORCE_DEPTH_ZERO_TO_ONE)

add_library(native-lib SHARED
    src/main.cpp

    # Phase 1
    src/core/job_system.cpp
    src/input/touch.cpp

    # Phase 2
    src/vk/vk_context.cpp
    src/vk/vk_shader.cpp
    src/vk/vk_buffer.cpp
    src/vk/vk_texture.cpp
    src/vk/vk_descriptors.cpp
    src/vk/vk_pipeline.cpp
    src/vk/vk_staging_pool.cpp
    src/world/chunk.cpp
    src/world/chunk_manager.cpp
    src/world/block.cpp

    # Phase 3
    src/render/atlas_builder.cpp
    src/render/mesh_builder.cpp
    src/render/chunk_renderer.cpp
    src/render/skybox.cpp
    src/render/instanced_renderer.cpp

    # Phase 4
    src/world/biome.cpp
    src/world/terrain.cpp
    src/world/features.cpp

    # Phase 5
    src/physics/collision.cpp
    src/physics/raycast.cpp
    src/physics/character_controller.cpp
    src/player/player.cpp

    # Phase 6
    src/ui/ui_atlas.cpp
    src/ui/ui_renderer.cpp
    src/ui/ui_context.cpp
    src/ui/ui_system.cpp
    src/render/block_outline.cpp

    # Phase 7
    src/world/ai/pathfinding.cpp
    src/mobs/mob_def.cpp
    src/mobs/mob_ai.cpp
    src/mobs/spawner.cpp
    src/render/mob_renderer.cpp

    # Phase 8
    src/combat/damage.cpp
    src/combat/weapon.cpp
    src/combat/enchantment.cpp
    src/combat/resonance.cpp
    src/combat/components.cpp
    src/combat/hit_detection.cpp
    src/combat/status_effects.cpp
    src/combat/combat_controller.cpp
    src/combat/projectile.cpp
    src/render/projectile_renderer.cpp

    # Phase 9
    src/progression/attributes.cpp
    src/progression/skill_tree.cpp
    src/progression/progression.cpp
    src/progression/resource_regen.cpp

    # Phase 10
    src/factions/faction.cpp
    src/quests/quest_def.cpp
    src/quests/quest.cpp
    src/quests/quest_generator.cpp
    src/npc/npc_def.cpp
    src/npc/dialogue.cpp
    src/npc/npc_ai.cpp
    src/npc/npc_spawner.cpp
    src/render/npc_renderer.cpp

    # Phase 11
    src/save/zlib_util.cpp
    src/save/world_delta.cpp
    src/save/save_slot.cpp
    src/save/save_player.cpp
    src/save/save_npc.cpp
    src/save/save_manager.cpp

    # Phase 12
    src/items/item_def.cpp
    src/items/item_stack.cpp
    src/items/inventory.cpp
    src/items/currency.cpp
    src/items/loot_table.cpp
    src/items/item_use.cpp
    src/items/item_pickup.cpp
    src/crafting/recipe.cpp
    src/crafting/crafting.cpp
    src/crafting/crafting_station.cpp
    src/trade/trade.cpp
    src/render/item_renderer.cpp
    src/save/save_inventory.cpp

    # Phase 13
    src/config/settings.cpp
    src/config/localization.cpp
    src/config/playtime.cpp
    src/ui/drag_drop.cpp
    src/ui/scroll.cpp
    src/ui/slider.cpp
    src/ui/minimap.cpp
    src/world/enchant_altar.cpp

    # Phase 14
    src/audio/sound_registry.cpp
    src/audio/audio_engine.cpp
    src/audio/music.cpp
    src/audio/audio_events.cpp

    # Phase 15
    src/combat/spatial_hash.cpp
)

target_include_directories(native-lib PRIVATE src)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/glm")
    target_include_directories(native-lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/glm")
endif()

target_compile_options(native-lib PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -fvisibility=hidden
)

find_library(Z_LIB z)
if(Z_LIB)
    target_link_libraries(native-lib ${Z_LIB})
else()
    target_link_libraries(native-lib z)
endif()

target_link_libraries(native-lib
    android
    log
    vulkan
    EGL
    GLESv3
    aaudio
)