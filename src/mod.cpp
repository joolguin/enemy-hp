// Enemy HP Multipliers — native Dusklight mod.
//
// Multiplies the health of enemies once, the first time each one runs, with separate settings
// for regular enemies, mini-bosses, dungeon bosses and the final bosses.
//
// Based on A Link Between Twilight's "true max HP" scaling (d_albw_hp_mult.cpp, CC0), rewritten
// as hooks so it works on an unmodified Dusklight:
//   - post-hook on fopAc_Execute: scales health (and field_0x560, the max-health value some actors
//     keep) of each enemy actor once;
//   - pre-hook on fopAc_Delete: forgets the actor.
//
// Damage taken by Link is not handled here: Dusklight already has Settings > Difficulty >
// Damage Multiplier.
//
// Known limit: some bosses reset their health from a fixed value between phases; for those, only
// the phases that use the scaled value are affected.

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

// Game headers
#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <unordered_set>

DEFINE_MOD();

IMPORT_SERVICE_VERSION(LogService, svc_log, 0);
IMPORT_SERVICE_VERSION(ConfigService, svc_config, 0);
IMPORT_SERVICE_VERSION(HookService, svc_hook, 0);
IMPORT_SERVICE_VERSION(UiService, svc_ui, 0);

DEFINE_HOOK_SYMBOL("fopAc_Execute", int(void*), ActorExecute);
DEFINE_HOOK_SYMBOL("fopAc_Delete", int(void*), ActorDelete);

namespace {

// ---------------------------------------------------------------------------------------------
// Categories (lists from A Link Between Twilight)
// ---------------------------------------------------------------------------------------------

enum Category { kNormal, kMiniBoss, kBoss, kFinalBoss, kCategoryCount };

const s16 kMiniBosses[] = {
    fpcNm_B_GG_e,     // Aeralfos
    fpcNm_B_TN_e,     // Darknut (Temple of Time)
    fpcNm_B_ZANTM_e,  // Phantom Zant (magic attack)
    fpcNm_E_DT_e,     // Deku Toad
    fpcNm_E_GOB_e,    // Dangoro
    fpcNm_E_MK_e,     // Ook
    fpcNm_E_PM_e,     // Skull Kid (Sacred Grove)
    fpcNm_E_PZ_e,     // Phantom Zant
    fpcNm_E_RDB_e,    // King Bulblin
    fpcNm_E_TH_e,     // Darkhammer
    fpcNm_E_VT_e,     // Death Sword
    fpcNm_E_YC_e,     // Twilit Carrier Kargarok
    fpcNm_E_YMB_e,    // Twilit Bloat
};

const s16 kBosses[] = {
    fpcNm_B_BQ_e,      // Diababa
    fpcNm_B_BH_e,      // Diababa's Deku Babas
    fpcNm_B_DS_e,      // Stallord
    fpcNm_B_DR_e,      // Argorok
    fpcNm_B_DRE_e,     // Argorok (child actor)
    fpcNm_B_GM_e,      // Armogohma
    fpcNm_B_OB_e,      // Morpheel body
    fpcNm_B_OH_e,      // Morpheel head
    fpcNm_B_OH2_e,     // Morpheel tentacle
    fpcNm_B_YO_e,      // Blizzeta
    fpcNm_B_YOI_e,     // Blizzeta ice
    fpcNm_B_ZANT_e,    // Zant
    fpcNm_B_ZANTS_e,   // Zant (Goron Mines phase)
    fpcNm_B_ZANTZ_e,   // Zant helmet
    fpcNm_E_FM_e,      // Fyrus
    fpcNm_E_HZELDA_e,  // Possessed Zelda
    fpcNm_NPC_KN_e,    // Hero's Shade (lesson Shades have no health and are left alone)
};

const s16 kFinalBosses[] = {
    fpcNm_B_GND_e,  // Ganondorf (horseback)
    fpcNm_B_MGN_e,  // Beast Ganon
};

template <size_t N>
bool contains(const s16 (&list)[N], s16 name) {
    return std::find(std::begin(list), std::end(list), name) != std::end(list);
}

Category category_of(s16 name) {
    if (contains(kFinalBosses, name)) {
        return kFinalBoss;
    }
    if (contains(kBosses, name)) {
        return kBoss;
    }
    if (contains(kMiniBosses, name)) {
        return kMiniBoss;
    }
    return kNormal;
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

const char* const kMultiplierLabels[] = {
    "1x (default)", "1.5x", "2x", "2.5x", "3x", "4x", "5x", "8x",
};
const float kMultiplierValues[] = {1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 8.0f};
constexpr size_t kMultiplierCount = std::size(kMultiplierValues);

struct CategorySetting {
    const char* var;
    const char* label;
    const char* help;
    ConfigVarHandle handle = 0;
};

CategorySetting g_settings[kCategoryCount] = {
    {"hp_normal", "Regular Enemies",
        "Health of regular enemies (Bokoblins, Lizalfos, Stalfos, Darknuts outside the Temple of "
        "Time...)."},
    {"hp_miniboss", "Mini-Bosses",
        "Health of mini-bosses: Ook, Dangoro, King Bulblin, Deku Toad, Death Sword, Darkhammer, "
        "Aeralfos, the Temple of Time Darknut, Phantom Zant and others."},
    {"hp_boss", "Dungeon Bosses",
        "Health of dungeon bosses, from Diababa to Zant, plus Possessed Zelda and the Hero's "
        "Shade. Some bosses reset their health between phases, so not every phase may change."},
    {"hp_final", "Final Bosses", "Health of Ganondorf on horseback and Beast Ganon."},
};

float multiplier_for(Category category) {
    int64_t index = 0;
    svc_config->get_int(mod_ctx, g_settings[category].handle, &index);
    if (index < 0 || static_cast<size_t>(index) >= kMultiplierCount) {
        index = 0;
    }
    return kMultiplierValues[index];
}

// ---------------------------------------------------------------------------------------------
// Scaling
// ---------------------------------------------------------------------------------------------

std::unordered_set<fpc_ProcID> g_processed;

s16 scale(s16 value, float multiplier) {
    if (value <= 1) {
        return value;
    }
    const float scaled = std::round(static_cast<float>(value) * multiplier);
    return static_cast<s16>(std::clamp(scaled, 1.0f, 32767.0f));
}

void try_scale(fopAc_ac_c* actor) {
    if (actor == nullptr || !fopAcM_IsActor(actor) || fopAcM_GetGroup(actor) != fopAc_ENEMY_e) {
        return;
    }
    const fpc_ProcID id = fpcM_GetID(actor);
    if (id == fpcM_ERROR_PROCESS_ID_e || g_processed.count(id) != 0) {
        return;
    }
    // Only once the enemy has its health set (cheap check first), and only during gameplay.
    if (actor->health <= 0 || fpcM_SearchByName(fpcNm_PLAY_SCENE_e) == nullptr) {
        return;
    }
    g_processed.insert(id);

    const float multiplier = multiplier_for(category_of(fopAcM_GetName(actor)));
    if (multiplier == 1.0f || actor->health <= 1) {
        return;
    }
    actor->health = scale(actor->health, multiplier);
    if (actor->field_0x560 > 1) {
        actor->field_0x560 = scale(actor->field_0x560, multiplier);
    } else if (actor->field_0x560 > 0) {
        actor->field_0x560 = actor->health;
    }
}

void on_execute_post(ModContext*, void* args, void*, void*) {
    try_scale(static_cast<fopAc_ac_c*>(mods::arg<void*>(args, 0)));
}

HookAction on_delete_pre(ModContext*, void* args, void*, void*) {
    void* actor = mods::arg<void*>(args, 0);
    if (actor != nullptr) {
        g_processed.erase(fpcM_GetID(actor));
    }
    return HOOK_CONTINUE;
}

// ---------------------------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------------------------

ModResult build_panel(ModContext*, UiElementHandle pane, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, pane, "Enemy Health");
    for (auto& setting : g_settings) {
        UiControlDesc control = UI_CONTROL_DESC_INIT;
        control.kind = UI_CONTROL_DROPDOWN;
        control.label = setting.label;
        control.help_rml = setting.help;
        control.tooltip = setting.help;
        control.binding = UI_BINDING_CONFIG_VAR;
        control.config_var = setting.handle;
        control.options = kMultiplierLabels;
        control.option_count = kMultiplierCount;
        svc_ui->pane_add_control(mod_ctx, pane, &control, nullptr);
    }
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError*) {
    for (auto& setting : g_settings) {
        ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
        desc.name = setting.var;
        desc.type = CONFIG_VAR_INT;
        desc.default_int = 0;
        if (svc_config->register_var(mod_ctx, &desc, &setting.handle) != MOD_OK) {
            mods::log::error("could not register config var {}", setting.var);
            return MOD_ERROR;
        }
    }

    ModResult result = mods::hook::add_post<ActorExecute>(on_execute_post);
    if (result != MOD_OK) {
        mods::log::error("could not hook fopAc_Execute");
        return result;
    }
    result = mods::hook::add_pre<ActorDelete>(on_delete_pre);
    if (result != MOD_OK) {
        mods::log::error("could not hook fopAc_Delete");
        return result;
    }

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panel);

    mods::log::info("Enemy HP Multipliers initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_processed.clear();
    return MOD_OK;
}

}  // extern "C"
