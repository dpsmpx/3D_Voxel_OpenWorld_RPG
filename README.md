# VoxelRPG

Воксельная Action-RPG для Android с открытым миром, процедурной
генерацией, боевой системой «Резонанс» и полным циклом прокачки.

**Технологии:** C++20, Vulkan 1.1, AAudio, EnTT, Android NDK 26.

> **Состояние проекта.** Код собирается и линкуется без ошибок, логика
> покрыта 100 автоматическими проверками (`tools/hostcheck/run.sh`).
> На физическом Android-устройстве сборка **не запускалась** — в среде
> разработки не было NDK и SDK. Подробности, что проверено и что нет,
> см. в [docs/CODE_REVIEW.md](docs/CODE_REVIEW.md).

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
- **2 босса подземелий** с фазами по порогам здоровья, яростью на
  последней фазе и ударом по площади; по одному на подземелье.
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
- **Настраиваемое управление**: чувствительность, инверсия по X и Y,
  лево/правша, размер и прозрачность кнопок, свободное перемещение
  кнопок по экрану, поддержка Bluetooth-геймпада.
- **Шрифт 5×7** с 80+ строками на 2 языках (EN/RU).
- **Миникарта** с top-down снимком вокселей.
- **Экран настроек**: 5 табов (Управление, UI, Аудио, Игра, Графика).
- **Realtime освещение** directional light + туман.
- **Skybox** с градиентом и солнцем.

### Аудио

- **AAudio low-latency** поток, PCM float32, stereo 48 kHz.
- **36 процедурно генерируемых SFX** (шаги, бой, магия, UI, мобы).
- **Динамическая музыка**: 4 трека (исследование, бой, подземелье,
  деревня) играют одновременно, переключение — кроссфейд громкостей.
- **3D-позиционирование** с distance attenuation и pan.

### Цикл суток

- **20-минутные игровые сутки**, сохраняются вместе с миром.
- Солнце ходит по дуге, небо меняет цвет от ночного индиго через
  рассветный янтарь к дневной лазури, ночью проступают звёзды.
- Враждебные мобы появляются только в темноте — по времени суток
  и по фактической освещённости точки.
- Торговцы обновляют ассортимент раз в игровой день.

### Оптимизация

- **ECS на EnTT** — хранение и итерация компонентов.
- **Spatial hash** для hit-detection.
- **Job system** с work-stealing; чанки генерируются и мешируются
  на воркерах, главный поток их не ждёт.
- **Frustum culling + occlusion culling + 4 уровня LOD** для чанков;
  LOD грузится на GPU по требованию, а не все четыре сразу.
- **Пакетная загрузка на GPU**: все копии кадра идут одним
  `vkQueueSubmit` с одним ожиданием.
- **`std::pmr` поверх арены** для временных данных меширования.
- **Staging pool** для Vulkan upload.
- Замер на x86-хосте: генерация + меширование чанка — **9,8 мс**
  при бюджете ТЗ 16 мс.

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

### Подготовка окружения (один раз)

```bash
git clone https://github.com/dpsmpx/3D_Voxel_OpenWorld_RPG.git
cd 3D_Voxel_OpenWorld_RPG
chmod +x tools/*.sh build.sh
./tools/termux-setup.sh
source ~/.voxelrpg-env
```

Скрипт ставит пакеты, скачивает Android NDK и прописывает переменные
окружения. Занимает несколько минут: NDK весит сотни мегабайт.

> **Почему не просто `pkg install`.**
> Пакета `termux-ndk` в Termux **не существует** — это название проекта
> на GitHub, а не пакета. Официальный NDK от Google собран под x86_64 и
> на телефоне не запустится; нужна сборка под `linux-aarch64`, которую
> публикует [lzhiyong/termux-ndk](https://github.com/lzhiyong/termux-ndk).
> Имя пакета Java тоже менялось: `openjdk-17` в свежих установках Termux
> уже не ставится, сейчас это `openjdk-21`. Поэтому имена не зашиты —
> `termux-setup.sh` проверяет, что доступно, и ставит подходящее.

Проверить, чего не хватает, и получить конкретную команду для каждого
пункта:

```bash
./tools/termux-doctor.sh
```

#### Если NDK не скачался автоматически

Скрипт берёт ссылку на архив из релизов GitHub. С мобильного IP GitHub
нередко отвечает «rate limit exceeded», а у части операторов API вообще
недоступен. Тогда архив нужно скачать браузером и отдать скрипту:

1. Открыть [github.com/lzhiyong/termux-ndk/releases](https://github.com/lzhiyong/termux-ndk/releases)
2. Скачать `android-ndk-*-aarch64.zip` (около гигабайта)
3. Выполнить:

```bash
termux-setup-storage                       # один раз, доступ к папке загрузок
./tools/termux-setup.sh --ndk ~/storage/downloads/android-ndk-r27-aarch64.zip
```

Принимается архив (`.zip`, `.tar.xz`, `.tar.gz`, `.7z`), прямая ссылка
(`--ndk https://...`) или уже распакованный каталог
(`--ndk ~/android-ndk-r27`). Пакеты при этом переустанавливать не нужно:
`--skip-packages` пропускает первый шаг.

После распаковки скрипт проверяет, что NDK рабочий: `clang` не просто
лежит на месте, а **запускается**, есть `android.toolchain.cmake` и
`native_app_glue`. Если что-то не так, `ANDROID_NDK_HOME` в
`~/.voxelrpg-env` **не пишется** — переменная, указывающая в никуда,
даёт невнятную ошибку CMake вместо понятного «NDK не найден».

Каталог NDK при этом намеренно **не добавляется в `PATH`**: кросс-компилятор
оттуда подменил бы `clang` самого Termux, и всё, что должно собираться для
текущей машины (утилита атласа текстур), перестало бы работать. CMake
получает компилятор явно, через `android.toolchain.cmake`.

#### «unexpected e_type: 2» при конфигурации CMake

```
error: ".../prebuilt/linux-x86_64/bin/clang-21" has unexpected e_type: 2
```

Так системный загрузчик Android отказывается запускать файл, собранный
не под эту архитектуру. Причина одна из двух, обе лечатся одной командой:

```bash
./tools/termux-setup.sh --fix-ndk
```

1. **Рядом с рабочим toolchain лежит `prebuilt/linux-x86_64`** из
   официального NDK. На телефоне он бесполезен, но занимает около
   гигабайта и сбивает с толку сборку. Команда его удаляет.
2. **В `android.toolchain.cmake` хост-тег зашит как `linux-x86_64`** —
   про хост aarch64 официальный файл не знает, поэтому CMake ищет
   компилятор не там, где он лежит. Команда добавляет симлинк
   `prebuilt/linux-x86_64 -> prebuilt/linux-aarch64`, и зашитый тег
   попадает на настоящий toolchain.

Что именно не так, покажет `./tools/termux-doctor.sh`: он не просто ищет
файл `clang`, а запускает его и печатает архитектуру.

#### «unexpected e_type» при упаковке APK

```
error: "/data/data/com.termux/files/usr/bin/aapt2" has unexpected e_type: 2
```

Та же причина, что и у NDK: `aapt2` из пакета собран под другую
архитектуру. Рабочая сборка под aarch64 лежит в `build-tools/`
установленного SDK, и `build.sh` теперь ищет инструменты там прежде
`PATH`, проверяя их запуском. Если рабочего `aapt2` нет нигде:

```bash
./tools/termux-setup.sh --skip-packages --sdk <архив android-sdk>
```

Архив под aarch64 — на той же странице
[lzhiyong/termux-ndk](https://github.com/lzhiyong/termux-ndk/releases).
Библиотека `libnative-lib.so` к этому моменту уже собрана, так что APK
можно упаковать и на любой другой машине с рабочим Android SDK.

#### Сжатие атласа в ASTC

Необязательно: без кодировщика игра строит атлас процедурно при старте
и работает, просто текстуры занимают вчетверо больше видеопамяти.
Включается установкой `astcenc` (`pkg install astc-encoder`); ищутся
все обычные имена сборок — `astcenc`, `astcenc-neon`, `astcenc-sse2`,
`astcenc-avx2`. Свой путь можно задать явно:

```bash
ASTCENC=/путь/к/astcenc ./build.sh
```

#### Звук и Android 7

AAudio появился в Android 8.0 (API 26), а минимальная версия по ТЗ —
API 24. Библиотека не линкуется напрямую: запись `DT_NEEDED` на неё
сделала бы `.so` незагружаемым, и на Android 7 приложение не
запустилось бы вовсе. Символы берутся через `dlopen`, на 7.0 и 7.1 игра
идёт без звука, на 8.0 и новее — со звуком.

За этим следит `tools/hostcheck/check_android_api.py`: он сверяет все
вызовы NDK с `API=` из `build.sh`. Хост-проверка компилирует код с
заглушками, поэтому атрибутов доступности из настоящих заголовков NDK
в ней нет, и без такой сверки подобное всплывает только на устройстве.

#### android.jar и упаковка APK

`aapt2`, `apksigner` и `zipalign` ставятся из `pkg`, а вот `android.jar`
в Termux не поставляется — без него `build.sh` соберёт `.so`, но APK не
упакует и предложит gradle. Взять его можно из сборки SDK под aarch64:

```bash
./tools/termux-setup.sh --skip-packages --sdk ~/storage/downloads/android-sdk-....zip
```

Годится любая платформа не ниже compileSdk: путь и версия нигде не зашиты,
скрипты ищут `platforms/android-*/android.jar`.

Если что-то пойдёт не так, ставьте вручную и смотрите фактические имена
пакетов через `pkg search`:

```bash
pkg install clang cmake ninja git make zip unzip curl wget zlib shaderc
pkg install openjdk-21          # если не найден: pkg search openjdk
pkg install tur-repo            # часть инструментов живёт в TUR
pkg install aapt2 apksigner
```

### Быстрая сборка

```bash
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