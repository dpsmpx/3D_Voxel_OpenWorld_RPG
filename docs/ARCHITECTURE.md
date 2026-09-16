# Архитектура VoxelRPG

Техническое описание внутренней структуры игры.

---

## 1. Общая схема

Игра построена как **data-driven** система с чистой ECS-архитектурой.
Все игровые объекты — сущности в `ecs::Registry`, поведение описывается
системами. Системы разбиты по слоям:

| Слой | Модули | Ответственность |
|------|--------|-----------------|
| Core | `core/`, `ecs/`, `input/` | базовые типы, ECS, job system, ввод |
| Data | `world/`, `items/`, `crafting/`, `trade/`, `quests/`, `factions/` | модели данных и логика без графа сцены |
| Sim | `physics/`, `combat/`, `mobs/`, `npc/`, `progression/`, `player/` | симуляция мира |
| Render | `vk/`, `render/`, `ui/` | графика |
| IO | `save/`, `config/` | файлы и настройки |
| Audio | `audio/` | звук |

---

## 2. ECS (Entity-Component-System)

### 2.1 Хранилище

Хранение и итерацию компонентов выполняет **EnTT** (требование ТЗ 3.2).
Библиотека заголовочная, клонируется `build.sh` в `third_party/entt`
и в репозиторий не коммитится.

`ecs::Registry` — тонкая обёртка над `entt::registry`. Она существует
не ради абстракции ради абстракции, а решает две конкретные задачи:

1. **Дескриптор, разложенный на части.** `entt::entity` упаковывает
   индекс и поколение в одно 32-битное число. Подсистемы урона,
   снарядов, диалогов и сохранений хранят сущность как «голый» `u32`,
   поэтому `ecs::Entity` держит `id` (индекс + 1) и `gen` (поколение)
   раздельно, а `Registry::fromId()` восстанавливает полный дескриптор
   через `entt::registry::current()`.

2. **Плотный доступ по индексу.** Игровой код повсеместно пишет

   ```cpp
   auto& pool = reg.pool<MobAI>();
   for (usize i = 0; i < pool.size(); ++i) {
       ecs::Entity e = pool.entityAt((u32)i);
       auto& ai = pool.at((u32)i);
   }
   ```

   `ComponentPool<T>` — это пара указателей на `entt::registry` и
   `entt::storage<T>`; сам он ничего не хранит.

Сборка идёт с `ENTT_NO_ETO`: EnTT хранит и пустые компоненты-теги.
Один байт на тег дешевле, чем ветвление `if constexpr` по всему
адаптеру — без этого `storage<PlayerTag>::get()` возвращал бы `void`.

### 2.2 Компоненты

POD-структуры в `ecs/components.h`: `Transform`, `Velocity`, `Health`,
`Mana`, `Stamina`, `Attributes`, `Experience`, `Kind`, `AIAgent`,
`Collider`, `Renderable`, `PersistentId` и теги `PlayerTag`,
`EnemyTag`, `NPCTag`. Специфичные для подсистем компоненты живут
рядом со своим кодом: `combat::Combatant`, `mobs::MobAI`,
`npc::NpcTag`, `trade::TradeInventory` и так далее.

### 2.3 Потоки

`Registry` не потокобезопасен и используется только из игрового
потока. Фоновые задачи (генерация и меширование чанков) работают с
`world::Chunk` под его собственными блокировками и в ECS не лезут.

## 3. Мир

### 3.1 Чанки

`world::Chunk` — 32×32×128 вокселей (uint16_t на блок + uint8_t light).

Хранение:
```cpp
std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>, ChunkCoordHash>
```

Соседние чанки нужны для корректного face-culling на границах.

### 3.2 Генерация

Пайплайн при создании чанка:

1. **Terrain** — базовый рельеф (Simplex noise + biome field).
2. **Caves** — ridged-3D шум, carving.
3. **Ores** — жилы, детерминированные по cell-hash.
4. **Liquids** — вода до уровня моря, лава в глубине.
5. **Structures** — деревни, подземелья, руины, алтари (super-chunk grid).
6. **Trees** — по биомам.
7. **Mesh** — greedy meshing, один меш на чанк.

Все шаги выполняются асинхронно в **job system**.

### 3.3 Greedy meshing

Алгоритм:

- Для каждой из 6 граней × 128 слоёв строится маска блоков,
  подлежащих выводу.
- Маска «жадно» объединяется в прямоугольные квады.
- Генерируются вершины с UV из атласа и **baked lighting**.

Face culling:
- Solid-сосед → не выводим грань.
- Transparent-сосед (стекло, листья) → выводим.
- Вода: только верхняя и боковые к воздуху.

### 3.4 Уровней детализации нет

Мир мешится и рисуется в полной детализации: один меш на чанк, одна
пара VBO/IBO. Раньше уровней было четыре (шаг 1/2/4/8 вокселей на
клетку), и вместе с ними в коде жили огрубление объёма, юбки на стыках
уровней, гистерезис у границ и машина состояний
`residentLod`/`targetLod`/`requestedLod` в рендере — всё это убрано.

Отличие от ТЗ (`Technical_Task.md`, п. «LOD»), принятое сознательно:
дальность прорисовки в этой игре — 4–12 чанков, то есть 128–384 блока,
и на таком расстоянии огрубление давало не выигрыш, а работу. Каждый
уровень — свой меш, своя задача меширования и своя пара буферов в
видеопамяти; переключение между уровнями перестраивало дальний рельеф
и было видно как мигание. Кадр (`tools/vkcheck`) упирается во
фрагменты, а не в геометрию, и срезать геометрию дальних чанков ему не
помогало.

### 3.5 World deltas

`save::WorldDeltaStore` записывает каждое изменение блока через
callback `ChunkManager::setBlockModifyCallback`. При сохранении
сериализуются только изменённые блоки.

---

## 4. Физика

### 4.1 AABB-коллизия

`physics::resolveMovement` разрешает движение по осям Y → X → Z.
Порядок важен: сначала вертикальное движение (гравитация), потом
горизонтальное — так `onGround` проставляется до горизонтального
скольжения.

### 4.2 Step-up

Автоматический подъём на препятствия высотой ≤ 0.6 м:
- проверяем 6 дискретных высот;
- для каждой проверяем свободное место AABB и наличие опоры;
- если ок — телепортируем вверх.

### 4.3 Snap-to-ground

При медленном падении (`vy < 0.6`) и наличии земли в пределах
0.35 м — притягиваем вниз. Устраняет «дрожание» при спуске.

### 4.4 Raycast

DDA-обход воксельной сетки (Amanatides & Woo, 1987).
Используется для:
- выделения блока под прицелом;
- hit-detection снарядов;
- проверки LOS в AI;
- коллизии пикапов с вокселями.

---

## 5. Бой

### 5.1 Резонанс

Центральная механика боя. Стек 0–5 накапливается за удары:

| Стек | DMG | SPD | CRIT | RANGE |
|------|-----|-----|------|-------|
| 0    | 1.00| 1.00| 0.00 | 1.00 |
| 1    | 1.05| 1.03| 0.02 | 1.02 |
| 2    | 1.10| 1.06| 0.04 | 1.04 |
| 3    | 1.18| 1.10| 0.06 | 1.06 |
| 4    | 1.28| 1.15| 0.09 | 1.09 |
| 5    | 1.40| 1.20| 0.12 | 1.12 |

При 5 стеках готов **Финишер** — AoE-удар с уроном ×3..5.
После финишера — кулдаун 2.5 с.

Резонанс затухает через 3 с без ударов со скоростью 45/с.

### 5.2 Машина состояний оружия

`WeaponState`: Idle → Windup → Active → Recovery → Idle.

- **Windup** — замах; длительность = `windupTime / speedMult`.
- **Active** — один кадр, в котором выполняются попадания.
- **Recovery** — восстановление; длительность = `recoveryTime / speedMult`.

Во время Windup/Active/Recovery игрок не может начать новую атаку.

### 5.3 Hit-detection

- **Melee**: конус (`meleeConeHits`), угол и дистанция из `WeaponDef`.
- **Ranged/Magic**: снаряд (компонент `Projectile`) с гравитацией.
- **AoE**: сфера (`sphereHits`).

Все запросы идут через `SpatialHash` (сетка 4×4 м) —
в худшем случае O(k), где k — число сущностей в радиусе.

### 5.4 Урон

`DamageInstance` содержит:
- amount, type (7 школ);
- isCritical, criticalMult;
- burnTime, slowAmount, stunDuration, poisonDps.

Применение (`applyDamage`):
1. Проверка уворота (dodgeChance из derived stats).
2. Расчёт финального урона через сопротивления.
3. Вычитание HP.
4. Наложение статусов.
5. Проверка смерти → награда XP.

---

## 6. Прогрессия

### 6.1 Формулы

Атрибуты:

- MaxHP = 100 + END·5 + (LV-1)·3
- MaxMP = 80 + INT·4 + (LV-1)·2
- MaxSP = 100 + END·2 + AGI·1 + (LV-1)·1
- MeleeDmg = 1 + STR·0.020
- MagicDmg = 1 + INT·0.025
- AttackSpeed = 1 + AGI·0.012
- Crit = AGI·0.004
- CritDmg = STR·0.008
- PhysResist = min(0.60, END·0.006)
- Dodge = min(0.50, AGI·0.003)

XP за уровень:
```
xp(n) = 50·(n-1)² + 100·(n-1)
```
От 150 XP (1→2) до 1 000 000 XP (99→100).

### 6.2 Древо Познания

3 ветки × 8 узлов × до 3 рангов:

- **Strength**: HP, melee dmg, range, knockback, resonance gain, finisher.
- **Agility**: speed, crit, attack speed, dodge, jump, combo.
- **Wisdom**: mana, regen, spell dmg, enchant, alchemy, craft tier.

Все бонусы — **аддитивные** множители к derived stats.

---

## 7. Мобы и NPC

### 7.1 FSM мобов

`AIAgent::State`: Idle → Patrol → Chase → Attack → Flee → Dead.

Переходы:
- **Idle → Patrol** через случайный таймер (1.5–4 с).
- **Idle/Patrol → Chase** при виде игрока в `aggroRange` и LOS.
- **Chase → Attack** при `dist < attackRange`.
- **Attack → Chase** при `dist > attackRange`.
- **Chase/Attack → Flee** при `hp < 30%`.
- **Any → Dead** при смерти.

### 7.2 A\*

Compressed grid по 4 направлениям (N/S/E/W) + прыжки + падения.

Стоимости:
- прямой шаг: 1.0
- прыжок: 1.6
- падение: 1.0 + 0.15 × depth

`maxIterations = 1500` — защита от фризов.

Path smoothing: удаление промежуточных waypoints с прямой
видимостью.

### 7.3 NPC

`NpcAI::State`: Idle / Wander / Talk / Combat / Flee / Dead.

Поведение зависит от `NpcRole`:
- Guard сканирует врагов и атакует.
- Торговец/лекарь стоят.
- Элдер выдаёт квесты.

Диалоги через `npc::startDialogue` — копируют шаблон, подставляют
динамические опции (AcceptQuest / CompleteQuest).

---

## 8. Квесты и репутация

### 8.1 Генерация квестов

`quests::generateQuest` использует:
- уровень игрока;
- биом вокруг NPC;
- seed = `npcEntity XOR playerLevel`.

Тип выбирается взвешенно, сложность — от уровня игрока.
Количество, награды и время финализируются.

### 8.2 Обновление прогресса

События (из `main.cpp`):
- `notifyMobKilled(mobId)` — при смерти моба.
- `notifyItemCollected(itemId)` — при pickup.
- `notifyLocationReached(pos)` — раз в 0.5 с.
- `tickQuestTime(dt)` — каждый кадр.

### 8.3 Репутация

5 фракций (Villagers, Traders, Mages, Bandits, Wildlings).
7 тиров. Множители цен/наград зависят от тира.

---

## 9. Рендер

### 9.1 Vulkan pipeline

Инициализация:
1. Instance → surface → physical device.
2. Logical device + queues (graphics family).
3. Swapchain (2-3 images).
4. Depth buffer (D32_SFLOAT).
5. Render pass (color + depth).
6. Framebuffers.
7. Command pool + buffers.
8. Sync: semaphores × frames, fences × frames.

### 9.2 Пайплайны

- **Voxel**: cull back, depth test/write on, no blend.
- **Instanced**: same + instance binding (pos/size/color/yaw).
- **UI**: cull none, depth off, blend on.
- **Outline**: cull none, depth test on, write off, blend on.

### 9.3 Frustum culling

`math::Frustum::fromViewProj` извлекает 6 плоскостей.
Чанки проверяются через `intersectsAABB`.

### 9.4 Доставка мешей

Уровней детализации нет (см. 3.4), и заказывать «недостающий уровень»
рендеру нечего: о готовом меше он узнаёт только из очереди
`ChunkManager::pollMeshesReady`, и ровно один раз. Поэтому забрать меш
из очереди и не выгрузить его — потеря навсегда, а на экране это дыра
в ландшафте. Все три выхода из `ChunkRenderer::uploadChunks` разобраны
отдельно: выгружено (квады отпускаются), выгружать нечего, не хватило
ресурсов — последний возвращает меш в очередь через
`ChunkManager::requeueMesh`.

---

## 10. UI

### 10.1 Immediate-mode

`ui::UiContext` — кадровый билдер вершин:
- `pushInteractiveRect` — регистрирует кликабельную зону.
- `rect / rectOutline / text` — примитивы.
- `button` — комбинированный виджет.
- Рендер — через `UiRenderer::flush` (2 атласа: шрифт + блоки).

### 10.2 Drag-and-drop

`ui::DragDrop`:
- `begin(id, slot, pos, stack)` при нажатии.
- `update(pos, dt)` каждый кадр.
- `end()` при успешном drop, `cancel()` при возврате.

### 10.3 Настройки

`config::Settings` — единая структура, сериализуется в `settings.cfg`.
Применяется через `onSettingsChanged` callback.

### 10.4 Локализация

`config::Localization` — 80+ ключей на EN/RU.
`T(StrKey::Menu_Resume)` → "RESUME" / "ПРОДОЛЖИТЬ".

---

## 11. Аудио

### 11.1 AAudio

- PCM float32, stereo, 48 kHz.
- `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`.
- Data callback → `AudioEngine::mixInto` → запись в out buffer.

### 11.2 Голоса

Пул из 64 голосов. Lock-free через atomic state machine:

```
Free → Claimed → Active → Freeing → Free
```

`Claimed` = main-thread пишет поля.
`Active` = callback читает и микширует.

### 11.3 Процедурная генерация

`SoundRegistry` генерирует 36 SFX при init:
- шум с экспоненциальной огибающей;
- синус с ADSR;
- sweep (падение частоты);
- металлический резонанс;
- комбинации двух тонов.

Музыкальные петли (explore 16 с, combat 8 с) — аккордовые
прогрессии с мелодией.

### 11.4 Music director

Две петли играют одновременно с cross-fade по tension (0..1).
Tension растёт при комбате, падает в мире.

---

## 12. Сохранения

### 12.1 Формат файла

```
[HEADER 44 байта]
  magic       u32 = 0x47525856 ("VXRG")
  version     u32 = 1
  profile     u32
  slot        u32
  seed        u64
  timestamp   u64 (ms)
  playtime    u32 (sec)
  origSize    u32 (байт несжатого тела)
  compSize    u32 (байт сжатого тела)
  checksum    u32 (CRC32 сжатого тела)

[BODY compSize байт]
  zlib(serialized data)
```

### 12.2 Что сериализуется

**Player:**
- Transform, Health, Mana, Stamina, Attributes.
- Progression, SkillTree.
- EquippedWeapon, Combatant, Resonance.
- QuestLog (active + history).
- Reputation.
- Inventory (41 слот + enchantment оружия).
- Wallet (gold).

**World:**
- WorldDeltaStore — только изменённые блоки.

**NPC:**
- Список убитых NPC (для фильтра респавна).

**Pickups:**
- Все предметы в мире с позицией и lifeRemaining.

### 12.3 Валидация

При загрузке:
- CRC32 сжатого тела.
- Размер распакованного = origSize.
- Seed в файле == seed мира.
- Атрибуты clamped в [1, 99].
- Уровень clamped в [1, 100].

Любая ошибка → `SaveStatus::CorruptedData`.

---

## 13. Оптимизации

| Область | Техника | Эффект |
|---------|---------|--------|
| Meshing | Greedy | ~10× меньше квадов |
| Culling | Frustum + face | ~3× меньше draw calls |
| Instancing | mobs/npc/items/grass | 1 draw на 100+ объектов |
| Hit-detection | SpatialHash 4×4 м | O(k) вместо O(n) |
| Async | Job system | чанки вне главного потока |
| Memory | Staging pool | нет аллокаций в hot path |
| Physics | Step-up + snap | −80% лишних коллизий |
| Save | Delta + zlib | −95% размера сейва |

---

## 14. Расширение

### Добавить нового моба

1. `MobId` в `mob_def.h`.
2. Запись в `MobRegistry::MobRegistry()`.
3. Лут-таблица в `LootRegistry`.
4. Спавн-правило в `Spawner::pickMobId`.

### Добавить новый блок

1. `BlockId` в `block.h`.
2. Регистрация в `block.cpp`.
3. Цвет тайла в `atlas_builder.cpp`.
4. Цвет для миникарты в `minimap.cpp`.

### Добавить новый рецепт крафта

Одна строка в `RecipeRegistry::RecipeRegistry()`.

### Добавить новый звук

1. `SoundId` в `audio_types.h`.
2. Функция генерации в `sound_registry.cpp::init`.
3. Вызов из `audio_events.cpp`.

### Добавить новую UI-кнопку

Один вызов `pushInteractiveRect` + `button` в нужном экране.

---

## 15. Производительность

### Профилирование

- FPS — в HUD + в logcat раз в секунду.
- Draw calls — счётчик в `RenderSystem`.
- Job queue depth — `world->pendingGeneration()`.

### Горячие пути

| Функция | Частота | Оптимизация |
|---------|---------|-------------|
| `buildGreedyMesh` | при генерации чанка | work-stealing |
| `resolveMovement` | каждый кадр | step-up перед коллизией |
| `updateCombat` | раз в кадр | раннее отсечение по фазе |
| `meleeConeHits` | при ударе | spatial hash |
| `mixInto` | ~375 раз/сек | lock-free, атомики |

### Целевые метрики

| Метрика | Цель |
|---------|------|
| FPS (Snapdragon 660) | 60 |
| Время кадра | < 16 мс |
| Время генерации чанка | < 16 мс |
| Draw calls | < 500 |
| RAM пик | < 1.5 ГБ |
| Размер APK | < 20 МБ |

---

## 16. Известные ограничения

1. **Миникарта** отрисовывается как упрощённая плашка — текстура
   готова, но не подключена в UI полностью.
2. **MusicDirector** не запускает треки — нужен `AudioEngine::playRawSound`.
3. **`toggleWidget`/`cycleWidget`** вызывают `onChanged` через
   callback, а не напрямую — API неконсистентен.
4. **Только ARM64** — ARMv7 не поддерживается.
5. **Vulkan-only** — OpenGL ES fallback запланирован, но не реализован.
6. **Не оптимизировано под планшеты** — UI рассчитан на landscape
   телефоны.