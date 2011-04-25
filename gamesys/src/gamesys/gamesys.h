#ifndef DM_GAMESYS_H
#define DM_GAMESYS_H

#include <dlib/configfile.h>

#include <script/script.h>

#include <resource/resource.h>

#include <gameobject/gameobject.h>

#include <gui/gui.h>
#include <input/input.h>
#include <render/render.h>
#include <render/font_renderer.h>
#include <physics/physics.h>

namespace dmGameSystem
{
    struct PhysicsContext
    {
        union
        {
            dmPhysics::HContext3D m_Context3D;
            dmPhysics::HContext2D m_Context2D;
        };
        bool m_Debug;
        bool m_3D;
    };

    struct EmitterContext
    {
        dmRender::HRenderContext m_RenderContext;
        uint32_t m_MaxEmitterCount;
        uint32_t m_MaxParticleCount;
        bool m_Debug;
    };

    struct RenderScriptPrototype
    {
        dmArray<dmRender::HMaterial>    m_Materials;
        dmhash_t                        m_NameHash;
        dmRender::HRenderScriptInstance m_Instance;
        dmRender::HRenderScript         m_Script;
    };

    struct GuiRenderContext
    {
        dmRender::HRenderContext    m_RenderContext;
        dmGui::HContext             m_GuiContext;
    };

    struct SpriteContext
    {
        dmRender::HRenderContext    m_RenderContext;
        uint32_t                    m_MaxSpriteCount;
    };

    void RegisterDDFTypes(dmScript::HContext script_context);

    dmResource::FactoryResult RegisterResourceTypes(dmResource::HFactory factory,
        dmRender::HRenderContext render_context,
        dmGui::HContext gui_context,
        dmInput::HContext input_context,
        PhysicsContext* physics_context);

    dmGameObject::Result RegisterComponentTypes(dmResource::HFactory factory,
                                                  dmGameObject::HRegister regist,
                                                  dmRender::HRenderContext render_context,
                                                  PhysicsContext* physics_context,
                                                  EmitterContext* emitter_context,
                                                  GuiRenderContext* gui_render_context,
                                                  SpriteContext* sprite_context);

    void RequestRayCast3D(dmGameObject::HInstance instance, uint8_t component_index, const Vectormath::Aos::Point3& from, const Vectormath::Aos::Point3& to, uint32_t mask);
    void RequestRayCast2D(dmGameObject::HInstance instance, uint8_t component_index, const Vectormath::Aos::Point3& from, const Vectormath::Aos::Point3& to, uint32_t mask);

    void GuiGetURLCallback(dmGui::HScene scene, dmMessage::URL* url);
    uintptr_t GuiGetUserDataCallback(dmGui::HScene scene);
    dmhash_t GuiResolvePathCallback(dmGui::HScene scene, const char* path, uint32_t path_size);
}

#endif // DM_GAMESYS_H
