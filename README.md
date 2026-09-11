# VoxelRPG

Воксельная Action-RPG для Android с открытым миром, процедурной
генерацией, боевой системой «Резонанс» и полным циклом прокачки.

**Технологии:** C++20, Vulkan 1.1, AAudio, ECS, Android NDK 26.

---

## Содержание

- [Обзор](#обзор)
- [Управление](#управление)
- [Сборка](#сборка)
- [Структура проекта](#структура-проекта)
- [Архитектура](#архитектура)
- [Системные требования](#системные-требования)
- [Отладка](#отладка)
- [Лицензия](#лицензия)

---

## Обзор

**VoxelRPG** — воксельная action-RPG с видом от третьего/первого лица,
разработанная полностью под Android. Основные механики:

### Мир

- **Процедурная генерация** чанков 32×32×128 с биомами (11 типов:
  океан, пляж, равнина, лес, тайга, пустыня, саванна, тундра, горы,
  болото, вулкан).
- **Пещеры** через ridged-3D-шум, рудные жилы, вода/лава.
- **Структуры**: деревни, подземелья, руины, алтари — генерируются
  детерминированно по seed через super-chunk grid 8×8.
- **Деревья** по биомам (5 форм: oak/pine/palm/cactus/dead).
- **Greedy meshing** с корректным face-culling, 4 уровня LOD.
- **Динамическая подгрузка/выгрузка** чанков (view distance 4–12).

### Бой

- **Система «Резонанс»**: накопление стека 0–5 за удары, рост урона
  /скорости/крита, «Финишер» на максимуме.
- **10 типов оружия**: мечи, топоры, копья, кинжалы, луки, арбалеты,
  метательные ножи, посохи, жезлы, браслеты.
- **7 школ урона**: Physical, Fire, Frost, Shock, Poison, Arcane.
- **8 зачарований**: Fire, Frost, Shock, Poison, Vampiric, Sharpness,
  Swift, по 1–3 уровня каждое.
- **Статусы**: поджог, заморозка, стан, яд, slow, flash.

### Прогрессия

- **100 уровней**, XP за убийства, квесты, исследование.
- **4 атрибута** (STR/AGI/INT/END), 2 очка за уровень.
- **Древо Познания**: 3 ветки × 8 узлов × 3 ранга = 24 узла.
- **Регенерация** HP/MP/SP с зависимостью от атрибутов.

### Мобы и NPC

- **7 типов мобов**: овца, корова, курица (пассивные); волк, скелет,
  гоблин, слизень (агрессивные).
- **FSM-ИИ**: Idle → Patrol → Chase → Attack → Flee → Dead.
- **A\*** навигация по воксельной сетке с smooth-проходом.
- **6 ролей NPC**: житель, элдер (квестодатель), торговец, кузнец,
  стражник, лекарь.
- **Диалоги** с ветвлением, реакция на репутацию.

### Квесты, торговля, крафт

- **5 типов квестов**: Kill/Collect/Explore/Defend/Deliver,
  5 уровней сложности, процедурная генерация.
- **Журнал квестов**, история, авто-обновление прогресса.
- **5 фракций** репутации, 7 тиров (Hated → Exalted).
- **24 рецепта крафта** на 3 станциях (верстак, наковальня,
  алхимический стол).
- **Торговля** с репутационно-зависимыми ценами.
- **16 рецептов зачарования** на алтаре.

### Инвентарь и предметы

- **41 слот**: 27 main, 9 hotbar, 4 armor, 1 accessory.
- **40+ предметов** с редкостями: Common, Uncommon, Rare, Epic,
  Legendary.
- **Drag-and-drop** в UI, автоподбор в мире.
- **Лут-таблицы** для всех мобов.

### Сохранения

- **3 профиля × 3 слота**, автосейв каждые 5 минут.
- **Бинарный формат** с zlib-сжатием и CRC32.
- **Дельта-компрессия** изменений мира (только изменённые блоки).
- **Валидация** seed и атрибутов при загрузке.

### UI и графика

- **Полностью нативный immediate-mode UI** на Vulkan.
- **Шрифт 5×7** с 80+ строками на 2 языках (EN/RU).
- **Миникарта** с top-down снимком вокселей.
- **Экран настроек**: 5 табов (Управление, UI, Аудио, Игра, Графика).
- **Realtime освещение** directional light + туман.
- **Skybox** с градиентом и солнцем.

### Аудио

- **AAudio low-latency** поток, PCM float32, stereo 48 kHz.
- **36 процедурно генерируемых SFX** (шаги, бой, магия, UI, мобы).
- **Динамическая музыка** с cross-fade (explore ↔ combat).
- **3D-позиционирование** с distance attenuation и pan.

### Оптимизация

- **Spatial hash** для hit-detection.
- **Step-up** 0.6 м и snap-to-ground.
- **Job system** с work-stealing.
- **Staging pool** для Vulkan upload.
- **Frustum culling + LOD** для чанков.

---

## Управление

### Основные кнопки (на экране)

| Кнопка | Позиция | Действие |
|--------|---------|----------|
| Attack | правая нижняя | атака / удержание для комбо |
| Finisher | правая средняя | финишер при готовности резонанса |
| Jump | правая верхняя | прыжок, плавание вверх |
| Sprint | левая средняя (удержание) | бег |
| Break | средняя под атакой | разрушить блок |
| Place | центр снизу | поставить блок |
| Interact (E) | левая нижняя | взаимодействие с NPC/станцией |
| Use Item | левая нижняя (ниже E) | использовать активный предмет |
| Camera | верх-право | переключение 1-е/3-е лицо |
| Menu (|||) | верх-право | пауза |

### Джойстик

- **Левый нижний угол** — плавающий джойстик движения.
- **Правая половина** — свайп для вращения камеры.

### Настройки

Все элементы UI настраиваются в **Settings → Input**:
- чувствительность камеры (0.1–5.0);
- инверсия Y;
- лево/правша;
- радиус и мёртвая зона джойстика.

---

## Сборка

### Требования

**Termux** (или любой ARM64 Linux):

```bash
pkg update
pkg install termux-ndk cmake ninja git clang shaderc
pkg install openjdk-17 aapt2 apksigner d8 zip
```

**Дополнительно** (опционально, для сборки через gradle):

```bash
# Android SDK
export ANDROID_HOME=$HOME/android-sdk
# Скачать command line tools + build-tools 34.0.0
```

### Быстрая сборка

```bash
cd voxel-rpg
chmod +x build.sh
./build.sh
```

APK появится в `build/apk/VoxelRPG.apk`.

### Debug-сборка

```bash
./build.sh debug
```

### Установка на устройство

```bash
./build.sh install
# или вручную:
adb install -r build/apk/VoxelRPG.apk
```

### Сборка через gradle

```bash
./build_gradle.sh
# или:
./gradlew assembleRelease
```

### Очистка

```bash
./build.sh clean
```

### Release-подпись

Для публикации создайте `app/release.keystore`:

```bash
keytool -genkeypair \
    -keystore app/release.keystore \
    -alias release \
    -keyalg RSA -keysize 2048 \
    -validity 10000 \
    -storepass <YOUR_PASSWORD> \
    -keypass <YOUR_PASSWORD> \
    -dname "CN=VoxelRPG, O=Your Org, C=US"
```

Затем задайте переменные окружения:

```bash
export RELEASE_KEYSTORE_PASSWORD=...
export RELEASE_KEY_ALIAS=release
export RELEASE_KEY_PASSWORD=...
```

Скрипт `build.sh` автоматически подхватит `release.keystore`.

---

## Структура проекта

```
voxel-rpg/
├── build.sh
├── build_gradle.sh
├── CMakeLists.txt                  (в src/main/cpp)
├── README.md
├── build.gradle
├── settings.gradle
├── gradle.properties
├── gradle/
│   └── wrapper/
│       └── gradle-wrapper.properties
├── app/
│   ├── build.gradle
│   ├── proguard-rules.pro
│   ├── debug.keystore              (генерируется при первой сборке)
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── res/                    (иконки)
│       ├── assets/shaders/         (*.spv, генерируются build.sh)
│       ├── jniLibs/arm64-v8a/      (libnative-lib.so, libc++_shared.so)
│       └── cpp/
│           ├── CMakeLists.txt
│           ├── shaders/            (*.vert, *.frag)
│           └── src/
│               ├── main.cpp
│               ├── core/           (job_system, math, log)
│               ├── config/         (settings, localization, playtime)
│               ├── input/          (touch)
│               ├── vk/             (Vulkan wrappers)
│               ├── world/          (chunks, terrain, features, biome)
│               ├── physics/        (collision, raycast, controller)
│               ├── player/         (player logic)
│               ├── render/         (renderers)
│               ├── combat/         (weapons, hit-detection, spatial hash)
│               ├── mobs/           (mob defs, AI, spawner)
│               ├── npc/            (npc defs, dialogue, spawner)
│               ├── quests/         (quest system)
│               ├── factions/       (reputation)
│               ├── progression/    (attributes, skill tree)
│               ├── items/          (item defs, inventory, pickups)
│               ├── crafting/       (recipes, stations)
│               ├── trade/          (trade system)
│               ├── audio/          (AAudio engine, sounds, music)
│               ├── save/           (save manager, zlib)
│               ├── ui/             (immediate-mode UI)
│               └── ecs/            (registry, components)
└── third_party/
    └── glm/                        (клонируется build.sh)
```

---

## Архитектура

### Слои

```
┌──────────────────────────────────────────────┐
│              main.cpp (Engine)               │
│   window lifecycle, input loop, update       │
└────────────────┬─────────────────────────────┘
                 │
    ┌────────────┼────────────┬────────────┐
    ▼            ▼            ▼            ▼
┌────────┐  ┌────────┐  ┌─────────┐  ┌────────┐
│ world  │  │ player │  │  mobs   │  │  npc   │
│  ECS   │──│ combat │──│spawner  │──│dialogue│
│ chunks │  │physics │  │         │  │        │
└────┬───┘  └────┬───┘  └────┬────┘  └────┬───┘
     │           │           │            │
     └───────────┴─────┬─────┴────────────┘
                       ▼
              ┌─────────────────┐
              │  render_system  │
              │   + audio_engine│
              │   + ui_system   │
              └────────┬────────┘
                       ▼
              ┌─────────────────┐
              │ vk_context      │
              │ (Vulkan + AAudio)│
              └─────────────────┘
```

### ECS

`ecs::Registry` — sparse-set хранилище компонентов. Компоненты — POD-структуры:
- Core: Transform, Velocity, Health, Mana, Stamina, Attributes, Experience
- Combat: Combatant, EquippedWeapon, WeaponState, ResonanceState, StatusEffects
- AI: AIAgent, MobAI, NpcAI
- Items: Inventory, Wallet
- Progression: Progression, SkillTree
- Quests: QuestLog, Quest, Reputation
- Story: ActiveDialogue

### Job system

`jobs::JobSystem` — thread pool с work-stealing. Используется для:
- генерации чанков (terrain, caves, ores, features);
- greedy meshing 4 уровней LOD.

### Рендер

- **Vulkan 1.1** с fallback на OpenGL ES 3.2 (планируется).
- **2 frames in flight**, fence-based sync.
- **Deferred-ish** пайплайн: skybox → voxels → mobs/npcs/projectiles/items
  (instanced) → grass (instanced, alpha-blended) → outline → UI.

### Audio

- **AAudio low-latency**, PCM float32 stereo 48 kHz.
- **Lock-free voice pool** из 64 голосов (atomic state machine:
  Free/Claimed/Active/Freeing).
- **36 процедурных SFX** генерируются один раз при старте.
- **Dynamic music**: две петли cross-fade по tension.

### Сохранения

- Формат: `<magic><version><profile><slot><seed><timestamp><playtime>
  <origSize><compSize><crc32>` + zlib-сжатое тело.
- Тело: player state + world deltas + npc state + pickups.
- World deltas: только изменённые блоки (HashMap по чанкам).

---

## Системные требования

### Минимальные

- **Android 7.0 (API 24)**, ARM64.
- **Vulkan 1.1** capable GPU.
- **2 GB RAM**.
- ~200 MB свободного места.

### Рекомендуемые

- **Android 10+**.
- **Snapdragon 660** или эквивалент.
- **4 GB RAM**.

### Целевая производительность

- **60 FPS** при view distance 7.
- **< 1.5 GB RAM** в пике.
- **< 16 мс** на генерацию чанка.

---

## Отладка

### Логирование

Все логи идут в logcat с тегом `VoxelRPG`:

```bash
adb logcat -s VoxelRPG:V
```

Формат:
```
FPS=60 LV=15 gold=1234 inv=12 mobs=8 npc=24 voices=4 cells=42 time=1m 23s
```

### Профилирование

- **FPS** показывается в HUD.
- **Debug pos** включается в Settings → UI → Show Debug Info.
- **Draw calls** видны в `render_system::render` через счётчики.

### Внутренние инструменты

- **Force crash** (отладка): добавьте `#define DEBUG_CRASH` в main.cpp.
- **Skip audio**: `audio::engine().init()` возвращает false — игра
  продолжит работать без звука.
- **Minimal view distance**: Settings → Render → View Distance = 4.

---

## Лицензия

**VoxelRPG** — учебный проект для демонстрации полного цикла
разработки 3D-игры на C++/Vulkan под Android.

Код распространяется под лицензией MIT. Все ассеты процедурно
сгенерированы — сторонних ресурсов нет.

### Использованные библиотеки

- **GLM** — MIT, https://github.com/g-truc/glm
- **zlib** — zlib License, https://zlib.net
- **Vulkan SDK** — Apache 2.0
- **Android NDK** — Apache 2.0

---

## Контакты и вклад

Проект создан как референс-реализация воксельной RPG.
Любые улучшения приветствуются через pull requests.