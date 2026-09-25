/**
 * @file content_ru.cpp
 * @brief Настройки, локализация, счётчик игрового времени.
 */
#include "localization.h"

namespace config {

// ============================================================
// Словарь содержимого: русский
// ============================================================
//
// Здесь лежит текст, который объявлен НЕ в таблице ключей, а рядом
// с самим содержимым: имя предмета — в реестре предметов, реплика
// жителя — в реестре диалогов, шаблон названия задания — в реестре
// заданий. Это правильное для них место: имя предмета есть часть
// предмета, и переносить его в общий enum значило бы разорвать
// описание надвое.
//
// Ключ — английский текст. Отсюда два следствия, оба полезные.
// Во-первых, одинаковый текст в разных реестрах переводится один
// раз: «Iron Sword» объявлен и предметом, и оружием, и рецептом, а
// строка здесь одна. Во-вторых, пропуск не ломает экран: tr()
// вернёт английское слово, а не пустоту.
//
// Что пропусков нет, проверяется не глазами и не поиском по
// исходнику, а перебором самих реестров: проверка берёт каждый
// предмет, тварь, жителя, рецепт, навык, зачарование и шаблон
// задания по номеру и требует, чтобы перевод нашёлся. Список
// строк в исходнике для этого не нужен — нужен список содержимого,
// а он в игре и есть.
const ContentPair CONTENT_RU[] = {
    // ---- Предметы: материалы ----
    { "Stone",                  "Камень" },
    { "Dirt",                   "Земля" },
    { "Grass",                  "Дёрн" },
    { "Sand",                   "Песок" },
    { "Wood",                   "Дерево" },
    { "Leaves",                 "Листва" },
    { "Snow",                   "Снег" },
    { "Ice",                    "Лёд" },
    { "Iron Ore",               "Железная руда" },
    { "Gold Ore",               "Золотая руда" },
    { "Cactus",                 "Кактус" },
    { "Brick",                  "Кирпич" },
    { "Torch",                  "Факел" },
    { "Iron Ingot",             "Железный слиток" },
    { "Gold Ingot",             "Золотой слиток" },
    { "Leather",                "Кожа" },
    { "Bone",                   "Кость" },
    { "Cloth",                  "Ткань" },
    { "Gold",                   "Золото" },

    // ---- Предметы: метательное и оружие ----
    { "Trampoline",             "Батут" },
    { "Shuriken",               "Сюрикен" },
    { "Iron Sword",             "Железный меч" },
    { "Iron Axe",               "Железный топор" },
    { "Iron Spear",             "Железное копьё" },
    { "Iron Dagger",            "Железный кинжал" },
    { "Hunting Bow",            "Охотничий лук" },
    { "Heavy Crossbow",         "Тяжёлый арбалет" },
    { "Throwing Knife",         "Метательный нож" },
    { "Fire Staff",             "Огненный посох" },
    { "Frost Wand",             "Морозная палочка" },
    { "Arcane Bracelet",        "Чародейский браслет" },
    // Заклинания Древа: одно имя у навыка и у заклинания в руке.
    { "Ice Needles",            "Ледяные иглы" },
    { "Flame",                  "Пламя" },
    { "Ice Needles Rune",       "Руна ледяных игл" },
    { "Flame Rune",             "Руна пламени" },

    // ---- Предметы: зелья и еда ----
    { "Health Potion",          "Зелье здоровья" },
    { "Greater Health Potion",  "Большое зелье здоровья" },
    { "Mana Potion",            "Зелье маны" },
    { "Greater Mana Potion",    "Большое зелье маны" },
    { "Stamina Potion",         "Зелье выносливости" },
    { "Elixir of Strength",     "Эликсир силы" },
    { "Elixir of Agility",      "Эликсир ловкости" },
    { "Elixir of Intellect",    "Эликсир разума" },
    { "Elixir of Endurance",    "Эликсир стойкости" },
    { "Bread",                  "Хлеб" },
    { "Raw Meat",               "Сырое мясо" },
    { "Cooked Meat",            "Жареное мясо" },
    { "Apple",                  "Яблоко" },
    { "nothing",                "ничто" },

    // ---- Редкость ----
    { "Common",                 "Обычное" },
    { "Uncommon",               "Необычное" },
    { "Rare",                   "Редкое" },
    { "Epic",                   "Эпическое" },
    { "Legendary",              "Легендарное" },

    // ---- Твари ----
    { "Sheep",                  "Овца" },
    { "Cow",                    "Корова" },
    { "Chicken",                "Курица" },
    { "Wolf",                   "Волк" },
    { "Skeleton",               "Скелет" },
    { "Goblin",                 "Гоблин" },
    { "Slime",                  "Слизень" },
    { "Witch",                  "Ведьма" },
    { "Boar",                   "Кабан" },
    { "Bandit",                 "Разбойник" },
    { "Stone Warden",           "Каменный Страж" },
    { "Hollow Lord",            "Полый Владыка" },

    // ---- Жители ----
    { "Villager",               "Житель" },
    { "Elder",                  "Староста" },
    { "Trader",                 "Торговец" },
    { "Blacksmith",             "Кузнец" },
    { "Guard",                  "Стражник" },
    { "Healer",                 "Целитель" },
    { "Courier",                "Посыльный" },

    // ---- Фракции и отношение ----
    { "Villagers",              "Селяне" },
    { "Traders",                "Торговая гильдия" },
    { "Mages",                  "Академия магии" },
    { "Bandits",                "Разбойники" },
    { "Wildlings",              "Дикие" },
    { "Hated",                  "Ненавидят" },
    { "Hostile",                "Враждебны" },
    { "Unfriendly",             "Недоверчивы" },
    { "Neutral",                "Нейтральны" },
    { "Friendly",               "Дружелюбны" },
    { "Honored",                "Почитают" },
    { "Exalted",                "Превозносят" },

    // ---- Зачарования ----
    { "Fire",                   "Огонь" },
    { "Frost",                  "Мороз" },
    { "Shock",                  "Разряд" },
    { "Poison",                 "Яд" },
    { "Vampiric",               "Вампиризм" },
    { "Sharpness",              "Острота" },
    { "Swift",                  "Быстрота" },

    // ---- Ремесло: станки и рецепты ----
    { "Workbench",              "Верстак" },
    { "Anvil",                  "Наковальня" },
    { "Alchemy Table",          "Алхимический стол" },
    { "Smelt Iron Ingot",       "Выплавить железный слиток" },
    { "Smelt Gold Ingot",       "Выплавить золотой слиток" },
    { "Cloth Strips",           "Полосы ткани" },
    { "Throwing Knives",        "Метательные ножи" },
    { "Torches",                "Факелы" },

    // ---- Навыки ----
    { "Heavy Hitter",           "Тяжёлая рука" },
    { "Quick Strike",           "Быстрый удар" },
    { "Cleave",                 "Размах" },
    { "Deadly Strike",          "Смертельный удар" },
    { "Berserker",              "Берсерк" },
    { "Executioner",            "Палач" },
    { "Impact",                 "Натиск" },
    { "Precision",              "Меткость" },
    { "Dodge",                  "Уклонение" },
    { "Swiftness",              "Прыть" },
    { "Acrobat",                "Акробат" },
    { "Shadowstep",             "Теневой шаг" },
    { "Combo Master",           "Мастер связок" },
    { "Toughness",              "Крепость" },
    { "Arcane Mind",            "Чародейский разум" },
    { "Mana Flow",              "Поток маны" },
    { "Spell Power",            "Сила заклинаний" },
    { "Arcane Shield",          "Чародейский щит" },
    { "Enchanter",              "Зачарователь" },
    { "Alchemist",              "Алхимик" },
    { "Craft Master",           "Мастер ремесла" },
    { "Resonance Master",       "Мастер Резонанса" },
    { "Resonant Strike",        "Резонансный удар" },
    { "Finisher",               "Добивание" },

    // ---- Навыки: что даёт ранг ----
    { "+10% melee damage per rank.",
      "+10% урона в ближнем бою за ранг." },
    { "+8% melee damage per rank.",
      "+8% урона в ближнем бою за ранг." },
    { "+12% melee damage per rank.",
      "+12% урона в ближнем бою за ранг." },
    { "+5% attack speed per rank.",
      "+5% скорости удара за ранг." },
    { "+6% attack speed per rank.",
      "+6% скорости удара за ранг." },
    { "+12% attack range per rank.",
      "+12% длины удара за ранг." },
    { "+2.5% crit chance per rank.",
      "+2,5% шанса крита за ранг." },
    { "+20% crit damage per rank.",
      "+20% урона от крита за ранг." },
    { "+30% knockback per rank.",
      "+30% отбрасывания за ранг." },
    { "+4% dodge chance per rank.",
      "+4% шанса уклонения за ранг." },
    { "+4% movement speed per rank.",
      "+4% скорости хода за ранг." },
    { "+6% movement speed per rank.",
      "+6% скорости хода за ранг." },
    { "+15% jump height per rank.",
      "+15% высоты прыжка за ранг." },
    { "+15 max HP per rank.",
      "+15 к запасу здоровья за ранг." },
    { "+20 max mana per rank.",
      "+20 к запасу маны за ранг." },
    { "+0.6 MP/s regen per rank.",
      "+0,6 маны в секунду за ранг." },
    { "+12% spell damage per rank.",
      "+12% урона заклинаний за ранг." },
    { "+5% magic resist per rank.",
      "+5% сопротивления магии за ранг." },
    { "+20% enchant power per rank.",
      "+20% силы зачарования за ранг." },
    { "+20% potion effect per rank.",
      "+20% действия зелий за ранг." },
    { "Recipes count you 1 level higher.",
      "Рецепты считают вас на уровень выше." },
    { "Cast a fan of ice needles made from mana; +1 needle per rank.",
      "Веер ледяных игл, слепленных из маны; +1 игла за ранг." },
    { "Breathe fire that sets foes and ground alight; longer and hotter per rank.",
      "Струя огня поджигает врагов и землю; с рангом длиннее и жарче." },
    { "+20% resonance gain per rank.",
      "+20% набора Резонанса за ранг." },
    { "+15% resonance reserve: peak holds longer.",
      "+15% запаса Резонанса: пик держится дольше." },
    { "+20% finisher damage per rank.",
      "+20% урона добивания за ранг." },

    // ---- Задания: «убить N таких-то» ----
    //
    // Имя цели в русском тексте стоит в именительном падеже и
    // вынесено отдельным полем через двоеточие — «Волк: убить 5».
    // Так сделано намеренно. Склонять его было бы правильнее по
    // языку, но для этого у каждой твари и каждого предмета
    // понадобились бы три формы вместо одного имени, и всякое новое
    // содержимое приходилось бы заводить со склонением. Двоеточие
    // стоит одной строки и не врёт.
    //
    // Порядок подстановок в паре обязан совпадать: сперва имя, потом
    // число. За этим следит линтер — перепутанный порядок иначе
    // кончается падением на первом же задании.
    { "Cull the %s",            "Прореживание: %s" },
    { "%s: slay %d near the village.",
      "%s: убить %d близ деревни." },
    { "Clear out %s",           "Зачистка: %s" },
    { "%s: cull %d from the surrounding lands.",
      "%s: извести %d в окрестных землях." },
    { "Hunt %s",                "Охота: %s" },
    { "%s: the village needs %d slain.",
      "%s: деревне нужно %d голов." },
    { "Purge the %s",           "Истребление: %s" },
    { "%s: eliminate %d. Beware their numbers.",
      "%s: уничтожить %d. Их много." },
    { "Extermination: %s",      "Большая охота: %s" },
    { "%s: only the strongest dare face %d.",
      "%s: на %d решится только сильный." },

    // ---- Задания: «принеси N таких-то» ----
    { "Gather %s",              "Сбор: %s" },
    { "%s: bring %d to the village elder.",
      "%s: принести %d старосте." },
    { "Supply %s",              "Поставка: %s" },
    { "%s: the craftsmen need %d.",
      "%s: ремесленникам нужно %d." },
    { "Stockpile %s",           "Запас: %s" },
    { "%s: a bulk order, %d required.",
      "%s: крупный заказ, нужно %d." },
    { "Bulk %s",                "Большой заказ: %s" },
    { "%s: rare material, %d needed. Take care.",
      "%s: редкий материал, нужно %d. Берегись." },
    { "Tribute of %s",          "Дань: %s" },
    { "%s: an epic demand, %d for the cause.",
      "%s: огромный запрос, %d на общее дело." },

    // ---- Задания: «дойти до места» ----
    //
    // В двух шаблонах из пяти здесь стояло «%s», которому нечего
    // было подставить: название копировалось через
    // snprintf(dst, "%s", pattern), то есть подстановка не
    // происходила никогда — и проценты с эс попадали игроку на
    // экран как есть. Подставлять сюда нечего и по смыслу: у
    // «дойти» цель — место, а не имя, и место называется
    // координатами отдельной строкой.
    { "Scout nearby",           "Разведка" },
    { "Explore the area near the village.",
      "Осмотреть окрестности деревни." },
    { "Scout the ruins",        "Старые развалины" },
    { "Locate the old ruins.",  "Найти древние руины." },
    { "Chart the way",          "Проложить путь" },
    { "Reach the coordinates shown on your map.",
      "Дойти до места, указанного на карте." },
    { "Expedition",             "Дальний поход" },
    { "Travel far from safety to explore.",
      "Уйти далеко от обжитых мест." },
    { "Beyond the veil",        "За грань" },
    { "A legendary journey awaits.",
      "Путь, о котором потом рассказывают." },

    // ---- Задания: тайник под руинами ----
    { "A rumour",               "Слух" },
    { "Old folk speak of a cache beneath the ruins.",
      "Старики говорят о тайнике под руинами." },
    { "Buried under stone",     "Замуровано" },
    { "Dig down at the ruins and see what is there.",
      "Копать у руин вниз и посмотреть, что там." },
    { "The sealed vault",       "Запечатанный склеп" },
    { "A walled chamber lies under the old ruins.",
      "Под старыми руинами есть замурованная камера." },
    { "Far cache",              "Дальний тайник" },
    { "The cache is far, and nobody has dug it out yet.",
      "Тайник далеко, и до него ещё никто не докопался." },
    { "The last hoard",         "Последний клад" },
    { "A hoard nobody has reached in living memory.",
      "Клад, до которого на людской памяти не дошёл никто." },

    // ---- Задания: «защитить» ----
    { "Hold the line",          "Держать строй" },
    { "Protect the villager for %d seconds.",
      "Защищать жителя %d секунд." },
    { "Guard duty",             "Караул" },
    { "Defend for %d seconds.", "Продержаться %d секунд." },
    { "Siege defence",          "Осада" },
    { "Survive an assault of %d seconds.",
      "Пережить приступ длиной %d секунд." },
    { "Last stand",             "Последний рубеж" },
    { "Hold out for %d seconds against all odds.",
      "Выстоять %d секунд вопреки всему." },
    { "Legendary guard",        "Легендарный караул" },
    { "Protect for %d seconds. No mercy.",
      "Защищать %d секунд. Без пощады." },

    // ---- Задания: «отнести» ----
    { "Delivery",               "Доставка" },
    { "Deliver the package to the recipient.",
      "Отнести свёрток получателю." },
    { "Special delivery",       "Особая доставка" },
    { "Transport the goods safely.",
      "Довезти товар в целости." },
    { "Courier run",            "Путь курьера" },
    { "Deliver through dangerous territory.",
      "Доставить через опасные земли." },
    { "Urgent dispatch",        "Срочная депеша" },
    { "Hurry: the recipient is far away.",
      "Спеши: получатель далеко." },
    { "Legendary courier",      "Легендарный курьер" },
    { "A delivery that will be remembered.",
      "Доставка, которую запомнят." },

    // ---- Задания: добавки к описанию ----
    { "Target: (%d, %d, %d).",  "Цель: (%d, %d, %d)." },
    { "Ruins at (%d, %d), buried %d blocks down.",
      "Руины в (%d, %d), закопано на %d блоков вглубь." },
    { "materials",              "материалы" },

    // ---- Сюжетная цепочка ----
    { "The road to the neighbours", "Дорога к соседям" },
    { "The elder asks you to follow the stone road to the next "
      "village and find out why nobody comes from there.",
      "Старик просит дойти до соседней деревни по каменной дороге "
      "и узнать, отчего оттуда никто не приходит." },
    { "A shadow in the west",   "Тень на западе" },
    { "They say a forest begins past the road where the birds do "
      "not sing. Go and see it for yourself.",
      "Говорят, за дорогой начинается лес, где не поют птицы. "
      "Дойти и посмотреть своими глазами." },
    { "The castle among dead wood", "Замок среди сухостоя" },
    { "Deep in the Black Forest stands a castle. Nobody remembers "
      "who built it, but its gates are there, and they are open.",
      "В глубине Чёрного леса стоит замок. Кто его строил, не "
      "помнит никто — но ворота там есть, и они открыты." },
    { "What the ruins hid",     "Что спрятали под руинами" },
    { "The old ruins remember more than people do. Walled up "
      "beneath them is the thing worth coming for.",
      "Старые развалины помнят больше людей. Под ними замуровано "
      "то, за чем и стоило идти." },
    { "The lair",               "Логово" },
    { "One thing is left: a land where a single beast is found. "
      "Deal with its masters.",
      "Осталось последнее: земля, где водится один-единственный "
      "зверь. Разобраться с её хозяевами." },

    { "Need %d.",               "Нужно %d." },
    { "Head for (%d, %d).",     "Идти к (%d, %d)." },

    // ---- Первая цель ----
    { "Find people",            "Найти людей" },
    { "There is a village somewhere near. Reach it and talk to "
      "whoever lives there.",
      "Где-то рядом есть деревня. Дойти до неё и поговорить с "
      "теми, кто там живёт." },
    { "The first tree",         "Первое дерево" },
    { "Nobody is in sight. Gather wood: both the axe and "
      "everything after it start with it.",
      "Людей поблизости не видно. Набрать дерева — с него "
      "начинается и топор, и всё остальное." },

    // ---- Реплики жителей ----
    { "Good day, traveller. The village is quiet today.",
      "Доброго дня, странник. В деревне сегодня тихо." },
    { "Good to see you again. The village remembers what you did.",
      "Рад тебя видеть. Деревня помнит, что ты для неё сделал." },
    { "What is this place?",    "Что это за место?" },
    { "We are a small settlement. The Elder might have work for you.",
      "Мы небольшое поселение. У старосты, может, найдётся для "
      "тебя работа." },
    { "I'll speak with the Elder.", "Поговорю со старостой." },
    { "Understood.",            "Ясно." },
    { "Goodbye.",               "До встречи." },

    { "Welcome, adventurer. There is much to do.",
      "Здравствуй, странник. Дел хватает." },
    { "What do you need?",      "Что нужно сделать?" },
    { "Here is what I have for you. Do you accept?",
      "Вот что у меня есть. Берёшься?" },
    { "Accept quest.",          "Берусь." },
    { "Not now.",               "Не сейчас." },
    { "Farewell.",              "Прощай." },
    { "I have completed my task.", "Я выполнил поручение." },

    { "Fine wares, friend. Interested?",
      "Хороший товар, друг. Интересует?" },
    { "Show me your wares.",    "Покажи товар." },
    { "Just looking.",          "Просто смотрю." },

    { "Steel and fire, that's all I need.",
      "Сталь да огонь — больше ничего и не надо." },
    { "Can you craft for me?",  "Сделаешь кое-что?" },
    { "Later.",                 "Потом." },

    { "Halt. State your business.",
      "Стой. С чем пришёл?" },
    { "I mean no harm.",        "Я без дурного." },
    { "None of yours.",         "Не твоё дело." },
    { "Move along, then. Keep the peace.",
      "Тогда проходи. И не шуми." },

    { "Wounds of body and mind. I can tend to both.",
      "Раны тела и раны разума. Лечу и те, и другие." },
    { "Heal me (%u gold).",     "Вылечи меня (%u золота)." },

    // ---- Надписи, написанные по месту ----
    //
    // Таблица StrKey держит то, что код зовёт по имени ключа.
    // Эти надписи объявлены прямо там, где рисуются, — в локальном
    // массиве строк характеристик, в подписи кнопки, в шаблоне
    // snprintf. Заводить им ключ значило бы уносить текст из того
    // места, где он читается вместе с кодом, который его рисует.
    { "STRENGTH",               "СИЛА" },
    { "AGILITY",                "ЛОВКОСТЬ" },
    { "INTELLIGENCE",           "РАЗУМ" },
    { "ENDURANCE",              "СТОЙКОСТЬ" },
    { "WISDOM",                 "МУДРОСТЬ" },
    { "+MELEE DAMAGE, +CRIT DAMAGE",
      "+УРОН В БЛИЖНЕМ БОЮ, +УРОН ОТ КРИТА" },
    { "+ATTACK SPEED, +CRIT, +MOVE",
      "+СКОРОСТЬ УДАРА, +КРИТ, +ХОД" },
    { "+MANA, +SPELL POWER",    "+МАНА, +СИЛА ЗАКЛИНАНИЙ" },
    { "+HEALTH, +RESIST, +STAMINA",
      "+ЗДОРОВЬЕ, +СОПРОТИВЛЕНИЕ, +ВЫНОСЛИВОСТЬ" },
    { "Skill Points: %d",       "Очки навыков: %d" },
    { "LV %u",                  "УР %u" },
    { "Level %u",               "Уровень %u" },
    { "Lv %u  |  %us",          "Ур %u  |  %uс" },
    { "Empty",                  "Пусто" },
    { "DEL",                    "УДАЛ" },
    { "Equipped:",              "В руках:" },
    { "Enchant: %s Lv%u",       "Зачарование: %s ур%u" },
    { "%u gold",                "%u золота" },
    { "ENCHANT",                "ЗАЧАРОВАТЬ" },
    { "Select a recipe",        "Выберите рецепт" },
    { "Lost %llu gold — it waits where you fell",
      "Потеряно %llu золота — оно ждёт там, где вы пали" },
    { "Lost %llu gold and %llu XP — the gold waits where you fell",
      "Потеряно %llu золота и %llu опыта — золото ждёт там, где вы пали" },
    { "You died and lost %llu XP",
      "Вы погибли и потеряли %llu опыта" },
    { "The %llu gold left at your last death is gone",
      "Золото с прошлой гибели (%llu) пропало" },
    { "Equipped",               "Надето" },
    { "Consumed",               "Использовано" },
    { "No effect",              "Не подействовало" },
    { "New spell: %s. Equip its rune and attack to cast.",
      "Новое заклинание: %s. Наденьте его руну и атакуйте, чтобы колдовать." },
    { "g",                      "з" },
    { "P%u  S%u",               "П%u  С%u" },
    { "INV",                    "СУМКА" },
    { "FIRE",                   "ОГОНЬ" },
    { "FROST",                  "МОРОЗ" },
    { "STUN",                   "ОГЛУШ" },
    { "POIS",                   "ЯД" },
    { "TREE OF KNOWLEDGE",      "ДРЕВО ПОЗНАНИЯ" },
    { "ENCHANT ALTAR",          "АЛТАРЬ ЗАЧАРОВАНИЯ" },
    { "SAVE GAME",              "СОХРАНЕНИЕ" },
    { "LOAD GAME",              "ЗАГРУЗКА" },
    { "MODE: SAVE",             "РЕЖИМ: СОХРАНИТЬ" },
    { "MODE: LOAD",             "РЕЖИМ: ЗАГРУЗИТЬ" },

    // ---- Ремесло: станки и отказы ----
    { "OK",                     "Готово" },
    { "Recipe not found",       "Рецепт не найден" },
    { "Missing ingredients",    "Не хватает материалов" },
    { "Requires station",       "Нужен станок" },
    { "Level too low",          "Уровень слишком мал" },
    { "Requires Craft Master",  "Нужен навык «Мастер ремесла»" },
    { "Inventory full",         "Сумка полна" },
    { "No space for output",    "Некуда положить готовое" },
    // ---- Месяцы в списке сохранений ----
    { "Jan",                    "янв" },
    { "Feb",                    "фев" },
    { "Mar",                    "мар" },
    { "Apr",                    "апр" },
    { "May",                    "мая" },
    { "Jun",                    "июн" },
    { "Jul",                    "июл" },
    { "Aug",                    "авг" },
    { "Sep",                    "сен" },
    { "Oct",                    "окт" },
    { "Nov",                    "ноя" },
    { "Dec",                    "дек" },
    // ---- Рецепты алтаря зачарования ----
    //
    // Название рецепта — само зачарование и его ступень. Ступень
    // римская и в переводе не нуждается; переводится слово.
    { "Fire I",                 "Огонь I" },
    { "Fire II",                "Огонь II" },
    { "Fire III",               "Огонь III" },
    { "Frost I",                "Мороз I" },
    { "Frost II",               "Мороз II" },
    { "Frost III",              "Мороз III" },
    { "Shock I",                "Разряд I" },
    { "Shock II",               "Разряд II" },
    { "Shock III",              "Разряд III" },
    { "Poison I",               "Яд I" },
    { "Poison II",              "Яд II" },
    { "Sharpness I",            "Острота I" },
    { "Sharpness II",           "Острота II" },
    { "Swift I",                "Быстрота I" },
    { "Swift II",               "Быстрота II" },
    { "Vampiric I",             "Вампиризм I" },

    // ---- Алтарь: чем кончилась попытка ----
    { "Enchanted!",             "Зачаровано!" },
    { "No weapon equipped",     "В руках ничего нет" },
    { "Unknown recipe",         "Неизвестный рецепт" },
    { "Not enough gold",        "Не хватает золота" },
    { "Missing materials",      "Не хватает материалов" },
    { "Already enchanted this way", "Такое зачарование уже стоит" },
    { "Weaker than current",    "Слабее нынешнего" },
};

const usize CONTENT_RU_COUNT = sizeof(CONTENT_RU) / sizeof(CONTENT_RU[0]);

} // namespace config
