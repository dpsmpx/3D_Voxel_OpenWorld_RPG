# Changelog

## v1.0.0 — 2026

Первый стабильный релиз.

### Реализовано

**Phase 1 — Фундамент:**
- Project structure, CMake, Termux build script.
- Job system с work-stealing thread pool.
- Memory arenas и pools.
- Sparse-set ECS с генерационными handles.
- Touch input с плавающим джойстиком и UI-роутером.
- Simplex noise.
- Chunk manager с async генерацией.
- Vulkan 1.1 context + swapchain + depth buffer.

**Phase 2 — Vulkan pipeline:**
- Shader loading, SPIR-V.
- GPU buffers со staging pool.
- Texture2D с mip-цепочкой.
- Descriptor management.
- Graphics pipeline с alpha blend.
- Camera с view/proj matrices.
- Procedural atlas 512×512.
- Chunk renderer с frustum culling.

**Phase 3 — Оптимизация рендера:**
- Back-face culling.
- 4 уровня LOD.
- Staging pool.
- Fog + skybox с солнцем.
- Instanced grass с animated wind.
- Perspective-correct UV.

**Phase 4 — Мир:**
- 11 биомов с климатической моделью.
- Пещеры (ridged noise).
- Рудные жилы (cell-hash).
- Деревья (5 форм).
- Структуры: деревни, подземелья, руины, алтари.
- LRU-выгрузка чанков.

**Phase 5 — Физика:**
- AABB-коллизия с раздельным разрешением.
- DDA-raycast.
- Character controller с coyote-time и jump buffer.
- Плавание.
- Камера 1-е/3-е лицо с коллизией.
- Break/Place блоков.

**Phase 6 — UI:**
- Immediate-mode UI на Vulkan.
- Шрифт 5×7.
- HUD: HP/MP/SP, XP, FPS, хотбар.
- Меню паузы, инвентарь, настройки.
- Block outline (push constants).
- Toast-уведомления.

**Phase 7 — Мобы:**
- 7 типов (овца, корова, курица, волк, скелет, гоблин, слизень).
- FSM-ИИ (Idle/Patrol/Chase/Attack/Flee/Dead).
- A* навигация + smooth.
- Instanced-рендер.
- Спавн по биомам и времени суток.

**Phase 8 — Бой:**
- 10 типов оружия.
- Система «Резонанс» с финишером.
- 7 школ урона.
- 8 зачарований.
- Снаряды (лук, посох).
- Hit FX.
- Статусы (burn, slow, stun, poison).

**Phase 9 — Прогрессия:**
- 100 уровней, 4 атрибута.
- Древо Познания (3 ветки × 8 узлов).
- Derived stats.
- Регенерация ресурсов.
- Расход маны/стамины в бою.
- Награда XP при убийствах (включая DoT).

**Phase 10 — Квесты и NPC:**
- 5 типов квестов, 5 сложностей.
- Процедурная генерация.
- Журнал квестов, история.
- 6 ролей NPC (житель, элдер, торговец, кузнец, стражник, лекарь).
- Диалоги с ветвлением.
- 5 фракций репутации, 7 тиров.
- Instanced-рендер NPC.

**Phase 11 — Сохранения:**
- Бинарный формат с zlib.
- CRC32.
- 3 профиля × 3 слота.
- Дельта-компрессия мира.
- Автосейв каждые 5 мин.
- Экран Save/Load.

**Phase 12 — Предметы:**
- 40+ предметов, 5 редкостей.
- Инвентарь 41 слот.
- Кошелёк.
- 24 рецепта крафта.
- 3 станции.
- Торговля с репутационными ценами.
- Лут-таблицы.
- Пикапы в мире с автоподбором.
- Instanced ItemRenderer.

**Phase 13 — Полировка UI:**
- Система настроек (5 табов).
- Локализация (EN/RU).
- Точный playtime.
- Drag-and-drop.
- Скролл.
- Слайдер.
- Миникарта.
- Реальные HP/MP/SP в HUD.
- Алтарь зачарования (16 рецептов).

**Phase 14 — Аудио:**
- AAudio low-latency stream.
- 36 процедурных SFX.
- Динамическая музыка (explore ↔ combat).
- 3D-позиционирование.
- AudioEvents dispatcher.
- Lock-free voice pool.

**Phase 15 — Оптимизация:**
- Step-up 0.6 м.
- Snap-to-ground.
- SpatialHash для hit-detection.
- Фикс audio volume накопления.
- Texture2D::upload.
- Minimap upload.
- Raycast-коллизия пикапов.
- Валидация сейвов.
- Клампинг атрибутов и уровня.

**Phase 16 — Сборка:**
- AndroidManifest с Vulkan requirement.
- Gradle build с release/debug signing.
- build.sh для ручной сборки APK в Termux.
- build_gradle.sh для gradle-сборки.
- ProGuard rules.
- README + ARCHITECTURE.
- MIT License.

### Известные ограничения

- Только ARM64.
- Только Vulkan (без OpenGL fallback).
- MusicDirector не запускает треки (каркас).
- Миникарта отрисовывается как плашка (текстура готова).
- `toggleWidget`/`cycleWidget` API неконсистентен.
- Нет ARMv7.
- Нет gamepad input.

### В планах

- OpenGL ES 3.2 fallback.
- Полный gamepad support.
- Networked multiplayer.
- Moddable через JSON.
- Редактор карт.
- Расширение мобов (12+).
- Сюжетная кампания.