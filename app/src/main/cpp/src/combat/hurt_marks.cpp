/**
 * @file hurt_marks.cpp
 * @brief Откуда игрока ударили: отметки по краю экрана.
 */
#include "hurt_marks.h"
#include "../ecs/components.h"
#include <algorithm>
#include <cmath>

namespace combat {

void noticeHurt(ecs::Registry& reg, ecs::Entity player, u32 sourceId,
                f32 amount)
{
    if (amount <= 0.f) return;
    auto* hm = reg.get<HurtMarks>(player);
    if (!hm) return;

    // Без источника направления нет. Яд, падение и утопление бьют
    // ниоткуда — и отметки за ними не остаётся.
    //
    // Проверка ровно одна: есть ли у источника место в мире. Стояла
    // ещё и «жив ли он», но она ничего не добавляла — `get` для
    // несуществующего номера и так возвращает пустоту, — а от
    // единственной настоящей опасности, переиспользованного номера,
    // не спасала ни та, ни эта: `alive` берёт текущее поколение и
    // ответит «жив» про НОВУЮ сущность с тем же номером. Показала
    // это мутация: снятие той проверки ничего не меняло.
    const auto* src = reg.get<ecs::Transform>(sourceId);
    if (!src) return;

    const auto* h = reg.get<ecs::Health>(player);
    const f32 maxHp = (h && h->max > 1.f) ? h->max : 100.f;
    const f32 weight = std::min(1.f, amount / maxHp);

    // Место ищется так: сперва свободное, иначе самое старое. Иначе
    // восьмой удар подряд остался бы незамеченным ровно в тот
    // момент, когда игрока бьют со всех сторон.
    u32 slot = 0;
    f32 worst = 1e9f;
    for (u32 i = 0; i < HurtMarks::CAPACITY; ++i) {
        if (hm->marks[i].life <= 0.f) { slot = i; break; }
        if (hm->marks[i].life < worst) { worst = hm->marks[i].life; slot = i; }
    }

    HurtMark& m = hm->marks[slot];
    m.from = src->position;
    m.life = HurtMarks::LIFETIME;
    m.weight = weight;
}

void tickHurtMarks(HurtMarks& hm, f32 dt) {
    for (auto& m : hm.marks) {
        if (m.life <= 0.f) continue;
        m.life -= dt;
        if (m.life < 0.f) m.life = 0.f;
    }
}

f32 hurtAngle(const glm::vec3& playerPos, f32 viewYaw, const glm::vec3& from) {
    const f32 dx = from.x - playerPos.x;
    const f32 dz = from.z - playerPos.z;
    if (dx * dx + dz * dz < 1e-6f) return 0.f;

    // Угол на источник в мире и угол взгляда — в одной системе, и
    // разница между ними и есть ответ. Приводится к (-π, π], иначе
    // удар чуть левее прямого «впереди» читался бы как удар сзади.
    //
    // Вычитается угол источника ИЗ угла взгляда, а не наоборот. Рост
    // yaw поворачивает взгляд ВЛЕВО: «вправо» у камеры — это
    // cross(вперёд, вверх) = (−cos yaw, 0, sin yaw), и при взгляде
    // на +Z правая сторона экрана — это −X. Здесь стояло
    // `toSrc − viewYaw`, и отметка показывала зеркально: били справа —
    // дуга загоралась слева. Та же ошибка была когда-то в шаге вбок.
    const f32 toSrc = std::atan2(dx, dz);
    f32 a = viewYaw - toSrc;
    while (a >  3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

} // namespace combat
