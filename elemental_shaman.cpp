// Seth Elemental Shaman - Midnight Season 2
// Class: Shaman (ID: 7)
// Spec: Elemental (ID: 262)
//
// Drop-in IRotation DLL profile built against the same SDK shape as the saved
// Restoration Shaman profile. The damage priority was cross-checked on
// 2026-08-16 against SimulationCraft's live Midnight branch and current
// Elemental guides. SimC is authoritative for proc/resource conditions; the
// public settings expose intent while target counts and thresholds stay smart.
//
// AUDIT SOURCES (checked 2026-08-14):
// - https://stormkeeper.ru/ele/guide.html
// - https://github.com/simulationcraft/simc/blob/midnight/engine/class_modules/apl/apl_shaman.cpp
// - https://www.wowhead.com/guide/classes/shaman/elemental/rotation-cooldowns-pve-dps
// - https://www.method.gg/guides/elemental-shaman/playstyle-and-rotation
// - https://www.icy-veins.com/wow/elemental-shaman-pve-dps-rotation-cooldowns-abilities
//
// INTENTIONAL API BOUNDARIES:
// - Titan SDK 23.3 supplies a real pull countdown, so the optional prepull Lava
//   Burst is timed to finish just after combat begins instead of risking an
//   early pull.
// - Titan 23.3 exposes slot-aware trinket actions. Each slot is configured
//   independently so passive trinkets are skipped while usable trinkets can be
//   fired on cooldown, on bosses, or immediately before the Major-CD package.
//
// =============================================================================
// QUICK GUIDE: WHERE EVERYTHING LIVES
// =============================================================================
// get_settings_schema()       = options shown in the rotation's settings UI.
// apply_settings()            = reads your selected UI values into the profile.
// get_current_settings()      = sends the current values back to the UI/client.
// get_custom_toggles()        = compact combat-bar buttons (Defensives, Utility, Mini/Major CDs, Burst Now).
// get_combat_action()         = master dispatcher; its order is the global order.
// update_cast_history()       = confirms what the game actually accepted.
// damage dispatch state       = one generic pending/confirm/escape record shared
//                               by every ordinary damage spell.
// target recovery            = in-combat Tab only; never from dummy/friendly/OOC.
// single_target_action()      = the one- and two-target Farseer priority.
// aoe_action()                = the 3+ target Farseer cleave/Mythic+ priority.
// cooldown_*_action()         = use-loss-aware Stormkeeper/Asc synchronization.
// movement_action()           = instant-cast priority while moving.
// defensive/interrupt/utility = survival and Mythic+ support behavior.
//
// HOW A PRIORITY LIST WORKS:
// The code checks from top to bottom and returns the FIRST usable action. That
// means an earlier block always has priority over every block below it.
//
// COMBAT BAR (built-in first, then custom):
// Pause / AOE / Cooldowns / Interrupt = Titan built-ins.
// Defensives            = Astral Shift, Stone Bulwark, emergency heal.
// Utility               = cleanse, group support, and optional Purge/stun.
// Mini CDs              = Stormkeeper and Ancestral Swiftness.
// Major CDs             = Ascendance, burst setup, racials, and trinkets.
// Burst Now             = spend CDs immediately; still requires Cooldowns on.
// SETTINGS GROUPS: Automation, Cooldowns & Trinkets, Advanced.
// Purge and Capacitor stun live in Advanced (off by default).
// Turn Cooldowns off to hold every damage cooldown for the current pull.
//
// OPTIONAL CLIENT LISTS (empty lists safely do nothing):
// Interrupt.Kick / Elemental.Interrupt.Priority = casts to kick early.
// Interrupt.Ignore / Elemental.Interrupt.Ignore = casts never to Wind Shear.
// Purge.HighPriority / Elemental.Purge.Priority = enemy buffs to purge first.
// Interrupt.Stun / Elemental.Interrupt.Stun     = casts for Capacitor Totem.
// Utility.Tremor / Elemental.Utility.Tremor     = casts to pre-Tremor.
// Defensive.Dangerous / Elemental.Defensive.Dangerous = personal hits to wall.
//
// The annotations explain both the code and the reason for each priority.

#include <sdk/sdk.h>
#include <sdk/external_rotation.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace rotations::sdk;

namespace elemental_ids {
    // Stable fallback IDs for core damage spells. New Midnight spells are
    // resolved by name at runtime because their numeric IDs may still change.
    constexpr uint32_t LIGHTNING_BOLT          = 188196;
    constexpr uint32_t CHAIN_LIGHTNING         = 188443;
    constexpr uint32_t LAVA_BURST              = 51505;
    constexpr uint32_t FLAME_SHOCK             = 188389;
    constexpr uint32_t EARTH_SHOCK             = 8042;
    constexpr uint32_t EARTHQUAKE              = 61882;
    constexpr uint32_t ELEMENTAL_BLAST         = 117014;
    constexpr uint32_t STORMKEEPER             = 191634;
    constexpr uint32_t ASCENDANCE              = 114050;
    constexpr uint32_t TEMPEST                 = 454009;
    constexpr uint32_t FROST_SHOCK             = 196840;
    constexpr uint32_t FIRE_ELEMENTAL          = 198067;

    // Maintenance, movement, defensive, healing, and Mythic+ utility spells.
    constexpr uint32_t LIGHTNING_SHIELD        = 192106;
    constexpr uint32_t SKYFURY                 = 462854;
    constexpr uint32_t FLAMETONGUE_WEAPON      = 318038;
    constexpr uint32_t SPIRITWALKERS_GRACE     = 79206;
    constexpr uint32_t GHOST_WOLF              = 2645;
    constexpr uint32_t WIND_SHEAR              = 57994;
    constexpr uint32_t ASTRAL_SHIFT            = 108271;
    constexpr uint32_t STONE_BULWARK_TOTEM     = 108270;
    constexpr uint32_t HEALING_SURGE            = 8004;
    constexpr uint32_t CLEANSE_SPIRIT           = 51886;
    constexpr uint32_t PURGE                    = 370;
    constexpr uint32_t CAPACITOR_TOTEM          = 192058;
    constexpr uint32_t EARTH_ELEMENTAL          = 198103;
    constexpr uint32_t ANCESTRAL_GUIDANCE       = 108281;
    constexpr uint32_t HEALING_STREAM_TOTEM     = 5394;
    constexpr uint32_t POISON_CLEANSING_TOTEM   = 383013;
    constexpr uint32_t TREMOR_TOTEM             = 8143;

    // Patch 12.1 Elemental four-piece aura. The name-based fallback below is
    // retained in case the live aura ID is overridden by a hotfix.
    constexpr uint32_t ELEMENTAL_S2_4PC         = 1296626;

    // Titan executes ground-targeted actions through pre-registered secure
    // macros. The ##ID## form is the SDK's spell-ID placeholder syntax.
    constexpr const char* EARTHQUAKE_CURSOR_MACRO = "/cast [@cursor] ##61882##";
    constexpr const char* CAPACITOR_PLAYER_MACRO = "/cast [@player] ##192058##";
}

class Sethelementalshaman : public IRotation {
public:
    Sethelementalshaman() = default;
    ~Sethelementalshaman() override = default;

    // Basic information displayed by the rotation loader.
    std::string get_name() const override { return "Llama's Elemental"; }
    std::string get_author() const override { return "Llama"; }
    std::string get_description() const override {
        return "Midnight Season 2 Elemental Shaman v2.3.7: stall out-of-range telemetry";
    }
    RotationVersion get_version() const override { return {2, 3, 7}; }
    std::string get_class_name() const override { return "Shaman"; }
    std::string get_spec_name() const override { return "Elemental"; }

    // -------------------------------------------------------------------------
    // SETTINGS UI
    // -------------------------------------------------------------------------
    // This builds the visible configuration screen. Changing a default here
    // changes what a fresh installation starts with, but apply_settings() is
    // what reads the user's saved value during play.
    SettingsSchema get_settings_schema() const override {
        SettingsSchema schema;

        // Titan persists enum settings by option index, not EnumOption.value.
        // Never reorder existing options; append new choices at the end only.

        SettingGroup automation;
        automation.id = "automation";
        automation.label = "Automation";
        automation.description = "Core combat, defensive, and utility behavior";
        automation.icon = elemental_ids::LIGHTNING_BOLT;
        automation.collapsible = true;
        automation.collapsed_default = false;
        schema.add_group(automation);
        schema.add(SettingDefinition::make_bool("automatic_target_recovery", "Auto Retarget", true)
            .set_description("Retargets only when your current enemy dies or becomes unreachable in combat.")
            .set_group("automation"));
        schema.add(SettingDefinition::make_bool("auto_ghost_wolf", "Auto Ghost Wolf", true)
            .set_description("Automatically enters Ghost Wolf while moving between pulls.")
            .set_group("automation"));
        schema.add(SettingDefinition::make_enum("survival_profile", "Defensive Profile", 1)
            .add_enum_option(0, "Aggressive", "Delay personals to preserve damage globals")
            .add_enum_option(1, "Balanced", "Recommended automatic thresholds")
            .add_enum_option(2, "Safe", "Use personals and healing earlier")
            .set_group("automation"));
        schema.add(SettingDefinition::make_enum("utility_profile", "Utility Profile", 0)
            .add_enum_option(0, "Smart", "Automate interrupts, dispels, and efficient group support")
            .add_enum_option(1, "Interrupts Only", "Automate Wind Shear only")
            .add_enum_option(2, "Manual", "Leave utility manual")
            .set_group("automation"));

        SettingGroup cooldowns;
        cooldowns.id = "cooldowns";
        cooldowns.label = "Cooldowns & Trinkets";
        cooldowns.description = "Control burst timing and on-use items";
        cooldowns.icon = elemental_ids::STORMKEEPER;
        cooldowns.collapsible = true;
        cooldowns.collapsed_default = false;
        schema.add_group(cooldowns);
        schema.add(SettingDefinition::make_enum("cooldown_policy", "Cooldown Strategy", 0)
            .add_enum_option(0, "Smart", "Use major cooldowns on bosses and worthwhile pulls without losing uses.")
            .add_enum_option(1, "On Cooldown", "Use major cooldowns whenever their toggles allow.")
            .add_enum_option(2, "Boss Only", "Reserve major cooldowns for bosses and training dummies.")
            .set_group("cooldowns"));
        schema.add(SettingDefinition::make_enum("trinket_1_mode", "Trinket 1", 2)
            .add_enum_option(0, "Disabled", "Never use this trinket automatically.")
            .add_enum_option(1, "Smart", "Automatically choose between immediate use and burst alignment.")
            .add_enum_option(2, "With Major CDs", "Use only inside the Ascendance burst package.")
            .add_enum_option(3, "Boss Only", "Use only on bosses and training dummies.")
            .add_enum_option(4, "On Cooldown", "Use whenever ready while cooldown automation is enabled.")
            .set_group("cooldowns"));
        schema.add(SettingDefinition::make_enum("trinket_2_mode", "Trinket 2", 2)
            .add_enum_option(0, "Disabled", "Never use this trinket automatically.")
            .add_enum_option(1, "Smart", "Automatically choose between immediate use and burst alignment.")
            .add_enum_option(2, "With Major CDs", "Use only inside the Ascendance burst package.")
            .add_enum_option(3, "Boss Only", "Use only on bosses and training dummies.")
            .add_enum_option(4, "On Cooldown", "Use whenever ready while cooldown automation is enabled.")
            .set_group("cooldowns"));
        schema.add(SettingDefinition::make_bool("automatic_prepull", "Automatic Prepull", true)
            .set_description("Uses Titan's pull timer to prepare Stormkeeper and Lava Burst.")
            .set_group("cooldowns"));

        SettingGroup advanced;
        advanced.id = "advanced";
        advanced.label = "Advanced";
        advanced.description = "Overrides, niche automation, and diagnostics";
        advanced.icon = elemental_ids::SKYFURY;
        advanced.collapsible = true;
        advanced.collapsed_default = true;
        schema.add_group(advanced);
        schema.add(SettingDefinition::make_enum("content_mode", "Content Override", 0)
            .add_enum_option(0, "Automatic", "Detect Mythic+, raid, and solo play")
            .add_enum_option(1, "Mythic+", "Force dungeon pull logic")
            .add_enum_option(2, "Raid", "Force boss/raid logic")
            .add_enum_option(3, "Solo", "Force open-world logic")
            .set_description("Leave Automatic unless content detection is incorrect.")
            .set_group("advanced"));
        schema.add(SettingDefinition::make_enum("hero_mode", "Hero Tree Override", 0)
            .add_enum_option(0, "Automatic", "Detect the active hero talents")
            .add_enum_option(1, "Farseer", "Force Farseer sequencing")
            .add_enum_option(2, "Stormbringer", "Force Stormbringer sequencing")
            .set_description("Leave Automatic unless testing a specific hero tree.")
            .set_group("advanced"));
        schema.add(SettingDefinition::make_bool("auto_purge", "Automatic Purge", false)
            .set_description("Automatically remove configured enemy magic buffs.")
            .set_group("advanced"));
        schema.add(SettingDefinition::make_bool("auto_stun", "Capacitor Backup Stun", false)
            .set_description("Use Capacitor Totem as a configured Mythic+ backup stop.")
            .set_group("advanced"));
        schema.add(SettingDefinition::make_bool("queue_window_casting", "Queue Window Casting", true)
            .set_description("Send the next Lightning Bolt/Chain Lightning during Titan's spell "
                             "queue window at the end of your own hardcast.")
            .set_group("advanced"));
        schema.add(SettingDefinition::make_bool("debug_diagnostics", "Debug Diagnostics", false)
            .set_description("Enable detailed rotation diagnostics for troubleshooting.")
            .set_group("advanced"));

        return schema;
    }

    // -------------------------------------------------------------------------
    // SETTINGS INPUT / OUTPUT
    // -------------------------------------------------------------------------
    // Reads values from the client. The second argument on each line is the
    // safe fallback used if that setting is missing from an older config.
    void apply_settings(const SettingsMap& settings) override {
        SettingsAccessor s(settings);
        content_mode_ = static_cast<int>(s.get_enum("content_mode", 0));
        hero_mode_ = static_cast<int>(s.get_enum("hero_mode", 0));
        automatic_target_recovery_ = s.get_bool("automatic_target_recovery", true);
        auto_ghost_wolf_ = s.get_bool("auto_ghost_wolf", true);
        queue_window_casting_ = s.get_bool("queue_window_casting", true);
        debug_diagnostics_ = s.get_bool("debug_diagnostics", false);
        cooldown_policy_ = static_cast<int>(s.get_enum("cooldown_policy", 0));
        trinket_1_mode_ = std::clamp(static_cast<int>(s.get_enum("trinket_1_mode", 2)), 0, 4);
        trinket_2_mode_ = std::clamp(static_cast<int>(s.get_enum("trinket_2_mode", 2)), 0, 4);
        automatic_prepull_ = s.get_bool("automatic_prepull", true);
        survival_profile_ = std::clamp(static_cast<int>(s.get_enum("survival_profile", 1)), 0, 2);
        utility_profile_ = std::clamp(static_cast<int>(s.get_enum("utility_profile", 0)), 0, 2);

        // Smart policy: intentionally not exposed as micro-settings. These
        // values follow the live Midnight APL and content context.
        use_exact_opener_ = false;
        aoe_threshold_ = 3;
        flame_shock_refresh_ = 5.4;
        max_flame_shocks_mplus_ = 3;
        max_flame_shocks_raid_ = 2;
        spender_deficit_ = 15;
        mplus_bank_maelstrom_ttd_ = 2.0;
        mplus_bank_maelstrom_pack_hp_ = 8.0;
        mplus_meaningful_enemy_ttd_ = 6.0;
        mplus_meaningful_enemy_hp_ = 10.0;
        debug_interval_ = 2.0;
        trinket_min_ttd_ = 8.0;
        prepull_stormkeeper_ = automatic_prepull_;
        prepull_lava_burst_ = automatic_prepull_;
        prepull_lava_burst_delay_ = 0.15;
        stormkeeper_hold_for_asc_ = 10.0;
        ascendance_stormkeeper_window_ = 15.0;
        raid_cooldown_min_ttd_ = 18.0;
        mplus_cooldown_min_enemies_ = 3;
        mplus_cooldown_min_ttd_ = 10.0;
        mplus_cooldown_min_pack_hp_ = 25.0;
        prevent_lost_cooldown_uses_ = true;
        use_ancestral_swiftness_ = true;
        use_natures_swiftness_ = true;
        use_racials_ = true;

        const double defensive_shift[3] = {-8.0, 0.0, 10.0};
        const double shift = defensive_shift[survival_profile_];
        astral_shift_hp_ = 55.0 + shift;
        stone_bulwark_hp_ = 72.0 + shift;
        predictive_defensives_ = survival_profile_ != 0;
        predictive_defensive_hp_ = 85.0 + shift * 0.5;
        predictive_enemy_count_ = survival_profile_ == 2 ? 2 : 3;
        predictive_cast_window_ = 2.0;
        scale_defensives_with_key_ = true;
        use_earth_elemental_defensive_ = false;
        earth_elemental_hp_ = 38.0 + shift;
        emergency_heal_hp_ = 30.0 + shift;
        spiritwalkers_grace_move_delay_ = 0.8;

        interrupt_mode_ = 0;
        interrupt_at_percent_ = 55.0;
        priority_interrupt_at_percent_ = 25.0;
        const bool full_utility = utility_profile_ == 0;
        auto_cleanse_curse_ = full_utility;
        auto_purge_ = s.get_bool("auto_purge", false);
        purge_mode_ = 0;
        auto_poison_cleansing_ = full_utility;
        poison_cleansing_min_units_ = 2;
        auto_ancestral_guidance_ = full_utility;
        ancestral_guidance_group_hp_ = 74.0;
        ancestral_guidance_min_injured_ = 3;
        auto_healing_stream_ = full_utility;
        healing_stream_group_hp_ = 84.0;
        healing_stream_min_injured_ = 2;
        auto_capacitor_ = s.get_bool("auto_stun", false);
        capacitor_enemy_count_ = 5;
    }

    // Exposes the active values back to the client so they can be displayed and
    // saved. Every setting read above should also appear in this function.
    SettingsMap get_current_settings() const override {
        SettingsMap settings;
        settings["content_mode"] = static_cast<int64_t>(content_mode_);
        settings["hero_mode"] = static_cast<int64_t>(hero_mode_);
        settings["automatic_target_recovery"] = automatic_target_recovery_;
        settings["auto_ghost_wolf"] = auto_ghost_wolf_;
        settings["queue_window_casting"] = queue_window_casting_;
        settings["debug_diagnostics"] = debug_diagnostics_;
        settings["cooldown_policy"] = static_cast<int64_t>(cooldown_policy_);
        settings["trinket_1_mode"] = static_cast<int64_t>(trinket_1_mode_);
        settings["trinket_2_mode"] = static_cast<int64_t>(trinket_2_mode_);
        settings["automatic_prepull"] = automatic_prepull_;
        settings["survival_profile"] = static_cast<int64_t>(survival_profile_);
        settings["utility_profile"] = static_cast<int64_t>(utility_profile_);
        settings["auto_purge"] = auto_purge_;
        settings["auto_stun"] = auto_capacitor_;
        return settings;
    }

    // -------------------------------------------------------------------------
    // QUICK TOGGLES
    // -------------------------------------------------------------------------
    // Keep this list short: Titan already shows Pause, AOE, Cooldowns, and
    // Interrupt. Extra buttons should be things you click during a pull.
    // Purge and Capacitor stun are settings, not overlay buttons.
    //
    // Titan owns live and persisted toggle state. default_enabled is only the
    // first-load fallback; later profile reloads keep the client's last values.
    // Names and declaration order are a compatibility contract.
    std::vector<CustomToggle> get_custom_toggles() const override {
        return {
            {"Defensives", true},
            {"Utility", true},
            {"Mini CDs", true},
            {"Major CDs", true},
            {"Burst Now", false},
        };
    }

    // Titan must reserve secure executable bindings before a rotation can cast
    // on party/raid focus units or use ground-targeted @cursor/@player spells.
    std::vector<FriendlySpellBinding> get_friendly_spell_bindings() const override {
        return {
            {elemental_ids::HEALING_SURGE, "Healing Surge"},
            {elemental_ids::CLEANSE_SPIRIT, "Cleanse Spirit"}
        };
    }

    std::vector<InterruptSpell> get_interrupt_spells() const override {
        return {
            {elemental_ids::WIND_SHEAR, 30.0, false, true, false},
            {elemental_ids::CAPACITOR_TOTEM, 8.0, true, false, true}
        };
    }

    std::vector<MacroBinding> get_macro_bindings() const override {
        return {
            {elemental_ids::EARTHQUAKE_CURSOR_MACRO, "Earthquake @cursor"},
            {elemental_ids::CAPACITOR_PLAYER_MACRO, "Capacitor Totem @player"}
        };
    }

    // -------------------------------------------------------------------------
    // ROTATION LIFECYCLE
    // -------------------------------------------------------------------------
    // Called once when the profile loads. Clearing SpellBook forces a clean
    // runtime lookup for the character's current talents and spell overrides.
    void initialize() override {
        spellbook_ = {};
        last_spellbook_refresh_ = 0.0;
        last_processed_success_index_ = 0;
        last_damage_success_index_ = 0;
        last_successful_damage_spell_.clear();
        last_stormkeeper_success_time_ = -999.0;
        last_ancestral_swiftness_success_time_ = -999.0;
        last_voltaic_blaze_success_time_ = -999.0;
        last_ascendance_success_time_ = -999.0;
        last_trinket_1_success_time_ = -999.0;
        last_trinket_2_success_time_ = -999.0;
        last_stormkeeper_dispatch_time_ = -999.0;
        last_ancestral_swiftness_dispatch_time_ = -999.0;
        last_ascendance_dispatch_time_ = -999.0;
        last_voltaic_setup_dispatch_time_ = -999.0;
        last_input_dispatch_time_ = -999.0;
        last_prepull_lava_burst_generation_ = 0;
        last_trinket_1_attempt_time_ = -999.0;
        last_trinket_2_attempt_time_ = -999.0;
        trinket_1_broken_ = false;
        trinket_2_broken_ = false;
        trinket_1_item_id_ = 0;
        trinket_2_item_id_ = 0;
        trinket_break_message_.clear();
        clear_major_trinket_sync();
        major_trinket_bypass_until_[0] = -999.0;
        major_trinket_bypass_until_[1] = -999.0;
        major_trinket_bypass_item_[0] = 0;
        major_trinket_bypass_item_[1] = 0;
        last_retarget_attempt_time_ = -999.0;
        target_unreachable_since_ = -999.0;
        recovery_watch_guid_.clear();
        last_global_lock_ = {};
        clear_all_setup_pending();
        tracked_combat_start_time_ = 0.0;
        opener_started_at_ = 0.0;
        opener_active_ = false;
        opener_stormkeeper_done_ = false;
        opener_swiftness_done_ = false;
        opener_voltaic_done_ = false;
        opener_ascendance_done_ = false;
        last_logged_hero_tree_ = HeroTree::Unknown;
        hero_tree_was_logged_ = false;
        last_debug_log_time_ = -999.0;
        last_cd_toggle_diag_time_ = -999.0;
        last_recovery_debug_time_ = -999.0;
        last_range_fallback_debug_time_ = -999.0;
        last_target_continuity_debug_time_ = -999.0;
        range_fallback_message_.clear();
        target_continuity_message_.clear();
        current_snapshot_target_guid_.clear();
        current_snapshot_target_range_ = 0.0;
        current_snapshot_target_range_valid_ = false;
        current_snapshot_target_hostile_ = false;
        clear_damage_dispatch_state();
        dispatch_probe_ = {};
        execution_suspect_spell_ = 0;
        execution_suspect_escapes_ = 0;
        execution_break_message_.clear();
        queued_for_cast_at_ = -999.0;
        expected_prior_success_spell_ = 0;
        expected_prior_success_until_ = -999.0;
        escape_watch_spell_ = 0;
        escape_watch_until_ = -999.0;
        escape_watch_dispatched_at_ = -999.0;
        queued_prior_cast_start_ = -999.0;
        stall_active_ = false;
        stall_started_at_ = -999.0;
        stall_last_tick_at_ = -999.0;
        stall_begin_pending_ = false;
        stall_begin_cast_tail_ = false;
        stall_begin_moving_ = false;
        stall_begin_pending_spell_ = 0;
        stall_begin_suppressed_spell_ = 0;
        stall_begin_out_of_range_ = false;
        last_maintenance_spell_id_ = 0;
        last_maintenance_dispatch_time_ = -999.0;
        last_ghost_wolf_dispatch_time_ = -999.0;
        last_observed_maelstrom_ = -1;
        last_maelstrom_sample_time_ = -999.0;
        last_snapshot_enemies_ = 0;
        last_dup_block_key_ = 0;
        last_dup_block_window_end_ = -999.0;
        last_pack_enemy_count_ = 0;
        last_pack_average_range_ = 0.0;
        last_pack_snapshot_time_ = -999.0;
        pack_stable_since_ = -999.0;
        last_earthquake_delay_time_ = -999.0;
        last_forecast_save_time_ = -999.0;
        telemetry_ = {};
        telemetry_combat_active_ = false;
    }

    void shutdown() override {
    }

    // Refreshes runtime spell IDs at most once per second. This lets talent or
    // Midnight spell overrides work without hardcoding every new numeric ID.
    // Successful casts are processed here as well, giving the opener and
    // consecutive-spell safeguards confirmation from the game rather than from
    // the previous recommendation.
    void on_tick(const RotationContext& context) override {
        const auto& api = context.api();
        refresh_spellbook(api);
        update_combat_telemetry(context);
        update_cast_history(api);
        observe_maelstrom(context);

        const HeroTree detected = detect_hero_tree(api);
        if (!hero_tree_was_logged_ || detected != last_logged_hero_tree_) {
            context.log("Llama's Elemental v2.3.7 hero mode: " + hero_tree_name(detected));
            last_logged_hero_tree_ = detected;
            hero_tree_was_logged_ = true;
        }
        maybe_log_cooldown_toggle_state(context);
    }

    // PREPULL ORDER:
    // 1. Maintain Lightning Shield, Skyfury, weapon imbue, and Ward.
    // 2. Precast Stormkeeper if Titan's Cooldowns toggle is on.
    // 3. Once per active countdown, begin Lava Burst late enough to land just
    //    after zero. Its live hasted cast time comes from Titan's spell data.
    RotationAction get_prepull_action(const RotationContext& ctx) override {
        const auto& api = ctx.api();
        refresh_spellbook(api);

        if (RotationAction action = maintenance_action(api, true); !action.is_none()) {
            return action;
        }

        if (prepull_stormkeeper_ && mini_cooldowns_allowed(api)) {
            if (RotationAction action = dispatch_player_setup(
                    api, spellbook_.stormkeeper, stormkeeper_pending_until_,
                    last_stormkeeper_success_time_, kStormkeeperSettle,
                    last_stormkeeper_dispatch_time_, kStormkeeperAttempt,
                    "Prepull Stormkeeper"); !action.is_none())
            {
                return action;
            }
        }

        if (api.unit_is_casting_or_channeling("player", true)) {
            return no_action("Prepull cast already in progress");
        }

        const double pull_remaining = ctx.prepull_seconds_remaining();
        const uint64_t pull_generation = ctx.prepull_generation();
        if (prepull_lava_burst_ && pull_generation != 0 && pull_remaining > 0.0 &&
            pull_generation != last_prepull_lava_burst_generation_ &&
            can_damage_unit(api, "target") &&
            api.is_spell_in_range(spellbook_.lava_burst, "target") &&
            can_cast(api, spellbook_.lava_burst))
        {
            double cast_time = 2.0;
            if (const auto data = api.get_spell(spellbook_.lava_burst);
                data && data->cast_time > 0)
            {
                cast_time = static_cast<double>(data->cast_time) / 1000.0;
            }
            const double start_at = std::max(0.10, cast_time - prepull_lava_burst_delay_);
            if (pull_remaining <= start_at) {
                last_prepull_lava_burst_generation_ = pull_generation;
                return cast_damage(api, spellbook_.lava_burst, "target",
                    "Countdown-timed prepull Lava Burst");
            }
        }

        return no_action("Ready to pull");
    }

    // Out of combat only maintains buffs, unless a training dummy is still
    // selected. Dummy parks drop combat at 0% HP, then regenerate; keep the
    // complete rotation running so dummy testing and timed parses do not stall.
    // Ordinary hostiles are never pulled automatically.
    RotationAction get_out_of_combat_action(const RotationContext& ctx) override {
        const auto& api = ctx.api();
        refresh_spellbook(api);
        if (unit_looks_like_dummy(api, "target")) {
            return get_combat_action(ctx);
        }
        if (RotationAction action = maintenance_action(api, true); !action.is_none()) {
            return action;
        }
        return auto_ghost_wolf_action(api);
    }

    // Never dismount the character by automatically casting while mounted.
    RotationAction get_mounted_action(const RotationContext& ctx) override {
        (void)ctx;
        return no_action("Mounted");
    }

    // -------------------------------------------------------------------------
    // MASTER COMBAT ORDER
    // -------------------------------------------------------------------------
    // Only the first available action is returned. This ordering means survival
    // and required support actions can interrupt the normal damage priority.
    RotationAction get_combat_action(const RotationContext& ctx) override {
        const auto& api = ctx.api();
        refresh_spellbook(api);
        update_cast_history(api);
        reconcile_damage_dispatch(api);
        for (const std::string& message : damage_debug_messages_) ctx.log(message);
        damage_debug_messages_.clear();

        // Do not replace or clip a cast/channel already in progress.
        if (api.unit_is_casting_or_channeling("player", true)) {
            if (debug_diagnostics_) {
                const double now = api.get_game_time();
                if ((now - last_debug_log_time_) >= debug_interval_) {
                    ctx.log(debug_cast_snapshot(api));
                    last_debug_log_time_ = now;
                }
            }
            return no_action(debug_diagnostics_ ? debug_cast_snapshot(api) : "Casting");
        }

        // SURVIVAL / INTERRUPT PRE-LAYER: an ordinary damage button that has not
        // resolved yet must never hold a kick or an emergency defensive behind
        // its confirmation window. This only runs on ticks that are about to
        // wait, so a normal tick keeps the original order and cost.
        const double settle_remaining =
            kInputSettleWindow - (api.get_game_time() - last_input_dispatch_time_);
        if (settle_remaining > 0.0 || damage_dispatch_.pending) {
            // Wind Shear is dispatched untracked, so it cannot collide with the
            // unresolved damage button.
            if (utility_profile_ != 2) {
                if (RotationAction action = interrupt_action(api); !action.is_none()) {
                    return action;
                }
            }
            if (emergency_defensive_possible(api)) {
                CombatState emergency = build_state(api);
                last_global_lock_ = {emergency.global_lock, emergency.gcd_desync,
                                     emergency.global_lock_remaining};
                if (RotationAction action = defensive_action(api, emergency); !action.is_none()) {
                    return action;
                }
            }
        }

        // Titan can keep a stale cast/GCD snapshot for ~100-170ms after a
        // button is sent. Wait out the remainder of the 0.20s window so the
        // next tick can see the resulting state. This is not a GCD and does
        // not throttle the rotation after the snapshot catches up.
        if (settle_remaining > 0.0) {
            return wait_action(settle_remaining * 1000.0, "Input settle");
        }

        // Our own hardcast is still running even though Titan reports the
        // player free for the queue window. Once the single queued filler for
        // this cast has been used, or queue casting is off, wait out the cast
        // instead of evaluating a list that cannot dispatch anything.
        if (damage_dispatch_.pending && damage_dispatch_.cast_started_at > 0.0) {
            const double cast_remaining = player_cast_remaining(api);
            const bool queue_used = !queue_window_casting_ ||
                queued_for_cast_at_ == damage_dispatch_.cast_started_at;
            if (cast_remaining > 0.0 && queue_used) {
                return wait_action(std::clamp(cast_remaining * 1000.0, 30.0, 750.0),
                    "CAST_TAIL");
            }
        }

        // Hardcast visibility can arrive well after the 0.20s settle. While an
        // ordinary damage dispatch is still unresolved it keeps ownership, so
        // bridge only the remainder of its confirmation window. Nothing here
        // re-sends, re-times, or replaces the pending action.
        if (damage_dispatch_.pending) {
            const double remaining = kDamageDispatchConfirmWindow +
                damage_dispatch_.queue_grace -
                (api.get_game_time() - damage_dispatch_.dispatched_at);
            if (remaining > 0.0) {
                return wait_action(remaining * 1000.0, "Damage dispatch pending");
            }
        }

        // Take one consistent snapshot of enemies, resources, talents, and buffs.
        CombatState state = build_state(api);
        last_global_lock_ = {state.global_lock, state.gcd_desync, state.global_lock_remaining};
        sync_exact_opener(api, state);
        if (!trinket_break_message_.empty()) {
            ctx.log(trinket_break_message_);
            trinket_break_message_.clear();
        }
        if (!execution_break_message_.empty()) {
            ctx.log(execution_break_message_);
            execution_break_message_.clear();
        }
        if (!range_fallback_message_.empty()) {
            ctx.log(range_fallback_message_);
            range_fallback_message_.clear();
        }
        if (!target_continuity_message_.empty()) {
            ctx.log(target_continuity_message_);
            target_continuity_message_.clear();
        }
        if (debug_diagnostics_) {
            const double now = api.get_game_time();
            if ((now - last_debug_log_time_) >= debug_interval_) {
                ctx.log(debug_combat_snapshot(api, state));
                last_debug_log_time_ = now;
            }
        }

        // GLOBAL PRIORITY 1: stay alive. The emergency branch above only covers
        // low health; the full profile still runs here with the real snapshot.
        if (RotationAction action = defensive_action(api, state); !action.is_none()) {
            return action;
        }

        // GLOBAL PRIORITY 2: stop an interruptible enemy cast with Wind Shear.
        // Ticks that were about to wait on the dispatcher already checked this
        // above, so a kick is never delayed by an unresolved damage button.
        if (utility_profile_ != 2) {
            if (RotationAction action = interrupt_action(api); !action.is_none()) {
                return action;
            }
        }

        // GLOBAL PRIORITY 3: group support, cleanse, Purge, or optional AoE stun.
        if (utility_profile_ == 0) {
            if (RotationAction action = utility_action(api, state); !action.is_none()) {
                return action;
            }
        }

        // Long-duration upkeep (Lightning Shield, Skyfury, Flametongue) belongs
        // to prepull and out-of-combat handling. Refreshing it here repeatedly
        // stole sustained combat globals from the damage list.

        // Combat recovery Tabs only while already in combat. Dummies, mounted,
        // paused, and out-of-combat friendlies are left alone. In combat, a
        // friendly/empty/dead target recovers immediately when a healthy
        // hostile alternative exists.
        if (RotationAction action = combat_recovery_action(ctx, state); !action.is_none()) {
            return action;
        }

        // Never invent a target. Dummy parks stay on the selected dummy through
        // 0% regen; a friendly or empty target is left alone.
        if (!can_damage_action_target(api, state.target)) {
            return no_action(debug_diagnostics_
                ? debug_target_failure(api, state)
                : "No valid enemy target");
        }

        // Slot modes On Cooldown and Boss Only operate independently. A slot
        // configured With Major CDs is held for ascendance_burst_action(),
        // where it fires immediately before the rest of the burst package.
        if (RotationAction action = independent_trinket_action(api, state); !action.is_none()) {
            return action;
        }

        // Shared-GCD snapshot disagreement: get_remaining_gcd() can be 0 while
        // several builders still show the same short cooldown. Wait; do not
        // force an action around can_cast_spell().
        if (state.global_lock) {
            const double wait_ms = std::clamp(state.global_lock_remaining * 1000.0, 50.0, 1500.0);
            return wait_action(wait_ms, state.gcd_desync ? "GCD_DESYNC" : "GCD");
        }

        // GLOBAL PRIORITY 5: while moving, only choose legal instant actions or
        // activate Spiritwalker's Grace. Normal hardcasts resume when possible.
        if (state.moving && !state.spiritwalkers_grace) {
            if (RotationAction action = movement_action(api, state); !action.is_none()) {
                return action;
            }
            const std::string reason = debug_diagnostics_
                ? debug_stall_reason(api, state, "MOVING_STALL")
                : std::string("Moving - no instant action");
            note_stall_tick(api, state, true, reason);
            return no_action(reason);
        }

        // GLOBAL PRIORITY 6: complete the cast-confirmed burst opener before
        // allowing the normal list to interleave ordinary damage globals.
        if (RotationAction action = exact_opener_action(api, state); !action.is_none()) {
            return action;
        }

        // GLOBAL PRIORITY 7: use the one/two-target list or the 3+ target
        // cleave/AoE list. Burst racials and Nature's Swiftness are handled
        // immediately beside Ascendance so they cannot fire too early.
        if (state.use_aoe_list) {
            if (RotationAction action = aoe_action(api, state); !action.is_none()) {
                return action;
            }
        } else {
            if (RotationAction action = single_target_action(api, state); !action.is_none()) {
                return action;
            }
        }

        if (RotationAction action = final_damage_fallback(api, state); !action.is_none()) {
            return action;
        }

        const std::string reason = debug_diagnostics_
            ? debug_stall_reason(api, state, "STALL")
            : std::string("No Elemental action");
        note_stall_tick(api, state, false, reason);
        return no_action(reason);
    }

private:
    // -------------------------------------------------------------------------
    // RUNTIME SPELL TABLE
    // -------------------------------------------------------------------------
    // Each field stores the spell ID that is valid for the character right now.
    // A value of 0 means that spell/talent was not found and will be skipped.
    struct SpellBook {
        // Core builders, DoT, and Maelstrom spenders.
        uint32_t lightning_bolt = 0;
        uint32_t chain_lightning = 0;
        uint32_t lava_burst = 0;
        uint32_t flame_shock = 0;
        uint32_t earth_shock = 0;
        uint32_t earthquake = 0;
        uint32_t elemental_blast = 0;

        // Major cooldowns and Midnight talent/hero-talent buttons.
        uint32_t stormkeeper = 0;
        uint32_t ascendance = 0;
        uint32_t tempest = 0;
        uint32_t voltaic_blaze = 0;
        uint32_t ancestral_swiftness = 0;
        uint32_t natures_swiftness = 0;
        uint32_t frost_shock = 0;
        uint32_t fire_elemental = 0; // Also identifies the guardian's player aura.
        uint32_t flowing_elements = 0; // Proc aura stack lookup.
        uint32_t power_of_the_maelstrom = 0; // Proc aura stack lookup.

        // Maintenance, movement, survival, and group utility.
        uint32_t lightning_shield = 0;
        uint32_t skyfury = 0;
        uint32_t flametongue_weapon = 0;
        uint32_t thunderstrike_ward = 0;
        uint32_t spiritwalkers_grace = 0;
        uint32_t ghost_wolf = 0;
        uint32_t wind_shear = 0;
        uint32_t astral_shift = 0;
        uint32_t stone_bulwark_totem = 0;
        uint32_t healing_surge = 0;
        uint32_t cleanse_spirit = 0;
        uint32_t purge = 0;
        uint32_t capacitor_totem = 0;
        uint32_t earth_elemental = 0;
        uint32_t ancestral_guidance = 0;
        uint32_t healing_stream_totem = 0;
        uint32_t poison_cleansing_totem = 0;
        uint32_t tremor_totem = 0;

        // Only the racial learned by the current character should resolve.
        uint32_t blood_fury = 0;
        uint32_t berserking = 0;
        uint32_t fireblood = 0;
        uint32_t ancestral_call = 0;
    };

    enum class HeroTree {
        Unknown = 0,
        Farseer = 1,
        Stormbringer = 2
    };

    // -------------------------------------------------------------------------
    // ONE-TICK COMBAT SNAPSHOT
    // -------------------------------------------------------------------------
    // build_state() fills this once, then all action lists make decisions from
    // the same snapshot instead of repeatedly asking the game for changing data.
    struct CombatState {
        std::string target = "target"; // Current target or best visible fallback.
        int enemies = 0;               // Hostile nameplates currently in combat.
        int meaningful_enemies = 0;    // Enemies healthy/long-lived enough for CDs.
        int enemies_targeting_player = 0; // Predictive personal-defense signal.
        int mythic_plus_level = 0;      // Used to raise defensive thresholds.
        int maelstrom = 0;             // Current Elemental resource.
        int maelstrom_max = 100;       // Current cap, read from the game.
        int maelstrom_deficit = 100;   // Cap minus current; 15 means 85/100.
        int flame_shocks = 0;          // Player-owned Flame Shocks in range.
        int lightning_rods = 0;        // Player-owned Lightning Rods in range.
        // Nameplate filter instrumentation. Diagnostic only; nothing in the APL
        // may read these.
        int np_raw = 0;                // Entries returned by the scan.
        int np_damageable = 0;         // Entries passing can_damage_unit().
        int np_hostile_flags = 0;      // Entries with enemy/attack flags set.
        int np_dummy = 0;              // Entries recognized as training dummies.
        int np_effective = 0;          // Entries that incremented enemies.
        int np_count_api = -1;         // Titan count API; -1 when not sampled.
        int np_count_any = -1;         // Same scan without the combat filter.
        uint32_t stormkeeper_stacks = 0; // Charged Lightning/Chain Lightning casts.
        uint32_t tempest_stacks = 0;     // Tempest charges; protect a two-stack cap.
        uint32_t flowing_elements_stacks = 0; // SimC gates some LvBs below two.
        uint32_t power_of_the_maelstrom_stacks = 0; // LvB rule requires two.
        int max_flame_shocks = 2;      // Active cap chosen from M+ or raid setting.
        double player_hp = 100.0;      // Used by defensive thresholds.
        double target_hp = 100.0;      // Zero-health dummies need target recovery.
        double fight_ttd = 999.0;      // Highest enemy time-to-die estimate.
        double pack_health_percent = 100.0; // Combined current/max pull health.
        double dangerous_cast_remaining = 999.0; // Listed incoming player hit.
        double movement_time = 0.0;    // Seconds moving continuously.
        double fire_elemental_remaining = 0.0; // Early Flame Shock refresh gate.
        double pack_stable_for = 0.0; // Range/count snapshot unchanged this long.
        double melee_ratio = 0.0;     // Nearby-enemy clustering signal.
        double encounter_danger_eta = 999.0; // Next deadly raid event.
        double encounter_burst_eta = 999.0;  // Configured burst event.
        int forecast_builder_gain = 0; // Conservative next-builder Maelstrom.
        HeroTree hero_tree = HeroTree::Unknown; // Auto/manual active hero branch.
        bool is_mplus = false;         // Effective content mode is Mythic+.
        bool is_raid = false;          // Effective content mode is raid.
        bool is_boss = false;          // Boss classification or boss1 exists.
        bool moving = false;           // Player currently has movement speed.
        bool use_aoe_list = false;     // AoE toggle on and enemy threshold met.
        bool bank_maelstrom = false;   // Save resource for the next M+ pull.
        bool dangerous_cast_targeting_player = false;
        bool training_dummy = false;
        bool healthy_hostile_alternative = false; // Allows safe Tab recovery.
        bool spiritwalkers_grace = false; // Hardcasts are legal while moving.
        bool pack_stable = true;
        bool earthquake_safe = true;
        bool forecast_overcap = false;
        bool encounter_burst_hold = false;
        bool encounter_burst_go = false;
        bool encounter_danger_incoming = false;
        bool global_lock = false;
        bool gcd_desync = false;
        double global_lock_remaining = 0.0;

        // Selected talents that materially change the priority list.
        bool talent_elemental_blast = false;
        bool talent_earthquake = false; // Midnight talent makes EQ target-anchored.
        bool talent_master_of_the_elements = false;
        bool talent_tempest = false;
        bool talent_inferno_arc = false;
        bool talent_purging_flames = false;
        bool talent_crackling_fury = false;

        // Live procs/buffs consumed by the current SimC-style decision tree.
        bool buff_master_of_the_elements = false;
        bool buff_lava_surge = false;
        bool buff_stormkeeper = false;
        bool buff_tempest = false;
        bool buff_ascendance = false;
        bool buff_flowing_elements = false;
        bool buff_power_of_the_maelstrom = false;
        bool buff_purging_flames = false;
        bool buff_overcharge_tier = false;
        bool buff_elemental_blast_stat = false;
    };

    SpellBook spellbook_;
    double last_spellbook_refresh_ = 0.0;

    // Successful-cast history is the source of truth for sequencing. Aura
    // detection remains a fallback for hidden/late events.
    uint32_t last_processed_success_index_ = 0;
    uint32_t last_damage_success_index_ = 0;
    std::string last_successful_damage_spell_;
    double last_stormkeeper_success_time_ = -999.0;
    double last_ancestral_swiftness_success_time_ = -999.0;
    double last_voltaic_blaze_success_time_ = -999.0;
    double last_ascendance_success_time_ = -999.0;
    double last_trinket_1_success_time_ = -999.0;
    double last_trinket_2_success_time_ = -999.0;
    double last_stormkeeper_dispatch_time_ = -999.0;
    double last_ancestral_swiftness_dispatch_time_ = -999.0;
    double last_ascendance_dispatch_time_ = -999.0;
    double last_voltaic_setup_dispatch_time_ = -999.0;
    double last_input_dispatch_time_ = -999.0;
    uint64_t last_prepull_lava_burst_generation_ = 0;

    // Per-combat exact-opener state. A mechanic or unavailable spell may skip a
    // step, but a requested spell is never considered complete merely because
    // the rotation recommended it; success or a live aura/cooldown confirms it.
    double tracked_combat_start_time_ = 0.0;
    double opener_started_at_ = 0.0;
    bool opener_active_ = false;
    bool opener_stormkeeper_done_ = false;
    bool opener_swiftness_done_ = false;
    bool opener_voltaic_done_ = false;
    bool opener_ascendance_done_ = false;
    HeroTree last_logged_hero_tree_ = HeroTree::Unknown;
    bool hero_tree_was_logged_ = false;

    // Stored values from the Automation settings group.
    int content_mode_ = 0;
    int hero_mode_ = 0;
    bool use_exact_opener_ = false;
    double exact_opener_timeout_ = 15.0;
    int aoe_threshold_ = 3;
    double flame_shock_refresh_ = 5.4;
    int max_flame_shocks_mplus_ = 3;
    int max_flame_shocks_raid_ = 2;
    int spender_deficit_ = 15;
    double mplus_bank_maelstrom_ttd_ = 2.0;
    double mplus_bank_maelstrom_pack_hp_ = 8.0;
    double mplus_meaningful_enemy_ttd_ = 6.0;
    double mplus_meaningful_enemy_hp_ = 10.0;
    bool debug_diagnostics_ = false;
    bool queue_window_casting_ = true;
    double debug_interval_ = 2.0;
    double last_debug_log_time_ = -999.0;
    double last_cd_toggle_diag_time_ = -999.0;
    double last_recovery_debug_time_ = -999.0;
    double last_range_fallback_debug_time_ = -999.0;
    double last_target_continuity_debug_time_ = -999.0;
    std::string range_fallback_message_;
    std::string target_continuity_message_;
    std::string current_snapshot_target_guid_;
    double current_snapshot_target_range_ = 0.0;
    bool current_snapshot_target_range_valid_ = false;
    bool current_snapshot_target_hostile_ = false;
    uint32_t last_dup_block_key_ = 0;
    double last_dup_block_window_end_ = -999.0;
    bool automatic_target_recovery_ = true;
    bool auto_ghost_wolf_ = true;
    bool automatic_prepull_ = true;
    int survival_profile_ = 1;
    int utility_profile_ = 0;
    double last_retarget_attempt_time_ = -999.0;
    double target_unreachable_since_ = -999.0;
    std::string recovery_watch_guid_;
    std::string last_hostile_target_guid_;
    double last_hostile_target_seen_time_ = -999.0;
    int last_pack_enemy_count_ = 0;
    double last_pack_average_range_ = 0.0;
    double last_pack_snapshot_time_ = -999.0;
    double pack_stable_since_ = -999.0;
    double last_earthquake_delay_time_ = -999.0;
    double last_forecast_save_time_ = -999.0;

    // -------------------------------------------------------------------------
    // DAMAGE DISPATCH STATE
    // -------------------------------------------------------------------------
    // One shared record for every ordinary rotational damage spell. Setup
    // cooldowns keep their own attempt/pending/success latches; this only
    // tracks whether the last recommended damage button actually became a cast.
    struct DamageDispatchState {
        uint32_t spell_id = 0;
        std::string target_guid;
        double dispatched_at = -999.0;
        double cast_started_at = -999.0;
        double succeeded_at = -999.0;
        bool pending = false;

        uint32_t suppressed_spell_id = 0;
        std::string suppressed_target_guid;
        double suppressed_until = -999.0;

        // Extra confirmation time granted because the button was sent inside
        // the spell queue window of a cast that was still finishing. The game
        // cannot start a queued spell before that cast ends.
        double queue_grace = 0.0;
        bool queued_send = false;
    } damage_dispatch_;

    // -------------------------------------------------------------------------
    // SPELL QUEUE WINDOW STATE
    // -------------------------------------------------------------------------
    // Titan reports the player as free during the tail of a hardcast when
    // include_rotation_spell_queue_window is true, which is how it invites the
    // next button. Exactly one filler may be queued per cast instance.
    double queued_for_cast_at_ = -999.0;
    uint32_t expected_prior_success_spell_ = 0;
    double expected_prior_success_until_ = -999.0;
    // Start time of the cast a button was queued behind. Chain Lightning after
    // Chain Lightning shares an ID, so only this distinguishes the finishing
    // cast bar from the queued one.
    double queued_prior_cast_start_ = -999.0;

    // Rare escape follow-up: proves whether an escaped button executed late.
    uint32_t escape_watch_spell_ = 0;
    double escape_watch_until_ = -999.0;
    double escape_watch_dispatched_at_ = -999.0;

    // -------------------------------------------------------------------------
    // STALL EPISODE STATE (fixed scalars only)
    // -------------------------------------------------------------------------
    bool stall_active_ = false;
    double stall_started_at_ = -999.0;
    double stall_last_tick_at_ = -999.0;
    bool stall_begin_pending_ = false;
    bool stall_begin_cast_tail_ = false;
    bool stall_begin_moving_ = false;
    uint32_t stall_begin_pending_spell_ = 0;
    uint32_t stall_begin_suppressed_spell_ = 0;
    bool stall_begin_out_of_range_ = false;

    std::vector<std::string> damage_debug_messages_;
    uint32_t last_damage_dispatch_log_spell_ = 0;
    double last_damage_dispatch_log_time_ = -999.0;
    double last_damage_suppressed_log_time_ = -999.0;
    double last_unmatched_success_log_time_ = -999.0;

    // -------------------------------------------------------------------------
    // DISPATCH EXECUTION PROBE
    // -------------------------------------------------------------------------
    // Runtime proved a rotational spell can be accepted by the executor and
    // still never reach the game (the v2.3.3 BUTTON5 Lava Burst case). The
    // probe records what was true at dispatch so a resolution line can name the
    // failure. v2.3.4 removed the production blocker: measured escapes are
    // frequently false, so no ordinary damage spell is ever parked.
    struct DispatchProbe {
        double gcd_at_dispatch = 0.0;
        bool locked_at_dispatch = false;
        bool instant_expected = false;
        bool queue_window_at_dispatch = false;
        uint32_t success_index_at_dispatch = 0;
        uint32_t cast_at_dispatch = 0;
    } dispatch_probe_;

    enum class DispatchResult { SuccessHistory, CastStarted, TargetChanged, Escaped };

    uint32_t execution_suspect_spell_ = 0;
    int execution_suspect_escapes_ = 0;
    std::string execution_break_message_;

    // Long-duration buff upkeep owns a single attempt latch. It never takes
    // part in DamageDispatchState.
    uint32_t last_maintenance_spell_id_ = 0;
    double last_maintenance_dispatch_time_ = -999.0;
    double last_ghost_wolf_dispatch_time_ = -999.0;

    // -------------------------------------------------------------------------
    // MAELSTROM OBSERVATION (DIAGNOSTIC ONLY)
    // -------------------------------------------------------------------------
    // Measures the real resource against resolved rotational actions. Nothing
    // here may influence action selection.
    // One resolved rotational action owns one resource packet. v2.3.4 closed the
    // sample on the first movement, which measured 1420 of the 3152 points of
    // positive Maelstrom seen in the reference log; the rest arrived after the
    // sample had already closed. The packet keeps accumulating until the
    // resource goes quiet or the hard lifetime expires.
    struct MaelstromPacket {
        bool active = false;
        bool ambiguous = false;
        uint32_t spell_id = 0;
        int start = 0;
        int last = 0;
        int positive_total = 0;
        int negative_total = 0;
        int first_positive = 0;
        int changes = 0;
        double opened_at = -999.0;
        double last_change_at = -999.0;
        int enemies = 0;
        bool stormkeeper = false;
        bool ancestral_swiftness = false;
        bool ascendance = false;
        bool lust = false;
    } maelstrom_packet_;

    // A resolution can be detected inside get_combat_action(), after the tick's
    // resource sample already ran. The request is serviced by the next sample so
    // every packet opens against a freshly observed baseline without adding a
    // second power read.
    struct MaelstromPacketRequest {
        bool pending = false;
        bool ambiguous = false;
        uint32_t spell_id = 0;
        int enemies = 0;
        bool stormkeeper = false;
        bool ancestral_swiftness = false;
        bool ascendance = false;
        bool lust = false;
    } maelstrom_request_;

    enum class BuilderBucket { LightningBolt, ChainLightning, LavaBurst, VoltaicBlaze,
                               Tempest, Other, Count };
    enum class PacketContext { Normal, Stormkeeper, AncestralSwiftness, Ascendance,
                               Lust, Count };
    static constexpr int kBuilderBuckets = static_cast<int>(BuilderBucket::Count);
    static constexpr int kEnemyBuckets = 5;
    static constexpr int kPacketContexts = static_cast<int>(PacketContext::Count);

    struct PacketStat {
        int count = 0;
        long long sum = 0;
        int min = 0;
        int max = 0;
        double ewma = 0.0;

        void add(int value) {
            if (count == 0) {
                min = value;
                max = value;
                ewma = static_cast<double>(value);
            } else {
                if (value < min) min = value;
                if (value > max) max = value;
                ewma = 0.25 * static_cast<double>(value) + 0.75 * ewma;
            }
            ++count;
            sum += value;
        }
        double average() const {
            return count > 0 ? static_cast<double>(sum) / count : 0.0;
        }
    };

    PacketStat packet_by_enemies_[kBuilderBuckets][kEnemyBuckets];
    PacketStat packet_by_context_[kBuilderBuckets][kPacketContexts];
    int packet_ambiguous_by_enemies_[kBuilderBuckets][kEnemyBuckets] = {};

    int last_observed_maelstrom_ = -1;
    double last_maelstrom_sample_time_ = -999.0;
    int last_snapshot_enemies_ = 0;

    struct CombatTelemetry {
        double started_at = 0.0;
        double last_sample_at = 0.0;
        double gcd_idle_seconds = 0.0;
        double near_cap_seconds = 0.0;
        int successful_casts = 0;
        int spenders = 0;
        int earthquakes = 0;
        int elemental_blasts = 0;
        int stormkeepers = 0;
        int ascendances = 0;
        int trinkets = 0;
        int target_recoveries = 0;
        int earthquake_delays = 0;
        int forecast_saves = 0;
        int duplicate_dispatches_prevented = 0;
        int earthquake_opportunities = 0;
        int lava_burst_dispatched = 0;
        int lava_burst_confirmed = 0;
        int lava_burst_cast_started = 0;
        int lava_burst_escaped = 0;
        int damage_dispatches = 0;
        int damage_started = 0;
        int damage_confirmed = 0;
        int damage_escapes = 0;
        int damage_suppressed = 0;
        int escape_late_success = 0;
        int queue_releases = 0;
        int queue_confirmed = 0;
        int queue_escapes = 0;
        int stall_episodes = 0;
        double stall_total_seconds = 0.0;
        double stall_max_seconds = 0.0;
        int stall_over_250ms = 0;
        int stall_over_500ms = 0;
        int stall_over_1000ms = 0;
        int stall_cast_tail = 0;
        int stall_pending = 0;
        int stall_suppression = 0;
        int stall_out_of_range = 0;
        int stall_apl_no_action = 0;
        int builder_resolved = 0;
        int builder_positive_delta = 0;
        int builder_gain_total = 0;
        int lightning_bolt_resolved = 0;
        int lightning_bolt_gain = 0;
        int chain_lightning_resolved = 0;
        int chain_lightning_gain = 0;
        int chain_lightning_gain_normal = 0;
        int chain_lightning_gain_stormkeeper = 0;
        int chain_lightning_gain_swiftness = 0;
        int trinket_sync_confirms = 0;
        int trinket_sync_retries = 0;
        int trinket_sync_failures = 0;
        int maelstrom_packets_total = 0;
        int maelstrom_packets_clean = 0;
        int maelstrom_packets_ambiguous = 0;
        int maelstrom_packets_timeout = 0;
        int maelstrom_packets_overlap = 0;
        int maelstrom_packets_capped = 0;
        int maelstrom_packets_learnable = 0;
        // Direct comparison against the v2.3.4 first-delta observer.
        int packet_first_sum = 0;
        int packet_total_sum = 0;
    } telemetry_;
    bool telemetry_combat_active_ = false;

    // Stored values from the Cooldowns settings group.
    int cooldown_policy_ = 0;
    int trinket_1_mode_ = 2;
    int trinket_2_mode_ = 2;
    double trinket_min_ttd_ = 8.0;
    bool prepull_stormkeeper_ = true;
    bool prepull_lava_burst_ = true;
    double prepull_lava_burst_delay_ = 0.15;
    double stormkeeper_hold_for_asc_ = 10.0;
    double ascendance_stormkeeper_window_ = 15.0;
    double raid_cooldown_min_ttd_ = 18.0;
    int mplus_cooldown_min_enemies_ = 3;
    double mplus_cooldown_min_ttd_ = 10.0;
    double mplus_cooldown_min_pack_hp_ = 25.0;
    bool prevent_lost_cooldown_uses_ = true;
    bool use_ancestral_swiftness_ = true;
    bool use_natures_swiftness_ = true;
    bool use_racials_ = true;
    double last_trinket_1_attempt_time_ = -999.0;
    double last_trinket_2_attempt_time_ = -999.0;
    bool trinket_1_broken_ = false;
    bool trinket_2_broken_ = false;
    uint32_t trinket_1_item_id_ = 0;
    uint32_t trinket_2_item_id_ = 0;
    std::string trinket_break_message_;
    double stormkeeper_pending_until_ = -999.0;
    double ancestral_swiftness_pending_until_ = -999.0;
    double voltaic_setup_pending_until_ = -999.0;
    double ascendance_pending_until_ = -999.0;
    double trinket_1_pending_until_ = -999.0;
    double trinket_2_pending_until_ = -999.0;

    // Mode-2 synchronization barrier. Runtime proved that a Titan-accepted item
    // action is not proof the item was used, so the Major-CD package holds here
    // until equipped-item state or a trinket spell event says otherwise.
    struct MajorTrinketSync {
        int slot = 0;
        uint32_t item_id = 0;
        double armed_at = -999.0;
        double dispatched_at = -999.0;
        int attempts = 0;
    } major_trinket_sync_;
    // Per-slot hold-off after a bounded failure so one bad press cannot loop the
    // burst package. Cleared by combat reset or an equipment change.
    double major_trinket_bypass_until_[2] = {-999.0, -999.0};
    uint32_t major_trinket_bypass_item_[2] = {0, 0};

    struct GlobalLockState {
        bool locked = false;
        bool desync = false;
        double remaining = 0.0;
    } last_global_lock_;

    // Stored values from the Survival & Movement settings group.
    double astral_shift_hp_ = 55.0;
    double stone_bulwark_hp_ = 72.0;
    bool predictive_defensives_ = true;
    double predictive_defensive_hp_ = 85.0;
    int predictive_enemy_count_ = 3;
    double predictive_cast_window_ = 2.0;
    bool scale_defensives_with_key_ = true;
    bool use_earth_elemental_defensive_ = false;
    double earth_elemental_hp_ = 38.0;
    double emergency_heal_hp_ = 30.0;
    double spiritwalkers_grace_move_delay_ = 0.8;

    // Stored values from the Mythic+ Utility settings group.
    int interrupt_mode_ = 0;
    double interrupt_at_percent_ = 55.0;
    double priority_interrupt_at_percent_ = 25.0;
    bool auto_cleanse_curse_ = true;
    bool auto_purge_ = false;
    int purge_mode_ = 0;
    bool auto_poison_cleansing_ = true;
    int poison_cleansing_min_units_ = 2;
    bool auto_ancestral_guidance_ = true;
    double ancestral_guidance_group_hp_ = 74.0;
    int ancestral_guidance_min_injured_ = 3;
    bool auto_healing_stream_ = true;
    double healing_stream_group_hp_ = 84.0;
    int healing_stream_min_injured_ = 2;
    bool auto_capacitor_ = false;
    int capacitor_enemy_count_ = 5;

    // -------------------------------------------------------------------------
    // STRING / SPELL RESOLUTION HELPERS
    // -------------------------------------------------------------------------
    // Case-insensitive exact comparison for English spell and aura names.
    static bool same_name(const std::string& left, const std::string& right) {
        if (left.size() != right.size()) return false;
        for (size_t i = 0; i < left.size(); ++i) {
            const unsigned char a = static_cast<unsigned char>(left[i]);
            const unsigned char b = static_cast<unsigned char>(right[i]);
            if (std::tolower(a) != std::tolower(b)) return false;
        }
        return true;
    }

    // Case-insensitive partial match, used for families such as the three
    // Elemental Blast stat buffs or a tier-set Overcharge proc.
    static bool contains_name(const std::string& text, const std::string& part) {
        std::string lowered_text = text;
        std::string lowered_part = part;
        std::transform(lowered_text.begin(), lowered_text.end(), lowered_text.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(lowered_part.begin(), lowered_part.end(), lowered_part.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered_text.find(lowered_part) != std::string::npos;
    }

    // Finds the character's current usable/known spell ID by name. Stable
    // fallback IDs cover older/core spells; 0 cleanly disables absent talents.
    uint32_t resolve_spell(const rotation_api::IRotationAPI& api,
                           const std::string& name,
                           uint32_t fallback) const
    {
        const auto matches = api.find_all_spells_by_name(name);
        uint32_t known = 0;

        for (const auto& data : matches) {
            uint32_t candidate = data.spell_id;
            if (data.override_spell_id != 0 &&
                same_name(api.get_spell_name(data.override_spell_id), name))
            {
                candidate = data.override_spell_id;
            }
            if (candidate == 0) continue;
            if (api.can_cast_spell(candidate)) return candidate;
            if (known == 0 && (data.is_spell_known || data.is_player_spell || api.is_spell_known_or_overrides_known(candidate))) {
                known = candidate;
            }
        }

        if (known != 0) return known;

        if (fallback != 0) {
            if (api.is_spell_known_or_overrides_known(fallback) || api.has_spell(fallback)) return fallback;
            const uint32_t override_id = api.get_override_spell(fallback);
            if (override_id != 0 &&
                api.is_spell_known_or_overrides_known(override_id) &&
                same_name(api.get_spell_name(override_id), name))
            {
                return override_id;
            }
        }

        return 0;
    }

    // Rebuilds the runtime spell table once per second. This is intentionally
    // throttled because name lookup is more expensive than reading cached IDs.
    void refresh_spellbook(const rotation_api::IRotationAPI& api) {
        const double now = api.get_game_time();
        if (last_spellbook_refresh_ > 0.0 && (now - last_spellbook_refresh_) < 1.0) return;
        last_spellbook_refresh_ = now;

        // Damage rotation spells.
        spellbook_.lightning_bolt = resolve_spell(api, "Lightning Bolt", elemental_ids::LIGHTNING_BOLT);
        spellbook_.chain_lightning = resolve_spell(api, "Chain Lightning", elemental_ids::CHAIN_LIGHTNING);
        spellbook_.lava_burst = resolve_spell(api, "Lava Burst", elemental_ids::LAVA_BURST);
        spellbook_.flame_shock = resolve_spell(api, "Flame Shock", elemental_ids::FLAME_SHOCK);
        spellbook_.earth_shock = resolve_spell(api, "Earth Shock", elemental_ids::EARTH_SHOCK);
        spellbook_.earthquake = resolve_spell(api, "Earthquake", elemental_ids::EARTHQUAKE);
        spellbook_.elemental_blast = resolve_spell(api, "Elemental Blast", elemental_ids::ELEMENTAL_BLAST);
        spellbook_.stormkeeper = resolve_spell(api, "Stormkeeper", elemental_ids::STORMKEEPER);
        spellbook_.ascendance = resolve_spell(api, "Ascendance", elemental_ids::ASCENDANCE);
        spellbook_.tempest = resolve_spell(api, "Tempest", elemental_ids::TEMPEST);
        spellbook_.voltaic_blaze = resolve_spell(api, "Voltaic Blaze", 0);
        spellbook_.ancestral_swiftness = resolve_spell(api, "Ancestral Swiftness", 0);
        spellbook_.natures_swiftness = resolve_spell(api, "Nature's Swiftness", 0);
        spellbook_.frost_shock = resolve_spell(api, "Frost Shock", elemental_ids::FROST_SHOCK);
        spellbook_.fire_elemental = resolve_spell(api, "Fire Elemental", elemental_ids::FIRE_ELEMENTAL);
        spellbook_.flowing_elements = resolve_spell(api, "Flowing Elements", 0);
        spellbook_.power_of_the_maelstrom = resolve_spell(api, "Power of the Maelstrom", 0);

        // Maintenance, defensive, movement, and utility spells.
        spellbook_.lightning_shield = resolve_spell(api, "Lightning Shield", elemental_ids::LIGHTNING_SHIELD);
        spellbook_.skyfury = resolve_spell(api, "Skyfury", elemental_ids::SKYFURY);
        spellbook_.flametongue_weapon = resolve_spell(api, "Flametongue Weapon", elemental_ids::FLAMETONGUE_WEAPON);
        spellbook_.thunderstrike_ward = resolve_spell(api, "Thunderstrike Ward", 0);
        spellbook_.spiritwalkers_grace = resolve_spell(api, "Spiritwalker's Grace", elemental_ids::SPIRITWALKERS_GRACE);
        spellbook_.ghost_wolf = resolve_spell(api, "Ghost Wolf", elemental_ids::GHOST_WOLF);
        spellbook_.wind_shear = resolve_spell(api, "Wind Shear", elemental_ids::WIND_SHEAR);
        spellbook_.astral_shift = resolve_spell(api, "Astral Shift", elemental_ids::ASTRAL_SHIFT);
        spellbook_.stone_bulwark_totem = resolve_spell(api, "Stone Bulwark Totem", elemental_ids::STONE_BULWARK_TOTEM);
        spellbook_.healing_surge = resolve_spell(api, "Healing Surge", elemental_ids::HEALING_SURGE);
        spellbook_.cleanse_spirit = resolve_spell(api, "Cleanse Spirit", elemental_ids::CLEANSE_SPIRIT);
        spellbook_.purge = resolve_spell(api, "Purge", elemental_ids::PURGE);
        spellbook_.capacitor_totem = resolve_spell(api, "Capacitor Totem", elemental_ids::CAPACITOR_TOTEM);
        spellbook_.earth_elemental = resolve_spell(api, "Earth Elemental", elemental_ids::EARTH_ELEMENTAL);
        spellbook_.ancestral_guidance = resolve_spell(api, "Ancestral Guidance", elemental_ids::ANCESTRAL_GUIDANCE);
        spellbook_.healing_stream_totem = resolve_spell(api, "Healing Stream Totem", elemental_ids::HEALING_STREAM_TOTEM);
        spellbook_.poison_cleansing_totem = resolve_spell(api, "Poison Cleansing Totem", elemental_ids::POISON_CLEANSING_TOTEM);
        spellbook_.tremor_totem = resolve_spell(api, "Tremor Totem", elemental_ids::TREMOR_TOTEM);

        // Damage racials. Unavailable racials remain 0 and are ignored.
        spellbook_.blood_fury = resolve_spell(api, "Blood Fury", 0);
        spellbook_.berserking = resolve_spell(api, "Berserking", 0);
        spellbook_.fireblood = resolve_spell(api, "Fireblood", 0);
        spellbook_.ancestral_call = resolve_spell(api, "Ancestral Call", 0);
    }

    // -------------------------------------------------------------------------
    // UNIT / AURA / TALENT HELPERS
    // -------------------------------------------------------------------------
    // Central castability guard: 0 means the talent/spell is unavailable.
    static bool can_cast(const rotation_api::IRotationAPI& api, uint32_t spell_id) {
        return spell_id != 0 && api.can_cast_spell(spell_id);
    }

    static constexpr double kSetupPendingWindow = 0.75;
    static constexpr double kVoltaicSetupPendingWindow = 0.50;
    static constexpr double kStormkeeperSettle = 2.0;
    static constexpr double kAncestralSwiftnessSettle = 1.75;
    static constexpr double kAscendanceSettle = 2.0;
    static constexpr double kTrinketSettle = 1.25;
    static constexpr double kVoltaicSetupSettle = 0.75;
    static constexpr double kStormkeeperAttempt = 2.5;
    static constexpr double kAncestralSwiftnessAttempt = 1.5;
    static constexpr double kAscendanceAttempt = 2.0;
    static constexpr double kTrinketAttempt = 1.5;
    static constexpr double kVoltaicSetupAttempt = 1.0;
    static constexpr double kInputSettleWindow = 0.20;
    static constexpr double kTabRateLimit = 0.75;
    static constexpr double kUnreachableDwell = 0.60;
    static constexpr double kCloseRangeFallback = 10.0;
    static constexpr double kHostileGuidGrace = 0.35;
    static constexpr double kDamageDispatchConfirmWindow = 0.45;
    static constexpr double kDamageDispatchEscapeWindow = 0.75;
    static constexpr double kMaintenanceAttemptWindow = 2.0;
    static constexpr double kGhostWolfMoveDelay = 1.0;
    static constexpr double kGhostWolfAttempt = 1.0;
    // Titan publishes item data every 500 ms. The v2.3.4 log shows the working
    // trinket press at 330954.04 first reported cooldown at 330954.60, so an
    // item is not judged failed until a full publication interval has passed.
    static constexpr double kTrinketSyncSettle = 0.60;
    // Total time the burst package may hold waiting for a strictly free window
    // before it gives up and proceeds without the item.
    static constexpr double kTrinketSyncHoldMax = 2.50;
    // Poll interval while the barrier holds the package.
    static constexpr double kTrinketSyncPollMs = 80.0;
    // Hold-off applied to a slot after the bounded failure path.
    static constexpr double kTrinketSyncBypass = 30.0;
    // Resource packets: a change may arrive one item/power publication after the
    // previous one, so the quiet window must exceed the 250 ms power interval.
    // 0.60 s is an A/B validation pass: two 250 ms intervals plus scheduling jitter.
    static constexpr double kMaelstromPacketQuiet = 0.60;
    // Hard lifetime. Measured spacing between resolved rotational actions in the
    // v2.3.4 log has a median of 1.33 s, so 1.20 s closes a packet before the
    // next action can normally contaminate it.
    static constexpr double kMaelstromPacketMax = 1.20;
    // Diagnostics only since v2.3.4. Measured escapes are frequently false, so
    // reaching this count reports a suspect spell and never blocks it.
    static constexpr int kExecutionSuspectEscapes = 4;
    // A button sent into the queue window cannot start before the current cast
    // ends, so its confirmation deadline grows by the remaining cast time.
    static constexpr double kQueueGraceMax = 0.75;
    // How long the finishing cast may still deliver its own success event after
    // the dispatcher was handed to a queued button.
    static constexpr double kPriorSuccessGrace = 1.50;
    // Late-execution proof window for an escaped button.
    static constexpr double kEscapeWatchWindow = 1.50;
    // Idle ticks further apart than this belong to separate stall episodes.
    static constexpr double kStallEpisodeGap = 0.50;
    static constexpr double kSharedLockMax = 1.50;
    static constexpr double kSharedLockCluster = 0.25;

    void clear_all_setup_pending() {
        stormkeeper_pending_until_ = -999.0;
        ancestral_swiftness_pending_until_ = -999.0;
        voltaic_setup_pending_until_ = -999.0;
        ascendance_pending_until_ = -999.0;
        trinket_1_pending_until_ = -999.0;
        trinket_2_pending_until_ = -999.0;
    }

    static bool still_pending(double until, double now) {
        return until > 0.0 && now < until;
    }

    static bool recently_succeeded(double last_success_time, double settle_window, double now) {
        return last_success_time > 0.0 && (now - last_success_time) < settle_window;
    }

    static bool recently_attempted(double last_dispatch_time, double attempt_window, double now) {
        return last_dispatch_time > 0.0 && (now - last_dispatch_time) < attempt_window;
    }

    void note_input_dispatch(const rotation_api::IRotationAPI& api) {
        last_input_dispatch_time_ = api.get_game_time();
        // Any real button ends an idle episode, including setup actions that do
        // not run through the damage dispatcher.
        note_stall_end(api, 0);
    }

    // -------------------------------------------------------------------------
    // STALL EPISODES
    // -------------------------------------------------------------------------
    // Transition based: one line when idling starts, one line when it ends,
    // plus fixed scalar counters. Nothing is printed per tick.
    void note_stall_tick(const rotation_api::IRotationAPI& api,
                         const CombatState& state,
                         bool moving,
                         const std::string& snapshot)
    {
        const double now = api.get_game_time();
        if (stall_active_) {
            // Idle ticks must be contiguous. A gap means the rotation was doing
            // something else (dead target, out of range, out of combat), which
            // does not belong to this episode.
            if ((now - stall_last_tick_at_) <= kStallEpisodeGap) {
                stall_last_tick_at_ = now;
                return;
            }
            finish_stall_episode(stall_last_tick_at_, 0);
        }
        const rotation_api::CastInfo info = current_cast_info(api, "player");
        stall_active_ = true;
        stall_started_at_ = now;
        stall_last_tick_at_ = now;
        stall_begin_pending_ = damage_dispatch_.pending;
        stall_begin_pending_spell_ = damage_dispatch_.spell_id;
        stall_begin_suppressed_spell_ = damage_dispatch_.suppressed_spell_id;
        stall_begin_moving_ = moving;
        stall_begin_cast_tail_ = damage_dispatch_.pending &&
            damage_dispatch_.cast_started_at > 0.0 && info.is_active();
        stall_begin_out_of_range_ =
            can_damage_action_target(api, state.target) &&
            !representative_damage_in_range(api, state.target) &&
            !state.healthy_hostile_alternative;
        if (!debug_diagnostics_) return;

        std::ostringstream out;
        out << "STALL_BEGIN v2.3.7"
            << " time=" << format_seconds(stall_started_at_)
            << " pending=" << (stall_begin_pending_ ? 1 : 0)
            << " pending_spell=" << stall_begin_pending_spell_
            << " cast_tail=" << (stall_begin_cast_tail_ ? 1 : 0)
            << " cast=" << info.spell_id
            << " cast_strict=" << (player_cast_active_strict(api) ? 1 : 0)
            << " suppressed_spell=" << stall_begin_suppressed_spell_
            << " oor=" << (stall_begin_out_of_range_ ? 1 : 0)
            << " last_success=" << (last_successful_damage_spell_.empty()
                ? std::string("<none>") : last_successful_damage_spell_)
            << " maelstrom=" << state.maelstrom
            << " enemies=" << state.enemies
            << ' ' << snapshot;
        queue_damage_debug(out.str());
    }

    // Classified only from state that was actually captured when idling began.
    const char* stall_classification() const {
        if (stall_begin_moving_) return "movement";
        if (stall_begin_cast_tail_) return "post_cast_tail";
        if (stall_begin_pending_) return "pending_resolution";
        if (stall_begin_suppressed_spell_ != 0) return "suppression";
        if (stall_begin_out_of_range_) return "out_of_range";
        return "apl_no_action";
    }

    void note_stall_end(const rotation_api::IRotationAPI& api, uint32_t next_spell_id) {
        if (!stall_active_) return;
        finish_stall_episode(api.get_game_time(), next_spell_id);
    }

    void finish_stall_episode(double ended_at, uint32_t next_spell_id) {
        if (!stall_active_) return;
        stall_active_ = false;
        const double duration = std::max(0.0, ended_at - stall_started_at_);
        const char* reason = stall_classification();

        if (telemetry_combat_active_) {
            ++telemetry_.stall_episodes;
            telemetry_.stall_total_seconds += duration;
            telemetry_.stall_max_seconds = std::max(telemetry_.stall_max_seconds, duration);
            if (duration > 0.25) ++telemetry_.stall_over_250ms;
            if (duration > 0.50) ++telemetry_.stall_over_500ms;
            if (duration > 1.00) ++telemetry_.stall_over_1000ms;
            if (stall_begin_cast_tail_) ++telemetry_.stall_cast_tail;
            else if (stall_begin_pending_) ++telemetry_.stall_pending;
            else if (stall_begin_suppressed_spell_ != 0) ++telemetry_.stall_suppression;
            else if (stall_begin_out_of_range_) ++telemetry_.stall_out_of_range;
            else ++telemetry_.stall_apl_no_action;
        }

        if (debug_diagnostics_) {
            queue_damage_debug("STALL_END duration=" + format_seconds(duration) +
                " next_spell=" + std::to_string(next_spell_id) +
                " reason=" + reason);
        }
        stall_begin_pending_ = false;
        stall_begin_cast_tail_ = false;
        stall_begin_moving_ = false;
        stall_begin_pending_spell_ = 0;
        stall_begin_suppressed_spell_ = 0;
        stall_begin_out_of_range_ = false;
    }

    // -------------------------------------------------------------------------
    // DAMAGE DISPATCH RECONCILIATION
    // -------------------------------------------------------------------------
    void clear_damage_dispatch_pending() {
        damage_dispatch_.spell_id = 0;
        damage_dispatch_.target_guid.clear();
        damage_dispatch_.dispatched_at = -999.0;
        damage_dispatch_.cast_started_at = -999.0;
        damage_dispatch_.pending = false;
        damage_dispatch_.queue_grace = 0.0;
        damage_dispatch_.queued_send = false;
    }

    // -------------------------------------------------------------------------
    // SPELL QUEUE WINDOW
    // -------------------------------------------------------------------------
    // Titan 23.3 exposes the queue window through the optional argument on the
    // casting queries plus get_rotation_spell_queue_window(). Passing true means
    // "treat the tail of my own cast as free", which is Titan asking for the
    // next button early. Strict state is the same query with false.
    static bool player_cast_active_strict(const rotation_api::IRotationAPI& api) {
        return api.unit_is_casting_or_channeling("player", false);
    }

    static bool player_free_for_queue(const rotation_api::IRotationAPI& api) {
        return !api.unit_is_casting_or_channeling("player", true);
    }

    static bool in_spell_queue_window(const rotation_api::IRotationAPI& api) {
        return player_cast_active_strict(api) && player_free_for_queue(api);
    }

    // Remaining time on the player's own cast, clamped to a sane bound so a
    // stale CastInfo can never grant an unbounded confirmation extension.
    static double player_cast_remaining(const rotation_api::IRotationAPI& api) {
        const rotation_api::CastInfo info = current_cast_info(api, "player");
        if (!info.is_active()) return 0.0;
        const double remaining = info.get_remaining(api.get_game_time());
        if (!(remaining > 0.0)) return 0.0;
        return std::min(remaining, kQueueGraceMax);
    }

    // The pending dispatch became a real cast that is now inside its queue
    // window. Exactly one ordinary hardcast filler may be sent into it.
    bool queue_release_available(const rotation_api::IRotationAPI& api, uint32_t spell_id) const {
        if (!queue_window_casting_) return false;
        if (!damage_dispatch_.pending) return false;
        if (damage_dispatch_.cast_started_at <= 0.0) return false;
        if (queued_for_cast_at_ == damage_dispatch_.cast_started_at) return false;
        if (!in_spell_queue_window(api)) return false;
        const rotation_api::CastInfo info = current_cast_info(api, "player");
        if (!info.is_active()) return false;
        if (queued_prior_cast_start_ > 0.0 &&
            std::abs(info.get_start_time() - queued_prior_cast_start_) < 0.05)
        {
            return false;
        }
        if (!logical_spell_match(api, damage_dispatch_.spell_id, info.spell_id)) return false;
        return is_queueable_filler(api, spell_id);
    }

    // Only the two ordinary hardcast fillers. Spenders, ground targets, and
    // setup actions keep the strict one-button-at-a-time model so no duplicate
    // Earthquake, Elemental Blast, or cooldown can ever be queued.
    bool is_queueable_filler(const rotation_api::IRotationAPI& api, uint32_t spell_id) const {
        if (spell_id == 0) return false;
        return (spellbook_.lightning_bolt != 0 &&
                logical_spell_match(api, spellbook_.lightning_bolt, spell_id)) ||
               (spellbook_.chain_lightning != 0 &&
                logical_spell_match(api, spellbook_.chain_lightning, spell_id));
    }

    // Hands the dispatcher over from the finishing cast to the queued button.
    // The finishing cast still owns its own success event, so that event is
    // consumed once instead of confirming the newly queued dispatch.
    void release_dispatcher_for_queue(const rotation_api::IRotationAPI& api) {
        const double now = api.get_game_time();
        queued_for_cast_at_ = damage_dispatch_.cast_started_at;
        queued_prior_cast_start_ = current_cast_info(api, "player").get_start_time();
        expected_prior_success_spell_ = damage_dispatch_.spell_id;
        expected_prior_success_until_ = now + kPriorSuccessGrace;
        if (telemetry_combat_active_) ++telemetry_.queue_releases;
        if (debug_diagnostics_) {
            queue_damage_debug("QUEUE_RELEASE cast=" +
                std::to_string(damage_dispatch_.spell_id) + '/' +
                api.get_spell_name(damage_dispatch_.spell_id) +
                " remaining=" + format_seconds(player_cast_remaining(api)) +
                " age=" + format_seconds(now - damage_dispatch_.dispatched_at));
        }
        clear_damage_dispatch_pending();
    }

    void clear_damage_dispatch_suppression() {
        damage_dispatch_.suppressed_spell_id = 0;
        damage_dispatch_.suppressed_target_guid.clear();
        damage_dispatch_.suppressed_until = -999.0;
    }

    void clear_damage_dispatch_state() {
        damage_dispatch_ = {};
        damage_debug_messages_.clear();
        last_damage_dispatch_log_spell_ = 0;
        last_damage_dispatch_log_time_ = -999.0;
        last_damage_suppressed_log_time_ = -999.0;
        last_unmatched_success_log_time_ = -999.0;
        maelstrom_packet_ = {};
        maelstrom_request_ = {};
    }

    // One generic identity test. Titan can report a runtime/override ID that
    // differs from the ID the rotation dispatched, so exact equality alone
    // produces false escapes.
    bool logical_spell_match(const rotation_api::IRotationAPI& api,
                             uint32_t left,
                             uint32_t right) const
    {
        if (left == 0 || right == 0) return false;
        if (left == right) return true;
        if (api.get_override_spell(left) == right) return true;
        if (api.get_override_spell(right) == left) return true;
        const std::string left_name = api.get_spell_name(left);
        if (left_name.empty()) return false;
        const std::string right_name = api.get_spell_name(right);
        if (right_name.empty()) return false;
        return same_name(left_name, right_name);
    }

    static std::string format_seconds(double seconds) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(2) << seconds;
        return out.str();
    }

    void queue_damage_debug(std::string message) {
        if (!debug_diagnostics_) return;
        if (damage_debug_messages_.size() >= 6) return;
        damage_debug_messages_.push_back(std::move(message));
    }

    // -------------------------------------------------------------------------
    // MAELSTROM OBSERVATION (DIAGNOSTIC ONLY)
    // -------------------------------------------------------------------------
    bool is_builder_spell(const rotation_api::IRotationAPI& api, uint32_t spell_id) const {
        const uint32_t builders[] = {
            spellbook_.lightning_bolt, spellbook_.chain_lightning,
            spellbook_.lava_burst, spellbook_.flame_shock,
            spellbook_.frost_shock, spellbook_.tempest,
            spellbook_.voltaic_blaze
        };
        for (uint32_t candidate : builders) {
            if (candidate != 0 && logical_spell_match(api, candidate, spell_id)) return true;
        }
        return false;
    }

    static bool has_lust_buff(const rotation_api::IRotationAPI& api) {
        static const char* names[] = {
            "Bloodlust", "Heroism", "Time Warp", "Fury of the Aspects",
            "Primal Rage", "Ancient Hysteria", "Drums of Rage"
        };
        for (const char* name : names) {
            if (has_buff_name(api, "player", name)) return true;
        }
        return false;
    }

    // Requests one resource packet when a rotational damage spell reports
    // success, which is where the game actually grants or spends Maelstrom.
    // Resource movement is measured, never assumed, and never feeds back into
    // action selection.
    void note_resolved_damage_action(const rotation_api::IRotationAPI& api, uint32_t spell_id) {
        if (spell_id == 0) return;
        if (!is_rotation_damage_spell_id(spell_id) &&
            !is_rotation_damage_spell_name(api.get_spell_name(spell_id)))
        {
            return;
        }

        // Two resolutions before the observer ran: neither owner is provable.
        if (maelstrom_request_.pending) {
            maelstrom_request_.ambiguous = true;
            maelstrom_request_.spell_id = spell_id;
            return;
        }

        maelstrom_request_.pending = true;
        maelstrom_request_.ambiguous = false;
        maelstrom_request_.spell_id = spell_id;
        maelstrom_request_.enemies = last_snapshot_enemies_;
        maelstrom_request_.stormkeeper = has_buff_name(api, "player", "Stormkeeper");
        maelstrom_request_.ancestral_swiftness =
            has_buff_name(api, "player", "Ancestral Swiftness");
        maelstrom_request_.ascendance = has_buff_name(api, "player", "Ascendance");
        maelstrom_request_.lust = has_lust_buff(api);
    }

    BuilderBucket builder_bucket(const rotation_api::IRotationAPI& api,
                                 uint32_t spell_id) const
    {
        if (spellbook_.lightning_bolt != 0 &&
            logical_spell_match(api, spellbook_.lightning_bolt, spell_id))
        {
            return BuilderBucket::LightningBolt;
        }
        if (spellbook_.chain_lightning != 0 &&
            logical_spell_match(api, spellbook_.chain_lightning, spell_id))
        {
            return BuilderBucket::ChainLightning;
        }
        if (spellbook_.lava_burst != 0 &&
            logical_spell_match(api, spellbook_.lava_burst, spell_id))
        {
            return BuilderBucket::LavaBurst;
        }
        if (spellbook_.voltaic_blaze != 0 &&
            logical_spell_match(api, spellbook_.voltaic_blaze, spell_id))
        {
            return BuilderBucket::VoltaicBlaze;
        }
        if (spellbook_.tempest != 0 &&
            logical_spell_match(api, spellbook_.tempest, spell_id))
        {
            return BuilderBucket::Tempest;
        }
        return BuilderBucket::Other;
    }

    static const char* builder_bucket_name(BuilderBucket bucket) {
        switch (bucket) {
            case BuilderBucket::LightningBolt: return "LB";
            case BuilderBucket::ChainLightning: return "CL";
            case BuilderBucket::LavaBurst: return "LVB";
            case BuilderBucket::VoltaicBlaze: return "VB";
            case BuilderBucket::Tempest: return "TEMPEST";
            case BuilderBucket::Other: return "OTHER";
            case BuilderBucket::Count: break;
        }
        return "OTHER";
    }

    static const char* packet_context_name(PacketContext context) {
        switch (context) {
            case PacketContext::Normal: return "normal";
            case PacketContext::Stormkeeper: return "sk";
            case PacketContext::AncestralSwiftness: return "as";
            case PacketContext::Ascendance: return "asc";
            case PacketContext::Lust: return "lust";
            case PacketContext::Count: break;
        }
        return "normal";
    }

    static int enemy_bucket_index(int enemies) {
        return std::clamp(enemies, 1, kEnemyBuckets) - 1;
    }

    static PacketContext packet_context_of(const MaelstromPacket& packet) {
        if (packet.stormkeeper) return PacketContext::Stormkeeper;
        if (packet.ancestral_swiftness) return PacketContext::AncestralSwiftness;
        if (packet.ascendance) return PacketContext::Ascendance;
        if (packet.lust) return PacketContext::Lust;
        return PacketContext::Normal;
    }

    // Only quiet-closed, unambiguous, uncapped builder packets describe real
    // generation. Structurally clean but capped packets are right-censored.
    void record_maelstrom_packet(const rotation_api::IRotationAPI& api,
                                 const MaelstromPacket& packet,
                                 bool clean,
                                 bool learnable)
    {
        if (!telemetry_combat_active_) return;
        if (!is_builder_spell(api, packet.spell_id)) return;

        const int bucket = static_cast<int>(builder_bucket(api, packet.spell_id));
        const int enemies = enemy_bucket_index(packet.enemies);
        if (!clean) {
            ++packet_ambiguous_by_enemies_[bucket][enemies];
            return;
        }
        if (!learnable) return;

        ++telemetry_.builder_resolved;
        if (packet.positive_total > 0) {
            ++telemetry_.builder_positive_delta;
            telemetry_.builder_gain_total += packet.positive_total;
            telemetry_.packet_first_sum += packet.first_positive;
            telemetry_.packet_total_sum += packet.positive_total;
            packet_by_enemies_[bucket][enemies].add(packet.positive_total);
            packet_by_context_[bucket][static_cast<int>(packet_context_of(packet))]
                .add(packet.positive_total);
        }

        if (bucket == static_cast<int>(BuilderBucket::LightningBolt)) {
            ++telemetry_.lightning_bolt_resolved;
            if (packet.positive_total > 0) {
                telemetry_.lightning_bolt_gain += packet.positive_total;
            }
        } else if (bucket == static_cast<int>(BuilderBucket::ChainLightning)) {
            ++telemetry_.chain_lightning_resolved;
            if (packet.positive_total > 0) {
                telemetry_.chain_lightning_gain += packet.positive_total;
                if (packet.stormkeeper) {
                    telemetry_.chain_lightning_gain_stormkeeper += packet.positive_total;
                } else if (packet.ancestral_swiftness) {
                    telemetry_.chain_lightning_gain_swiftness += packet.positive_total;
                } else {
                    telemetry_.chain_lightning_gain_normal += packet.positive_total;
                }
            }
        }
    }

    void apply_maelstrom_delta(int current, double now) {
        if (!maelstrom_packet_.active) return;
        const int delta = current - maelstrom_packet_.last;
        if (delta == 0) return;
        if (delta > 0) {
            maelstrom_packet_.positive_total += delta;
            if (maelstrom_packet_.first_positive == 0) {
                maelstrom_packet_.first_positive = delta;
            }
        } else {
            maelstrom_packet_.negative_total += delta;
        }
        maelstrom_packet_.last = current;
        maelstrom_packet_.last_change_at = now;
        ++maelstrom_packet_.changes;
    }

    void close_maelstrom_packet(const RotationContext& context,
                                int current,
                                double now,
                                const char* close_reason,
                                bool timed_out,
                                bool overlapped)
    {
        const auto& api = context.api();
        MaelstromPacket packet = maelstrom_packet_;
        maelstrom_packet_ = {};
        if (!packet.active) return;

        // A builder that also lost resource, or a spender that also gained it,
        // shared its window with something else.
        const bool builder = is_builder_spell(api, packet.spell_id);
        if (builder && packet.negative_total < 0) packet.ambiguous = true;
        if (!builder && packet.positive_total > 0) packet.ambiguous = true;
        if (overlapped) packet.ambiguous = true;

        const bool clean = !packet.ambiguous && !timed_out && !overlapped;
        const int maelstrom_max = std::max(1, api.get_player_power_max("maelstrom"));
        const bool capped = builder && packet.positive_total > 0 && current >= maelstrom_max;
        const bool learnable = builder && clean && !capped;

        if (telemetry_combat_active_) {
            ++telemetry_.maelstrom_packets_total;
            if (clean) ++telemetry_.maelstrom_packets_clean;
            if (packet.ambiguous) ++telemetry_.maelstrom_packets_ambiguous;
            if (timed_out) ++telemetry_.maelstrom_packets_timeout;
            if (overlapped) ++telemetry_.maelstrom_packets_overlap;
            if (capped) ++telemetry_.maelstrom_packets_capped;
            if (learnable) ++telemetry_.maelstrom_packets_learnable;
        }
        record_maelstrom_packet(api, packet, clean, learnable);

        if (!debug_diagnostics_) return;
        if (packet.changes == 0) return;

        const char* quality = "clean_spender";
        if (overlapped) quality = "overlap";
        else if (timed_out) quality = "timeout";
        else if (packet.ambiguous) quality = "ambiguous";
        else if (capped) quality = "capped";
        else if (builder) quality = "full_clean";

        std::ostringstream out;
        out << "MAELSTROM_PACKET spell=" << packet.spell_id << '/'
            << api.get_spell_name(packet.spell_id)
            << " start=" << packet.start
            << " end=" << current
            << " first=" << packet.first_positive
            << " positive_total=" << packet.positive_total
            << " negative_total=" << packet.negative_total
            << " enemies=" << packet.enemies
            << " stormkeeper=" << (packet.stormkeeper ? 1 : 0)
            << " ancestral_swiftness=" << (packet.ancestral_swiftness ? 1 : 0)
            << " ascendance=" << (packet.ascendance ? 1 : 0)
            << " lust=" << (packet.lust ? 1 : 0)
            << " duration=" << format_seconds(now - packet.opened_at)
            << " changes=" << packet.changes
            << " ambiguous=" << (packet.ambiguous ? 1 : 0)
            << " capped=" << (capped ? 1 : 0)
            << " quality=" << quality
            << " close=" << close_reason;
        context.log(out.str());
    }

    // The baseline is the previous sample, not the current one, so resource that
    // arrived between the two samples still belongs to the spell that resolved.
    void open_maelstrom_packet(int baseline, double now, bool inherited_ambiguity) {
        maelstrom_packet_ = {};
        maelstrom_packet_.active = true;
        maelstrom_packet_.ambiguous = inherited_ambiguity || maelstrom_request_.ambiguous;
        maelstrom_packet_.spell_id = maelstrom_request_.spell_id;
        maelstrom_packet_.start = baseline;
        maelstrom_packet_.last = baseline;
        maelstrom_packet_.opened_at = now;
        maelstrom_packet_.last_change_at = now;
        maelstrom_packet_.enemies = maelstrom_request_.enemies;
        maelstrom_packet_.stormkeeper = maelstrom_request_.stormkeeper;
        maelstrom_packet_.ancestral_swiftness = maelstrom_request_.ancestral_swiftness;
        maelstrom_packet_.ascendance = maelstrom_request_.ascendance;
        maelstrom_packet_.lust = maelstrom_request_.lust;
        maelstrom_request_ = {};
    }

    // Stage 1: a newly resolved action owns current-vs-previous. The old packet
    // is closed first so that delta cannot land on both owners.
    void observe_maelstrom(const RotationContext& context) {
        const auto& api = context.api();
        const int previous = last_observed_maelstrom_;
        const int current = api.get_player_power("maelstrom");
        const double now = api.get_game_time();
        const bool have_previous = previous >= 0;
        const int baseline = have_previous ? previous : current;

        if (maelstrom_request_.pending) {
            const bool overlapped = maelstrom_packet_.active;
            if (overlapped) {
                close_maelstrom_packet(context, baseline, now, "overlap", false, true);
            }
            open_maelstrom_packet(baseline, now, overlapped);
            apply_maelstrom_delta(current, now);
        } else {
            apply_maelstrom_delta(current, now);
        }

        if (maelstrom_packet_.active) {
            const bool quiet = maelstrom_packet_.changes > 0 &&
                (now - maelstrom_packet_.last_change_at) >= kMaelstromPacketQuiet;
            const bool expired =
                (now - maelstrom_packet_.opened_at) >= kMaelstromPacketMax;
            if (quiet) {
                close_maelstrom_packet(context, current, now, "quiet", false, false);
            } else if (expired) {
                close_maelstrom_packet(context, current, now, "timeout", true, false);
            }
        }

        last_observed_maelstrom_ = current;
        last_maelstrom_sample_time_ = now;
    }

    // -------------------------------------------------------------------------
    // DISPATCH EXECUTION EVIDENCE
    // -------------------------------------------------------------------------
    bool is_lava_burst_id(const rotation_api::IRotationAPI& api, uint32_t spell_id) const {
        return spellbook_.lava_burst != 0 &&
            logical_spell_match(api, spellbook_.lava_burst, spell_id);
    }

    // The binding Titan would press for this spell. Looked up only while
    // building a diagnostic line or when the breaker trips, never per tick.
    std::string spell_keybind_text(const rotation_api::IRotationAPI& api, uint32_t spell_id) const {
        if (spell_id == 0) return "<none>";
        if (const auto slot = api.find_spell_slot_by_id(spell_id)) {
            return slot->keybind.empty() ? "<unbound>" : slot->keybind;
        }
        return "<no_slot>";
    }

    static const char* dispatch_result_name(DispatchResult result) {
        switch (result) {
            case DispatchResult::SuccessHistory: return "success_history";
            case DispatchResult::CastStarted: return "cast_started";
            case DispatchResult::TargetChanged: return "target_changed";
            case DispatchResult::Escaped: return "escaped";
        }
        return "unknown";
    }

    // One compact line per interesting attempt: every escape, everything about
    // a spell already under suspicion, and every Lava Burst attempt.
    void log_dispatch_result(const rotation_api::IRotationAPI& api, DispatchResult result) {
        if (!debug_diagnostics_) return;
        const uint32_t spell_id = damage_dispatch_.spell_id;
        const bool lava_burst = is_lava_burst_id(api, spell_id);
        const bool suspect = execution_suspect_spell_ != 0 &&
            logical_spell_match(api, execution_suspect_spell_, spell_id);
        if (result != DispatchResult::Escaped && !lava_burst && !suspect) return;

        const double now = api.get_game_time();
        const rotation_api::CastInfo info = current_cast_info(api, "player");
        std::ostringstream out;
        out << (lava_burst ? "LVB_DISPATCH_RESULT" : "DISPATCH_RESULT")
            << " result=" << dispatch_result_name(result)
            << " spell=" << spell_id << '/' << api.get_spell_name(spell_id)
            << " key=" << spell_keybind_text(api, spell_id)
            << " age=" << format_seconds(now - damage_dispatch_.dispatched_at)
            << " gcd=" << format_seconds(dispatch_probe_.gcd_at_dispatch)
            << '/' << format_seconds(api.get_remaining_gcd())
            << " lock=" << (dispatch_probe_.locked_at_dispatch ? 1 : 0)
            << " cast=" << info.spell_id << '/'
            << (info.name.empty() ? std::string("<none>") : info.name)
            << " succ=" << (api.get_last_spellcast_succeeded_index() -
                            dispatch_probe_.success_index_at_dispatch)
            << " instant=" << (dispatch_probe_.instant_expected ? 1 : 0)
            << " charges=" << api.get_spell_current_charges(spell_id)
            << '/' << api.get_spell_max_charges(spell_id)
            << " castable=" << (api.can_cast_spell(spell_id) ? 1 : 0)
            << " escapes=" << (suspect ? execution_suspect_escapes_ : 0)
            // Queue-window context: what the game was busy with when the button
            // was sent, and how much extra confirmation time that bought.
            << " qwin=" << (dispatch_probe_.queue_window_at_dispatch ? 1 : 0)
            << " qbehind=" << dispatch_probe_.cast_at_dispatch
            << " qgrace=" << format_seconds(damage_dispatch_.queue_grace)
            << " cast_strict=" << (player_cast_active_strict(api) ? 1 : 0)
            << " cd=" << format_seconds(api.get_spell_cooldown_remaining(spell_id))
            << " guid=" << (damage_dispatch_.target_guid.empty()
                ? std::string("<none>") : damage_dispatch_.target_guid)
            << " same_target=" << ((api.unit_exists("target") &&
                api.get_unit_guid("target") == damage_dispatch_.target_guid) ? 1 : 0)
            << " suppress=" << ((result == DispatchResult::Escaped)
                ? format_seconds(kDamageDispatchEscapeWindow) : format_seconds(0.0));
        queue_damage_debug(out.str());
    }

    // Tracks consecutive unexplained failures per logical spell. Any genuine
    // execution evidence clears the suspicion immediately.
    void note_dispatch_resolution(const rotation_api::IRotationAPI& api, DispatchResult result) {
        const uint32_t spell_id = damage_dispatch_.spell_id;
        const bool lava_burst = is_lava_burst_id(api, spell_id);
        const bool suspect_match = execution_suspect_spell_ != 0 &&
            logical_spell_match(api, execution_suspect_spell_, spell_id);

        if (result == DispatchResult::Escaped) {
            if (suspect_match) {
                ++execution_suspect_escapes_;
            } else {
                execution_suspect_spell_ = spell_id;
                execution_suspect_escapes_ = 1;
            }
            if (telemetry_combat_active_ && lava_burst) ++telemetry_.lava_burst_escaped;
            log_dispatch_result(api, result);

            // Report only. A working spell must never be parked: runtime shows
            // escapes that were merely late queued executions.
            if (execution_suspect_escapes_ == kExecutionSuspectEscapes) {
                execution_break_message_ =
                    "EXECUTION_SUSPECT spell=" + std::to_string(spell_id) + "/" +
                    api.get_spell_name(spell_id) +
                    " key=" + spell_keybind_text(api, spell_id) +
                    " escapes=" + std::to_string(execution_suspect_escapes_) +
                    " reason=no cast start and no success event; check this binding"
                    " (diagnostic only, the spell is not blocked)";
            }
            return;
        }

        // Real evidence the spell reached the game.
        if (result == DispatchResult::SuccessHistory || result == DispatchResult::CastStarted) {
            if (telemetry_combat_active_ && lava_burst) {
                if (result == DispatchResult::SuccessHistory) ++telemetry_.lava_burst_confirmed;
                else ++telemetry_.lava_burst_cast_started;
            }
            if (suspect_match) {
                execution_suspect_spell_ = 0;
                execution_suspect_escapes_ = 0;
            }
        }
        log_dispatch_result(api, result);
    }

    // Success events are authoritative, which matters most for instants that
    // never expose a cast bar.
    void confirm_damage_dispatch(const rotation_api::IRotationAPI& api) {
        const double now = api.get_game_time();
        queue_damage_debug("DAMAGE_CONFIRMED spell=" +
            std::to_string(damage_dispatch_.spell_id) +
            " age=" + format_seconds(now - damage_dispatch_.dispatched_at) +
            " queued=" + (damage_dispatch_.queued_send ? "1" : "0"));
        if (telemetry_combat_active_) {
            ++telemetry_.damage_confirmed;
            if (damage_dispatch_.queued_send) ++telemetry_.queue_confirmed;
        }
        note_resolved_damage_action(api, damage_dispatch_.spell_id);
        note_dispatch_resolution(api, DispatchResult::SuccessHistory);
        clear_damage_dispatch_pending();
        damage_dispatch_.succeeded_at = now;
    }

    // Runs before the active-cast guard so a hardcast that genuinely started is
    // never classified as a failed dispatch.
    void reconcile_damage_dispatch(const rotation_api::IRotationAPI& api) {
        const double now = api.get_game_time();
        const std::string current_guid = api.unit_exists("target")
            ? api.get_unit_guid("target") : std::string{};

        // A new enemy must never inherit the previous enemy's failed attempt.
        if (!damage_dispatch_.suppressed_target_guid.empty() &&
            (damage_dispatch_.suppressed_target_guid != current_guid ||
             now >= damage_dispatch_.suppressed_until))
        {
            clear_damage_dispatch_suppression();
        }

        if (!damage_dispatch_.pending) return;

        if (damage_dispatch_.target_guid != current_guid) {
            note_dispatch_resolution(api, DispatchResult::TargetChanged);
            clear_damage_dispatch_pending();
            return;
        }

        const rotation_api::CastInfo info = current_cast_info(api, "player");

        // A button queued behind a same-name cast must not inherit that cast's
        // bar as its own start. Only a genuinely new cast instance counts.
        const bool finishing_prior_cast = queued_prior_cast_start_ > 0.0 &&
            info.is_active() &&
            std::abs(info.get_start_time() - queued_prior_cast_start_) < 0.05;
        if (!finishing_prior_cast) queued_prior_cast_start_ = -999.0;

        if (info.is_active() && !finishing_prior_cast &&
            logical_spell_match(api, damage_dispatch_.spell_id, info.spell_id))
        {
            if (damage_dispatch_.cast_started_at < 0.0) {
                damage_dispatch_.cast_started_at = now;
                if (telemetry_combat_active_) ++telemetry_.damage_started;
                queue_damage_debug("DAMAGE_CAST_STARTED spell=" +
                    std::to_string(damage_dispatch_.spell_id) +
                    " age=" + format_seconds(now - damage_dispatch_.dispatched_at));
                note_dispatch_resolution(api, DispatchResult::CastStarted);
            }
            return;
        }

        // The cast bar was seen at least once, so the attempt reached the game.
        // Its outcome belongs to the success history, not to failure recovery.
        if (damage_dispatch_.cast_started_at > 0.0) {
            clear_damage_dispatch_pending();
            return;
        }

        // A button sent inside a cast's queue window cannot be executed by the
        // game until that cast finishes, so its deadline includes that wait.
        const double deadline = kDamageDispatchConfirmWindow + damage_dispatch_.queue_grace;
        if ((now - damage_dispatch_.dispatched_at) < deadline) return;

        queue_damage_debug("DAMAGE_DISPATCH_ESCAPE spell=" +
            std::to_string(damage_dispatch_.spell_id) +
            " target=" + api.get_unit_name("target") +
            " age=" + format_seconds(now - damage_dispatch_.dispatched_at) +
            " queued=" + (damage_dispatch_.queued_send ? "1" : "0"));
        if (telemetry_combat_active_) {
            ++telemetry_.damage_escapes;
            if (damage_dispatch_.queued_send) ++telemetry_.queue_escapes;
        }
        note_dispatch_resolution(api, DispatchResult::Escaped);

        // Rare path: watch for the button executing after the deadline. That
        // proves the input reached the game and the escape was false.
        escape_watch_spell_ = damage_dispatch_.spell_id;
        escape_watch_dispatched_at_ = damage_dispatch_.dispatched_at;
        escape_watch_until_ = now + kEscapeWatchWindow;

        damage_dispatch_.suppressed_spell_id = damage_dispatch_.spell_id;
        damage_dispatch_.suppressed_target_guid = damage_dispatch_.target_guid;
        damage_dispatch_.suppressed_until = now + kDamageDispatchEscapeWindow;
        clear_damage_dispatch_pending();
    }

    // Suppression is exact: this logical spell, on this GUID, for this window.
    bool damage_dispatch_suppressed(const rotation_api::IRotationAPI& api,
                                    uint32_t spell_id,
                                    const std::string& unit) const
    {
        if (damage_dispatch_.suppressed_spell_id == 0) return false;
        if (!logical_spell_match(api, damage_dispatch_.suppressed_spell_id, spell_id)) {
            return false;
        }
        if (api.get_game_time() >= damage_dispatch_.suppressed_until) return false;
        const std::string guid = api.get_unit_guid(unit);
        return !guid.empty() && guid == damage_dispatch_.suppressed_target_guid;
    }

    // Surfaces what Titan actually reports when a rotational success cannot be
    // tied to the pending dispatch. Purely informational.
    void note_unmatched_success(const rotation_api::IRotationAPI& api,
                                uint32_t event_spell_id,
                                const std::string& event_name)
    {
        if (!debug_diagnostics_) return;
        const double now = api.get_game_time();
        if ((now - last_unmatched_success_log_time_) < 1.0) return;
        last_unmatched_success_log_time_ = now;
        queue_damage_debug("DAMAGE_UNMATCHED_SUCCESS pending=" +
            std::to_string(damage_dispatch_.spell_id) + "/" +
            api.get_spell_name(damage_dispatch_.spell_id) +
            " event=" + std::to_string(event_spell_id) + "/" + event_name);
    }

    void note_damage_suppressed(const rotation_api::IRotationAPI& api, uint32_t spell_id) {
        const double now = api.get_game_time();
        if (telemetry_combat_active_) ++telemetry_.damage_suppressed;
        if ((now - last_damage_suppressed_log_time_) < 0.75) return;
        last_damage_suppressed_log_time_ = now;
        queue_damage_debug("DAMAGE_SUPPRESSED spell=" + std::to_string(spell_id) +
            " target=" + api.get_unit_name("target") +
            " remaining=" + format_seconds(damage_dispatch_.suppressed_until - now));
    }

    void record_damage_dispatch(const rotation_api::IRotationAPI& api,
                                uint32_t spell_id,
                                const std::string& unit)
    {
        const double now = api.get_game_time();
        const std::string guid = api.get_unit_guid(unit);

        // Nothing may replace an unresolved dispatch, so the original
        // timestamp always survives and the confirmation window can expire.
        if (damage_dispatch_.pending) return;

        // A cast that is still finishing holds the game's spell queue, so the
        // confirmation deadline has to include the rest of that cast. Both cast
        // signals must agree: CastInfo alone can linger after a cast completed.
        const rotation_api::CastInfo active = current_cast_info(api, "player");
        const bool queue_window = in_spell_queue_window(api);
        const double queued_behind = (queue_window && active.is_active())
            ? std::max(0.0, std::min(active.get_remaining(now), kQueueGraceMax))
            : 0.0;

        damage_dispatch_.spell_id = spell_id;
        damage_dispatch_.target_guid = guid;
        damage_dispatch_.dispatched_at = now;
        damage_dispatch_.cast_started_at = -999.0;
        damage_dispatch_.pending = true;
        damage_dispatch_.queue_grace = queued_behind;
        damage_dispatch_.queued_send = queued_behind > 0.0;

        // Cheap scalars only. These describe the conditions the button was
        // sent under so a later escape can be explained instead of guessed.
        dispatch_probe_.gcd_at_dispatch = api.get_remaining_gcd();
        dispatch_probe_.locked_at_dispatch = last_global_lock_.locked;
        dispatch_probe_.instant_expected = has_buff_name(api, "player", "Lava Surge") &&
            is_lava_burst_id(api, spell_id);
        dispatch_probe_.queue_window_at_dispatch = queue_window;
        dispatch_probe_.cast_at_dispatch = active.spell_id;
        dispatch_probe_.success_index_at_dispatch = api.get_last_spellcast_succeeded_index();

        if (telemetry_combat_active_) {
            ++telemetry_.damage_dispatches;
            if (is_lava_burst_id(api, spell_id)) ++telemetry_.lava_burst_dispatched;
        }
        if (spell_id != last_damage_dispatch_log_spell_ ||
            (now - last_damage_dispatch_log_time_) >= 0.50)
        {
            last_damage_dispatch_log_spell_ = spell_id;
            last_damage_dispatch_log_time_ = now;
            queue_damage_debug("DAMAGE_DISPATCH spell=" + std::to_string(spell_id) +
                " target=" + api.get_unit_name(unit) +
                " guid=" + damage_dispatch_.target_guid);
        }
    }

    void note_duplicate_prevented(uint32_t key, double window_end) {
        if (!telemetry_combat_active_) return;
        if (key == last_dup_block_key_ &&
            std::abs(window_end - last_dup_block_window_end_) < 0.05)
        {
            return;
        }
        last_dup_block_key_ = key;
        last_dup_block_window_end_ = window_end;
        ++telemetry_.duplicate_dispatches_prevented;
    }

    bool setup_action_blocked(uint32_t key,
                              double pending_until,
                              double last_success_time,
                              double settle_window,
                              double last_dispatch_time,
                              double attempt_window,
                              double now,
                              bool otherwise_eligible)
    {
        const bool attempted = recently_attempted(last_dispatch_time, attempt_window, now);
        const bool pending = still_pending(pending_until, now);
        const bool settling = recently_succeeded(last_success_time, settle_window, now);
        if (!(attempted || pending || settling)) return false;
        if (otherwise_eligible) {
            double window_end = now;
            if (attempted) window_end = std::max(window_end, last_dispatch_time + attempt_window);
            if (pending) window_end = std::max(window_end, pending_until);
            if (settling) window_end = std::max(window_end, last_success_time + settle_window);
            note_duplicate_prevented(key, window_end);
        }
        return true;
    }

    RotationAction dispatch_player_setup(const rotation_api::IRotationAPI& api,
                                         uint32_t spell_id,
                                         double& pending_until,
                                         double last_success_time,
                                         double settle_window,
                                         double& last_dispatch_time,
                                         double attempt_window,
                                         const std::string& reason)
    {
        const double now = api.get_game_time();
        const bool eligible = can_cast(api, spell_id);
        if (setup_action_blocked(spell_id, pending_until, last_success_time, settle_window,
                last_dispatch_time, attempt_window, now, eligible))
        {
            return no_action("Setup settling");
        }
        if (!eligible) return no_action("Unavailable");
        pending_until = now + kSetupPendingWindow;
        last_dispatch_time = now;
        note_input_dispatch(api);
        return spell(spell_id, "player", reason);
    }

    RotationAction dispatch_voltaic_setup(const rotation_api::IRotationAPI& api,
                                          const CombatState& state,
                                          const std::string& reason)
    {
        const double now = api.get_game_time();
        const bool eligible = can_cast(api, spellbook_.voltaic_blaze);
        if (setup_action_blocked(spellbook_.voltaic_blaze, voltaic_setup_pending_until_,
                last_voltaic_blaze_success_time_, kVoltaicSetupSettle,
                last_voltaic_setup_dispatch_time_, kVoltaicSetupAttempt, now, eligible))
        {
            return no_action("Voltaic setup settling");
        }
        if (!eligible) return no_action("Unavailable");
        RotationAction action = cast_damage(api, spellbook_.voltaic_blaze, state.target, reason,
            true, false);
        if (!action.is_none()) {
            voltaic_setup_pending_until_ = now + kVoltaicSetupPendingWindow;
            last_voltaic_setup_dispatch_time_ = now;
            last_input_dispatch_time_ = now;
        }
        return action;
    }

    GlobalLockState compute_global_lock(const rotation_api::IRotationAPI& api) const {
        GlobalLockState lock;
        const double gcd = api.get_remaining_gcd();
        const uint32_t ids[] = {
            spellbook_.lightning_bolt,
            spellbook_.chain_lightning,
            spellbook_.lava_burst,
            spellbook_.voltaic_blaze != 0 ? spellbook_.voltaic_blaze : spellbook_.flame_shock
        };
        double short_cds[4] = {};
        int short_count = 0;
        for (uint32_t id : ids) {
            if (id == 0) continue;
            const double cd = api.get_spell_cooldown_remaining(id);
            if (cd > 0.08 && cd <= kSharedLockMax) {
                short_cds[short_count++] = cd;
            }
        }

        double cluster_remaining = 0.0;
        int cluster_size = 0;
        for (int i = 0; i < short_count; ++i) {
            int size = 0;
            double min_cd = short_cds[i];
            for (int j = 0; j < short_count; ++j) {
                if (std::abs(short_cds[j] - short_cds[i]) <= kSharedLockCluster) {
                    ++size;
                    min_cd = std::min(min_cd, short_cds[j]);
                }
            }
            if (size > cluster_size) {
                cluster_size = size;
                cluster_remaining = min_cd;
            }
        }

        const bool clustered = cluster_size >= 2;
        if (gcd > 0.05) {
            lock.locked = true;
            lock.remaining = gcd;
            if (clustered) lock.remaining = std::max(lock.remaining, cluster_remaining);
        }
        if (clustered && gcd <= 0.05) {
            lock.locked = true;
            lock.desync = true;
            lock.remaining = cluster_remaining;
        }
        return lock;
    }

    bool representative_damage_in_range(const rotation_api::IRotationAPI& api,
                                        const std::string& unit)
    {
        if (unit.empty()) return false;
        const uint32_t ids[] = {
            spellbook_.lightning_bolt,
            spellbook_.chain_lightning,
            spellbook_.lava_burst,
            spellbook_.voltaic_blaze,
            spellbook_.flame_shock
        };
        int known = 0;
        int in_range = 0;
        for (uint32_t id : ids) {
            if (id == 0) continue;
            ++known;
            if (damage_spell_in_range(api, id, unit, false)) ++in_range;
        }
        return known == 0 ? true : in_range > 0;
    }

    RotationAction combat_recovery_action(const RotationContext& ctx,
                                          const CombatState& state)
    {
        const auto& api = ctx.api();
        if (!automatic_target_recovery_) return no_action("Recovery disabled");
        if (api.is_mounted() || api.is_paused() || !api.is_in_combat_lockdown()) {
            target_unreachable_since_ = -999.0;
            recovery_watch_guid_.clear();
            return no_action("Recovery not in combat");
        }
        if (unit_looks_like_dummy(api, "target")) {
            target_unreachable_since_ = -999.0;
            recovery_watch_guid_.clear();
            return no_action("Dummy park keeps current target");
        }

        const bool exists = api.unit_exists("target");
        const bool dead = exists && api.unit_is_dead("target");
        const bool hostile = exists && current_target_is_hostile(api);
        const bool friendly_or_invalid = exists && !hostile && !dead;
        const bool dead_or_empty = !exists || dead;

        const std::string guid = exists ? api.get_unit_guid("target") : std::string{};
        if (guid != recovery_watch_guid_) {
            recovery_watch_guid_ = guid;
            target_unreachable_since_ = -999.0;
        }

        bool unreachable = false;
        if (hostile && !dead) {
            if (representative_damage_in_range(api, "target")) {
                target_unreachable_since_ = -999.0;
            } else {
                const double now = api.get_game_time();
                if (target_unreachable_since_ < 0.0) target_unreachable_since_ = now;
                unreachable = (now - target_unreachable_since_) >= kUnreachableDwell;
            }
        } else {
            target_unreachable_since_ = -999.0;
        }

        const bool immediate_recovery = (friendly_or_invalid || dead_or_empty) &&
            state.healthy_hostile_alternative;
        const bool needs_recovery = immediate_recovery ||
            (unreachable && state.healthy_hostile_alternative);
        if (!needs_recovery) {
            return no_action("No combat recovery");
        }

        const char* recovery_reason = "unreachable";
        if (friendly_or_invalid) recovery_reason = "friendly";
        else if (!exists) recovery_reason = "empty";
        else if (dead) recovery_reason = "dead";

        const double now = api.get_game_time();
        if (debug_diagnostics_ && (now - last_recovery_debug_time_) >= kTabRateLimit) {
            ctx.log(std::string("TARGET_RECOVERY ") + recovery_reason);
            last_recovery_debug_time_ = now;
        }

        if ((now - last_retarget_attempt_time_) < kTabRateLimit) {
            return no_action("Tab rate limited");
        }
        last_retarget_attempt_time_ = now;
        target_unreachable_since_ = -999.0;
        if (telemetry_combat_active_) ++telemetry_.target_recoveries;
        return keybind("TAB");
    }

    // Titan's foreground dispatcher already calls get_combat_action only with
    // a valid attackable combat target. UnitCanAttack and the synthesized LOS
    // fields can briefly be false while the new snapshot is settling, which
    // previously blocked the entire damage list even though Titan had a target.
    // Spell-specific range/LOS failures are still rejected by can_cast_spell()
    // and is_spell_in_range() at action construction time.
    static bool unit_looks_like_dummy(const rotation_api::IRotationAPI& api, const std::string& unit) {
        if (unit.empty() || !api.unit_exists(unit)) return false;
        if (api.unit_is_training_dummy(unit)) return true;
        const std::string name = api.get_unit_name(unit);
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("dummy") != std::string::npos;
    }

    static bool can_damage_unit(const rotation_api::IRotationAPI& api, const std::string& unit) {
        if (unit.empty() || !api.unit_exists(unit)) return false;
        // Dummies stay targetable at 0% and then regenerate. Keep them legal
        // even if the client briefly marks them dead.
        if (unit_looks_like_dummy(api, unit)) return true;
        if (api.unit_can_attack(unit) || api.unit_is_enemy(unit)) {
            return !api.unit_is_dead(unit);
        }
        return false;
    }

    // Titan's enemy/attack flags for the current target can read false for a
    // snapshot right after a target swap. Hostility therefore accepts three
    // independent proofs, in descending strength. Nameplate proof requires the
    // exact selected GUID to appear in this tick's strict hostile scan, so a
    // friendly unit or the player can never qualify.
    enum class HostileProof { None, Dummy, Flags, Nameplate, RecentGuid };

    HostileProof current_target_hostile_proof(const rotation_api::IRotationAPI& api) const {
        if (!api.unit_exists("target")) return HostileProof::None;
        if (unit_looks_like_dummy(api, "target")) return HostileProof::Dummy;
        if (api.unit_is_dead("target")) return HostileProof::None;
        if (api.unit_is_enemy("target") || api.unit_can_attack("target")) {
            return HostileProof::Flags;
        }
        const std::string guid = api.get_unit_guid("target");
        if (guid.empty()) return HostileProof::None;
        if (current_snapshot_target_hostile_ && guid == current_snapshot_target_guid_) {
            return HostileProof::Nameplate;
        }
        if (guid == last_hostile_target_guid_ &&
            (api.get_game_time() - last_hostile_target_seen_time_) <= kHostileGuidGrace)
        {
            return HostileProof::RecentGuid;
        }
        return HostileProof::None;
    }

    bool current_target_is_hostile(const rotation_api::IRotationAPI& api) const {
        return current_target_hostile_proof(api) != HostileProof::None;
    }

    // Only the current target gets the resilient path. Nameplate enumeration
    // and every other unit keep the strict guard.
    bool can_damage_action_target(const rotation_api::IRotationAPI& api,
                                  const std::string& unit) const
    {
        if (unit != "target") return can_damage_unit(api, unit);
        return current_target_is_hostile(api);
    }

    void note_target_continuity(const rotation_api::IRotationAPI& api, const char* proof) {
        if (!debug_diagnostics_) return;
        const double now = api.get_game_time();
        if ((now - last_target_continuity_debug_time_) < 0.75) return;
        last_target_continuity_debug_time_ = now;
        target_continuity_message_ = std::string("TARGET_CONTINUITY ") + proof +
            " target=" + api.get_unit_name("target");
    }

    std::string current_damage_target(const rotation_api::IRotationAPI& api) {
        const HostileProof proof = current_target_hostile_proof(api);
        if (proof == HostileProof::None) return {};

        // Flag and nameplate proofs are real confirmations, so they refresh the
        // same-GUID grace. The grace itself must never extend itself.
        if (proof != HostileProof::RecentGuid) {
            last_hostile_target_guid_ = api.get_unit_guid("target");
            last_hostile_target_seen_time_ = api.get_game_time();
        }
        if (proof == HostileProof::Nameplate) note_target_continuity(api, "nameplate");
        else if (proof == HostileProof::RecentGuid) note_target_continuity(api, "recent_guid");
        return "target";
    }

    // Equivalent friendly-unit guard used for curse cleansing/self-healing.
    static bool can_heal_unit(const rotation_api::IRotationAPI& api, const std::string& unit) {
        if (unit.empty() || !api.unit_exists(unit) || api.unit_is_dead_or_ghost(unit)) return false;
        if (!api.unit_in_range_for_healing(unit)) return false;
        return api.get_unit_line_of_sight(unit);
    }

    // Produces unique player + party/raid units for utility scans. Raid frames
    // normally include the player's raidN token, so GUID de-duplication avoids
    // counting the same poisoned/injured player twice.
    static std::vector<std::string> friendly_units(const rotation_api::IRotationAPI& api) {
        std::vector<std::string> units;
        std::unordered_set<std::string> seen_guids;
        const auto add_unit = [&](const std::string& unit) {
            if (!api.unit_exists(unit)) return;
            const std::string guid = api.get_unit_guid(unit);
            if (!guid.empty() && !seen_guids.insert(guid).second) return;
            units.push_back(unit);
        };

        add_unit("player");
        if (api.is_in_raid()) {
            for (int i = 1; i <= 40; ++i) {
                add_unit("raid" + std::to_string(i));
            }
        } else if (api.is_in_group()) {
            for (int i = 1; i <= 4; ++i) {
                add_unit("party" + std::to_string(i));
            }
        }
        return units;
    }

    // Target-first hostile scan used by interrupts, Purge, and configured
    // stop/totem lists. GUID de-duplication preserves target priority without
    // processing the same creature again through its nameplate token.
    static std::vector<std::string> hostile_units(const rotation_api::IRotationAPI& api,
                                                   double range)
    {
        std::vector<std::string> units;
        std::unordered_set<std::string> seen_guids;
        const auto add_unit = [&](const std::string& unit) {
            if (!can_damage_unit(api, unit)) return;
            if (!api.unit_is_enemy(unit) && !api.unit_can_attack(unit)) return;
            const std::string guid = api.get_unit_guid(unit);
            if (!guid.empty() && !seen_guids.insert(guid).second) return;
            units.push_back(unit);
        };

        add_unit("target");
        for (const auto& enemy : api.get_nameplates_in_range(range, true, false)) {
            add_unit(enemy.unit_token);
        }
        return units;
    }

    // Name-based aura helpers avoid depending on unstable Midnight aura IDs.
    static bool has_buff_name(const rotation_api::IRotationAPI& api,
                              const std::string& unit,
                              const std::string& name,
                              bool player_only = false)
    {
        for (const auto& aura : api.get_buffs(unit, false)) {
            if (player_only && !aura.is_from_player_or_pet()) continue;
            if (same_name(aura.name, name)) return true;
        }
        return false;
    }

    static bool has_buff_containing(const rotation_api::IRotationAPI& api,
                                    const std::string& unit,
                                    const std::string& text)
    {
        for (const auto& aura : api.get_buffs(unit, false)) {
            if (contains_name(aura.name, text)) return true;
        }
        return false;
    }

    // Use the stable spell/aura ID for timing. If a hotfix hides the numeric
    // aura but leaves its visible name, 999 means "active, exact time unknown"
    // and safely disables only the optional last-seconds refresh optimization.
    static double buff_remaining_by_name(const rotation_api::IRotationAPI& api,
                                         const std::string& unit,
                                         const std::string& name,
                                         uint32_t spell_id)
    {
        if (spell_id != 0) {
            const double remaining = api.get_buff_remaining(unit, spell_id, false);
            if (remaining > 0.0) return remaining;
        }
        return has_buff_name(api, unit, name) ? 999.0 : 0.0;
    }

    // Reads stack count from the runtime-resolved spell/aura ID. If a hidden
    // aura exposes only its name, presence still counts as one usable charge.
    // Keeping this helper ID-based avoids an expensive spell-database search on
    // every combat decision tick.
    static uint32_t buff_stacks_by_name(const rotation_api::IRotationAPI& api,
                                        const std::string& unit,
                                        const std::string& name,
                                        uint32_t fallback_spell_id)
    {
        uint32_t stacks = 0;
        if (fallback_spell_id != 0) {
            stacks = std::max(stacks, api.get_buff_stacks(unit, fallback_spell_id, false));
        }

        // Some hidden auras report presence but not a public stack count. Treat
        // that case as one charge so the normal (non-cap-protection) rule works.
        if (stacks == 0 && has_buff_name(api, unit, name)) stacks = 1;
        return stacks;
    }

    static bool has_debuff_name(const rotation_api::IRotationAPI& api,
                                const std::string& unit,
                                const std::string& name,
                                bool player_only = false)
    {
        for (const auto& aura : api.get_debuffs(unit, false)) {
            if (player_only && !aura.is_from_player_or_pet()) continue;
            if (same_name(aura.name, name)) return true;
        }
        return false;
    }

    static bool has_talent_name(const rotation_api::IRotationAPI& api, const std::string& name) {
        for (const auto& node : api.get_active_talents()) {
            if (node.active_rank == 0 || node.active_entry.spell_id == 0) continue;
            if (same_name(api.get_spell_name(node.active_entry.spell_id), name)) return true;
        }
        return false;
    }

    static std::string hero_tree_name(HeroTree tree) {
        switch (tree) {
            case HeroTree::Farseer: return "Farseer";
            case HeroTree::Stormbringer: return "Stormbringer";
            default: return "Unknown / generic fallback";
        }
    }

    // Manual mode wins. Auto mode uses signature talents instead of hardcoded
    // subtree IDs, which makes the profile resilient to config/subtree ID churn.
    HeroTree detect_hero_tree(const rotation_api::IRotationAPI& api) const {
        if (hero_mode_ == 1) return HeroTree::Farseer;
        if (hero_mode_ == 2) return HeroTree::Stormbringer;

        const bool farseer_signature =
            has_talent_name(api, "Call of the Ancestors") ||
            has_talent_name(api, "Ancestral Swiftness") ||
            has_talent_name(api, "Ancient Fellowship") ||
            has_talent_name(api, "Routine Communication") ||
            has_talent_name(api, "Final Calling") ||
            spellbook_.ancestral_swiftness != 0;
        if (farseer_signature) return HeroTree::Farseer;

        const bool stormbringer_signature =
            has_talent_name(api, "Tempest") ||
            has_talent_name(api, "Awakening Storms") ||
            has_talent_name(api, "Rolling Thunder") ||
            has_talent_name(api, "Unlimited Power") ||
            spellbook_.tempest != 0;
        if (stormbringer_signature) return HeroTree::Stormbringer;

        // Read the numeric value as a future-proofing hook. The SDK currently
        // exposes no stable cross-patch mapping for these subtree IDs, so an
        // unfamiliar nonzero value deliberately falls back to the generic list.
        (void)api.get_active_hero_talent_spec();
        return HeroTree::Unknown;
    }

    static bool is_rotation_damage_spell_name(const std::string& name) {
        static const char* names[] = {
            "Lightning Bolt", "Chain Lightning", "Lava Burst", "Flame Shock",
            "Earth Shock", "Earthquake", "Elemental Blast", "Tempest",
            "Voltaic Blaze", "Frost Shock"
        };
        for (const char* candidate : names) {
            if (same_name(name, candidate)) return true;
        }
        return false;
    }

    bool is_rotation_damage_spell_id(uint32_t spell_id) const {
        if (spell_id == 0) return false;
        const uint32_t ids[] = {
            spellbook_.lightning_bolt, spellbook_.chain_lightning,
            spellbook_.lava_burst, spellbook_.flame_shock,
            spellbook_.earth_shock, spellbook_.earthquake,
            spellbook_.elemental_blast, spellbook_.tempest,
            spellbook_.voltaic_blaze, spellbook_.frost_shock
        };
        for (uint32_t candidate : ids) {
            if (candidate != 0 && spell_id == candidate) return true;
        }
        return spell_id == elemental_ids::LAVA_BURST;
    }

    // Consume UNIT_SPELLCAST_SUCCEEDED history. This prevents recommendations
    // from advancing opener state when a cast was rejected, interrupted, or
    // never reached the server.
    void update_cast_history(const rotation_api::IRotationAPI& api) {
        const uint32_t latest_index = api.get_last_spellcast_succeeded_index();
        if (latest_index < last_processed_success_index_) {
            // The SDK resets the event index after combat ends.
            last_processed_success_index_ = 0;
            last_damage_success_index_ = 0;
            last_successful_damage_spell_.clear();
            clear_damage_dispatch_state();
        }

        uint32_t highest_seen = last_processed_success_index_;
        for (const auto& event : api.get_last_spellcast_succeeded()) {
            highest_seen = std::max(highest_seen, event.index);
            if (event.index <= last_processed_success_index_ || event.unit != "player") continue;

            if (telemetry_combat_active_) {
                ++telemetry_.successful_casts;
                if (event.spell_id == spellbook_.earthquake) {
                    ++telemetry_.earthquakes;
                    ++telemetry_.spenders;
                } else if (event.spell_id == spellbook_.elemental_blast) {
                    ++telemetry_.elemental_blasts;
                    ++telemetry_.spenders;
                } else if (event.spell_id == spellbook_.earth_shock) {
                    ++telemetry_.spenders;
                } else if (event.spell_id == spellbook_.stormkeeper) {
                    ++telemetry_.stormkeepers;
                } else if (event.spell_id == spellbook_.ascendance) {
                    ++telemetry_.ascendances;
                }
                for (int slot = 1; slot <= 2; ++slot) {
                    const auto trinket = equipped_trinket(api, slot);
                    if (trinket && trinket->spell.spell_id == event.spell_id) {
                        ++telemetry_.trinkets;
                    }
                }
            }

            std::string name = api.get_spell_name(event.spell_id);
            if (event.spell_id == spellbook_.lava_burst ||
                event.spell_id == elemental_ids::LAVA_BURST)
            {
                // Canonicalize even if a localized/override spell name is late.
                name = "Lava Burst";
            }
            if (same_name(name, "Stormkeeper") || event.spell_id == spellbook_.stormkeeper) {
                last_stormkeeper_success_time_ = event.time;
                stormkeeper_pending_until_ = -999.0;
                if (opener_active_) opener_stormkeeper_done_ = true;
            } else if (same_name(name, "Ancestral Swiftness") ||
                       event.spell_id == spellbook_.ancestral_swiftness) {
                last_ancestral_swiftness_success_time_ = event.time;
                ancestral_swiftness_pending_until_ = -999.0;
                if (opener_active_) opener_swiftness_done_ = true;
            } else if (same_name(name, "Voltaic Blaze") || event.spell_id == spellbook_.voltaic_blaze) {
                last_voltaic_blaze_success_time_ = event.time;
                voltaic_setup_pending_until_ = -999.0;
                if (opener_active_) opener_voltaic_done_ = true;
            } else if (same_name(name, "Ascendance") || event.spell_id == spellbook_.ascendance) {
                last_ascendance_success_time_ = event.time;
                ascendance_pending_until_ = -999.0;
                if (opener_active_) opener_ascendance_done_ = true;
            }
            for (int slot = 1; slot <= 2; ++slot) {
                const auto trinket = equipped_trinket(api, slot);
                if (trinket && trinket->spell.spell_id == event.spell_id) {
                    if (slot == 1) {
                        last_trinket_1_success_time_ = event.time;
                        trinket_1_pending_until_ = -999.0;
                    } else {
                        last_trinket_2_success_time_ = event.time;
                        trinket_2_pending_until_ = -999.0;
                    }
                }
            }

            const bool rotational_damage_event =
                is_rotation_damage_spell_name(name) || is_rotation_damage_spell_id(event.spell_id);

            // An escaped button that executes late proves the input reached the
            // game and the escape was a confirmation-timing artifact.
            if (escape_watch_spell_ != 0) {
                if (event.time > escape_watch_until_) {
                    escape_watch_spell_ = 0;
                } else if (logical_spell_match(api, escape_watch_spell_, event.spell_id)) {
                    if (telemetry_combat_active_) ++telemetry_.escape_late_success;
                    if (debug_diagnostics_) {
                        queue_damage_debug("ESCAPE_LATE_SUCCESS spell=" +
                            std::to_string(event.spell_id) + '/' + name +
                            " delay=" + format_seconds(event.time - escape_watch_dispatched_at_));
                    }
                    escape_watch_spell_ = 0;
                }
            }

            // The cast that handed the dispatcher to a queued button still owns
            // its own success event. Consume it once so it cannot confirm the
            // queued dispatch instead.
            bool consumed_prior_success = false;
            if (expected_prior_success_spell_ != 0) {
                if (event.time > expected_prior_success_until_) {
                    expected_prior_success_spell_ = 0;
                } else if (logical_spell_match(api, expected_prior_success_spell_, event.spell_id)) {
                    expected_prior_success_spell_ = 0;
                    consumed_prior_success = true;
                    if (rotational_damage_event) note_resolved_damage_action(api, event.spell_id);
                }
            }

            if (consumed_prior_success) {
                // Sequencing and Maelstrom attribution already handled above.
            } else if (damage_dispatch_.pending) {
                if (logical_spell_match(api, damage_dispatch_.spell_id, event.spell_id)) {
                    confirm_damage_dispatch(api);
                } else if (rotational_damage_event) {
                    // Diagnostic only. Titan reporting an unfamiliar ID must not
                    // suppress or escape anything by itself.
                    note_unmatched_success(api, event.spell_id, name);
                    note_resolved_damage_action(api, event.spell_id);
                }
            } else if (rotational_damage_event) {
                note_resolved_damage_action(api, event.spell_id);
            }

            if (damage_dispatch_.suppressed_spell_id != 0 &&
                logical_spell_match(api, damage_dispatch_.suppressed_spell_id, event.spell_id))
            {
                clear_damage_dispatch_suppression();
            }

            if (rotational_damage_event &&
                event.index > last_damage_success_index_)
            {
                last_damage_success_index_ = event.index;
                last_successful_damage_spell_ = name;
            }
        }
        last_processed_success_index_ = highest_seen;
    }

    void reset_packet_statistics() {
        for (int spell = 0; spell < kBuilderBuckets; ++spell) {
            for (int enemies = 0; enemies < kEnemyBuckets; ++enemies) {
                packet_by_enemies_[spell][enemies] = {};
                packet_ambiguous_by_enemies_[spell][enemies] = 0;
            }
            for (int flavor = 0; flavor < kPacketContexts; ++flavor) {
                packet_by_context_[spell][flavor] = {};
            }
        }
    }

    // One compact line per populated bucket, emitted only at combat end.
    void log_packet_summary(const RotationContext& context) const {
        for (int spell = 0; spell < kBuilderBuckets; ++spell) {
            const char* spell_name = builder_bucket_name(static_cast<BuilderBucket>(spell));
            for (int enemies = 0; enemies < kEnemyBuckets; ++enemies) {
                const PacketStat& stat = packet_by_enemies_[spell][enemies];
                const int ambiguous = packet_ambiguous_by_enemies_[spell][enemies];
                if (stat.count == 0 && ambiguous == 0) continue;
                std::ostringstream out;
                out << "MS_PKT_" << spell_name << '_' << (enemies + 1)
                    << (enemies + 1 == kEnemyBuckets ? "plus" : "")
                    << " n=" << stat.count
                    << std::fixed << std::setprecision(1)
                    << " avg=" << stat.average()
                    << " min=" << stat.min
                    << " max=" << stat.max
                    << " ewma=" << stat.ewma
                    << " ambiguous=" << ambiguous;
                context.log(out.str());
            }
            for (int flavor = 0; flavor < kPacketContexts; ++flavor) {
                const PacketStat& stat = packet_by_context_[spell][flavor];
                if (stat.count == 0) continue;
                std::ostringstream out;
                out << "MS_PKT_" << spell_name << '_'
                    << packet_context_name(static_cast<PacketContext>(flavor))
                    << " n=" << stat.count
                    << std::fixed << std::setprecision(1)
                    << " avg=" << stat.average()
                    << " min=" << stat.min
                    << " max=" << stat.max
                    << " ewma=" << stat.ewma;
                context.log(out.str());
            }
        }
    }

    void update_combat_telemetry(const RotationContext& context) {
        const auto& api = context.api();
        const double now = api.get_game_time();
        const bool in_combat = api.is_in_combat_lockdown();

        if (in_combat && !telemetry_combat_active_) {
            telemetry_ = {};
            reset_packet_statistics();
            telemetry_.started_at = now;
            telemetry_.last_sample_at = now;
            telemetry_combat_active_ = true;
            return;
        }

        if (in_combat && telemetry_combat_active_) {
            const double dt = std::clamp(now - telemetry_.last_sample_at, 0.0, 0.25);
            telemetry_.last_sample_at = now;
            const bool hostile_target = can_damage_unit(api, "target") &&
                (api.unit_is_enemy("target") || api.unit_can_attack("target"));
            if (hostile_target && !api.unit_is_casting_or_channeling("player", true) &&
                !last_global_lock_.locked && api.get_remaining_gcd() <= 0.05)
            {
                telemetry_.gcd_idle_seconds += dt;
            }
            const int resource = api.get_player_power("maelstrom");
            const int maximum = std::max(1, api.get_player_power_max("maelstrom"));
            if (resource >= maximum - 5) telemetry_.near_cap_seconds += dt;
            return;
        }

        if (!in_combat && telemetry_combat_active_) {
            // An episode still open at combat end is counted up to its last
            // idle tick, never up to the moment combat dropped.
            if (stall_active_) finish_stall_episode(stall_last_tick_at_, 0);
            const double duration = std::max(0.0, now - telemetry_.started_at);
            if (duration >= 3.0) {
                std::ostringstream report;
                report << std::fixed << std::setprecision(1)
                    << "ELEMENTAL REPORT v2.3.7 duration=" << duration << "s"
                    << " casts=" << telemetry_.successful_casts
                    << " idle=" << telemetry_.gcd_idle_seconds << "s"
                    << " near_cap=" << telemetry_.near_cap_seconds << "s"
                    << " spenders=" << telemetry_.spenders
                    << " EQ=" << telemetry_.earthquakes
                    << " EQ_opp=" << telemetry_.earthquake_opportunities
                    << " EQ_cpm=" << std::fixed << std::setprecision(2)
                    << (duration > 0.0 ? telemetry_.earthquakes * 60.0 / duration : 0.0)
                    << std::setprecision(1)
                    << " EB=" << telemetry_.elemental_blasts
                    << " SK=" << telemetry_.stormkeepers
                    << " Asc=" << telemetry_.ascendances
                    << " trinkets=" << telemetry_.trinkets
                    << " retargets=" << telemetry_.target_recoveries
                    << " EQ_holds=" << telemetry_.earthquake_delays
                    << " forecast_saves=" << telemetry_.forecast_saves
                    << " dup_blocks=" << telemetry_.duplicate_dispatches_prevented
                    << " lvb_dispatch=" << telemetry_.lava_burst_dispatched
                    << " lvb_confirmed=" << telemetry_.lava_burst_confirmed
                    << " lvb_cast_started=" << telemetry_.lava_burst_cast_started
                    << " lvb_escaped=" << telemetry_.lava_burst_escaped
                    << " dmg_dispatches=" << telemetry_.damage_dispatches
                    << " dmg_started=" << telemetry_.damage_started
                    << " dmg_confirmed=" << telemetry_.damage_confirmed
                    << " dmg_escapes=" << telemetry_.damage_escapes
                    << " dmg_escapes_late_success=" << telemetry_.escape_late_success
                    << " dmg_suppressed=" << telemetry_.damage_suppressed
                    << " queue_releases=" << telemetry_.queue_releases
                    << " queue_confirmed=" << telemetry_.queue_confirmed
                    << " queue_escapes=" << telemetry_.queue_escapes
                    << " stall_episodes=" << telemetry_.stall_episodes
                    << " stall_total_seconds=" << std::setprecision(2)
                    << telemetry_.stall_total_seconds
                    << " stall_max_seconds=" << telemetry_.stall_max_seconds
                    << std::setprecision(1)
                    << " stall_over_250ms=" << telemetry_.stall_over_250ms
                    << " stall_over_500ms=" << telemetry_.stall_over_500ms
                    << " stall_over_1000ms=" << telemetry_.stall_over_1000ms
                    << " stall_cast_tail=" << telemetry_.stall_cast_tail
                    << " stall_pending=" << telemetry_.stall_pending
                    << " stall_suppression=" << telemetry_.stall_suppression
                    << " stall_out_of_range=" << telemetry_.stall_out_of_range
                    << " stall_apl_no_action=" << telemetry_.stall_apl_no_action
                    << " builder_resolved=" << telemetry_.builder_resolved
                    << " builder_positive_delta=" << telemetry_.builder_positive_delta
                    << " builder_gain_total=" << telemetry_.builder_gain_total
                    << " lb_resolved=" << telemetry_.lightning_bolt_resolved
                    << " lb_gain=" << telemetry_.lightning_bolt_gain
                    << " cl_resolved=" << telemetry_.chain_lightning_resolved
                    << " cl_gain=" << telemetry_.chain_lightning_gain
                    << " cl_gain_normal=" << telemetry_.chain_lightning_gain_normal
                    << " cl_gain_stormkeeper=" << telemetry_.chain_lightning_gain_stormkeeper
                    << " cl_gain_ancestral_swiftness=" << telemetry_.chain_lightning_gain_swiftness
                    << " trinket_sync_confirms=" << telemetry_.trinket_sync_confirms
                    << " trinket_sync_retries=" << telemetry_.trinket_sync_retries
                    << " trinket_sync_failures=" << telemetry_.trinket_sync_failures
                    << " maelstrom_packets_total=" << telemetry_.maelstrom_packets_total
                    << " maelstrom_packets_clean=" << telemetry_.maelstrom_packets_clean
                    << " maelstrom_packets_ambiguous=" << telemetry_.maelstrom_packets_ambiguous
                    << " maelstrom_packets_timeout=" << telemetry_.maelstrom_packets_timeout
                    << " maelstrom_packets_overlap=" << telemetry_.maelstrom_packets_overlap
                    << " maelstrom_packets_capped=" << telemetry_.maelstrom_packets_capped
                    << " maelstrom_packets_learnable=" << telemetry_.maelstrom_packets_learnable
                    << " packet_first_sum=" << telemetry_.packet_first_sum
                    << " packet_total_sum=" << telemetry_.packet_total_sum;
                context.log(report.str());
                log_packet_summary(context);
            }
            telemetry_combat_active_ = false;
            clear_major_trinket_sync();
            clear_damage_dispatch_state();
        }
    }

    bool last_damage_was_lava_burst() const {
        return same_name(last_successful_damage_spell_, "Lava Burst");
    }

    static bool id_in_any_named_list(const rotation_api::IRotationAPI& api,
                                     uint32_t id,
                                     std::initializer_list<const char*> names)
    {
        if (id == 0) return false;
        for (const char* name : names) {
            if (api.is_id_in_list(id, std::string(name))) return true;
        }
        return false;
    }

    // -------------------------------------------------------------------------
    // FLAME SHOCK / MULTI-DOT HELPERS
    // -------------------------------------------------------------------------
    // Prefer the numeric-ID lookup, then use the aura's visible name as backup.
    bool has_flame_shock(const rotation_api::IRotationAPI& api, const std::string& unit) const {
        if (spellbook_.flame_shock != 0 && api.has_debuff(unit, spellbook_.flame_shock, true)) return true;
        return has_debuff_name(api, unit, "Flame Shock", true);
    }

    // Returns 999 when Flame Shock exists but its exact remaining time cannot be
    // read. That safely prevents repeated recasts until the aura disappears.
    double flame_shock_remaining(const rotation_api::IRotationAPI& api, const std::string& unit) const {
        if (spellbook_.flame_shock != 0) {
            const double remaining = api.get_debuff_remaining(unit, spellbook_.flame_shock, true);
            if (remaining > 0.0) return remaining;
        }
        return has_flame_shock(api, unit) ? 999.0 : 0.0;
    }

    // True when missing or inside the configured pandemic refresh window.
    bool flame_shock_refreshable(const rotation_api::IRotationAPI& api, const std::string& unit) const {
        return !has_flame_shock(api, unit) || flame_shock_remaining(api, unit) <= flame_shock_refresh_;
    }

    // Counts player-applied copies on hostile nameplates in 40 yards.
    int count_debuff(const rotation_api::IRotationAPI& api, const std::string& name) const {
        int count = 0;
        for (const auto& enemy : api.get_nameplates_in_range(40.0, true, false)) {
            if (enemy.unit_token.empty() || !can_damage_unit(api, enemy.unit_token)) continue;
            if (has_debuff_name(api, enemy.unit_token, name, true)) ++count;
        }
        if (count == 0 && current_target_is_hostile(api) &&
            has_debuff_name(api, "target", name, true)) {
            count = 1;
        }
        return count;
    }

    // Titan 23.3 documents hostile SpellAction units as current-target casts;
    // arbitrary hostile nameplate tokens do not create an off-target binding.
    // Keep nameplate scans for encounter state, but only recommend executable
    // hostile actions against the user's current target.
    std::string best_flame_shock_target(const rotation_api::IRotationAPI& api,
                                         const CombatState& state,
                                         bool allow_spread) const
    {
        (void)allow_spread;
        if (can_damage_action_target(api, state.target) &&
            flame_shock_refreshable(api, state.target))
        {
            return state.target;
        }
        return "";
    }

    // SimC's Fire Elemental fade rule uses target_if=min:dot.flame_shock.remains.
    // Unlike best_flame_shock_target(), this deliberately selects an existing
    // DoT even outside pandemic range so the guardian can extend the refreshed
    // Flame Shock before it disappears.
    std::string lowest_flame_shock_target(const rotation_api::IRotationAPI& api,
                                          const CombatState& state) const
    {
        if (!can_damage_action_target(api, state.target) ||
            !has_flame_shock(api, state.target)) return "";
        if (spellbook_.flame_shock != 0 &&
            !api.is_spell_in_range(spellbook_.flame_shock, state.target)) return "";
        return state.target;
    }

    // Tempest applies Lightning Rod, so the guide explicitly sends it to an
    // enemy that does not already have the player's Rod. Among valid choices,
    // prefer the longest-lived unit so the newly applied debuff has value.
    std::string best_lightning_rod_target(const rotation_api::IRotationAPI& api,
                                          const CombatState& state) const
    {
        (void)api;
        return state.target;
    }

    // A live boss frame is strongest evidence; classification is the fallback.
    static bool unit_is_boss(const rotation_api::IRotationAPI& api, const std::string& unit) {
        if (api.unit_exists("boss1")) return true;
        const std::string classification = api.get_unit_classification(unit);
        return classification == "worldboss" || classification == "rareelite";
    }

    // Returns: 1=M+, 2=Raid, 3=Solo. A manual setting overrides detection.
    int effective_content_mode(const rotation_api::IRotationAPI& api) const {
        if (content_mode_ != 0) return content_mode_;
        if (api.is_mythic_plus_active()) return 1;
        if (api.is_in_raid() || api.get_instance_type() == "raid") return 2;
        return 3;
    }

    // -------------------------------------------------------------------------
    // BUILD THE ONE-TICK COMBAT SNAPSHOT
    // -------------------------------------------------------------------------
    CombatState build_state(const rotation_api::IRotationAPI& api) {
        CombatState state;
        const int mode = effective_content_mode(api);
        state.is_mplus = mode == 1;
        state.is_raid = mode == 2;
        state.mythic_plus_level = state.is_mplus
            ? static_cast<int>(api.get_mythic_plus_level()) : 0;
        state.hero_tree = detect_hero_tree(api);
        // The raw selection is recorded before validation so the hostile
        // nameplate scan below can vouch for a target whose enemy/attack flags
        // have not settled yet. Titan's secure foreground executor does not
        // translate arbitrary nameplate tokens into target changes, so the
        // rotation still never invents an executable fallback target.
        const std::string raw_target_guid = api.unit_exists("target")
            ? api.get_unit_guid("target") : std::string{};

        const auto nameplates = api.get_nameplates_in_range(40.0, true, false);

        current_snapshot_target_guid_ = raw_target_guid;
        current_snapshot_target_range_ = 0.0;
        current_snapshot_target_range_valid_ = false;
        current_snapshot_target_hostile_ = false;

        // Build a health-aware pull model. Enemies about to die remain valid
        // damage targets but stop inflating the smart cooldown pull size.
        double pack_health = 0.0;
        double pack_health_max = 0.0;
        double pack_range_sum = 0.0;
        int pack_range_samples = 0;
        state.np_raw = static_cast<int>(nameplates.size());
        for (const auto& enemy : nameplates) {
            const std::string unit = enemy.unit_token;
            if (unit.empty() || !can_damage_unit(api, unit)) continue;
            ++state.np_damageable;

            const bool hostile_flags = api.unit_is_enemy(unit) || api.unit_can_attack(unit);
            if (hostile_flags) ++state.np_hostile_flags;

            // can_damage_unit() deliberately keeps a recognized training dummy
            // legal when its unit state reads oddly, but the hostility test
            // below used to discard it again. The exception is re-applied here
            // for positively recognized dummies only. The check is skipped for
            // ordinary hostiles unless diagnostics asked for the tally.
            bool dummy = false;
            if (!hostile_flags || debug_diagnostics_) {
                dummy = unit_looks_like_dummy(api, unit);
                if (dummy) ++state.np_dummy;
            }
            if (!hostile_flags && !dummy) continue;

            ++state.enemies;
            ++state.np_effective;
            if (enemy.range.has_value()) {
                pack_range_sum += enemy.range.value();
                ++pack_range_samples;
            }
            const double hp_percent = enemy.hp_max > 0.0
                ? 100.0 * enemy.hp / enemy.hp_max
                : api.get_unit_health_percent(unit);
            if (enemy.hp_max > 0.0) {
                pack_health += std::max(0.0, enemy.hp);
                pack_health_max += enemy.hp_max;
            }

            const bool enough_ttd = enemy.time_to_death <= 0.0 ||
                enemy.time_to_death >= mplus_meaningful_enemy_ttd_;
            const bool enough_hp = hp_percent >= mplus_meaningful_enemy_hp_;
            if (enough_ttd && enough_hp) ++state.meaningful_enemies;

            const std::string enemy_guid = api.get_unit_guid(unit);
            const bool is_current_target = !raw_target_guid.empty() &&
                ((!enemy.guid.empty() && enemy.guid == raw_target_guid) ||
                 (!enemy_guid.empty() && enemy_guid == raw_target_guid));
            if (is_current_target) {
                current_snapshot_target_hostile_ = true;
                if (enemy.range.has_value()) {
                    current_snapshot_target_range_ = enemy.range.value();
                    current_snapshot_target_range_valid_ = true;
                }
            }
            if (!is_current_target && hp_percent > 0.1) {
                state.healthy_hostile_alternative = true;
            }

            if (api.is_nameplate_targeting_player(unit)) {
                ++state.enemies_targeting_player;
                const uint32_t cast_id = api.get_nameplate_cast_id(unit).value_or(0);
                const double remaining = api.get_nameplate_cast_remaining(unit);
                if (remaining > 0.0 &&
                    id_in_any_named_list(api, cast_id,
                        {"Defensive.Dangerous", "Defensive.Personal", "Elemental.Defensive.Dangerous"}) &&
                    remaining < state.dangerous_cast_remaining)
                {
                    state.dangerous_cast_targeting_player = true;
                    state.dangerous_cast_remaining = remaining;
                }
            }
        }

        // Titan's own count is sampled only while diagnostics are on. The
        // second sample drops the in-combat filter so a nameplate the scan
        // never returned can be told apart from one the filters removed.
        if (debug_diagnostics_) {
            state.np_count_api = static_cast<int>(api.get_nameplates_in_range_count(40.0, true, false));
            state.np_count_any = static_cast<int>(api.get_nameplates_in_range_count(40.0, false, false));
        }

        // Hostile-nameplate evidence for this tick is complete, so the current
        // selection can finally be validated against it.
        state.target = current_damage_target(api);

        if (!state.target.empty()) {
            state.enemies = std::max(1, state.enemies);
            state.meaningful_enemies = std::max(1, state.meaningful_enemies);
        }
        state.pack_health_percent = pack_health_max > 0.0
            ? std::clamp(100.0 * pack_health / pack_health_max, 0.0, 100.0)
            : 100.0;

        // Titan does not expose the tank's destination, so pack motion is
        // inferred from enemy-count churn, average range movement, and current
        // target speed. Earthquake is delayed only through the brief gather
        // phase; an unknown range snapshot degrades to "stable".
        const double now = api.get_game_time();
        const double average_range = pack_range_samples > 0
            ? pack_range_sum / static_cast<double>(pack_range_samples) : 0.0;
        const bool snapshot_changed = last_pack_snapshot_time_ < 0.0 ||
            state.enemies != last_pack_enemy_count_ ||
            (pack_range_samples > 0 && last_pack_average_range_ > 0.0 &&
             std::fabs(average_range - last_pack_average_range_) > 1.75) ||
            api.get_target_speed() > 1.0;
        if (snapshot_changed) pack_stable_since_ = now;
        if (pack_stable_since_ < 0.0) pack_stable_since_ = now;
        state.pack_stable_for = std::max(0.0, now - pack_stable_since_);
        state.melee_ratio = api.get_melee_ratio(8.0, 40.0);
        state.pack_stable = state.enemies < 3 || pack_range_samples == 0 ||
            state.pack_stable_for >= 0.75;
        last_pack_enemy_count_ = state.enemies;
        last_pack_average_range_ = average_range;
        last_pack_snapshot_time_ = now;

        // The main AoE toggle is authoritative for list selection and smart
        // multi-target cooldown qualification.
        if (!api.is_aoe_enabled()) {
            state.enemies = std::min(1, state.enemies);
            state.meaningful_enemies = std::min(1, state.meaningful_enemies);
        }
        state.use_aoe_list = api.is_aoe_enabled() &&
            state.enemies >= std::max(3, aoe_threshold_);

        // Resource/health/TTD data drives spenders, defensives, and smart CDs.
        state.maelstrom = api.get_player_power("maelstrom");
        state.maelstrom_max = std::max(1, api.get_player_power_max("maelstrom"));
        state.maelstrom_deficit = std::max(0, state.maelstrom_max - state.maelstrom);
        const int capped_targets = std::min(5, std::max(1, state.enemies));
        state.forecast_builder_gain = state.use_aoe_list
            ? capped_targets * (capped_targets + 4)
            : (has_buff_name(api, "player", "Stormkeeper") ? 20 : 8);
        state.forecast_overcap = state.maelstrom_deficit <= state.forecast_builder_gain;
        last_snapshot_enemies_ = state.enemies;
        state.player_hp = api.get_unit_health_percent("player");
        if (!state.target.empty()) {
            state.target_hp = api.get_unit_health_percent(state.target);
        }
        state.fight_ttd = api.get_highest_nameplate_ttd(true, false);
        if (state.fight_ttd <= 0.0) state.fight_ttd = 999.0;
        state.earthquake_safe = state.pack_stable || state.maelstrom_deficit <= 10 ||
            state.fight_ttd <= 4.0;
        state.is_boss = !state.target.empty() && unit_is_boss(api, state.target);
        state.training_dummy = !state.target.empty() && unit_looks_like_dummy(api, state.target);

        // Encounter events are trustworthy for timing, but only configured
        // spell lists define burst windows. Deadly timeline icons are safe for
        // proactive defensives without hardcoding raid encounter IDs.
        for (const auto& event : api.get_encounter_timeline_events()) {
            if (!event.is_valid()) continue;
            const double eta = api.time_until_next_event(event.id);
            if (eta <= 0.0 || eta > 12.0) continue;
            if ((event.icons & api.get_encounter_event_deadly_effect()) != 0U &&
                eta < state.encounter_danger_eta)
            {
                state.encounter_danger_eta = eta;
            }
            if (id_in_any_named_list(api, event.spell_id,
                    {"Elemental.Burst.Window", "Cooldown.Burst.Window"}) &&
                eta < state.encounter_burst_eta)
            {
                state.encounter_burst_eta = eta;
            }
        }
        state.encounter_danger_incoming = state.is_raid &&
            state.encounter_danger_eta <= 2.5;
        state.encounter_burst_hold = state.is_raid &&
            state.encounter_burst_eta > 3.0 && state.encounter_burst_eta <= 10.0;
        state.encounter_burst_go = state.is_raid && state.encounter_burst_eta <= 3.0;
        const bool bank_by_ttd = mplus_bank_maelstrom_ttd_ > 0.0 &&
            state.fight_ttd <= mplus_bank_maelstrom_ttd_;
        const bool bank_by_health = mplus_bank_maelstrom_pack_hp_ > 0.0 &&
            state.pack_health_percent <= mplus_bank_maelstrom_pack_hp_;
        state.bank_maelstrom = state.is_mplus && !state.is_boss && !state.training_dummy &&
            (bank_by_ttd || bank_by_health);
        if (state.training_dummy) {
            state.fight_ttd = 999.0;
            state.pack_health_percent = std::max(state.pack_health_percent, 100.0);
        }

        // Movement duration prevents Spiritwalker's Grace from firing for a
        // tiny sidestep when an instant spell would be sufficient.
        state.moving = api.is_player_moving();
        if (state.moving) {
            const double started = api.get_player_started_moving_time();
            state.movement_time = started > 0.0 ? std::max(0.0, api.get_game_time() - started) : 0.0;
        }
        state.spiritwalkers_grace = has_buff_name(api, "player", "Spiritwalker's Grace");

        // Count active DoTs/debuffs for multi-dot and Lightning Rod decisions.
        state.max_flame_shocks = state.is_mplus ? max_flame_shocks_mplus_ : max_flame_shocks_raid_;
        state.flame_shocks = count_debuff(api, "Flame Shock");
        state.lightning_rods = count_debuff(api, "Lightning Rod");

        // Talent detection chooses the correct branches without requiring a
        // specific imported build string.
        state.talent_elemental_blast = has_talent_name(api, "Elemental Blast") ||
            (spellbook_.elemental_blast != 0 && api.is_spell_known_or_overrides_known(spellbook_.elemental_blast));
        // The Midnight Earthquake talent tooltip states that the spell is cast
        // at the hostile target. Prefer a normal target SpellAction whenever
        // that talent/spell is learned; retain the @cursor macro only as an
        // older-client compatibility fallback when talent data is unavailable.
        state.talent_earthquake = has_talent_name(api, "Earthquake") ||
            (spellbook_.earthquake != 0 &&
             (api.is_spell_known_or_overrides_known(spellbook_.earthquake) ||
              api.has_spell(spellbook_.earthquake)));
        state.talent_master_of_the_elements = has_talent_name(api, "Master of the Elements");
        state.talent_tempest = has_talent_name(api, "Tempest") || spellbook_.tempest != 0;
        state.talent_inferno_arc = has_talent_name(api, "Inferno Arc");
        state.talent_purging_flames = has_talent_name(api, "Purging Flames");
        state.talent_crackling_fury = has_talent_name(api, "Crackling Fury");

        // Proc snapshot used by the ST/AoE priorities below.
        state.buff_master_of_the_elements = has_buff_name(api, "player", "Master of the Elements");
        state.buff_lava_surge = has_buff_name(api, "player", "Lava Surge");
        state.buff_stormkeeper = has_buff_name(api, "player", "Stormkeeper");
        state.buff_tempest = has_buff_name(api, "player", "Tempest");
        state.stormkeeper_stacks = buff_stacks_by_name(
            api, "player", "Stormkeeper", spellbook_.stormkeeper);
        state.tempest_stacks = buff_stacks_by_name(
            api, "player", "Tempest", spellbook_.tempest);
        state.buff_ascendance = has_buff_name(api, "player", "Ascendance");
        state.flowing_elements_stacks = buff_stacks_by_name(
            api, "player", "Flowing Elements", spellbook_.flowing_elements);
        state.power_of_the_maelstrom_stacks = buff_stacks_by_name(
            api, "player", "Power of the Maelstrom", spellbook_.power_of_the_maelstrom);
        state.buff_flowing_elements = state.flowing_elements_stacks > 0;
        state.buff_power_of_the_maelstrom = state.power_of_the_maelstrom_stacks > 0;
        state.buff_purging_flames = has_buff_name(api, "player", "Purging Flames");
        state.buff_overcharge_tier = has_buff_containing(api, "player", "Overcharge") ||
            has_buff_containing(api, "player", "12.1 Class Set") ||
            api.has_buff("player", elemental_ids::ELEMENTAL_S2_4PC, false);
        state.buff_elemental_blast_stat = has_buff_containing(api, "player", "Elemental Blast");
        state.fire_elemental_remaining = buff_remaining_by_name(
            api, "player", "Fire Elemental", spellbook_.fire_elemental);

        const GlobalLockState lock = compute_global_lock(api);
        state.global_lock = lock.locked;
        state.gcd_desync = lock.desync;
        state.global_lock_remaining = lock.remaining;

        return state;
    }

    // -------------------------------------------------------------------------
    // ACTION CONSTRUCTION HELPERS
    // -------------------------------------------------------------------------
    // Creates a hostile-target action only when target, spell, LOS, and range
    // are valid. "reason" is the label visible in rotation debugging.
    bool damage_spell_in_range(const rotation_api::IRotationAPI& api,
                               uint32_t spell_id,
                               const std::string& unit,
                               bool log_fallback)
    {
        if (api.is_spell_in_range(spell_id, unit)) return true;
        if (unit != "target") return false;
        if (!api.unit_exists("target") || !current_target_is_hostile(api)) return false;
        if (api.unit_is_dead("target") && !unit_looks_like_dummy(api, "target")) return false;
        if (!current_snapshot_target_range_valid_) return false;
        const std::string guid = api.get_unit_guid("target");
        if (guid.empty() || guid != current_snapshot_target_guid_) return false;
        if (current_snapshot_target_range_ > kCloseRangeFallback) return false;
        if (log_fallback) note_range_fallback(api, spell_id);
        return true;
    }

    void note_range_fallback(const rotation_api::IRotationAPI& api, uint32_t spell_id) {
        if (!debug_diagnostics_) return;
        const double now = api.get_game_time();
        if ((now - last_range_fallback_debug_time_) < 0.75) return;
        last_range_fallback_debug_time_ = now;
        std::ostringstream out;
        out << "RANGE_FALLBACK spell=" << spell_id
            << " target=" << api.get_unit_name("target")
            << " range=" << std::fixed << std::setprecision(1)
            << current_snapshot_target_range_;
        range_fallback_message_ = out.str();
    }

    // "track_dispatch" is false for actions that own a different confirmation
    // system: setup Voltaic Blaze, interrupts, and utility Purge.
    RotationAction cast_damage(const rotation_api::IRotationAPI& api,
                               uint32_t spell_id,
                               const std::string& unit,
                               const std::string& reason,
                               bool check_range = true,
                               bool track_dispatch = true)
    {
        if (!can_damage_action_target(api, unit) || !can_cast(api, spell_id)) {
            return no_action("Unavailable");
        }
        // An unresolved dispatch owns the damage dispatcher. No tracked damage
        // action - the same spell or a different one - may be sent until it
        // confirms, starts casting, invalidates, or escapes. The one exception
        // is the spell queue window of the dispatch's own hardcast, where Titan
        // reports the player free precisely so the next filler can be queued.
        bool release_for_queue = false;
        if (track_dispatch && damage_dispatch_.pending) {
            if (!queue_release_available(api, spell_id)) {
                return no_action("Damage dispatch pending");
            }
            release_for_queue = true;
        }
        if (track_dispatch && damage_dispatch_suppressed(api, spell_id, unit)) {
            note_damage_suppressed(api, spell_id);
            return no_action("Damage dispatch suppressed");
        }
        if (check_range && !damage_spell_in_range(api, spell_id, unit, true)) {
            return no_action("Out of range");
        }
        if (release_for_queue) release_dispatcher_for_queue(api);
        note_stall_end(api, spell_id);
        note_input_dispatch(api);
        if (track_dispatch) record_damage_dispatch(api, spell_id, unit);
        return spell(spell_id, unit, reason);
    }

    // Convenience wrapper for player-targeted buffs/defensives.
    RotationAction cast_self(const rotation_api::IRotationAPI& api,
                             uint32_t spell_id,
                             const std::string& reason)
    {
        if (!can_cast(api, spell_id)) return no_action("Unavailable");
        note_input_dispatch(api);
        return spell(spell_id, "player", reason);
    }

    // Central spender selector. Elemental Blast/Earth Shock use SimC's
    // target_if=min:debuff.lightning_rod.remains behavior at 2+ targets.
    RotationAction cast_spender(const rotation_api::IRotationAPI& api,
                                const CombatState& state,
                                bool aoe,
                                const std::string& reason)
    {
        if (aoe && can_cast(api, spellbook_.earthquake)) {
            if (RotationAction action = cast_earthquake(api, state, reason); !action.is_none()) {
                return action;
            }
        }
        const std::string spender_target = state.enemies >= 2
            ? best_lightning_rod_target(api, state) : state.target;
        if (state.talent_elemental_blast && can_cast(api, spellbook_.elemental_blast)) {
            if (RotationAction action = cast_damage(api, spellbook_.elemental_blast, spender_target,
                    reason + " Elemental Blast"); !action.is_none())
            {
                return action;
            }
        }
        if (can_cast(api, spellbook_.earth_shock)) {
            if (RotationAction action = cast_damage(api, spellbook_.earth_shock, spender_target,
                    reason + " Earth Shock"); !action.is_none())
            {
                return action;
            }
        }
        return no_action("No spender");
    }

    // Midnight's talented Earthquake is anchored to the current hostile
    // target. Titan 23.3 executes that through a normal target SpellAction.
    // The secure @cursor macro remains only for legacy spell variants whose
    // talent/known-spell metadata does not identify the target-anchored form.
    RotationAction cast_earthquake(const rotation_api::IRotationAPI& api,
                                   const CombatState& state,
                                   const std::string& reason)
    {
        if (!can_cast(api, spellbook_.earthquake)) return no_action("Earthquake unavailable");
        if (!state.earthquake_safe) {
            if ((api.get_game_time() - last_earthquake_delay_time_) >= 0.75) {
                last_earthquake_delay_time_ = api.get_game_time();
                if (telemetry_combat_active_) ++telemetry_.earthquake_delays;
            }
            return no_action("Earthquake held while pack is gathering");
        }
        // The APL only reaches this point when Earthquake is a legitimate
        // current candidate, which makes it the one unambiguous place to count
        // discrete opportunities against confirmed casts.
        if (telemetry_combat_active_ && state.use_aoe_list && state.enemies >= 3) {
            ++telemetry_.earthquake_opportunities;
        }
        if (state.talent_earthquake) {
            return cast_damage(api, spellbook_.earthquake, state.target,
                reason + " Earthquake on target");
        }
        note_input_dispatch(api);
        return macro(elemental_ids::EARTHQUAKE_CURSOR_MACRO,
            reason + " Earthquake @cursor compatibility fallback");
    }

    // -------------------------------------------------------------------------
    // MAINTENANCE / SURVIVAL / MYTHIC+ SUPPORT
    // -------------------------------------------------------------------------
    // Long-duration upkeep runs prepull and out of combat only, where
    // "allow_weapon" also permits the weapon imbue check.
    // Redundant aura evidence. A hidden or renamed numeric aura must not make
    // the profile believe a long-duration buff is missing.
    bool has_maintenance_aura(const rotation_api::IRotationAPI& api,
                              uint32_t cast_id,
                              uint32_t known_aura_id,
                              const std::string& name) const
    {
        if (cast_id != 0 && api.has_buff("player", cast_id, false)) return true;
        if (known_aura_id != 0 && api.has_buff("player", known_aura_id, false)) return true;
        return has_buff_name(api, "player", name);
    }

    // Aura propagation is not instant. One shared latch prevents the
    // "Lightning Shield x5" burst that used to consume several globals.
    RotationAction maintenance_dispatch(const rotation_api::IRotationAPI& api,
                                        uint32_t spell_id,
                                        const std::string& reason)
    {
        const double now = api.get_game_time();
        if (spell_id == last_maintenance_spell_id_ &&
            recently_attempted(last_maintenance_dispatch_time_, kMaintenanceAttemptWindow, now))
        {
            return no_action("Maintenance settling");
        }
        last_maintenance_spell_id_ = spell_id;
        last_maintenance_dispatch_time_ = now;
        note_input_dispatch(api);
        return spell(spell_id, "player", reason);
    }

    RotationAction maintenance_action(const rotation_api::IRotationAPI& api, bool allow_weapon) {
        // Priority 1: Lightning Shield is required for core Shaman interactions.
        if (can_cast(api, spellbook_.lightning_shield) &&
            !has_maintenance_aura(api, spellbook_.lightning_shield,
                                  elemental_ids::LIGHTNING_SHIELD, "Lightning Shield"))
        {
            if (RotationAction action = maintenance_dispatch(api, spellbook_.lightning_shield,
                    "Maintain Lightning Shield"); !action.is_none())
            {
                return action;
            }
        }

        // Priority 2: the supplied guide requires the one-hour Skyfury group
        // buff. Casting it on the player applies it to the party/raid as well.
        if (can_cast(api, spellbook_.skyfury) &&
            !has_maintenance_aura(api, spellbook_.skyfury, elemental_ids::SKYFURY, "Skyfury"))
        {
            if (RotationAction action = maintenance_dispatch(api, spellbook_.skyfury,
                    "Maintain Skyfury"); !action.is_none())
            {
                return action;
            }
        }

        // Priority 3: maintain Flametongue only when weapon maintenance is safe.
        if (allow_weapon && can_cast(api, spellbook_.flametongue_weapon) && !api.has_main_hand_enchant()) {
            if (RotationAction action = maintenance_dispatch(api, spellbook_.flametongue_weapon,
                    "Maintain Flametongue Weapon"); !action.is_none())
            {
                return action;
            }
        }

        // Priority 4: maintain the Midnight Thunderstrike Ward when talented.
        if (can_cast(api, spellbook_.thunderstrike_ward) &&
            !has_maintenance_aura(api, spellbook_.thunderstrike_ward, 0, "Thunderstrike Ward"))
        {
            if (RotationAction action = maintenance_dispatch(api, spellbook_.thunderstrike_ward,
                    "Maintain Thunderstrike Ward"); !action.is_none())
            {
                return action;
            }
        }

        return no_action(allow_weapon ? "Maintenance complete" : "No combat maintenance");
    }

    // Travel form between pulls only. Combat movement stays with the existing
    // Spiritwalker's Grace / instant-cast handling.
    RotationAction auto_ghost_wolf_action(const rotation_api::IRotationAPI& api) {
        if (!auto_ghost_wolf_) return no_action("Auto Ghost Wolf disabled");
        if (api.is_in_combat_lockdown()) return no_action("In combat");
        if (spellbook_.ghost_wolf == 0) return no_action("Ghost Wolf unknown");
        if (!api.is_player_moving()) return no_action("Not moving");
        if (api.unit_is_casting_or_channeling("player", true)) return no_action("Casting");
        if (has_buff_name(api, "player", "Ghost Wolf")) return no_action("Ghost Wolf active");

        const double now = api.get_game_time();
        const double moving_since = api.get_player_started_moving_time();
        if (moving_since <= 0.0 || (now - moving_since) < kGhostWolfMoveDelay) {
            return no_action("Movement too short");
        }
        if (!can_cast(api, spellbook_.ghost_wolf)) return no_action("Ghost Wolf unavailable");
        if (recently_attempted(last_ghost_wolf_dispatch_time_, kGhostWolfAttempt, now)) {
            return no_action("Ghost Wolf settling");
        }

        last_ghost_wolf_dispatch_time_ = now;
        note_input_dispatch(api);
        return spell(spellbook_.ghost_wolf, "player", "Auto Ghost Wolf");
    }

    // Defensive thresholds rise gradually in high keys. A level 17 key, for
    // example, adds 15 percentage points, encouraging mitigation before the
    // same raw hit becomes lethal rather than after health has already dropped.
    double defensive_key_bonus(const CombatState& state) const {
        if (!scale_defensives_with_key_ || !state.is_mplus) return 0.0;
        return std::clamp((state.mythic_plus_level - 7) * 1.5, 0.0, 15.0);
    }

    // Defensive order intentionally goes from major instant mitigation to an
    // emergency heal. Listed dangerous casts can trigger mitigation before the
    // hit lands; ordinary HP thresholds remain available as the fallback.
    // Cheap gate for the pre-dispatcher survival layer. Only a genuinely low
    // player pays for the extra snapshot; the predictive branches keep running
    // in the normal order with the tick's real state.
    bool emergency_defensive_possible(const rotation_api::IRotationAPI& api) const {
        if (!api.is_toggle_enabled("Defensives")) return false;
        const double gate = std::min(100.0,
            std::max({astral_shift_hp_, stone_bulwark_hp_, emergency_heal_hp_,
                      earth_elemental_hp_}) + 15.0);
        return api.get_unit_health_percent("player") <= gate;
    }

    RotationAction defensive_action(const rotation_api::IRotationAPI& api, const CombatState& state) {
        if (!api.is_toggle_enabled("Defensives")) return no_action("Defensives disabled");

        const double key_bonus = defensive_key_bonus(state);
        const double astral_hp = std::min(95.0, astral_shift_hp_ + key_bonus);
        const double bulwark_hp = std::min(100.0, stone_bulwark_hp_ + key_bonus);
        const double predictive_hp = std::min(100.0, predictive_defensive_hp_ + key_bonus);
        const double earth_hp = std::min(90.0, earth_elemental_hp_ + key_bonus);
        const double emergency_hp = std::min(85.0, emergency_heal_hp_ + key_bonus);

        const bool listed_hit_incoming = predictive_defensives_ &&
            state.dangerous_cast_targeting_player &&
            state.dangerous_cast_remaining <= predictive_cast_window_;
        const bool player_has_pack_threat = predictive_defensives_ &&
            state.enemies_targeting_player >= predictive_enemy_count_ &&
            state.player_hp <= predictive_hp;
        const bool predictive_trigger = listed_hit_incoming || player_has_pack_threat;
        const bool encounter_trigger = predictive_defensives_ &&
            state.encounter_danger_incoming;

        // 1. Astral Shift: strongest immediate personal damage reduction.
        if ((predictive_trigger || encounter_trigger || state.player_hp <= astral_hp) &&
            can_cast(api, spellbook_.astral_shift) &&
            !has_buff_name(api, "player", "Stone Bulwark") &&
            !has_buff_name(api, "player", "Astral Shift"))
        {
            return spell(spellbook_.astral_shift, "player",
                encounter_trigger ? "Astral Shift before deadly encounter event" :
                listed_hit_incoming ? "Astral Shift before listed danger" :
                player_has_pack_threat ? "Astral Shift under pack threat" : "Astral Shift");
        }

        // 2. Stone Bulwark: backup for predicted damage and a proactive absorb
        // at its separately configured health threshold.
        if ((predictive_trigger || encounter_trigger || state.player_hp <= bulwark_hp) &&
            can_cast(api, spellbook_.stone_bulwark_totem) &&
            !has_buff_name(api, "player", "Astral Shift") &&
            !has_buff_name(api, "player", "Stone Bulwark"))
        {
            return spell(spellbook_.stone_bulwark_totem, "player",
                (predictive_trigger || encounter_trigger)
                    ? "Stone Bulwark before incoming damage" : "Stone Bulwark Totem");
        }

        // 3. Optional Earth Elemental. The guide treats Primordial Bond as an
        // extra defensive, but automation is disabled by default because the
        // elemental can taunt dungeon enemies away from the tank.
        if (use_earth_elemental_defensive_ && state.player_hp <= earth_hp &&
            can_cast(api, spellbook_.earth_elemental))
        {
            return spell(spellbook_.earth_elemental, "player", "Earth Elemental defensive");
        }

        // 4. Healing Surge only at emergency HP. Nature's Swiftness makes it
        // instant when available; otherwise movement rules must permit casting.
        if (state.player_hp <= emergency_hp && can_cast(api, spellbook_.healing_surge)) {
            if (use_natures_swiftness_ && can_cast(api, spellbook_.natures_swiftness)) {
                return spell(spellbook_.natures_swiftness, "player", "Nature's Swiftness emergency heal");
            }
            if (!state.moving || has_buff_name(api, "player", "Nature's Swiftness") || state.spiritwalkers_grace) {
                return spell(spellbook_.healing_surge, "player", "Emergency Healing Surge");
            }
        }

        return no_action("No defensive");
    }

    struct InterruptCandidate {
        std::string unit;
        uint32_t spell_id = 0;
        double remaining = std::numeric_limits<double>::max();
        bool priority = false;
        bool valid = false;
    };

    // Converts enemy cast timestamps to completion percentage. Priority-list
    // spells use the earlier threshold; every cast still has a 0.25-sec failsafe.
    static bool interrupt_time_reached(const rotation_api::CastInfo& info,
                                       double now,
                                       double threshold)
    {
        const double duration = info.get_duration();
        if (duration <= 0.0) return true;
        const double progress = 100.0 *
            std::clamp(info.get_elapsed(now) / duration, 0.0, 1.0);
        return progress >= threshold || info.get_remaining(now) <= 0.25;
    }

    static rotation_api::CastInfo current_cast_info(const rotation_api::IRotationAPI& api,
                                                    const std::string& unit)
    {
        return api.unit_is_channeling(unit)
            ? api.get_unit_channel_info(unit)
            : api.get_unit_casting_info(unit);
    }

    InterruptCandidate evaluate_interrupt(const rotation_api::IRotationAPI& api,
                                          const std::string& unit) const
    {
        InterruptCandidate candidate;
        candidate.unit = unit;
        if (!can_damage_unit(api, unit) || !api.unit_is_casting_or_channeling(unit) ||
            !api.is_spell_in_range(spellbook_.wind_shear, unit))
        {
            return candidate;
        }

        const bool channeling = api.unit_is_channeling(unit);
        const bool interruptible = channeling
            ? api.unit_channel_interruptible(unit)
            : api.unit_cast_interruptible(unit);
        if (!interruptible) return candidate;

        const rotation_api::CastInfo info = current_cast_info(api, unit);
        candidate.spell_id = info.spell_id;
        if (id_in_any_named_list(api, candidate.spell_id,
                {"Interrupt.Ignore", "Elemental.Interrupt.Ignore"}))
        {
            return candidate;
        }

        const bool listed_priority = id_in_any_named_list(api, candidate.spell_id,
            {"Interrupt.Kick", "Interrupt.Priority", "Elemental.Interrupt.Priority"});
        if (interrupt_mode_ == 1 && !listed_priority) return candidate;
        // "Any Interruptible" deliberately applies one timing rule to all
        // casts; Smart/Priority-only retain the early-list behavior.
        candidate.priority = interrupt_mode_ != 2 && listed_priority;

        const double now = api.get_game_time();
        const double threshold = candidate.priority
            ? priority_interrupt_at_percent_ : interrupt_at_percent_;
        if (!interrupt_time_reached(info, now, threshold)) return candidate;

        candidate.remaining = info.get_remaining(now);
        candidate.valid = true;
        return candidate;
    }

    // Titan's hostile action path is current-target only. Priority-list timing
    // still applies, but the rotation never reports an off-target kick that the
    // secure executor would redirect to the current target.
    RotationAction interrupt_action(const rotation_api::IRotationAPI& api) {
        if (!api.is_interrupt_enabled() || !can_cast(api, spellbook_.wind_shear)) {
            return no_action("Interrupt disabled");
        }

        // Hostile SpellAction units execute on the current target in Titan
        // 23.3, so never claim an off-target kick that cannot be bound safely.
        const InterruptCandidate best = evaluate_interrupt(api, "target");

        if (!best.valid) return no_action("No interrupt");
        if (RotationAction action = cast_damage(api, spellbook_.wind_shear, best.unit,
                best.priority ? "Wind Shear priority cast" : "Wind Shear", true, false);
            !action.is_none())
        {
            return action;
        }
        return no_action("Wind Shear snapshot failed");
    }

    static bool has_dispellable_magic_buff(const rotation_api::IRotationAPI& api,
                                            const std::string& unit)
    {
        for (const auto& buff : api.get_buffs(unit, false)) {
            if (same_name(buff.dispel_name, "Magic")) return true;
        }
        return false;
    }

    static bool unit_has_listed_buff(const rotation_api::IRotationAPI& api,
                                     const std::string& unit,
                                     std::initializer_list<const char*> list_names)
    {
        for (const auto& buff : api.get_buffs(unit, false)) {
            if (buff.aura_id == 0) continue;
            for (const char* list_name : list_names) {
                if (api.is_id_in_list(buff.aura_id, std::string(list_name))) return true;
            }
        }
        return false;
    }

    // Utility order favors encounter-prevention and dispels, then group healing,
    // offensive dispels, and finally opt-in AoE stops.
    RotationAction utility_action(const rotation_api::IRotationAPI& api, const CombatState& state) {
        if (!api.is_toggle_enabled("Utility")) return no_action("Utility disabled");

        // Empty lists do nothing. Add enemy cast IDs to Utility.Tremor or
        // Elemental.Utility.Tremor to pre-place Tremor for known fear effects.
        if (can_cast(api, spellbook_.tremor_totem)) {
            for (const auto& unit : hostile_units(api, 40.0)) {
                if (!api.unit_is_casting_or_channeling(unit)) continue;
                const rotation_api::CastInfo info = current_cast_info(api, unit);
                if (info.get_remaining(api.get_game_time()) > 0.0 &&
                    id_in_any_named_list(api, info.spell_id,
                        {"Utility.Tremor", "Elemental.Utility.Tremor"}))
                {
                    return spell(spellbook_.tremor_totem, "player", "Tremor Totem before listed fear");
                }
            }
        }

        // Build one friendly snapshot for all support decisions this tick. This
        // avoids repeating full raid aura scans for each individual utility.
        int poisoned_units = 0;
        int ancestral_guidance_injured = 0;
        int healing_stream_injured = 0;
        std::string curse_target;
        for (const auto& unit : friendly_units(api)) {
            if (!can_heal_unit(api, unit)) continue;

            const double hp = api.get_unit_health_percent(unit);
            if (hp <= ancestral_guidance_group_hp_) ++ancestral_guidance_injured;
            if (hp <= healing_stream_group_hp_) ++healing_stream_injured;

            bool poisoned = false;
            for (const auto& debuff : api.get_debuffs(unit, false)) {
                poisoned = poisoned || same_name(debuff.dispel_name, "Poison");
                if (curse_target.empty() && same_name(debuff.dispel_name, "Curse")) {
                    curse_target = unit;
                }
            }
            if (poisoned) ++poisoned_units;
        }

        // Poison Cleansing Totem wins when several group members are poisoned;
        // one isolated Curse is handled by the single-target cleanse below.
        if (auto_poison_cleansing_ && can_cast(api, spellbook_.poison_cleansing_totem) &&
            poisoned_units >= poison_cleansing_min_units_)
        {
            return spell(spellbook_.poison_cleansing_totem, "player",
                "Poison Cleansing Totem for group poisons");
        }

        // Cleanse Spirit scans player and the full party/raid for a Curse.
        if (auto_cleanse_curse_ && !curse_target.empty() &&
            can_cast(api, spellbook_.cleanse_spirit))
        {
            return spell(spellbook_.cleanse_spirit, curse_target, "Cleanse Spirit");
        }

        const double group_hp = api.get_average_group_hp_percent(40.0, true);

        // Ancestral Guidance converts current damage into group healing, so it
        // is only recommended while enemies are alive long enough to feed it.
        if (auto_ancestral_guidance_ && state.enemies > 0 && state.fight_ttd > 5.0 &&
            group_hp <= ancestral_guidance_group_hp_ &&
            ancestral_guidance_injured >= ancestral_guidance_min_injured_ &&
            can_cast(api, spellbook_.ancestral_guidance))
        {
            return spell(spellbook_.ancestral_guidance, "player", "Ancestral Guidance group recovery");
        }

        if (auto_healing_stream_ && group_hp <= healing_stream_group_hp_ &&
            healing_stream_injured >= healing_stream_min_injured_ &&
            can_cast(api, spellbook_.healing_stream_totem))
        {
            return spell(spellbook_.healing_stream_totem, "player", "Healing Stream Totem group support");
        }

        // Automatic Purge is a Utility setting, not an overlay button.
        // Smart mode searches configured high-priority aura IDs on every unit,
        // then permits a generic Magic purge only on the current target.
        if (auto_purge_ && can_cast(api, spellbook_.purge)) {
            if (can_damage_unit(api, state.target) &&
                has_dispellable_magic_buff(api, state.target) &&
                unit_has_listed_buff(api, state.target,
                        {"Purge.HighPriority", "Dispel.Purge", "Elemental.Purge.Priority"}))
            {
                if (RotationAction action = cast_damage(api, spellbook_.purge, state.target,
                        "Purge listed priority buff on current target", true, false);
                    !action.is_none())
                {
                    return action;
                }
            }

            if (purge_mode_ == 0 && can_damage_unit(api, state.target) &&
                has_dispellable_magic_buff(api, state.target))
            {
                if (RotationAction action = cast_damage(api, spellbook_.purge, state.target,
                        "Purge current-target magic buff", true, false);
                    !action.is_none())
                {
                    return action;
                }
            }

        }

        // Automatic Capacitor is M+-only and requires the Utility setting.
        // configured stun-list cast gets priority when enough cast time remains
        // for the totem to arm; otherwise the ordinary big-pull rule applies.
        if (auto_capacitor_ && state.is_mplus &&
            can_cast(api, spellbook_.capacitor_totem))
        {
            for (const auto& unit : hostile_units(api, 8.0)) {
                if (!api.is_nameplate_in_range(unit, 8.0)) continue;
                if (!api.unit_is_casting_or_channeling(unit)) continue;
                const rotation_api::CastInfo info = current_cast_info(api, unit);
                const double remaining = info.get_remaining(api.get_game_time());
                if (remaining >= 1.5 &&
                    id_in_any_named_list(api, info.spell_id,
                        {"Interrupt.Stun", "Elemental.Interrupt.Stun"}))
                {
                    return macro(elemental_ids::CAPACITOR_PLAYER_MACRO,
                        "Capacitor Totem @player for listed stop");
                }
            }

            const int nearby_enemies = static_cast<int>(
                api.get_nameplates_in_range_count(8.0, true, false));
            if (state.meaningful_enemies >= capacitor_enemy_count_ &&
                nearby_enemies >= capacitor_enemy_count_)
            {
                return macro(elemental_ids::CAPACITOR_PLAYER_MACRO,
                    "Capacitor Totem @player for big pull");
            }
        }

        return no_action("No utility");
    }

    // -------------------------------------------------------------------------
    // CAST-CONFIRMED OPENER STATE
    // -------------------------------------------------------------------------
    void sync_exact_opener(const rotation_api::IRotationAPI& api, const CombatState& state) {
        double combat_start = api.get_player_combat_start_time();
        if (combat_start <= 0.0) {
            combat_start = tracked_combat_start_time_ > 0.0
                ? tracked_combat_start_time_ : api.get_game_time();
        }
        if (tracked_combat_start_time_ > 0.0 &&
            std::fabs(combat_start - tracked_combat_start_time_) < 0.1)
        {
            return;
        }

        tracked_combat_start_time_ = combat_start;
        opener_started_at_ = api.get_game_time();
        const double gcd = std::max(0.1, api.get_remaining_gcd());
        const double recent_prepull = combat_start - 4.0;

        opener_stormkeeper_done_ = !mini_cooldowns_allowed(api) || spellbook_.stormkeeper == 0 ||
            state.buff_stormkeeper || last_stormkeeper_success_time_ >= recent_prepull ||
            api.get_spell_cooldown_remaining(spellbook_.stormkeeper) > gcd;
        opener_swiftness_done_ = !mini_cooldowns_allowed(api) || state.hero_tree != HeroTree::Farseer ||
            !use_ancestral_swiftness_ || spellbook_.ancestral_swiftness == 0 ||
            last_ancestral_swiftness_success_time_ >= recent_prepull ||
            api.get_spell_cooldown_remaining(spellbook_.ancestral_swiftness) > gcd;
        // Voltaic Blaze is part of the opener only when there are at least two
        // targets. Current ST openers go directly from swiftness to Ascendance;
        // the two-target SimC list still uses VB for Purging Flames.
        opener_voltaic_done_ = state.enemies < 2 ||
            state.hero_tree != HeroTree::Farseer ||
            !state.talent_purging_flames || spellbook_.voltaic_blaze == 0 ||
            state.buff_purging_flames || last_voltaic_blaze_success_time_ >= recent_prepull ||
            api.get_spell_cooldown_remaining(spellbook_.voltaic_blaze) > gcd;
        opener_ascendance_done_ = !cooldowns_allowed(api, state) || spellbook_.ascendance == 0 ||
            state.buff_ascendance || last_ascendance_success_time_ >= recent_prepull ||
            api.get_spell_cooldown_remaining(spellbook_.ascendance) > gcd;
        opener_active_ = use_exact_opener_ && !opener_ascendance_done_;
    }

    RotationAction exact_opener_action(const rotation_api::IRotationAPI& api,
                                       const CombatState& state)
    {
        if (!opener_active_) return no_action("Exact opener inactive");
        if ((api.get_game_time() - opener_started_at_) > exact_opener_timeout_) {
            opener_active_ = false;
            return no_action("Exact opener timed out");
        }
        const double gcd = std::max(0.1, api.get_remaining_gcd());

        // A package can be toggled off after combat begins. Mark only its own
        // opener steps complete so the other package continues independently.
        if (!mini_cooldowns_allowed(api)) {
            opener_stormkeeper_done_ = true;
            opener_swiftness_done_ = true;
        }
        if (!cooldowns_allowed(api, state)) {
            opener_ascendance_done_ = true;
        }

        if (state.buff_stormkeeper || last_stormkeeper_success_time_ >= opener_started_at_ - 4.0) {
            opener_stormkeeper_done_ = true;
        }
        if (last_ancestral_swiftness_success_time_ >= opener_started_at_ - 4.0) {
            opener_swiftness_done_ = true;
        }
        if (state.enemies < 2 || state.buff_purging_flames ||
            last_voltaic_blaze_success_time_ >= opener_started_at_ - 4.0) {
            opener_voltaic_done_ = true;
        }
        if (state.buff_ascendance || last_ascendance_success_time_ >= opener_started_at_ - 4.0) {
            opener_ascendance_done_ = true;
        }

        // Step 1: Stormkeeper. Prepull success/buff normally completes this.
        if (!opener_stormkeeper_done_) {
            if (still_pending(stormkeeper_pending_until_, api.get_game_time()) ||
                recently_attempted(last_stormkeeper_dispatch_time_, kStormkeeperAttempt,
                    api.get_game_time()))
            {
                return no_action("Exact opener waiting for Stormkeeper");
            }
            if (RotationAction action = dispatch_player_setup(
                    api, spellbook_.stormkeeper, stormkeeper_pending_until_,
                    last_stormkeeper_success_time_, kStormkeeperSettle,
                    last_stormkeeper_dispatch_time_, kStormkeeperAttempt,
                    "Exact opener 1: Stormkeeper"); !action.is_none())
            {
                return action;
            }
            if (api.get_spell_cooldown_remaining(spellbook_.stormkeeper) > gcd) {
                opener_stormkeeper_done_ = true;
            } else {
                return no_action("Exact opener waiting for Stormkeeper");
            }
        }

        // Step 2: Farseer Ancestral Swiftness.
        if (!opener_swiftness_done_) {
            if (still_pending(ancestral_swiftness_pending_until_, api.get_game_time()) ||
                recently_attempted(last_ancestral_swiftness_dispatch_time_,
                    kAncestralSwiftnessAttempt, api.get_game_time()))
            {
                return no_action("Exact opener waiting for Ancestral Swiftness");
            }
            if (RotationAction action = dispatch_player_setup(
                    api, spellbook_.ancestral_swiftness, ancestral_swiftness_pending_until_,
                    last_ancestral_swiftness_success_time_, kAncestralSwiftnessSettle,
                    last_ancestral_swiftness_dispatch_time_, kAncestralSwiftnessAttempt,
                    "Exact opener 2: Ancestral Swiftness"); !action.is_none())
            {
                return action;
            }
            if (api.get_spell_cooldown_remaining(spellbook_.ancestral_swiftness) > gcd) {
                opener_swiftness_done_ = true;
            } else {
                return no_action("Exact opener waiting for Ancestral Swiftness");
            }
        }

        // Step 3: two-plus-target Purging Flames setup with Voltaic Blaze.
        // A damaging single-target Lava Burst precast cannot be automated here
        // because this API exposes no countdown-to-pull value; get_prepull_action
        // therefore remains non-hostile and safe for manual pull timers.
        if (!opener_voltaic_done_) {
            if (still_pending(voltaic_setup_pending_until_, api.get_game_time())) {
                return no_action("Exact opener waiting for Voltaic Blaze");
            }
            if (RotationAction action = dispatch_voltaic_setup(
                    api, state, "Exact opener 3: Voltaic Blaze"); !action.is_none())
            {
                return action;
            }
            if (api.get_spell_cooldown_remaining(spellbook_.voltaic_blaze) > gcd) {
                opener_voltaic_done_ = true;
            } else {
                return no_action("Exact opener waiting for Voltaic Blaze");
            }
        }

        // Step 4: Nature's Swiftness/racials/Ascendance package. The helper
        // returns one action per tick until Ascendance is confirmed successful.
        if (!opener_ascendance_done_) {
            if (RotationAction action = ascendance_burst_action(api, state); !action.is_none()) {
                return action;
            }
            if (api.get_spell_cooldown_remaining(spellbook_.ascendance) > gcd ||
                last_ascendance_success_time_ >= opener_started_at_ - 4.0)
            {
                opener_ascendance_done_ = true;
            } else {
                return no_action("Exact opener waiting for Ascendance");
            }
        }

        opener_active_ = false;
        return no_action("Exact opener completed");
    }

    // -------------------------------------------------------------------------
    // DAMAGE COOLDOWN GATING AND SYNCHRONIZATION
    // -------------------------------------------------------------------------
    bool burst_now(const rotation_api::IRotationAPI& api) const {
        return api.is_toggle_enabled("Burst Now");
    }

    static double spell_base_cooldown_seconds(const rotation_api::IRotationAPI& api,
                                              uint32_t spell_id,
                                              double fallback_seconds)
    {
        if (spell_id == 0) return fallback_seconds;
        const double seconds = static_cast<double>(api.get_spell_base_cooldown(spell_id)) / 1000.0;
        return seconds > 0.0 ? seconds : fallback_seconds;
    }

    // Number of casts still possible when the first use is delayed by `delay`.
    // Casting exactly as the fight expires is not counted as a useful use.
    static int projected_cooldown_uses(double fight_ttd,
                                       double delay,
                                       double base_cooldown)
    {
        if (fight_ttd <= delay || base_cooldown <= 0.0) return 0;
        return 1 + static_cast<int>(std::floor(
            std::max(0.0, fight_ttd - delay - 0.001) / base_cooldown));
    }

    // Holding is rejected only when it reduces the number of casts remaining
    // in this encounter/pull. Turning the setting off restores strict syncing.
    bool hold_would_lose_use(const rotation_api::IRotationAPI& api,
                             const CombatState& state,
                             uint32_t spell_id,
                             double delay,
                             double fallback_cooldown) const
    {
        if (!prevent_lost_cooldown_uses_ || delay <= 0.0) return false;
        const double base = spell_base_cooldown_seconds(api, spell_id, fallback_cooldown);
        return projected_cooldown_uses(state.fight_ttd, delay, base) <
            projected_cooldown_uses(state.fight_ttd, 0.0, base);
    }

    void maybe_log_cooldown_toggle_state(const RotationContext& context) {
        const auto& api = context.api();
        if (!api.are_cooldowns_enabled()) return;
        if (api.is_toggle_enabled("Mini CDs") || api.is_toggle_enabled("Major CDs")) return;
        const double now = api.get_game_time();
        if ((now - last_cd_toggle_diag_time_) < 8.0) return;
        last_cd_toggle_diag_time_ = now;
        context.log("Cooldown automation inactive: Mini CDs and Major CDs are both OFF");
    }

    // Titan's Cooldowns toggle is the master gate. Mini and Major then split
    // Stormkeeper/Swiftness from Ascendance, racials, and aligned trinkets.
    bool damage_cooldown_master_enabled(const rotation_api::IRotationAPI& api) const {
        return api.are_cooldowns_enabled();
    }

    bool mini_cooldowns_allowed(const rotation_api::IRotationAPI& api) const {
        return damage_cooldown_master_enabled(api) && api.is_toggle_enabled("Mini CDs");
    }

    // Major CDs retain the content-aware policy that previously controlled all
    // cooldowns. This package includes Ascendance, its setup, racials, and any
    // trinket slot configured With Major CDs.
    bool cooldowns_allowed(const rotation_api::IRotationAPI& api, const CombatState& state) const {
        if (!damage_cooldown_master_enabled(api) || !api.is_toggle_enabled("Major CDs")) return false;
        // Manual burst bypasses content policy, pull-size, and health gates,
        // but never bypasses either hard stop above.
        if (burst_now(api)) return true;
        if (state.encounter_burst_hold && !state.encounter_burst_go) return false;
        // Policy 1 = On Cooldown; policy 2 = Boss Only.
        if (cooldown_policy_ == 1) return true;
        if (cooldown_policy_ == 2) return state.is_boss || state.training_dummy;

        // Smart policy: bosses always qualify. Raid and M+ trash must live long
        // enough; M+ also requires meaningful enemies and enough combined HP.
        if (state.is_boss || state.training_dummy) return true;
        if (state.is_raid) return state.fight_ttd >= raid_cooldown_min_ttd_;
        if (state.is_mplus) {
            return state.meaningful_enemies >= mplus_cooldown_min_enemies_ &&
                state.fight_ttd >= mplus_cooldown_min_ttd_ &&
                state.pack_health_percent >= mplus_cooldown_min_pack_hp_;
        }
        return false;
    }

    // ---------------------------------------------------------------------
    // RATE-LIMITED TROUBLESHOOTING SNAPSHOTS
    // ---------------------------------------------------------------------
    // These helpers intentionally use the same public API checks as the action
    // lists. A DBG line therefore shows the exact inputs the rotation saw,
    // including transient GCD/castability and target-range results.
    static const char* debug_bool(bool value) {
        return value ? "1" : "0";
    }

    static std::string debug_mode_name(const CombatState& state) {
        if (state.is_mplus) return "mplus";
        if (state.is_raid) return "raid";
        return "solo";
    }

    // Names the active Elemental Blast stat aura. Built only inside the
    // rate-limited debug snapshot, so it costs one aura scan per debug line.
    std::string debug_elemental_blast_buff(const rotation_api::IRotationAPI& api,
                                           const CombatState& state) const
    {
        if (!state.buff_elemental_blast_stat) return "none";
        for (const auto& aura : api.get_buffs("player", false)) {
            if (!contains_name(aura.name, "Elemental Blast")) continue;
            if (contains_name(aura.name, "Critical")) return "crit";
            if (contains_name(aura.name, "Haste")) return "haste";
            if (contains_name(aura.name, "Mastery")) return "mastery";
            return "1";
        }
        return "1";
    }

    // Reports the first reason the live AoE list cannot reach an Earthquake,
    // using the real v2.3.2 conditions. Diagnostic only: this mirrors the APL,
    // it never influences it.
    std::string earthquake_gate_debug(const rotation_api::IRotationAPI& api,
                                      const CombatState& state) const
    {
        if (spellbook_.earthquake == 0) return "0:unknown";
        if (!api.is_aoe_enabled()) return "0:aoe_off";
        if (!state.use_aoe_list) return "0:targets";
        if (!can_cast(api, spellbook_.earthquake)) return "0:not_castable";
        if (!state.earthquake_safe) return "0:unsafe";

        // AOE 6: Lightning Rod spread.
        const int rod_threshold = 3 + (state.talent_elemental_blast ? 1 : 0);
        const bool rod_targets = state.enemies >= rod_threshold;
        const bool rod_buff = state.buff_elemental_blast_stat || !state.talent_elemental_blast;
        if (state.tempest_stacks < 2 && state.lightning_rods < state.enemies &&
            rod_targets && rod_buff)
        {
            return "1:rod_spread";
        }

        // AOE 8: tier free spender routes to Earthquake without EB talented.
        if (state.buff_overcharge_tier && !state.talent_elemental_blast) return "1:tier";

        // AOE 14: paid Earthquake exists only without Elemental Blast.
        if (!state.talent_elemental_blast) {
            if (state.bank_maelstrom) return "0:bank";
            const int chain_targets = std::min(5, std::max(1, state.enemies));
            const int gate = spender_deficit_ +
                (state.buff_stormkeeper ? chain_targets * (chain_targets + 2) : 0);
            return state.maelstrom_deficit < gate ? "1:normal" : "0:deficit";
        }

        if (state.tempest_stacks >= 2) return "0:tempest";
        if (state.lightning_rods >= state.enemies) return "0:rods_full";
        if (!rod_targets) return "0:eb_targets";
        if (!state.buff_elemental_blast_stat) return "0:eb_setup";
        return "0:eb_spender";
    }

    std::string debug_cooldown_gate(const rotation_api::IRotationAPI& api,
                                    const CombatState& state) const
    {
        if (!api.are_cooldowns_enabled()) return "main_toggle_off";
        if (!api.is_toggle_enabled("Major CDs")) return "major_cds_off";
        if (burst_now(api)) return "burst_now";
        if (state.encounter_burst_hold && !state.encounter_burst_go) {
            return "encounter_burst_hold";
        }
        if (cooldown_policy_ == 1) return "on_cooldown";
        if (cooldown_policy_ == 2) {
            return (state.is_boss || state.training_dummy)
                ? "boss_only_allowed" : "boss_only_hold";
        }
        if (state.is_boss || state.training_dummy) return "smart_boss_allowed";
        if (state.is_raid) {
            return state.fight_ttd >= raid_cooldown_min_ttd_
                ? "smart_raid_allowed" : "smart_raid_ttd_hold";
        }
        if (state.is_mplus) {
            if (state.meaningful_enemies < mplus_cooldown_min_enemies_) {
                return "smart_mplus_enemy_hold";
            }
            if (state.fight_ttd < mplus_cooldown_min_ttd_) {
                return "smart_mplus_ttd_hold";
            }
            if (state.pack_health_percent < mplus_cooldown_min_pack_hp_) {
                return "smart_mplus_pack_hp_hold";
            }
            return "smart_mplus_allowed";
        }
        return "smart_solo_hold";
    }

    std::string debug_spell_status(const rotation_api::IRotationAPI& api,
                                   uint32_t spell_id,
                                   const std::string& unit,
                                   bool check_range) const
    {
        std::ostringstream out;
        out << spell_id
            << ",c" << debug_bool(can_cast(api, spell_id));
        if (check_range) {
            out << ",r" << debug_bool(spell_id != 0 && !unit.empty() &&
                api.is_spell_in_range(spell_id, unit));
        }
        out << ",cd" << std::fixed << std::setprecision(1)
            << (spell_id != 0 ? api.get_spell_cooldown_remaining(spell_id) : -1.0);
        return out.str();
    }

    std::string debug_trinket_status(const rotation_api::IRotationAPI& api,
                                     int slot) const
    {
        const auto equipped = equipped_trinket(api, slot);
        if (!equipped || !equipped->is_valid()) {
            return "empty,mode" + std::to_string(trinket_mode(slot));
        }
        std::ostringstream out;
        out << equipped->item_name
            << '#' << equipped->item_id
            << ",spell" << equipped->spell.spell_id
            << ",usable" << debug_bool(equipped->is_usable)
            << ",can" << debug_bool(equipped->can_use())
            << ",cd" << std::fixed << std::setprecision(1)
            << equipped->cooldown.get_remaining(api.get_game_time())
            << ",bind" << debug_bool(api.has_item_keybind(equipped->item_id))
            << ",broken" << debug_bool(slot == 1 ? trinket_1_broken_ : trinket_2_broken_)
            << ",mode" << trinket_mode(slot);
        return out.str();
    }

    std::string debug_cast_snapshot(const rotation_api::IRotationAPI& api) const {
        const double now = api.get_game_time();
        const rotation_api::CastInfo info = current_cast_info(api, "player");
        std::ostringstream out;
        out << "DBG_CAST v2.3.7 spell="
            << (info.name.empty() ? "<empty>" : info.name)
            << '#' << info.spell_id
            << " active=" << debug_bool(info.is_active())
            << " elapsed=" << std::fixed << std::setprecision(2) << info.get_elapsed(now)
            << " remain=" << info.get_remaining(now)
            << " duration=" << info.get_duration()
            << " gcd=" << api.get_remaining_gcd()
            << " moving=" << debug_bool(api.is_player_moving());
        return out.str();
    }

    std::string debug_target_failure(const rotation_api::IRotationAPI& api,
                                     const CombatState& state) const
    {
        const bool exists = api.unit_exists("target");
        std::ostringstream out;
        out << "NO_TARGET v2.3.7 name="
            << (exists ? api.get_unit_name("target") : "<none>")
            << " exists=" << debug_bool(exists)
            << " dead=" << debug_bool(exists && api.unit_is_dead("target"))
            << " enemy=" << debug_bool(exists && api.unit_is_enemy("target"))
            << " attack=" << debug_bool(exists && api.unit_can_attack("target"))
            << " los=" << debug_bool(exists && api.get_unit_line_of_sight("target"))
            << " snapshot_target=" << (state.target.empty() ? "empty" : state.target)
            << " enemies=" << state.enemies
            << " gcd=" << std::fixed << std::setprecision(2) << api.get_remaining_gcd();
        return out.str();
    }

    std::string debug_stall_reason(const rotation_api::IRotationAPI& api,
                                   const CombatState& state,
                                   const std::string& prefix) const
    {
        std::ostringstream out;
        out << prefix << " v2.3.7"
            << " pend=" << debug_bool(damage_dispatch_.pending)
            << '/' << damage_dispatch_.spell_id
            << " supp=" << damage_dispatch_.suppressed_spell_id
            << " qwin=" << debug_bool(in_spell_queue_window(api))
            << " gcd=" << std::fixed << std::setprecision(2) << api.get_remaining_gcd()
            << " lock=" << (state.gcd_desync ? "desync" : (state.global_lock ? "gcd" : "0"))
            << "/" << std::setprecision(2) << state.global_lock_remaining
            << " move=" << debug_bool(state.moving)
            << " target=" << debug_bool(can_damage_unit(api, state.target))
            << " mode=" << debug_mode_name(state)
            << " cd_gate=" << debug_cooldown_gate(api, state)
            << " LB{" << debug_spell_status(api, spellbook_.lightning_bolt, state.target, true) << '}'
            << " CL{" << debug_spell_status(api, spellbook_.chain_lightning, state.target, true) << '}'
            << " LvB{" << debug_spell_status(api, spellbook_.lava_burst, state.target, true) << '}'
            << " VB{" << debug_spell_status(api, spellbook_.voltaic_blaze, state.target, true) << '}';
        return out.str();
    }

    std::string debug_combat_snapshot(const rotation_api::IRotationAPI& api,
                                      const CombatState& state) const
    {
        const bool target_exists = api.unit_exists("target");
        std::ostringstream out;
        // Queue window sizes come straight from Titan so the next log can show
        // what it actually grants instead of an assumed value.
        out << "DBG v2.3.7"
            << " q=" << std::fixed << std::setprecision(2)
            << api.get_rotation_spell_queue_window()
            << '/' << api.get_in_game_spell_queue_window()
            << " qwin=" << debug_bool(in_spell_queue_window(api))
            << " qcast=" << debug_bool(queue_window_casting_)
            << " target="
            << (target_exists ? api.get_unit_name("target") : "<none>")
            << "[ex" << debug_bool(target_exists)
            << ",dead" << debug_bool(target_exists && api.unit_is_dead("target"))
            << ",enemy" << debug_bool(target_exists && api.unit_is_enemy("target"))
            << ",atk" << debug_bool(target_exists && api.unit_can_attack("target"))
            << ",los" << debug_bool(target_exists && api.get_unit_line_of_sight("target")) << ']'
            << " gcd=" << std::fixed << std::setprecision(2) << api.get_remaining_gcd()
            << " lock=" << (state.gcd_desync ? "desync" : (state.global_lock ? "gcd" : "0"))
            << "/" << std::setprecision(2) << state.global_lock_remaining
            << " move=" << debug_bool(state.moving)
            << " mode=" << debug_mode_name(state)
            << " enemies=" << state.enemies << '/' << state.meaningful_enemies
            << " aoe=" << debug_bool(state.use_aoe_list)
            << " np=" << state.np_raw << '/' << state.np_damageable
            << '/' << state.np_hostile_flags << '/' << state.np_dummy
            << '/' << state.np_effective
            << " npc=" << state.np_count_api << '/' << state.np_count_any
            << " rods=" << state.lightning_rods << '/' << state.enemies
            << " ebbuff=" << debug_elemental_blast_buff(api, state)
            << " eq=" << earthquake_gate_debug(api, state)
            << " mael=" << state.maelstrom << '/' << state.maelstrom_max
            << " ttd=" << std::setprecision(1) << state.fight_ttd
            << " packhp=" << state.pack_health_percent
            << " stable=" << debug_bool(state.pack_stable)
            << '/' << std::setprecision(1) << state.pack_stable_for
            << " melee=" << state.melee_ratio
            << " eqsafe=" << debug_bool(state.earthquake_safe)
            << " eqgate=" << (state.talent_elemental_blast ? "eb3" : "eq3")
            << " forecast=" << state.forecast_builder_gain
            << '/' << debug_bool(state.forecast_overcap)
            << " event=" << state.encounter_danger_eta
            << '/' << state.encounter_burst_eta
            << " cd_gate=" << debug_cooldown_gate(api, state)
            << "[main" << debug_bool(api.are_cooldowns_enabled())
            << ",mini" << debug_bool(api.is_toggle_enabled("Mini CDs"))
            << '/' << debug_bool(mini_cooldowns_allowed(api))
            << ",major" << debug_bool(api.is_toggle_enabled("Major CDs"))
            << ",burst" << debug_bool(burst_now(api))
            << ",policy" << cooldown_policy_
            << ",allowed" << debug_bool(cooldowns_allowed(api, state)) << ']'
            << " opener=" << debug_bool(opener_active_)
            << '[' << debug_bool(opener_stormkeeper_done_)
            << debug_bool(opener_swiftness_done_)
            << debug_bool(opener_voltaic_done_)
            << debug_bool(opener_ascendance_done_) << ']'
            << " SK{" << debug_spell_status(api, spellbook_.stormkeeper, "player", false)
            << ",buff" << debug_bool(state.buff_stormkeeper) << '}'
            << " Asc{" << debug_spell_status(api, spellbook_.ascendance, "player", false)
            << ",buff" << debug_bool(state.buff_ascendance) << '}'
            << " LB{" << debug_spell_status(api, spellbook_.lightning_bolt, state.target, true) << '}'
            << " CL{" << debug_spell_status(api, spellbook_.chain_lightning, state.target, true) << '}'
            << " LvB{" << debug_spell_status(api, spellbook_.lava_burst, state.target, true) << '}'
            << " VB{" << debug_spell_status(api, spellbook_.voltaic_blaze, state.target, true) << '}'
            << " EB{" << debug_spell_status(api, spellbook_.elemental_blast, state.target, true) << '}'
            << " EQ{" << debug_spell_status(api, spellbook_.earthquake, state.target,
                state.talent_earthquake)
            << ",targeted" << debug_bool(state.talent_earthquake) << '}'
            << " T1{" << debug_trinket_status(api, 1) << '}'
            << " T2{" << debug_trinket_status(api, 2) << '}';
        return out.str();
    }

    // Estimated time until Ascendance should actually be used, including smart
    // policy and Stormkeeper synchronization.
    // Rotation maintenance uses this instead of looking at raw cooldown alone.
    double ascendance_use_eta(const rotation_api::IRotationAPI& api,
                              const CombatState& state) const
    {
        if (!cooldowns_allowed(api, state) || spellbook_.ascendance == 0) return 999.0;

        const double gcd = std::max(0.1, api.get_remaining_gcd());
        const double ascendance_cd = api.get_spell_cooldown_remaining(spellbook_.ascendance);
        if (ascendance_cd > gcd) return ascendance_cd;
        if (burst_now(api) || state.fight_ttd < 20.0) return 0.0;

        const double stormkeeper_cd = spellbook_.stormkeeper != 0
            ? api.get_spell_cooldown_remaining(spellbook_.stormkeeper) : 999.0;
        if (mini_cooldowns_allowed(api) &&
            stormkeeper_cd > gcd && stormkeeper_cd <= ascendance_stormkeeper_window_ &&
            !hold_would_lose_use(api, state, spellbook_.ascendance,
                stormkeeper_cd, 180.0))
        {
            return stormkeeper_cd;
        }
        return 0.0;
    }

    // The list contains every supported damage racial, but only the character's
    // learned racial resolves to a castable spell ID.
    RotationAction racial_action(const rotation_api::IRotationAPI& api) {
        const uint32_t racials[] = {
            spellbook_.blood_fury,
            spellbook_.berserking,
            spellbook_.fireblood,
            spellbook_.ancestral_call
        };
        for (uint32_t racial : racials) {
            if (can_cast(api, racial)) return spell(racial, "player", "Damage racial");
        }
        return no_action("No racial");
    }

    // ---------------------------------------------------------------------
    // SLOT-AWARE ON-USE TRINKETS
    // ---------------------------------------------------------------------
    // Titan exposes the actual equipped item snapshot for each trinket slot.
    // Passive trinkets have no usable item spell and are skipped automatically,
    // so configuring both slots never causes the passive slot to be spammed.
    static std::optional<rotation_api::EquippedItem> equipped_trinket(
        const rotation_api::IRotationAPI& api, int slot)
    {
        return slot == 1 ? api.get_trinket1() : api.get_trinket2();
    }

    bool trinket_slot_broken(int slot) const {
        return slot == 1 ? trinket_1_broken_ : trinket_2_broken_;
    }

    bool trinket_ready(const rotation_api::IRotationAPI& api, int slot) const {
        if (trinket_slot_broken(slot)) return false;
        const auto equipped = equipped_trinket(api, slot);
        if (!equipped || !equipped->is_valid() || !equipped->spell.has_spell() ||
            !equipped->can_use())
        {
            return false;
        }
        const double now = api.get_game_time();
        const double pending_until = slot == 1 ? trinket_1_pending_until_ : trinket_2_pending_until_;
        const double last_success = slot == 1 ? last_trinket_1_success_time_ : last_trinket_2_success_time_;
        const double last_attempt = slot == 1 ? last_trinket_1_attempt_time_ : last_trinket_2_attempt_time_;
        if (still_pending(pending_until, now) ||
            recently_attempted(last_attempt, kTrinketAttempt, now) ||
            recently_succeeded(last_success, kTrinketSettle, now))
        {
            return false;
        }
        return true;
    }

    RotationAction use_trinket_slot(const rotation_api::IRotationAPI& api,
                                    int slot,
                                    const std::string& reason,
                                    bool bypass_attempt_latch = false)
    {
        if (trinket_slot_broken(slot)) return no_action("Trinket slot broken");
        const auto equipped = equipped_trinket(api, slot);
        if (!equipped || !equipped->is_valid()) return no_action("Trinket snapshot unavailable");

        if (slot == 1) {
            if (equipped->item_id != trinket_1_item_id_) {
                trinket_1_item_id_ = equipped->item_id;
                trinket_1_broken_ = false;
            }
        } else if (equipped->item_id != trinket_2_item_id_) {
            trinket_2_item_id_ = equipped->item_id;
            trinket_2_broken_ = false;
        }

        if (!equipped->spell.has_spell() || !equipped->can_use()) {
            return no_action("Trinket unavailable");
        }

        const double now = api.get_game_time();
        double& pending_until = slot == 1 ? trinket_1_pending_until_ : trinket_2_pending_until_;
        const double last_success = slot == 1 ? last_trinket_1_success_time_ : last_trinket_2_success_time_;
        const double last_attempt = slot == 1 ? last_trinket_1_attempt_time_ : last_trinket_2_attempt_time_;
        // The synchronization barrier owns its own timing evidence, so a
        // verified failed press is allowed one retry inside the attempt latch.
        if (!bypass_attempt_latch &&
            setup_action_blocked(0x80000000u + static_cast<uint32_t>(slot),
                pending_until, last_success, kTrinketSettle,
                last_attempt, kTrinketAttempt, now, true)) {
            return no_action("Trinket settling");
        }

        // Titan executes item/trinket actions through an action-bar keybind.
        // A missing executable binding is a session-level failure, not a retry.
        if (!api.has_item_keybind(equipped->item_id)) {
            if (slot == 1) trinket_1_broken_ = true;
            else trinket_2_broken_ = true;
            trinket_break_message_ =
                "Trinket " + std::to_string(slot) + " keybind missing; slot disabled (" +
                equipped->item_name + ")";
            return no_action("Trinket keybind missing");
        }

        if (slot == 1) last_trinket_1_attempt_time_ = now;
        else last_trinket_2_attempt_time_ = now;
        pending_until = now + kSetupPendingWindow;
        note_input_dispatch(api);

        const std::string note = reason + " - slot " + std::to_string(slot) +
            " " + equipped->item_name;
        return item(equipped->item_id, "", note);
    }

    int trinket_mode(int slot) const {
        return slot == 1 ? trinket_1_mode_ : trinket_2_mode_;
    }

    static bool likely_buff_trinket(const rotation_api::EquippedItem& item) {
        const std::string text = item.item_name + " " + item.spell.name;
        static constexpr const char* hints[] = {
            "intellect", "haste", "mastery", "critical", "empower", "focus",
            "inspiration", "potential", "lens", "emblem", "idol", "badge"
        };
        for (const char* hint : hints) {
            if (contains_name(text, hint)) return true;
        }
        return false;
    }

    // Modes 1, 3, and 4 do not wait for Ascendance. They still belong to the
    // Major category, so Titan's master Cooldowns toggle and Major CDs remain
    // final. Mode 2 never fires here; it is exclusive to the burst package.
    RotationAction independent_trinket_action(const rotation_api::IRotationAPI& api,
                                               const CombatState& state)
    {
        if (!damage_cooldown_master_enabled(api) || !api.is_toggle_enabled("Major CDs")) {
            return no_action("Major trinkets held");
        }

        for (int slot = 1; slot <= 2; ++slot) {
            const int mode = trinket_mode(slot);
            if (mode == 0 || mode == 2) continue;
            const auto equipped = equipped_trinket(api, slot);
            const bool enough_time = state.is_boss || state.training_dummy ||
                state.fight_ttd >= trinket_min_ttd_;
            const double ascendance_eta = ascendance_use_eta(api, state);
            const bool hold_smart_buff = mode == 1 && equipped &&
                likely_buff_trinket(*equipped) && ascendance_eta > 0.0 &&
                ascendance_eta <= 15.0 && state.fight_ttd > ascendance_eta + 8.0;
            const bool smart_use = mode == 1 && enough_time &&
                !hold_smart_buff && !state.encounter_burst_hold;
            const bool boss_only = mode == 3 && (state.is_boss || state.training_dummy);
            const bool on_cooldown = mode == 4;
            if (smart_use || boss_only || on_cooldown) {
                if (RotationAction action = use_trinket_slot(api, slot,
                        on_cooldown ? "Trinket on cooldown" :
                        smart_use ? "Smart trinket use" : "Boss-only trinket");
                    !action.is_none())
                {
                    return action;
                }
            }
        }
        return no_action("No independent trinket");
    }

    // Runtime evidence for the barrier below (v2.3.4 log, Emberwing Feather):
    //   330954.04 item pressed with no cast active -> 330954.60 cd119.5 (used)
    //   331078.39 item pressed 0.42 s before a Chain Lightning cast completed
    //             -> can1/cd0.0 for the remaining 23 s of the log (never used)
    // Titan reported [OK] both times, so only equipped-item state or a trinket
    // spell event can prove activation.
    bool major_trinket_activated(const rotation_api::IRotationAPI& api,
                                 int slot,
                                 const rotation_api::EquippedItem& equipped) const
    {
        const double last_success = slot == 1
            ? last_trinket_1_success_time_ : last_trinket_2_success_time_;
        if (last_success > 0.0 && last_success >= major_trinket_sync_.dispatched_at - 0.05) {
            return true;
        }
        if (equipped.cooldown.get_remaining(api.get_game_time()) > 0.0) return true;
        // The barrier only arms while the item reports can_use(), so losing that
        // state without an equipment change is consistent with activation.
        return !equipped.can_use();
    }

    // An item press only reaches the game when the player is genuinely idle.
    // Both cast signals are required: the failing press happened while strict
    // casting already reported free and CastInfo still showed the cast running.
    static bool trinket_press_window_open(const rotation_api::IRotationAPI& api) {
        if (player_cast_active_strict(api)) return false;
        const rotation_api::CastInfo info = current_cast_info(api, "player");
        if (!info.is_active()) return true;
        // A CastInfo bar with no time left is stale and must not hold the item.
        return info.get_remaining(api.get_game_time()) <= 0.0;
    }

    void clear_major_trinket_sync() {
        major_trinket_sync_ = {};
    }

    void log_trinket_sync_failure(const rotation_api::IRotationAPI& api,
                                  int slot,
                                  const rotation_api::EquippedItem* equipped,
                                  const char* cause)
    {
        if (telemetry_combat_active_) ++telemetry_.trinket_sync_failures;
        if (!debug_diagnostics_) return;
        const double now = api.get_game_time();
        std::ostringstream out;
        out << "TRINKET_SYNC_FAIL slot=" << slot
            << " item=" << (equipped ? equipped->item_id : major_trinket_sync_.item_id)
            << '/' << (equipped ? equipped->item_name : std::string("<gone>"))
            << " cause=" << cause
            << " attempts=" << major_trinket_sync_.attempts
            << " elapsed=" << format_seconds(now - major_trinket_sync_.armed_at)
            << " since_press=" << format_seconds(now - major_trinket_sync_.dispatched_at)
            << " bind=" << debug_bool(equipped && api.has_item_keybind(equipped->item_id))
            << " can_use=" << debug_bool(equipped && equipped->can_use())
            << " usable=" << debug_bool(equipped && equipped->is_usable)
            << " cd=" << format_seconds(equipped
                    ? equipped->cooldown.get_remaining(now) : 0.0)
            << " cast_strict=" << debug_bool(player_cast_active_strict(api))
            << " cast_info=" << debug_bool(current_cast_info(api, "player").is_active());
        queue_damage_debug(out.str());
    }

    // Bounded failure: the slot stops holding the package, but nothing is marked
    // broken. A missed press is not a missing keybind.
    RotationAction abandon_major_trinket(const rotation_api::IRotationAPI& api,
                                         const rotation_api::EquippedItem* equipped,
                                         const char* cause)
    {
        const int slot = major_trinket_sync_.slot;
        if (slot == 1 || slot == 2) {
            log_trinket_sync_failure(api, slot, equipped, cause);
            major_trinket_bypass_until_[slot - 1] = api.get_game_time() + kTrinketSyncBypass;
            major_trinket_bypass_item_[slot - 1] = major_trinket_sync_.item_id;
        }
        clear_major_trinket_sync();
        return no_action("Trinket sync abandoned");
    }

    bool major_trinket_bypassed(int slot, uint32_t item_id, double now) const {
        const int index = slot - 1;
        if (index < 0 || index > 1) return false;
        if (major_trinket_bypass_item_[index] != item_id) return false;
        return now < major_trinket_bypass_until_[index];
    }

    // Mode 2 is a synchronization barrier, not a suggestion. The Major package
    // does not advance to Nature's Swiftness, racials, or Ascendance until the
    // item is proven active, retried once, or written off.
    RotationAction major_synced_trinket_action(const rotation_api::IRotationAPI& api,
                                               const CombatState& state)
    {
        const double now = api.get_game_time();

        if (major_trinket_sync_.slot != 0) {
            const int slot = major_trinket_sync_.slot;
            const auto equipped = equipped_trinket(api, slot);
            if (!equipped || !equipped->is_valid() ||
                equipped->item_id != major_trinket_sync_.item_id)
            {
                clear_major_trinket_sync();
                return no_action("Trinket changed during sync");
            }

            if (major_trinket_activated(api, slot, *equipped)) {
                if (telemetry_combat_active_) ++telemetry_.trinket_sync_confirms;
                if (debug_diagnostics_) {
                    queue_damage_debug("TRINKET_SYNC_OK slot=" + std::to_string(slot) +
                        " item=" + std::to_string(equipped->item_id) +
                        " attempts=" + std::to_string(major_trinket_sync_.attempts) +
                        " delay=" + format_seconds(now - major_trinket_sync_.dispatched_at) +
                        " cd=" + format_seconds(equipped->cooldown.get_remaining(now)));
                }
                clear_major_trinket_sync();
                return no_action("Trinket confirmed");
            }

            if ((now - major_trinket_sync_.armed_at) >= kTrinketSyncHoldMax) {
                return abandon_major_trinket(api, &*equipped,
                    major_trinket_sync_.attempts >= 2 ? "retry_failed" : "hold_expired");
            }

            // Titan publishes item data every 500 ms, so a press is not judged
            // until a full publication interval has passed.
            if ((now - major_trinket_sync_.dispatched_at) < kTrinketSyncSettle) {
                return wait_action(kTrinketSyncPollMs, "TRINKET_SYNC_SETTLE");
            }

            if (major_trinket_sync_.attempts >= 2) {
                return abandon_major_trinket(api, &*equipped, "retry_failed");
            }

            if (!trinket_press_window_open(api)) {
                return wait_action(kTrinketSyncPollMs, "TRINKET_SYNC_CAST");
            }

            // The press demonstrably failed: settled, still usable, no cooldown.
            if (RotationAction action = use_trinket_slot(api, slot,
                    "Trinket retry with Major CDs", true); !action.is_none())
            {
                ++major_trinket_sync_.attempts;
                major_trinket_sync_.dispatched_at = now;
                if (telemetry_combat_active_) ++telemetry_.trinket_sync_retries;
                return action;
            }
            return abandon_major_trinket(api, &*equipped, "retry_rejected");
        }

        if (!cooldowns_allowed(api, state)) return no_action("Major trinkets held");

        for (int slot = 1; slot <= 2; ++slot) {
            if (trinket_mode(slot) != 2) continue;
            if (trinket_slot_broken(slot)) continue;
            const auto equipped = equipped_trinket(api, slot);
            if (!equipped || !equipped->is_valid() || !equipped->spell.has_spell()) continue;
            if (!equipped->can_use()) continue;
            if (major_trinket_bypassed(slot, equipped->item_id, now)) continue;

            // Never press an item into the tail of a hardcast.
            if (!trinket_press_window_open(api)) {
                if (major_trinket_sync_.armed_at < 0.0) {
                    major_trinket_sync_.armed_at = now;
                }
                if ((now - major_trinket_sync_.armed_at) >= kTrinketSyncHoldMax) {
                    major_trinket_sync_.slot = slot;
                    major_trinket_sync_.item_id = equipped->item_id;
                    return abandon_major_trinket(api, &*equipped, "no_free_window");
                }
                return wait_action(kTrinketSyncPollMs, "TRINKET_SYNC_CAST");
            }

            if (RotationAction action = use_trinket_slot(api, slot, "Trinket with Major CDs");
                !action.is_none())
            {
                major_trinket_sync_.slot = slot;
                major_trinket_sync_.item_id = equipped->item_id;
                major_trinket_sync_.dispatched_at = now;
                if (major_trinket_sync_.armed_at < 0.0) major_trinket_sync_.armed_at = now;
                major_trinket_sync_.attempts = 1;
                return action;
            }
        }

        clear_major_trinket_sync();
        return no_action("No Major-synced trinket");
    }

    // Opening portion shared by ST and AoE:
    // 1. Stormkeeper unless Ascendance is inside the configured hold window.
    // 2. Ancestral Swiftness when talented and enabled.
    RotationAction list_opener_cooldown_action(const rotation_api::IRotationAPI& api,
                                               const CombatState& state)
    {
        if (!mini_cooldowns_allowed(api)) return no_action("Mini CDs held");

        const double ascendance_cd = spellbook_.ascendance != 0
            ? api.get_spell_cooldown_remaining(spellbook_.ascendance) : 999.0;
        const double gcd = std::max(0.1, api.get_remaining_gcd());
        const bool fight_ending = state.fight_ttd < 20.0;
        const bool force_burst = burst_now(api);
        const bool major_allowed = cooldowns_allowed(api, state);

        const double now = api.get_game_time();
        if (!still_pending(stormkeeper_pending_until_, now) &&
            !recently_succeeded(last_stormkeeper_success_time_, kStormkeeperSettle, now) &&
            !recently_attempted(last_stormkeeper_dispatch_time_, kStormkeeperAttempt, now) &&
            can_cast(api, spellbook_.stormkeeper))
        {
            const bool hold_for_ascendance = major_allowed && !force_burst && !fight_ending &&
                ascendance_cd > gcd && ascendance_cd <= stormkeeper_hold_for_asc_ &&
                !hold_would_lose_use(api, state, spellbook_.stormkeeper,
                    ascendance_cd, 60.0);
            if (hold_for_ascendance) {
                return no_action("Stormkeeper held briefly for Ascendance");
            }
            if (RotationAction action = dispatch_player_setup(api, spellbook_.stormkeeper,
                    stormkeeper_pending_until_, last_stormkeeper_success_time_, kStormkeeperSettle,
                    last_stormkeeper_dispatch_time_, kStormkeeperAttempt,
                    force_burst ? "Stormkeeper - Burst Now" : "Stormkeeper synchronized use");
                !action.is_none())
            {
                return action;
            }
        }

        if (state.hero_tree == HeroTree::Farseer && use_ancestral_swiftness_) {
            if (RotationAction action = dispatch_player_setup(
                    api, spellbook_.ancestral_swiftness, ancestral_swiftness_pending_until_,
                    last_ancestral_swiftness_success_time_, kAncestralSwiftnessSettle,
                    last_ancestral_swiftness_dispatch_time_, kAncestralSwiftnessAttempt,
                    "Ancestral Swiftness"); !action.is_none())
            {
                return action;
            }
        }

        return no_action("No list opener cooldown");
    }

    // Complete Farseer Ascendance package. It is called directly after the
    // Stormkeeper / Ancestral Swiftness block so the burst order is:
    //   1 target: SK -> AS -> trinkets -> Asc;
    //   2+ targets: SK -> AS -> VB -> trinkets -> Asc.
    // Voltaic Blaze is inserted only with Purging Flames. Nature's Swiftness
    // and damage racials are kept beside Ascendance instead of firing globally.
    RotationAction ascendance_burst_action(const rotation_api::IRotationAPI& api,
                                           const CombatState& state)
    {
        if (!cooldowns_allowed(api, state)) {
            clear_major_trinket_sync();
            return no_action("Cooldowns held");
        }

        const double stormkeeper_cd = spellbook_.stormkeeper != 0
            ? api.get_spell_cooldown_remaining(spellbook_.stormkeeper) : 999.0;
        const double gcd = std::max(0.1, api.get_remaining_gcd());
        const bool fight_ending = state.fight_ttd < 20.0;
        const bool force_burst = burst_now(api);

        const double now = api.get_game_time();
        const bool ascendance_eligible = can_cast(api, spellbook_.ascendance);
        if (setup_action_blocked(spellbook_.ascendance, ascendance_pending_until_,
                last_ascendance_success_time_, kAscendanceSettle,
                last_ascendance_dispatch_time_, kAscendanceAttempt, now, ascendance_eligible))
        {
            // The package has already reached Ascendance, so the Mode-2 barrier
            // is finished for this burst.
            clear_major_trinket_sync();
            return no_action("Ascendance settling");
        }
        if (!ascendance_eligible) {
            clear_major_trinket_sync();
            return no_action("Ascendance not ready");
        }

        const bool hold_for_stormkeeper = mini_cooldowns_allowed(api) &&
            !force_burst && !fight_ending &&
            stormkeeper_cd > gcd && stormkeeper_cd <= ascendance_stormkeeper_window_ &&
            !hold_would_lose_use(api, state, spellbook_.ascendance,
                stormkeeper_cd, 180.0);
        if (hold_for_stormkeeper) {
            return no_action("Ascendance held briefly for Stormkeeper");
        }

        // At 2+ targets, Purging Flames should be prepared immediately before
        // Ascendance. Pure ST deliberately skips this extra setup global.
        const bool recent_voltaic_blaze =
            (api.get_game_time() - last_voltaic_blaze_success_time_) <= 4.0;
        if (state.enemies >= 2 && state.talent_purging_flames &&
            !state.buff_purging_flames &&
            !recent_voltaic_blaze)
        {
            if (RotationAction action = dispatch_voltaic_setup(
                    api, state, "Voltaic Blaze before Ascendance"); !action.is_none())
            {
                return action;
            }
            if (still_pending(voltaic_setup_pending_until_, api.get_game_time()) ||
                recently_attempted(last_voltaic_setup_dispatch_time_, kVoltaicSetupAttempt,
                    api.get_game_time()) ||
                recently_succeeded(last_voltaic_blaze_success_time_, kVoltaicSetupSettle,
                    api.get_game_time()))
            {
                return no_action("Voltaic Blaze setup settling");
            }
        }

        // On-use trinkets configured With Major CDs fire after required DoT/VB
        // setup but before Nature's Swiftness, racials, and Ascendance. This is
        // a barrier: while it returns an action the package cannot advance.
        if (RotationAction action = major_synced_trinket_action(api, state); !action.is_none()) {
            return action;
        }

        if (use_natures_swiftness_ && can_cast(api, spellbook_.natures_swiftness)) {
            return spell(spellbook_.natures_swiftness, "player", "Nature's Swiftness with Ascendance");
        }

        if (use_racials_) {
            if (RotationAction action = racial_action(api); !action.is_none()) return action;
        }

        return dispatch_player_setup(api, spellbook_.ascendance, ascendance_pending_until_,
            last_ascendance_success_time_, kAscendanceSettle,
            last_ascendance_dispatch_time_, kAscendanceAttempt,
            force_burst ? "Ascendance - Burst Now" : "Ascendance synchronized burst");

    }

    // -------------------------------------------------------------------------
    // MOVEMENT PRIORITY (NO SPIRITWALKER'S GRACE ACTIVE)
    // -------------------------------------------------------------------------
    // 1 SWG -> 2 Tempest -> 3 avoid resource cap -> 4 Lava Surge ->
    // 5 Voltaic Blaze -> 6 Flame Shock -> 7 Frost Shock filler.
    RotationAction movement_action(const rotation_api::IRotationAPI& api, const CombatState& state) {
        // Use SWG only after the configured continuous-movement delay.
        if (state.movement_time >= spiritwalkers_grace_move_delay_ &&
            can_cast(api, spellbook_.spiritwalkers_grace))
        {
            return spell(spellbook_.spiritwalkers_grace, "player", "Spiritwalker's Grace");
        }

        // Tempest is the highest-value natural instant available while moving.
        if (can_cast(api, spellbook_.tempest)) {
            if (RotationAction action = cast_damage(api, spellbook_.tempest,
                    best_lightning_rod_target(api, state),
                    "Moving Tempest / spread Lightning Rod"); !action.is_none())
            {
                return action;
            }
        }

        // Prevent movement from causing a Maelstrom overcap, except while the
        // M+ end-of-pack banking rule is deliberately saving that resource.
        if (!state.bank_maelstrom && state.maelstrom_deficit <= spender_deficit_) {
            const bool use_earthquake = state.use_aoe_list && state.enemies >= 4;
            if (RotationAction action = cast_spender(
                    api, state, use_earthquake, "Moving near-cap spender"); !action.is_none())
            {
                return action;
            }
        }

        // Lava Surge makes Lava Burst instant; require Flame Shock on target.
        if (!last_damage_was_lava_burst() && state.buff_lava_surge &&
            can_cast(api, spellbook_.lava_burst) && has_flame_shock(api, state.target))
        {
            if (RotationAction action = cast_damage(api, spellbook_.lava_burst, state.target,
                    "Moving Lava Surge"); !action.is_none())
            {
                return action;
            }
        }

        if (can_cast(api, spellbook_.voltaic_blaze) &&
            !still_pending(voltaic_setup_pending_until_, api.get_game_time()))
        {
            if (RotationAction action = cast_damage(api, spellbook_.voltaic_blaze, state.target,
                    "Moving Voltaic Blaze"); !action.is_none())
            {
                return action;
            }
        }

        const std::string flame_target = best_flame_shock_target(api, state, true);
        if (!flame_target.empty() && can_cast(api, spellbook_.flame_shock)) {
            if (RotationAction action = cast_damage(api, spellbook_.flame_shock, flame_target,
                    "Moving Flame Shock"); !action.is_none())
            {
                return action;
            }
        }

        if (can_cast(api, spellbook_.frost_shock)) {
            if (RotationAction action = cast_damage(api, spellbook_.frost_shock, state.target,
                    "Moving Frost Shock"); !action.is_none())
            {
                return action;
            }
        }

        return no_action("No movement action");
    }

    // -------------------------------------------------------------------------
    // ONE- AND TWO-TARGET PRIORITY (RAID BOSSES AND SMALL M+ PULLS)
    // -------------------------------------------------------------------------
    // This order follows the live Midnight APL. Remember: the first usable
    // block wins, so the numbered comments below are the actual priority.
    RotationAction single_target_action(const rotation_api::IRotationAPI& api, const CombatState& state) {
        const double ascendance_eta = ascendance_use_eta(api, state);

        // ST 1: Stormkeeper / Ancestral Swiftness opener.
        if (RotationAction action = list_opener_cooldown_action(api, state); !action.is_none()) {
            return action;
        }

        // ST 2: normal pandemic Flame Shock maintenance. At two targets the
        // same list may apply a second copy while Chain Lightning is the filler.
        const std::string flame_target = best_flame_shock_target(api, state, state.enemies == 2);
        if (!flame_target.empty() &&
            !state.buff_master_of_the_elements &&
            ascendance_eta > 5.0 &&
            can_cast(api, spellbook_.flame_shock))
        {
            if (RotationAction action = cast_damage(api, spellbook_.flame_shock, flame_target,
                    "Maintain Flame Shock"); !action.is_none())
            {
                return action;
            }
        }

        // ST 3: just before Fire Elemental fades, refresh the existing Flame
        // Shock with the least time remaining. SimC uses <2 sec at one target
        // and <8 sec at two targets (6 * active_enemies - 4).
        const double fire_elemental_refresh_window = 6.0 * state.enemies - 4.0;
        if (!state.buff_master_of_the_elements && !state.buff_ascendance &&
            state.fire_elemental_remaining > 0.0 &&
            state.fire_elemental_remaining < fire_elemental_refresh_window &&
            can_cast(api, spellbook_.flame_shock))
        {
            const std::string fade_target = lowest_flame_shock_target(api, state);
            if (!fade_target.empty()) {
                if (RotationAction action = cast_damage(api, spellbook_.flame_shock, fade_target,
                        "Refresh lowest Flame Shock before Fire Elemental fades");
                    !action.is_none())
                {
                    return action;
                }
            }
        }

        // ST 4: ordinary Voltaic Blaze maintenance before Ascendance. It is a
        // normal DoT refresh only when Asc is >5 sec away, or a Purging Flames
        // button at exactly two targets.
        if (can_cast(api, spellbook_.voltaic_blaze) &&
            !still_pending(voltaic_setup_pending_until_, api.get_game_time()) &&
            !state.buff_master_of_the_elements &&
            ((flame_shock_refreshable(api, state.target) && ascendance_eta > 5.0) ||
             (state.talent_purging_flames && state.enemies == 2)))
        {
            if (RotationAction action = cast_damage(api, spellbook_.voltaic_blaze, state.target,
                    "Voltaic Blaze maintenance"); !action.is_none())
            {
                return action;
            }
        }

        // ST 5: synchronized Ascendance package after required DoT/VB setup.
        if (RotationAction action = ascendance_burst_action(api, state); !action.is_none()) {
            return action;
        }

        // ST 6: consume the current tier interaction before another builder can
        // overwrite/waste it.
        if (state.buff_flowing_elements && state.buff_overcharge_tier) {
            if (RotationAction action = cast_spender(api, state, false, "Consume tier proc"); !action.is_none()) {
                return action;
            }
        }

        // These flags combine charge protection, procs, MotE setup, and being
        // close enough to a spender that Lava Burst should precede it.
        const bool lava_burst_ready = !last_damage_was_lava_burst() &&
            can_cast(api, spellbook_.lava_burst) && has_flame_shock(api, state.target);
        const bool lava_burst_charge_cap = spellbook_.lava_burst != 0 &&
            api.get_spell_fractional_charges(spellbook_.lava_burst) > 1.8;
        const bool spender_soon = state.maelstrom_deficit <= 45;
        const bool st_voltaic_returning = spellbook_.voltaic_blaze != 0 &&
            api.get_spell_cooldown_remaining(spellbook_.voltaic_blaze) < 2.0;
        const bool purging_lava_window = state.buff_purging_flames &&
            state.enemies == 2 && state.flame_shocks >= 2 && st_voltaic_returning;
        const bool proc_lava_burst = state.buff_lava_surge || state.buff_flowing_elements ||
            state.power_of_the_maelstrom_stacks >= 2 || purging_lava_window;

        // ST 7: Lava Burst for Master of the Elements, proc consumption, or
        // charge-cap protection. Do not use it when a spender must happen now.
        if (lava_burst_ready && state.maelstrom_deficit > spender_deficit_) {
            if (state.talent_master_of_the_elements && !state.buff_master_of_the_elements &&
                (lava_burst_charge_cap || spender_soon || proc_lava_burst))
            {
                if (RotationAction action = cast_damage(api, spellbook_.lava_burst, state.target,
                        "Lava Burst for Master of the Elements"); !action.is_none())
                {
                    return action;
                }
            }
            if (!state.talent_master_of_the_elements && (lava_burst_charge_cap || proc_lava_burst)) {
                if (RotationAction action = cast_damage(api, spellbook_.lava_burst, state.target,
                        "Lava Burst proc / charge protection"); !action.is_none())
                {
                    return action;
                }
            }
        }

        // ST 8: consume Tempest with MotE when possible.
        if (can_cast(api, spellbook_.tempest) &&
            (state.buff_master_of_the_elements || !state.talent_master_of_the_elements))
        {
            if (RotationAction action = cast_damage(api, spellbook_.tempest,
                    best_lightning_rod_target(api, state),
                    "Tempest with Master of the Elements"); !action.is_none())
            {
                return action;
            }
        }

        // ST 9: Stormkeeper Lightning Bolt only when MotE is up and Tempest is
        // talented, matching live SimC. Forecast is diagnostic-only and is not
        // a gate. If this line does not fire, later fillers still consume LB.
        if (state.buff_stormkeeper &&
            state.buff_master_of_the_elements &&
            state.talent_tempest &&
            can_cast(api, spellbook_.lightning_bolt))
        {
            if (RotationAction action = cast_damage(api, spellbook_.lightning_bolt, state.target,
                    "Stormkeeper Lightning Bolt"); !action.is_none())
            {
                return action;
            }
        }

        // ST 10: Elemental Blast/Earth Shock with MotE or near Maelstrom cap.
        if (!state.bank_maelstrom &&
            (state.buff_master_of_the_elements || state.maelstrom_deficit < spender_deficit_))
        {
            if (RotationAction action = cast_spender(api, state, false, "Single-target spender"); !action.is_none()) {
                return action;
            }
        }

        // ST 11: a spare MotE can empower a needed Flame Shock refresh.
        if (state.buff_master_of_the_elements && flame_shock_refreshable(api, state.target) &&
            can_cast(api, spellbook_.flame_shock))
        {
            if (RotationAction action = cast_damage(api, spellbook_.flame_shock, state.target,
                    "Master of the Elements Flame Shock"); !action.is_none())
            {
                return action;
            }
        }

        // ST 12: Crackling Fury's Voltaic Blaze use outside Ascendance.
        if (state.talent_crackling_fury && !state.buff_ascendance &&
            can_cast(api, spellbook_.voltaic_blaze) &&
            !still_pending(voltaic_setup_pending_until_, api.get_game_time()))
        {
            if (RotationAction action = cast_damage(api, spellbook_.voltaic_blaze, state.target,
                    "Crackling Fury Voltaic Blaze"); !action.is_none())
            {
                return action;
            }
        }

        // ST 13: any remaining Tempest proc.
        if (can_cast(api, spellbook_.tempest)) {
            if (RotationAction action = cast_damage(api, spellbook_.tempest,
                    best_lightning_rod_target(api, state),
                    "Tempest / spread Lightning Rod"); !action.is_none())
            {
                return action;
            }
        }

        // ST 14: Chain Lightning is the filler at exactly two meaningful targets.
        if (state.enemies == 2 && state.meaningful_enemies >= 2 &&
            can_cast(api, spellbook_.chain_lightning)) {
            if (RotationAction action = cast_damage(api, spellbook_.chain_lightning, state.target,
                    "Two-target Chain Lightning"); !action.is_none())
            {
                return action;
            }
        }

        // ST 15: Lightning Bolt is the final stationary filler.
        if (can_cast(api, spellbook_.lightning_bolt)) {
            if (RotationAction action = cast_damage(api, spellbook_.lightning_bolt, state.target,
                    "Lightning Bolt filler"); !action.is_none())
            {
                return action;
            }
        }

        // Last fallback catches an instant if movement began during evaluation.
        return movement_action(api, state);
    }

    // -------------------------------------------------------------------------
    // 3+ TARGET CLEAVE / AOE PRIORITY (PRIMARY MYTHIC+ LIST)
    // -------------------------------------------------------------------------
    // This mirrors the live SimC list's state-based spender rules. With
    // Elemental Blast, EB establishes a stat buff and Earthquake then fills
    // missing Lightning Rods at 4+; at exactly three targets EB remains the
    // spender. Without EB, Earthquake starts at three targets.
    RotationAction aoe_action(const rotation_api::IRotationAPI& api, const CombatState& state) {
        const double ascendance_eta = ascendance_use_eta(api, state);
        const std::string rod_target = best_lightning_rod_target(api, state);
        const int chain_targets = std::min(5, std::max(1, state.enemies));

        // SimC's `>?` operator means minimum. These two formulas reserve room
        // for a Stormkeeper Chain Lightning and reproduce its near-cap EQ gate.
        const int stormkeeper_chain_gain = chain_targets * (chain_targets + 4);
        const int stormkeeper_eq_buffer = chain_targets * (chain_targets + 2);

        // AOE 1: Stormkeeper, then Ancestral Swiftness.
        if (RotationAction action = list_opener_cooldown_action(api, state); !action.is_none()) {
            return action;
        }

        // AOE 2: exactly three targets can maintain a direct Flame Shock for
        // Master of the Elements + Inferno Arc, including the FE-fade refresh.
        if (state.enemies == 3 && state.talent_master_of_the_elements &&
            state.talent_inferno_arc && !state.buff_master_of_the_elements &&
            can_cast(api, spellbook_.flame_shock))
        {
            std::string flame_target;
            if (ascendance_eta > 5.0) {
                // SimC does not cycle this action; keep one direct Flame Shock
                // instead of spending globals multi-dotting the entire pack.
                flame_target = best_flame_shock_target(api, state, false);
            }
            if (flame_target.empty() && state.fire_elemental_remaining > 0.0 &&
                state.fire_elemental_remaining < 2.0)
            {
                flame_target = lowest_flame_shock_target(api, state);
            }
            if (!flame_target.empty()) {
                if (RotationAction action = cast_damage(api, spellbook_.flame_shock, flame_target,
                        "Three-target Inferno Arc / Fire Elemental Flame Shock");
                    !action.is_none())
                {
                    return action;
                }
            }
        }

        // AOE 3: Voltaic Blaze precedes Ascendance when it refreshes Flame
        // Shock, catches the last two seconds of Fire Elemental, or generates
        // Purging Flames. With Purging Flames this is effectively on cooldown.
        const bool voltaic_needed =
            (flame_shock_refreshable(api, state.target) && ascendance_eta > 5.0) ||
            (state.fire_elemental_remaining > 0.0 && state.fire_elemental_remaining < 2.0) ||
            state.talent_purging_flames;
        if (!state.buff_master_of_the_elements && voltaic_needed &&
            can_cast(api, spellbook_.voltaic_blaze) &&
            !still_pending(voltaic_setup_pending_until_, api.get_game_time()))
        {
            if (RotationAction action = cast_damage(api, spellbook_.voltaic_blaze, state.target,
                    "Voltaic Blaze: DoT / Fire Elemental / Purging Flames");
                !action.is_none())
            {
                return action;
            }
        }

        // AOE 4: Ascendance after the required Voltaic Blaze setup.
        if (RotationAction action = ascendance_burst_action(api, state); !action.is_none()) {
            return action;
        }

        // AOE 5: with EB talented, first establish one of its three stat buffs.
        // Send the spender to the target with the least Lightning Rod time.
        if (state.tempest_stacks < 2 && state.talent_elemental_blast &&
            !state.buff_elemental_blast_stat && can_cast(api, spellbook_.elemental_blast))
        {
            if (RotationAction action = cast_damage(api, spellbook_.elemental_blast, rod_target,
                    "Elemental Blast before Lightning Rod Earthquake"); !action.is_none())
            {
                return action;
            }
        }

        // AOE 6: spread missing Lightning Rods with Earthquake. EB raises this
        // breakpoint from 3 to 4 and must already have supplied a stat buff.
        const int earthquake_rod_threshold = 3 + (state.talent_elemental_blast ? 1 : 0);
        if (state.tempest_stacks < 2 && state.lightning_rods < state.enemies &&
            state.enemies >= earthquake_rod_threshold &&
            (state.buff_elemental_blast_stat || !state.talent_elemental_blast) &&
            can_cast(api, spellbook_.earthquake))
        {
            if (RotationAction action = cast_earthquake(api, state,
                    "Spread missing Lightning Rods with"); !action.is_none())
            {
                return action;
            }
        }

        // AOE 7: at exactly three targets, Elemental Blast remains the spender
        // even after its stat buff is active.
        if (state.enemies == 3 && state.tempest_stacks < 2 &&
            state.talent_elemental_blast && can_cast(api, spellbook_.elemental_blast))
        {
            if (RotationAction action = cast_damage(api, spellbook_.elemental_blast, rod_target,
                    "Three-target Elemental Blast"); !action.is_none())
            {
                return action;
            }
        }

        // AOE 8: consume the tier's free spender before another builder can
        // replace it. Free casts ignore the custom end-of-pack banking rule.
        if (state.buff_overcharge_tier) {
            const bool tier_earthquake = !state.talent_elemental_blast;
            if (RotationAction action = cast_spender(
                    api, state, tier_earthquake, "Consume 12.1 tier free spender"); !action.is_none())
            {
                return action;
            }
        }

        // AOE 9: consume Purging Flames with Lava Burst when Lava Surge makes
        // it attractive or Voltaic Blaze is about to return. Cast history keeps
        // consecutive Lava Bursts from breaking the MotE alternation pattern.
        const bool voltaic_returning = spellbook_.voltaic_blaze != 0 &&
            api.get_spell_cooldown_remaining(spellbook_.voltaic_blaze) < 2.0;
        if (!last_damage_was_lava_burst() && state.buff_purging_flames &&
            ((state.buff_lava_surge && state.flowing_elements_stacks < 2) ||
             voltaic_returning) &&
            can_cast(api, spellbook_.lava_burst) &&
            (!state.talent_master_of_the_elements || !state.buff_master_of_the_elements) &&
            has_flame_shock(api, state.target))
        {
            if (RotationAction action = cast_damage(api, spellbook_.lava_burst, state.target,
                    "Spend Purging Flames"); !action.is_none())
            {
                return action;
            }
        }

        // AOE 10: at three targets, a Lava Surge Lava Burst prepares MotE for
        // a waiting Tempest.
        if (!last_damage_was_lava_burst() && state.enemies == 3 &&
            state.buff_tempest && state.buff_lava_surge &&
            state.talent_master_of_the_elements &&
            !state.buff_master_of_the_elements && can_cast(api, spellbook_.lava_burst) &&
            has_flame_shock(api, state.target))
        {
            if (RotationAction action = cast_damage(api, spellbook_.lava_burst, state.target,
                    "Lava Burst before three-target Tempest"); !action.is_none())
            {
                return action;
            }
        }

        // AOE 11: spend Tempest under Master of the Elements and apply its Rod
        // to a target that does not already have one.
        if (can_cast(api, spellbook_.tempest) && state.buff_master_of_the_elements) {
            if (RotationAction action = cast_damage(api, spellbook_.tempest, rod_target,
                    "AoE Tempest with MotE / spread Lightning Rod"); !action.is_none())
            {
                return action;
            }
        }

        // AOE 12: protect a two-charge Tempest cap unless Stormkeeper itself is
        // at four stacks; in that case make resource room and spend SK first.
        if (state.tempest_stacks >= 2 && state.stormkeeper_stacks < 4 &&
            can_cast(api, spellbook_.tempest))
        {
            if (RotationAction action = cast_damage(api, spellbook_.tempest, rod_target,
                    "Two-charge Tempest / spread Lightning Rod"); !action.is_none())
            {
                return action;
            }
        }

        // AOE 13: consume Stormkeeper only when the entire Chain Lightning
        // Maelstrom packet fits. Five targets is the generation cap in SimC.
        if (state.buff_stormkeeper &&
            state.maelstrom_deficit > stormkeeper_chain_gain &&
            can_cast(api, spellbook_.chain_lightning))
        {
            if (RotationAction action = cast_damage(api, spellbook_.chain_lightning, state.target,
                    "Stormkeeper Chain Lightning with Maelstrom room"); !action.is_none())
            {
                return action;
            }
        }

        // AOE 14: without Elemental Blast, Earthquake early enough to create
        // room for the pending Stormkeeper Chain Lightning. The custom banking
        // option still wins near the end of a non-boss M+ pull.
        const int earthquake_deficit_gate = spender_deficit_ +
            (state.buff_stormkeeper ? stormkeeper_eq_buffer : 0);
        if (!state.bank_maelstrom && !state.talent_elemental_blast &&
            state.maelstrom_deficit < earthquake_deficit_gate &&
            can_cast(api, spellbook_.earthquake))
        {
            if (RotationAction action = cast_earthquake(api, state,
                    "Make Maelstrom / Stormkeeper room with"); !action.is_none())
            {
                return action;
            }
        }

        // AOE 15: with Elemental Blast, it is the normal paid spender after the
        // special no-buff and Lightning-Rod-spread cases above.
        if (!state.bank_maelstrom && state.talent_elemental_blast &&
            can_cast(api, spellbook_.elemental_blast))
        {
            if (RotationAction action = cast_damage(api, spellbook_.elemental_blast, rod_target,
                    "AoE Elemental Blast spender"); !action.is_none())
            {
                return action;
            }
        }

        // AOE 16: use any remaining Tempest, then Chain Lightning filler.
        if (can_cast(api, spellbook_.tempest)) {
            if (RotationAction action = cast_damage(api, spellbook_.tempest, rod_target,
                    "AoE Tempest / spread Lightning Rod"); !action.is_none())
            {
                return action;
            }
        }
        if (can_cast(api, spellbook_.chain_lightning)) {
            if (RotationAction action = cast_damage(api, spellbook_.chain_lightning, state.target,
                    "Three-plus target Chain Lightning filler"); !action.is_none())
            {
                return action;
            }
        }

        // Last fallback catches an instant if movement began during evaluation.
        return movement_action(api, state);
    }

    // Last-resort builder that still honors Titan can_cast_spell(). This cannot
    // bridge a real GCD/action-lock snapshot disagreement.
    RotationAction final_damage_fallback(const rotation_api::IRotationAPI& api, const CombatState& state) {
        if (state.target.empty()) return no_action("No filler target");
        if (can_cast(api, spellbook_.chain_lightning) && state.use_aoe_list) {
            if (RotationAction action = cast_damage(api, spellbook_.chain_lightning, state.target,
                    "Fallback Chain Lightning"); !action.is_none())
            {
                return action;
            }
        }
        if (can_cast(api, spellbook_.lightning_bolt)) {
            if (RotationAction action = cast_damage(api, spellbook_.lightning_bolt, state.target,
                    "Fallback Lightning Bolt"); !action.is_none())
            {
                return action;
            }
        }
        if (can_cast(api, spellbook_.lava_burst)) {
            if (RotationAction action = cast_damage(api, spellbook_.lava_burst, state.target,
                    "Fallback Lava Burst"); !action.is_none())
            {
                return action;
            }
        }
        if (can_cast(api, spellbook_.flame_shock)) {
            if (RotationAction action = cast_damage(api, spellbook_.flame_shock, state.target,
                    "Fallback Flame Shock"); !action.is_none())
            {
                return action;
            }
        }
        if (can_cast(api, spellbook_.frost_shock)) {
            if (RotationAction action = cast_damage(api, spellbook_.frost_shock, state.target,
                    "Fallback Frost Shock"); !action.is_none())
            {
                return action;
            }
        }
        return no_action("No fallback damage");
    }
};

// Generates the DLL entry points expected by the external rotation loader.
IMPLEMENT_ROTATION_EXPORTS(Sethelementalshaman)
