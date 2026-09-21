/**
 * @file projectile_renderer.h
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_buffer.h"
#include "instance_ring.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_context.h"
#include "../ecs/registry.h"
#include "mob_renderer.h"
#include "../world/precipitation.h"
#include "../world/particles.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <vector>

namespace render {

/// Превратить капли в инстансы куба.
///
/// Отдельно от самого рендера, потому что проверять надо именно это:
/// капля обязана лететь ВДОЛЬ своей скорости (иначе дождь идёт
/// кубиками), снежинка — быть крупной и белой, а мёртвых частиц в
/// списке быть не должно вовсе.
void precipInstances(const world::Precipitation& p, f32 snowMix,
                     std::vector<MobInstance>& out);

/// Во сколько раз `shaders/projectile.frag` осветляет цвет инстанса.
///
/// Конвейер снарядов писался под светящееся: заклинание, искру,
/// каплю, поймавшую свет. Осколку это ни к чему — он обязан быть
/// цвета того блока, от которого откололся. Поэтому цвет частицы
/// делится здесь ровно на то, на что шейдер потом умножит.
///
/// Число обязано совпадать с шейдером дословно, и за этим следит
/// проверка: она читает `projectile.frag` и ищет в нём этот
/// множитель.
constexpr f32 PROJECTILE_FRAG_GAIN = 1.4f;

/// Ближе этого расстояния от глаза осколок не рисуется вовсе.
///
/// Ближняя плоскость стоит на 0.1 блока, а ребро осколка — около
/// того же. Частица, прошедшая рядом с глазом, закрывает собой
/// пол-экрана цветным пятном — и это не редкость: пыль при
/// приземлении рождается у ступней, часть её летит вверх, а от
/// первого лица голова и есть камера.
constexpr f32 PARTICLE_NEAR_HIDE = 0.35f;

/// Дальше этого — рисуется в полную силу; между ними растворяется.
///
/// Через затухание, а не отсечением по порогу: осколок, пропадающий
/// разом, читается как мигание, и на границе он мигал бы каждый
/// кадр.
constexpr f32 PARTICLE_NEAR_FULL = 0.90f;

/// Превратить осколки в инстансы куба.
///
/// Отдельно от рендера по той же причине, что и капли: проверять
/// надо именно это — мёртвых в списке нет, размер и цвет взяты из
/// частицы, куб кувыркается, а не висит гранями по осям мира, и у
/// самого глаза его не видно.
///
/// `eye` — где стоит камера этого кадра.
void particleInstances(const world::Particles& p, const glm::vec3& eye,
                       std::vector<MobInstance>& out);

class ProjectileRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    void rebuild(ecs::Registry& reg);

    /// Осадки: те же кубы, тот же формат инстанса, то же смешивание.
    ///
    /// Рисуются этим же конвейером намеренно. Свой конвейер ради них
    /// означал бы вторую копию того же куба, того же буфера вершин и
    /// тех же двух шейдеров — ради разницы в том, откуда берётся
    /// список инстансов.
    ///
    /// Зовётся ДО rebuild(): тот собирает кадр целиком.
    void setPrecip(const MobInstance* data, u32 count);

    /// Осколки: тот же поток кубов, что и осадки.
    ///
    /// Второй поставщик, а не второй рендер. Ради этого направление
    /// и выбиралось: частицам не понадобилось ни конвейера, ни
    /// прохода, ни правки меширования.
    ///
    /// Зовётся ДО rebuild().
    void setParticles(const MobInstance* data, u32 count);

    u32 precipCount() const { return precipCount_; }
    u32 particleCount() const { return particleCount_; }
    void upload(vk::Context& ctx);
    /// set передаётся явно и привязывается своим layout'ом. Раньше
    /// дескрипторы брались те, что оставил после себя рендер чанков:
    /// Vulkan гарантирует их сохранность только при совместимых
    /// layout'ах, а совместимость ломается от любого расхождения —
    /// например от push-константы, которой у чанков теперь есть.
    void render(vk::Context& ctx, VkDescriptorSet set);

    u32 instanceCount() const { return instanceCount_; }

private:
    VkDevice             dev_ = VK_NULL_HANDLE;
    vk::ShaderCache      shaders_;
    vk::GraphicsPipeline pipeline_;
    vk::Buffer           vbo_;
    vk::Buffer           ibo_;
    InstanceRing         instances_;

    std::vector<MobInstance> cpu_;
    std::vector<MobInstance> precip_;
    std::vector<MobInstance> particles_;
    u32 instanceCount_ = 0;
    u32 precipCount_ = 0;
    u32 particleCount_ = 0;
};

} // namespace render
