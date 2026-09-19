/**
 * @file story.h
 * @brief Квесты: сюжетная цепочка — главы, идущие одна за другой.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "quest_def.h"

namespace quests {

/// Сколько глав в цепочке.
///
/// Пять: каждая ведёт в своё место мира — к соседней деревне, в
/// Чёрный лес, к замку, к тайнику под руинами и в логово. Цепочка
/// короткая намеренно: её задача — провести игрока по тому, что в
/// мире есть, а не заменить собой игру.
constexpr u8 STORY_CHAPTERS = 5;

/// Продвижение игрока по цепочке — компонент ECS.
///
/// Живёт на игроке, а не в глобальной переменной: сохранение пишет
/// игрока целиком, и глава едет вместе с ним. Глобальная переменная
/// пережила бы загрузку чужого сейва и соврала бы про чужой мир.
struct StoryProgress {
    u8   chapter   = 0;      ///< сколько глав ЗАВЕРШЕНО
    u32  activeId  = 0;      ///< id выданной главы, 0 — не выдана
};

/// Название и суть главы. Не зависит от мира — это текст.
struct StoryChapter {
    const char* title;
    const char* text;
    QuestType   type;
};

/// Глава по номеру. Номер за пределами цепочки — последняя.
const StoryChapter& storyChapter(u8 index);

/// Создать задание очередной главы.
///
/// Цель берётся ИЗ МИРА: деревня, Чёрный лес, замок, тайник и логово
/// ищутся вокруг игрока по-настоящему. Если искомого рядом нет, глава
/// не выдаётся вовсе — отправлять игрока туда, где ничего нет, хуже,
/// чем не дать задания.
///
/// @return сущность задания или недействительная, если выдавать нечего
ecs::Entity offerStoryChapter(ecs::Registry& reg,
                              world::ChunkManager& world,
                              ecs::Entity player,
                              const glm::ivec3& around);

/// Отметить главу сданной и подвинуть цепочку.
/// @return true, если это была именно сюжетная глава
bool completeStoryChapter(ecs::Registry& reg, ecs::Entity player, u32 questId);

/// Вся ли цепочка пройдена.
bool storyFinished(ecs::Registry& reg, ecs::Entity player);

} // namespace quests
