cmake_minimum_required(VERSION 3.20)
project(voxel_rpg C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -ffast-math допустим: во всей игре нет зависимостей от NaN/Inf.
#
# LTO включается и на компиляции, и на линковке разделяемой библиотеки —
# native-lib собирается как SHARED, поэтому CMAKE_EXE_LINKER_FLAGS к ней
# не применяется. Выключается переключателем: при сборке на самом
# телефоне линковка с LTO держит в памяти сразу весь модуль, и на
# устройстве с малым объёмом ОЗУ процесс убивает ядро — сборка
# обрывается без внятного сообщения. build.sh снимает LTO сам, если
# свободной памяти мало.
option(VOXEL_LTO "Межмодульная оптимизация при сборке release" ON)

if(VOXEL_LTO)
    set(VOXEL_LTO_FLAG "-flto")
else()
    set(VOXEL_LTO_FLAG "")
    message(STATUS "LTO выключена (VOXEL_LTO=OFF)")
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -ffast-math -fno-exceptions -fno-rtti ${VOXEL_LTO_FLAG}")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -DDEBUG -fno-exceptions -fno-rtti")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${VOXEL_LTO_FLAG}")

add_compile_definitions(
    GLM_FORCE_DEPTH_ZERO_TO_ONE   # Vulkan: глубина в [0,1], а не [-1,1]
    GLM_FORCE_RADIANS
    VK_USE_PLATFORM_ANDROID_KHR   # открывает vkCreateAndroidSurfaceKHR
    ENTT_NO_ETO                   # EnTT хранит и пустые компоненты-теги
)

# ============================================================
# android_native_app_glue из NDK: реализует NativeActivity и
# вызывает android_main. Собственных копий в репозитории нет.
# ============================================================
if(DEFINED ANDROID_NDK)
    set(GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
    if(NOT EXISTS "${GLUE_DIR}/android_native_app_glue.c")
        message(FATAL_ERROR
            "native_app_glue не найден в NDK: ${GLUE_DIR}. "
            "Проверьте ANDROID_NDK_HOME.")
    endif()
    add_library(native_app_glue STATIC "${GLUE_DIR}/android_native_app_glue.c")
    target_include_directories(native_app_glue PUBLIC "${GLUE_DIR}")
endif()

# Отпечаток сборки в журнале. Без него по логу с устройства нельзя
# сказать, какая именно версия установлена, и починенная ошибка
# выглядит как непочиненная.
#
# Считается на КАЖДУЮ сборку, а не при настройке CMake: настройка
# происходит однажды и переиспользуется, поэтому прежняя метка
# застревала и подписывала свежие сборки давнишним коммитом. Подробности
# и пометка «грязный» — в cmake/build_sha.cmake.
set(VOXEL_SHA_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(VOXEL_SHA_H   ${VOXEL_SHA_DIR}/build_sha.h)
file(MAKE_DIRECTORY ${VOXEL_SHA_DIR})
add_custom_target(voxel_build_sha ALL
    COMMAND ${CMAKE_COMMAND}
            -DOUT=${VOXEL_SHA_H}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_sha.cmake
    BYPRODUCTS ${VOXEL_SHA_H}
    COMMENT "отпечаток сборки")

add_library(native-lib SHARED
    src/main.cpp

    # Ядро: планировщик задач, память, журнал падений
    src/core/clipboard.cpp
    src/core/crashlog.cpp
    src/core/job_system.cpp
    src/core/shared_dir.cpp

    # Настройки, локализация, таймер игры
    src/config/localization.cpp
    src/config/playtime.cpp
    src/config/settings.cpp

    # Сенсорный ввод
    src/input/touch.cpp

    # Vulkan: контекст, буферы, пайплайны
    src/vk/vk_buffer.cpp
    src/vk/vk_context.cpp
    src/vk/vk_renderpass.cpp
    src/vk/vk_descriptors.cpp
    src/vk/vk_pipeline.cpp
    src/vk/vk_shader.cpp
    src/vk/vk_staging_pool.cpp
    src/vk/vk_texture.cpp

    # Мир: чанки, генерация, навигация
    src/world/ai/pathfinding.cpp
    src/world/biome.cpp
    src/world/block.cpp
    src/world/chunk.cpp
    src/world/chunk_manager.cpp
    src/world/debug_scene.cpp
    src/world/enchant_altar.cpp
    src/world/features.cpp
    src/world/hazards.cpp
    src/world/noise.cpp
    src/world/spawn.cpp
    src/world/terrain.cpp
    src/world/world_spec.cpp

    # Рендер
    src/render/block_outline.cpp
    src/render/chunk_renderer.cpp
    src/render/item_renderer.cpp
    src/render/mesh_builder.cpp
    src/render/pass_sweep.cpp
    src/render/mob_renderer.cpp
    src/render/npc_renderer.cpp
    src/render/projectile_renderer.cpp
    src/render/render_system.cpp
    src/render/voxel_pipeline.cpp
    src/render/skybox.cpp

    # Физика
    src/physics/character_controller.cpp
    src/physics/collision.cpp
    src/physics/raycast.cpp

    # Игрок
    src/player/player.cpp
    src/player/player_rig.cpp

    # Интерфейс
    src/ui/drag_drop.cpp
    src/ui/scroll.cpp
    src/ui/slider.cpp
    src/ui/text_entry.cpp
    src/ui/ui_atlas.cpp
    src/ui/ui_context.cpp
    src/ui/ui_renderer.cpp
    src/ui/ui_system.cpp

    # Мобы
    src/mobs/mob_ai.cpp
    src/entity/beast_rig.cpp
    src/entity/humanoid_rig.cpp
    src/entity/mob_rigs.cpp
    src/entity/showcase.cpp
    src/mobs/mob_def.cpp
    src/mobs/spawner.cpp

    # Бой
    src/combat/combat_controller.cpp
    src/combat/components.cpp
    src/combat/damage.cpp
    src/combat/enchantment.cpp
    src/combat/hit_detection.cpp
    src/combat/projectile.cpp
    src/combat/resonance.cpp
    src/combat/spatial_hash.cpp
    src/combat/status_effects.cpp
    src/combat/weapon.cpp

    # Прогрессия
    src/progression/attributes.cpp
    src/progression/progression.cpp
    src/progression/resource_regen.cpp
    src/progression/skill_tree.cpp

    # Фракции
    src/factions/faction.cpp

    # Квесты
    src/quests/quest.cpp
    src/quests/quest_def.cpp
    src/quests/quest_generator.cpp
    src/quests/story.cpp

    # NPC
    src/npc/dialogue.cpp
    src/npc/npc_ai.cpp
    src/npc/npc_rig.cpp
    src/npc/npc_def.cpp
    src/npc/npc_spawner.cpp

    # Сохранения
    src/save/save_export.cpp
    src/save/save_inventory.cpp
    src/save/save_manager.cpp
    src/save/save_npc.cpp
    src/save/save_player.cpp
    src/save/save_slot.cpp
    src/save/world_delta.cpp
    src/save/zlib_util.cpp

    # Предметы и инвентарь
    src/items/currency.cpp
    src/items/inventory.cpp
    src/items/item_def.cpp
    src/items/item_pickup.cpp
    src/items/item_stack.cpp
    src/items/item_use.cpp
    src/items/throwable.cpp
    src/items/loot_table.cpp

    # Крафт
    src/crafting/crafting.cpp
    src/crafting/crafting_station.cpp
    src/crafting/recipe.cpp

    # Торговля
    src/trade/trade.cpp

    # Звук
    src/audio/audio_engine.cpp
    src/audio/audio_events.cpp
    src/audio/music.cpp
    src/audio/sound_registry.cpp
)

target_include_directories(native-lib PRIVATE src)

# Заголовочные зависимости: GLM и EnTT. Обе клонируются build.sh
# в third_party/ и в репозиторий не коммитятся.
set(TP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party")

if(EXISTS "${TP_DIR}/glm/glm")
    target_include_directories(native-lib SYSTEM PRIVATE "${TP_DIR}/glm")
else()
    message(FATAL_ERROR
        "GLM не найден в third_party/glm. Запустите build.sh — он её клонирует.")
endif()

# EnTT — ECS из ТЗ 3.2. Собирается без исключений и RTTI.
if(EXISTS "${TP_DIR}/entt/include/entt/entt.hpp")
    target_include_directories(native-lib SYSTEM PRIVATE "${TP_DIR}/entt/include")
else()
    message(FATAL_ERROR
        "EnTT не найден в third_party/entt. Запустите build.sh — он её клонирует.")
endif()

target_compile_options(native-lib PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    # Структуры Vulkan инициализируются как VkXxx{VK_STRUCTURE_TYPE_...}:
    # остальные поля зануляются, и предупреждение про pNext — шум.
    -Wno-missing-field-initializers
    -fvisibility=hidden
)

add_dependencies(native-lib voxel_build_sha)
target_include_directories(native-lib PRIVATE ${VOXEL_SHA_DIR})

# Диагностическая сборка: -DVOXEL_DEBUG_SCENE=ON.
#
# Включает минимальную детерминированную сцену НА УРОВНЕ СБОРКИ. На
# свежем устройстве settings.cfg отсутствует, и полагаться на его
# ручную правку нельзя; а уже существующий файл с debug_scene = false
# не должен уметь выключить диагностический APK обратно.
#
# По умолчанию выключено: обычные сборки остаются обычной игрой.
option(VOXEL_DEBUG_SCENE "Собрать диагностический APK с минимальной сценой" OFF)
if(VOXEL_DEBUG_SCENE)
    target_compile_definitions(native-lib PRIVATE VOXEL_DEBUG_SCENE=1)
    message(STATUS "VoxelRPG: диагностическая сборка, debug_scene=true")
endif()

if(TARGET native_app_glue)
    target_link_libraries(native-lib native_app_glue)

    # Без этой строки приложение падает мгновенно и без следа.
    #
    # native_app_glue — статическая библиотека, а из неё линковщик берёт
    # только те объекты, на чьи символы кто-то ссылается. На
    # ANativeActivity_onCreate не ссылается никто: его ищет система
    # через dlsym уже в рантайме. В результате glue не попадает в .so
    # вовсе, NativeActivity не находит точку входа и убивает процесс
    # ещё до первой нашей строки — ни логов, ни отчёта о падении.
    #
    # -u объявляет символ нужным, и объект вытягивается из архива.
    target_link_options(native-lib PRIVATE -u ANativeActivity_onCreate)
endif()

find_library(Z_LIB z)
if(Z_LIB)
    target_link_libraries(native-lib ${Z_LIB})
else()
    target_link_libraries(native-lib z)
endif()

# libaaudio намеренно НЕ линкуется: она появилась в API 26, а минимум
# по ТЗ — API 24. Запись DT_NEEDED на неё сделала бы .so незагружаемым
# на Android 7.0/7.1 — приложение не стартовало бы вообще, вместо того
# чтобы просто работать без звука. Символы берутся через dlopen в
# audio_engine.cpp.
target_link_libraries(native-lib
    android
    log
    vulkan
)
