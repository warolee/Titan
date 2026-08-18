-- Llama's Elemental
-- Native Titan Lua rotation. Midnight Season 2 / patch 12.1.
-- Class: Shaman (7) / Specialization: Elemental (262)
--
-- This is not a C++ port. C++ v2.3.7 is reference-only for settings IDs,
-- toggle names/order, dummy/target safety, and known Titan failure modes.
-- APL spenders follow Icy Veins 12.1 + MadBubble (EQ at 3+), not C++ AoE 5/6/7/14/15.
--
-- FIRST STRING BELOW is the public description Titan's announcer scrapes.
local publish_description = "Midnight Season 2 Elemental Shaman v2.4.0: native Lua"

local CLASS_ID = 7
local SPEC_ID = 262

local IDS = {
    LIGHTNING_BOLT = 188196,
    CHAIN_LIGHTNING = 188443,
    LAVA_BURST = 51505,
    FLAME_SHOCK = 188389,
    EARTH_SHOCK = 8042,
    EARTHQUAKE = 61882,
    ELEMENTAL_BLAST = 117014,
    STORMKEEPER = 191634,
    ASCENDANCE = 114050,
    TEMPEST = 454009,
    FROST_SHOCK = 196840,
    FIRE_ELEMENTAL = 198067,
    LIGHTNING_SHIELD = 192106,
    SKYFURY = 462854,
    FLAMETONGUE_WEAPON = 318038,
    SPIRITWALKERS_GRACE = 79206,
    GHOST_WOLF = 2645,
    WIND_SHEAR = 57994,
    ASTRAL_SHIFT = 108271,
    STONE_BULWARK_TOTEM = 108270,
    HEALING_SURGE = 8004,
    CLEANSE_SPIRIT = 51886,
    PURGE = 370,
    CAPACITOR_TOTEM = 192058,
    EARTH_ELEMENTAL = 198103,
    ANCESTRAL_GUIDANCE = 108281,
    HEALING_STREAM_TOTEM = 5394,
    LIGHTNING_ROD = 197209,
}

local EQ_CURSOR_MACRO = "/cast [@cursor] ##61882##"
local CAPACITOR_PLAYER_MACRO = "/cast [@player] ##192058##"

local AOE_THRESHOLD = 3
local SPENDER_DEFICIT = 15
local FS_REFRESH = 5.4
local SWG_MOVE_DELAY = 0.8
local DEBUG_INTERVAL = 2.0
local SPELLBOOK_REFRESH = 1.0
local SETUP_SETTLE = 1.25
local UNREACHABLE_DWELL = 0.75
local RECOVERY_RATE = 0.35
local TRINKET_MIN_TTD = 8.0
local SK_ASC_HOLD = 10.0
local ASC_SK_WAIT = 15.0

local book = {}
local cfg = {}
local runtime = {
    last_spellbook = -999.0,
    last_success_index = 0,
    last_sk_success = -999.0,
    last_as_success = -999.0,
    last_asc_success = -999.0,
    last_vb_success = -999.0,
    last_debug = -999.0,
    last_recovery = -999.0,
    unreachable_since = -999.0,
    recovery_guid = "",
    queued_cast_start = 0,
    last_setup_id = 0,
    last_setup_time = -999.0,
}

local function num(v)
    local t = type(v)
    if t == "number" then return v end
    if t == "string" then return tonumber(v) or 0 end
    return 0
end

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local function lower(s)
    if type(s) ~= "string" then return "" end
    return string.lower(s)
end

local function same_name(a, b)
    return lower(a) == lower(b)
end

local function contains_name(hay, needle)
    if hay == nil or needle == nil or needle == "" then return false end
    return string.find(lower(hay), lower(needle), 1, true) ~= nil
end

local function can_cast(api, spell_id)
    return spell_id ~= nil and spell_id > 0 and api.can_cast_spell(spell_id) == true
end

local function cd_remains(api, spell_id)
    if spell_id == nil or spell_id == 0 then return 999.0 end
    return num(api.get_spell_cooldown_remaining(spell_id))
end

local function gcd_remains(api)
    return num(api.get_remaining_gcd())
end

local function resolve_spell(api, name, fallback)
    local matches = api.find_all_spells_by_name(name)
    local known = 0
    if matches ~= nil then
        for i = 1, #matches do
            local data = matches[i]
            local candidate = num(data.spell_id)
            local override_id = num(data.override_spell_id)
            if override_id ~= 0 and same_name(api.get_spell_name(override_id), name) then
                candidate = override_id
            end
            if candidate > 0 then
                if api.can_cast_spell(candidate) then return candidate end
                if known == 0 and (data.is_spell_known or data.is_player_spell
                    or api.is_spell_known_or_overrides_known(candidate)) then
                    known = candidate
                end
            end
        end
    end
    if known > 0 then return known end
    if fallback ~= nil and fallback > 0 then
        if api.is_spell_known_or_overrides_known(fallback) or api.has_spell(fallback) then
            return fallback
        end
        local override_id = num(api.get_override_spell(fallback))
        if override_id ~= 0 and api.is_spell_known_or_overrides_known(override_id)
            and same_name(api.get_spell_name(override_id), name) then
            return override_id
        end
    end
    return 0
end

local function refresh_spellbook(api)
    local now = num(api.get_game_time())
    if runtime.last_spellbook > 0.0 and (now - runtime.last_spellbook) < SPELLBOOK_REFRESH then
        return
    end
    runtime.last_spellbook = now
    book.lightning_bolt = resolve_spell(api, "Lightning Bolt", IDS.LIGHTNING_BOLT)
    book.chain_lightning = resolve_spell(api, "Chain Lightning", IDS.CHAIN_LIGHTNING)
    book.lava_burst = resolve_spell(api, "Lava Burst", IDS.LAVA_BURST)
    book.flame_shock = resolve_spell(api, "Flame Shock", IDS.FLAME_SHOCK)
    book.earth_shock = resolve_spell(api, "Earth Shock", IDS.EARTH_SHOCK)
    book.earthquake = resolve_spell(api, "Earthquake", IDS.EARTHQUAKE)
    book.elemental_blast = resolve_spell(api, "Elemental Blast", IDS.ELEMENTAL_BLAST)
    book.stormkeeper = resolve_spell(api, "Stormkeeper", IDS.STORMKEEPER)
    book.ascendance = resolve_spell(api, "Ascendance", IDS.ASCENDANCE)
    book.tempest = resolve_spell(api, "Tempest", IDS.TEMPEST)
    book.voltaic_blaze = resolve_spell(api, "Voltaic Blaze", 0)
    book.ancestral_swiftness = resolve_spell(api, "Ancestral Swiftness", 0)
    book.natures_swiftness = resolve_spell(api, "Nature's Swiftness", 0)
    book.frost_shock = resolve_spell(api, "Frost Shock", IDS.FROST_SHOCK)
    book.fire_elemental = resolve_spell(api, "Fire Elemental", IDS.FIRE_ELEMENTAL)
    book.storm_elemental = resolve_spell(api, "Storm Elemental", 0)
    book.lightning_shield = resolve_spell(api, "Lightning Shield", IDS.LIGHTNING_SHIELD)
    book.skyfury = resolve_spell(api, "Skyfury", IDS.SKYFURY)
    book.flametongue_weapon = resolve_spell(api, "Flametongue Weapon", IDS.FLAMETONGUE_WEAPON)
    book.thunderstrike_ward = resolve_spell(api, "Thunderstrike Ward", 0)
    book.spiritwalkers_grace = resolve_spell(api, "Spiritwalker's Grace", IDS.SPIRITWALKERS_GRACE)
    book.ghost_wolf = resolve_spell(api, "Ghost Wolf", IDS.GHOST_WOLF)
    book.wind_shear = resolve_spell(api, "Wind Shear", IDS.WIND_SHEAR)
    book.astral_shift = resolve_spell(api, "Astral Shift", IDS.ASTRAL_SHIFT)
    book.stone_bulwark = resolve_spell(api, "Stone Bulwark Totem", IDS.STONE_BULWARK_TOTEM)
    book.healing_surge = resolve_spell(api, "Healing Surge", IDS.HEALING_SURGE)
    book.cleanse_spirit = resolve_spell(api, "Cleanse Spirit", IDS.CLEANSE_SPIRIT)
    book.purge = resolve_spell(api, "Purge", IDS.PURGE)
    book.capacitor = resolve_spell(api, "Capacitor Totem", IDS.CAPACITOR_TOTEM)
    book.earth_elemental = resolve_spell(api, "Earth Elemental", IDS.EARTH_ELEMENTAL)
    book.blood_fury = resolve_spell(api, "Blood Fury", 0)
    book.berserking = resolve_spell(api, "Berserking", 0)
    book.fireblood = resolve_spell(api, "Fireblood", 0)
    book.ancestral_call = resolve_spell(api, "Ancestral Call", 0)
end

local function has_buff_named(api, unit, name)
    local auras = api.get_buffs(unit, false)
    if auras == nil then return false, 0, 0.0 end
    for i = 1, #auras do
        local aura = auras[i]
        if same_name(aura.name, name) then
            local stacks = num(aura.stacks)
            if stacks < 1 then stacks = 1 end
            return true, stacks, num(aura.remaining)
        end
    end
    return false, 0, 0.0
end

local function has_buff_containing(api, unit, part)
    local auras = api.get_buffs(unit, false)
    if auras == nil then return false end
    for i = 1, #auras do
        if contains_name(auras[i].name, part) then return true end
    end
    return false
end

local function debuff_remaining(api, unit, name)
    local auras = api.get_debuffs(unit, false)
    if auras == nil then return 0.0 end
    for i = 1, #auras do
        local aura = auras[i]
        if same_name(aura.name, name) and (aura.source == "player" or aura.source == "pet") then
            return num(aura.remaining)
        end
    end
    return 0.0
end

local function has_talent_named(api, name)
    local nodes = api.get_active_talents()
    if nodes == nil then return false end
    for i = 1, #nodes do
        local node = nodes[i]
        if num(node.active_rank) > 0 and same_name(node.name, name) then
            return true
        end
        local entry = node.active_entry
        if entry ~= nil and num(entry.spell_id) > 0 then
            if same_name(api.get_spell_name(entry.spell_id), name) then return true end
        end
    end
    return false
end

local function looks_like_dummy(api, unit)
    if unit == nil or unit == "" or not api.unit_exists(unit) then return false end
    if api.unit_is_training_dummy(unit) == true then return true end
    return contains_name(api.get_unit_name(unit), "dummy")
end

local function can_damage_unit(api, unit)
    if unit == nil or unit == "" or not api.unit_exists(unit) then return false end
    if looks_like_dummy(api, unit) then return true end
    if api.unit_is_enemy(unit) or api.unit_can_attack(unit) then
        return api.unit_is_dead(unit) ~= true
    end
    return false
end

local function unit_is_boss(api, unit)
    if unit == nil or unit == "" or not api.unit_exists(unit) then return false end
    local classif = lower(api.get_unit_classification(unit))
    return classif == "worldboss" or classif == "rareelite" or classif == "boss"
end

local function observe_successes(api)
    local latest = num(api.get_last_spellcast_succeeded_index())
    if latest < runtime.last_success_index then
        runtime.last_success_index = 0
    end
    local events = api.get_last_spellcast_succeeded()
    if events == nil then return end
    local highest = runtime.last_success_index
    for i = 1, #events do
        local event = events[i]
        local index = num(event.index)
        if index > highest then highest = index end
        local mine = event.unit == nil or event.unit == "" or event.unit == "player"
        if index > runtime.last_success_index and mine then
            local id = num(event.spell_id)
            local t = num(event.time)
            if id == book.stormkeeper then runtime.last_sk_success = t end
            if id == book.ancestral_swiftness then runtime.last_as_success = t end
            if id == book.ascendance then runtime.last_asc_success = t end
            if id == book.voltaic_blaze then runtime.last_vb_success = t end
        end
    end
    runtime.last_success_index = highest
end

local function recently_succeeded(stamp, window, now)
    return stamp > 0.0 and (now - stamp) <= window
end

local function note_setup(spell_id, now)
    runtime.last_setup_id = spell_id
    runtime.last_setup_time = now
end

local function setup_pending(spell_id, now)
    return runtime.last_setup_id == spell_id and (now - runtime.last_setup_time) <= SETUP_SETTLE
end

local function snapshot(api)
    local state = {
        target = "",
        dummy = looks_like_dummy(api, "target"),
        enemies = 0,
        meaningful = 0,
        healthy_alt = false,
        maelstrom = num(api.get_player_power("maelstrom")),
        maelstrom_max = math.max(1, num(api.get_player_power_max("maelstrom"))),
        maelstrom_deficit = num(api.get_player_power_deficit("maelstrom")),
        player_hp = num(api.get_unit_health_percent("player")),
        fight_ttd = num(api.get_highest_nameplate_ttd(true, false)),
        moving = api.is_player_moving() == true,
        movement_time = 0.0,
        pack_hp = 100.0,
        is_mplus = false,
        is_raid = false,
        is_boss = false,
        earthquake_safe = true,
        use_aoe = false,
        band = 1,
        ancestors_up = nil,
        rods_known = false,
        lightning_rods = 0,
        flame_shock_remains = 0.0,
        flame_shocks = 0,
    }

    if api.unit_exists("target") and can_damage_unit(api, "target") then
        state.target = "target"
        state.is_boss = unit_is_boss(api, "target")
    end

    local mode = cfg.content_mode or 0
    local itype = lower(api.get_instance_type())
    if mode == 1 then
        state.is_mplus = true
    elseif mode == 2 then
        state.is_raid = true
    elseif mode == 0 then
        state.is_mplus = itype == "party"
        state.is_raid = itype == "raid"
        if num(api.get_mythic_plus_level()) > 0 then state.is_mplus = true end
    end

    -- Dummy parks are often out of combat. Do not require in-combat nameplates there.
    local plates = api.get_nameplates_in_range(40.0, not state.dummy, false)
    local hp_sum, hp_max = 0.0, 0.0
    if plates ~= nil then
        for i = 1, #plates do
            local enemy = plates[i]
            local unit = enemy.unit_token
            if unit ~= nil and unit ~= "" and can_damage_unit(api, unit) then
                local hostile = api.unit_is_enemy(unit) or api.unit_can_attack(unit)
                local dummy = looks_like_dummy(api, unit)
                if hostile or dummy then
                    state.enemies = state.enemies + 1
                    local hp = num(enemy.hp)
                    local maxhp = num(enemy.hp_max)
                    if maxhp > 0 then
                        hp_sum = hp_sum + math.max(0.0, hp)
                        hp_max = hp_max + maxhp
                    end
                    local pct = maxhp > 0 and (100.0 * hp / maxhp) or num(api.get_unit_health_percent(unit))
                    local ttd = num(enemy.time_to_death)
                    if (ttd <= 0.0 or ttd >= 6.0) and pct >= 10.0 then
                        state.meaningful = state.meaningful + 1
                    end
                    if state.target ~= "" then
                        local guid = api.get_unit_guid("target")
                        local same = (enemy.guid ~= nil and enemy.guid == guid)
                            or api.get_unit_guid(unit) == guid
                        if not same and pct > 0.1 then state.healthy_alt = true end
                    end
                end
            end
        end
    end
    if state.target ~= "" then
        state.enemies = math.max(1, state.enemies)
        state.meaningful = math.max(1, state.meaningful)
    end
    if hp_max > 0 then state.pack_hp = clamp(100.0 * hp_sum / hp_max, 0.0, 100.0) end
    if state.dummy then
        state.fight_ttd = 999.0
        state.pack_hp = math.max(state.pack_hp, 100.0)
    elseif state.fight_ttd <= 0.0 then
        state.fight_ttd = 999.0
    end

    if state.moving then
        local started = num(api.get_player_started_moving_time())
        local now = num(api.get_game_time())
        state.movement_time = started > 0.0 and math.max(0.0, now - started) or 0.0
    end

    local aoe_on = api.is_aoe_enabled() == true
    if not aoe_on then
        state.enemies = math.min(1, state.enemies)
        state.meaningful = math.min(1, state.meaningful)
    end
    state.use_aoe = aoe_on and state.enemies >= AOE_THRESHOLD
    if state.enemies <= 1 then
        state.band = 1
    elseif state.enemies == 2 then
        state.band = 2
    elseif state.enemies == 3 then
        state.band = 3
    else
        state.band = 4
    end

    -- Simplest first-pass EQ safety: dummy always, otherwise stand still or near-cap.
    state.earthquake_safe = state.dummy or (not state.moving) or state.maelstrom_deficit <= 10

    local hero_mode = cfg.hero_mode or 0
    state.talent_elemental_blast = has_talent_named(api, "Elemental Blast")
        or (book.elemental_blast > 0 and api.is_spell_known_or_overrides_known(book.elemental_blast))
    state.talent_earthquake = has_talent_named(api, "Earthquake")
        or (book.earthquake > 0 and (api.is_spell_known_or_overrides_known(book.earthquake) or api.has_spell(book.earthquake)))
    state.talent_mote = has_talent_named(api, "Master of the Elements")
    state.talent_tempest = has_talent_named(api, "Tempest") or book.tempest > 0
    state.talent_inferno = has_talent_named(api, "Inferno Arc")
    state.talent_purging = has_talent_named(api, "Purging Flames")
    state.talent_crackling = has_talent_named(api, "Crackling Fury")
    state.talent_ancestors = has_talent_named(api, "Call of the Ancestors")

    if hero_mode == 1 then
        state.hero_farseer = true
        state.hero_stormbringer = false
    elseif hero_mode == 2 then
        state.hero_farseer = false
        state.hero_stormbringer = true
    else
        state.hero_farseer = state.talent_ancestors
            or book.ancestral_swiftness > 0
            or has_talent_named(api, "Ancestral Swiftness")
            or has_talent_named(api, "Ancient Fellowship")
            or has_talent_named(api, "Routine Communication")
            or has_talent_named(api, "Final Calling")
        state.hero_stormbringer = state.talent_tempest
            or book.tempest > 0
            or has_talent_named(api, "Awakening Storms")
            or has_talent_named(api, "Rolling Thunder")
            or has_talent_named(api, "Unlimited Power")
        if state.hero_farseer and state.hero_stormbringer then
            state.hero_stormbringer = false
        end
    end

    local mote, _, _ = has_buff_named(api, "player", "Master of the Elements")
    state.buff_mote = mote
    local surge, _, _ = has_buff_named(api, "player", "Lava Surge")
    state.buff_lava_surge = surge
    local sk, sk_stacks, _ = has_buff_named(api, "player", "Stormkeeper")
    state.buff_stormkeeper = sk
    state.stormkeeper_stacks = sk_stacks
    local tmp, tmp_stacks, _ = has_buff_named(api, "player", "Tempest")
    state.buff_tempest = tmp
    state.tempest_stacks = tmp_stacks
    local asc, _, _ = has_buff_named(api, "player", "Ascendance")
    state.buff_ascendance = asc
    local pf, _, _ = has_buff_named(api, "player", "Purging Flames")
    state.buff_purging = pf
    local fe, _, _ = has_buff_named(api, "player", "Flowing Elements")
    state.buff_flowing = fe
    local _, potm_stacks, _ = has_buff_named(api, "player", "Power of the Maelstrom")
    state.potm_stacks = potm_stacks
    state.buff_overcharge = has_buff_containing(api, "player", "Overcharge")
        or has_buff_containing(api, "player", "12.1 Class Set")
    state.buff_eb_stat = has_buff_containing(api, "player", "Elemental Blast")
    local _, _, fe_time = has_buff_named(api, "player", "Fire Elemental")
    state.fire_elemental_remaining = fe_time
    local swg, _, _ = has_buff_named(api, "player", "Spiritwalker's Grace")
    state.buff_swg = swg
    local ls, _, _ = has_buff_named(api, "player", "Lightning Shield")
    state.buff_lightning_shield = ls
    local sf, _, _ = has_buff_named(api, "player", "Skyfury")
    state.buff_skyfury = sf

    local cot, _, _ = has_buff_named(api, "player", "Call of the Ancestors")
    if cot or has_buff_containing(api, "player", "Ancestor") then
        state.ancestors_up = true
    else
        -- Unknown vs false: Farseer without a visible ancestor buff is treated unknown.
        if state.hero_farseer then
            state.ancestors_up = nil
        else
            state.ancestors_up = false
        end
    end

    state.flame_shock_remains = debuff_remaining(api, "target", "Flame Shock")
    if state.flame_shock_remains > 0 then state.flame_shocks = 1 end
    if plates ~= nil then
        local fs = 0
        for i = 1, #plates do
            local unit = plates[i].unit_token
            if unit ~= nil and can_damage_unit(api, unit) and debuff_remaining(api, unit, "Flame Shock") > 0 then
                fs = fs + 1
            end
        end
        if fs > 0 then state.flame_shocks = fs end
    end

    -- Rod observation is optional. False does not prove absence; leave unknown.
    -- Never invent a count. Never block 3+ EQ on unknown rods.
    if state.target ~= "" then
        local ok, player_has = pcall(function()
            return api.has_debuff("target", IDS.LIGHTNING_ROD, true)
        end)
        local ok2, any_has = pcall(function()
            return api.has_debuff("target", IDS.LIGHTNING_ROD, false)
        end)
        if (ok and player_has == true) or (ok2 and any_has == true) then
            state.rods_known = true
            state.lightning_rods = 1
        end
    end

    local n = math.min(5, math.max(1, state.enemies))
    state.sk_cl_gain = n * (n + 4)
    state.sk_eq_buffer = n * (n + 2)
    state.bank_maelstrom = state.is_mplus and not state.is_boss and not state.dummy
        and (state.fight_ttd <= 2.0 or state.pack_hp <= 8.0)

    return state
end

local function toggle_on(api, name)
    return api.is_toggle_enabled(name) == true
end

local function burst_now(api)
    return toggle_on(api, "Burst Now")
end

local function cooldowns_allowed(api, state)
    if api.are_cooldowns_enabled() ~= true or not toggle_on(api, "Major CDs") then
        return false
    end
    if burst_now(api) then return true end
    local policy = cfg.cooldown_policy or 0
    if policy == 1 then return true end
    if policy == 2 then return state.is_boss or state.dummy end
    if state.is_boss or state.dummy then return true end
    if state.is_raid then return state.fight_ttd >= 18.0 end
    if state.is_mplus then
        return state.meaningful >= 3 and state.fight_ttd >= 10.0 and state.pack_hp >= 25.0
    end
    return false
end

local function mini_allowed(api)
    return api.are_cooldowns_enabled() == true and toggle_on(api, "Mini CDs")
end

local function spell_action(context, spell_id, unit, note)
    if not can_cast(context.api, spell_id) then return nil end
    if unit == nil or unit == "" then unit = "target" end
    return context.action.spell(spell_id, unit, note)
end

local function try_setup(context, api, spell_id, unit, note, now)
    if spell_id == nil or spell_id == 0 then return nil end
    if setup_pending(spell_id, now) then return nil end
    if recently_succeeded(runtime.last_sk_success, SETUP_SETTLE, now) and spell_id == book.stormkeeper then
        return nil
    end
    if recently_succeeded(runtime.last_as_success, SETUP_SETTLE, now) and spell_id == book.ancestral_swiftness then
        return nil
    end
    if recently_succeeded(runtime.last_asc_success, SETUP_SETTLE, now) and spell_id == book.ascendance then
        return nil
    end
    if recently_succeeded(runtime.last_vb_success, SETUP_SETTLE, now) and spell_id == book.voltaic_blaze then
        return nil
    end
    local action = spell_action(context, spell_id, unit, note)
    if action ~= nil then note_setup(spell_id, now) end
    return action
end

local function cast_earthquake(context, api, state, note)
    if not state.earthquake_safe then return nil end
    if book.earthquake == 0 and not state.talent_earthquake then return nil end
    if book.earthquake > 0 and cd_remains(api, book.earthquake) > 0.05 then return nil end
    if can_cast(api, book.earthquake) then
        return context.action.spell(book.earthquake, "target", note or "Earthquake")
    end
    return context.action.macro(EQ_CURSOR_MACRO, note or "Earthquake @cursor")
end

local function cast_eb(context, api, state, note)
    if not state.talent_elemental_blast then return nil end
    return spell_action(context, book.elemental_blast, state.target, note or "Elemental Blast")
end

local function cast_es(context, api, state, note)
    return spell_action(context, book.earth_shock, state.target, note or "Earth Shock")
end

local function near_cap(state)
    return state.maelstrom_deficit < SPENDER_DEFICIT
end

-- 2-target EQ is talent-aware. Unknown ancestors do not unlock EQ.
local function two_target_spender(context, api, state)
    if state.hero_stormbringer and state.talent_elemental_blast then
        return cast_eb(context, api, state, "2t Stormbringer Elemental Blast")
            or cast_es(context, api, state, "2t Earth Shock")
    end
    if not state.talent_elemental_blast then
        if state.ancestors_up == true then
            return cast_earthquake(context, api, state, "2t Farseer EQ with ancestors")
                or cast_es(context, api, state, "2t Earth Shock")
        end
        return cast_es(context, api, state, "2t Earth Shock")
    end
    if state.ancestors_up == true then
        return cast_earthquake(context, api, state, "2t Farseer EB-build EQ with ancestors")
            or cast_eb(context, api, state, "2t Elemental Blast")
    end
    return cast_eb(context, api, state, "2t Elemental Blast")
        or cast_es(context, api, state, "2t Earth Shock")
end

local function three_target_spender(context, api, state)
    -- Stormbringer + EB wants a 4-target EQ minimum.
    if state.hero_stormbringer and state.talent_elemental_blast then
        return cast_eb(context, api, state, "3t Stormbringer Elemental Blast")
            or cast_es(context, api, state, "3t Earth Shock")
    end
    if state.ancestors_up == true then
        return cast_earthquake(context, api, state, "3t EQ with ancestors")
            or cast_eb(context, api, state, "3t Elemental Blast")
    end
    if state.ancestors_up == false and state.talent_elemental_blast then
        return cast_eb(context, api, state, "3t EB no ancestors")
            or cast_earthquake(context, api, state, "3t Earthquake")
    end
    -- Unknown ancestors: Farseer/default prefer EQ at 3+.
    return cast_earthquake(context, api, state, "3t Earthquake")
        or cast_eb(context, api, state, "3t Elemental Blast")
        or cast_es(context, api, state, "3t Earth Shock")
end

local function aoe_spender(context, api, state)
    if state.bank_maelstrom then return nil end
    if state.enemies >= 6 then
        return cast_earthquake(context, api, state, "6+ Earthquake")
    end
    if state.talent_elemental_blast and state.ancestors_up == false and state.enemies <= 5 then
        return cast_eb(context, api, state, "4-5t EB no ancestors")
            or cast_earthquake(context, api, state, "4+ Earthquake")
    end
    return cast_earthquake(context, api, state, "4+ Earthquake")
        or cast_eb(context, api, state, "4+ Elemental Blast")
end

local function list_stormkeeper(context, api, state, now)
    if not mini_allowed(api) then return nil end
    if not can_cast(api, book.stormkeeper) then return nil end
    local gcd = math.max(0.75, gcd_remains(api))
    local asc_cd = cd_remains(api, book.ascendance)
    local fight_ending = state.fight_ttd < 20.0 and not state.dummy
    if not (asc_cd > SK_ASC_HOLD or asc_cd < gcd or fight_ending) then return nil end
    return try_setup(context, api, book.stormkeeper, "player", "Stormkeeper", now)
end

local function list_ancestral_swiftness(context, api, now)
    if not mini_allowed(api) then return nil end
    return try_setup(context, api, book.ancestral_swiftness, "player", "Ancestral Swiftness", now)
end

local function list_ascendance(context, api, state, now)
    if not cooldowns_allowed(api, state) then return nil end
    if not can_cast(api, book.ascendance) then return nil end
    local sk_cd = cd_remains(api, book.stormkeeper)
    local fight_ending = state.fight_ttd < 20.0 and not state.dummy
    if not (sk_cd > ASC_SK_WAIT or fight_ending or burst_now(api)) then return nil end
    if cfg.use_natures_swiftness and can_cast(api, book.natures_swiftness) then
        return spell_action(context, book.natures_swiftness, "player", "Nature's Swiftness with Ascendance")
    end
    if cfg.use_racials then
        local racial = spell_action(context, book.blood_fury, "player", "Blood Fury")
            or spell_action(context, book.berserking, "player", "Berserking")
            or spell_action(context, book.fireblood, "player", "Fireblood")
            or spell_action(context, book.ancestral_call, "player", "Ancestral Call")
        if racial ~= nil then return racial end
    end
    return try_setup(context, api, book.ascendance, "player", "Ascendance", now)
end

local function list_fire_elemental(context, api, state)
    if not cooldowns_allowed(api, state) then return nil end
    return spell_action(context, book.storm_elemental, "player", "Storm Elemental")
        or spell_action(context, book.fire_elemental, "player", "Fire Elemental")
end

local function list_voltaic(context, api, state, now, refresh)
    if state.buff_mote then return nil end
    if not can_cast(api, book.voltaic_blaze) then return nil end
    local need = refresh or state.talent_purging
        or (state.fire_elemental_remaining > 0.0 and state.fire_elemental_remaining < 2.0)
    if not need then return nil end
    return try_setup(context, api, book.voltaic_blaze, state.target, "Voltaic Blaze", now)
end

local function trinket_mode(slot)
    if slot == 1 then return cfg.trinket_1_mode or 2 end
    return cfg.trinket_2_mode or 2
end

local function trinket_ready(api, slot)
    local item = slot == 1 and api.get_trinket1() or api.get_trinket2()
    if item == nil or num(item.item_id) == 0 then return false, nil end
    if item.is_usable ~= true then return false, item end
    if item.is_on_cooldown == true then return false, item end
    return true, item
end

local function try_trinket(context, api, state, slot, major_package)
    if api.are_cooldowns_enabled() ~= true then return nil end
    -- Smallest hardcast guard: never fire a trinket during an active hardcast.
    if api.unit_is_casting_or_channeling("player", false) then return nil end
    local mode = trinket_mode(slot)
    if mode == 0 then return nil end
    local ready = trinket_ready(api, slot)
    if not ready then return nil end
    if state.fight_ttd < TRINKET_MIN_TTD and not state.dummy and not state.is_boss then
        return nil
    end
    local allow = false
    if mode == 4 then
        allow = true
    elseif mode == 3 then
        allow = state.is_boss or state.dummy
    elseif mode == 2 then
        allow = major_package == true
    elseif mode == 1 then
        local asc_cd = cd_remains(api, book.ascendance)
        allow = major_package or asc_cd > 20.0 or state.fight_ttd < 21.0 or state.dummy
    end
    if not allow then return nil end
    return context.action.trinket(slot, "player", "Trinket " .. tostring(slot))
end

local function independent_trinkets(context, api, state)
    return try_trinket(context, api, state, 1, false)
        or try_trinket(context, api, state, 2, false)
end

local function major_trinkets(context, api, state)
    return try_trinket(context, api, state, 1, true)
        or try_trinket(context, api, state, 2, true)
end

local function defensive_action(context, api, state)
    if not toggle_on(api, "Defensives") then return nil end
    local shift = ({ -8.0, 0.0, 10.0 })[(cfg.survival_profile or 1) + 1] or 0.0
    if state.player_hp <= (30.0 + shift) and can_cast(api, book.healing_surge) then
        return spell_action(context, book.healing_surge, "player", "Emergency Healing Surge")
    end
    if state.player_hp <= (55.0 + shift) and can_cast(api, book.astral_shift) then
        return spell_action(context, book.astral_shift, "player", "Astral Shift")
    end
    if state.player_hp <= (72.0 + shift) and can_cast(api, book.stone_bulwark) then
        return spell_action(context, book.stone_bulwark, "player", "Stone Bulwark Totem")
    end
    return nil
end

local function interrupt_action(context, api)
    if api.is_interrupt_enabled() ~= true then return nil end
    if not can_cast(api, book.wind_shear) then return nil end
    if not api.unit_exists("target") then return nil end
    if api.unit_is_casting_or_channeling("target", false) ~= true then return nil end
    local info = api.get_unit_casting_info("target")
    if info ~= nil and info.not_interruptible == true then return nil end
    return spell_action(context, book.wind_shear, "target", "Wind Shear")
end

local function utility_action(context, api, state)
    if not toggle_on(api, "Utility") then return nil end
    if (cfg.utility_profile or 0) == 2 then return nil end
    if (cfg.utility_profile or 0) == 0 and can_cast(api, book.cleanse_spirit) then
        -- Declared friendly spell; only self-cleanse in this first build.
    end
    if cfg.auto_purge and can_cast(api, book.purge) then
        return nil
    end
    return nil
end

local function in_range(api, spell_id, unit)
    if spell_id == 0 or unit == nil or unit == "" then return false end
    return api.is_spell_in_range(spell_id, unit) == true
end

local function combat_recovery(context, api, state)
    if not cfg.automatic_target_recovery then return nil end
    if api.is_mounted() or api.is_paused() or api.is_in_combat_lockdown() ~= true then
        runtime.unreachable_since = -999.0
        runtime.recovery_guid = ""
        return nil
    end
    if state.dummy then
        runtime.unreachable_since = -999.0
        runtime.recovery_guid = ""
        return nil
    end
    local exists = api.unit_exists("target")
    local dead = exists and api.unit_is_dead("target")
    local hostile = exists and can_damage_unit(api, "target")
    local guid = exists and api.get_unit_guid("target") or ""
    if guid ~= runtime.recovery_guid then
        runtime.recovery_guid = guid
        runtime.unreachable_since = -999.0
    end
    local now = num(api.get_game_time())
    local unreachable = false
    if hostile and not dead then
        local ok = in_range(api, book.lightning_bolt, "target")
            or in_range(api, book.lava_burst, "target")
            or in_range(api, book.flame_shock, "target")
        if ok then
            runtime.unreachable_since = -999.0
        else
            if runtime.unreachable_since < 0.0 then runtime.unreachable_since = now end
            unreachable = (now - runtime.unreachable_since) >= UNREACHABLE_DWELL
        end
    else
        runtime.unreachable_since = -999.0
    end
    local need = ((not exists or dead or not hostile) and state.healthy_alt)
        or (unreachable and state.healthy_alt)
    if not need then return nil end
    if (now - runtime.last_recovery) < RECOVERY_RATE then return nil end
    runtime.last_recovery = now
    return context.action.keybind("TAB")
end

local function movement_action(context, api, state)
    if state.buff_swg then return nil end
    if state.movement_time >= SWG_MOVE_DELAY and can_cast(api, book.spiritwalkers_grace) then
        return spell_action(context, book.spiritwalkers_grace, "player", "Spiritwalker's Grace")
    end
    if can_cast(api, book.tempest) then
        return spell_action(context, book.tempest, state.target, "Moving Tempest")
    end
    if state.buff_lava_surge and can_cast(api, book.lava_burst) and state.flame_shock_remains > 0 then
        return spell_action(context, book.lava_burst, state.target, "Moving Lava Surge")
    end
    if can_cast(api, book.voltaic_blaze) then
        return spell_action(context, book.voltaic_blaze, state.target, "Moving Voltaic Blaze")
    end
    if can_cast(api, book.flame_shock) then
        return spell_action(context, book.flame_shock, state.target, "Moving Flame Shock")
    end
    if can_cast(api, book.frost_shock) then
        return spell_action(context, book.frost_shock, state.target, "Moving Frost Shock")
    end
    return nil
end

local function lvb_charges(api)
    if book.lava_burst == 0 then return 0.0 end
    return num(api.get_spell_fractional_charges(book.lava_burst))
end

local function single_target(context, api, state, now)
    local action = list_stormkeeper(context, api, state, now)
        or list_ancestral_swiftness(context, api, now)
    if action then return action end
    local refresh = state.flame_shock_remains <= FS_REFRESH
    if not state.buff_mote and refresh and cd_remains(api, book.ascendance) > 5.0 then
        action = list_voltaic(context, api, state, now, true)
            or spell_action(context, book.flame_shock, state.target, "Refresh Flame Shock")
        if action then return action end
    end
    if not state.buff_mote and not state.buff_ascendance
        and state.fire_elemental_remaining > 0.0
        and state.fire_elemental_remaining < (6.0 * math.max(1, state.enemies) - 4.0) then
        action = spell_action(context, book.flame_shock, state.target, "Fire Elemental Flame Shock")
        if action then return action end
    end
    action = list_voltaic(context, api, state, now, refresh)
    if action then return action end
    action = list_fire_elemental(context, api, state)
        or major_trinkets(context, api, state)
        or list_ascendance(context, api, state, now)
    if action then return action end
    if state.buff_flowing and state.buff_overcharge then
        action = cast_eb(context, api, state, "Free tier Elemental Blast")
            or cast_es(context, api, state, "Free tier Earth Shock")
        if action then return action end
    end
    local deficit_ok = state.maelstrom_deficit > SPENDER_DEFICIT
    if state.talent_mote and not state.buff_mote and deficit_ok
        and state.flame_shock_remains > 0 and can_cast(api, book.lava_burst) then
        local charges = lvb_charges(api)
        if charges > 1.8 or state.buff_lava_surge or state.buff_flowing or state.potm_stacks >= 2 then
            action = spell_action(context, book.lava_burst, state.target, "Lava Burst for MotE")
            if action then return action end
        end
        if state.maelstrom > (52 + (state.talent_elemental_blast and 30 or 0) - 15) then
            action = spell_action(context, book.lava_burst, state.target, "Lava Burst before spender")
            if action then return action end
        end
    end
    if not state.talent_mote and deficit_ok and state.buff_lava_surge
        and state.flame_shock_remains > 0 then
        action = spell_action(context, book.lava_burst, state.target, "Lava Surge")
        if action then return action end
    end
    if can_cast(api, book.tempest) and (state.buff_mote or not state.talent_mote) then
        return spell_action(context, book.tempest, state.target, "Tempest")
    end
    if state.buff_stormkeeper and state.buff_mote and state.talent_tempest
        and can_cast(api, book.lightning_bolt) then
        return spell_action(context, book.lightning_bolt, state.target, "Stormkeeper Lightning Bolt")
    end
    if state.buff_mote or near_cap(state) then
        action = cast_eb(context, api, state, "ST Elemental Blast")
            or cast_es(context, api, state, "ST Earth Shock")
        if action then return action end
    end
    if state.buff_mote and state.flame_shock_remains <= FS_REFRESH then
        action = spell_action(context, book.flame_shock, state.target, "MotE Flame Shock")
        if action then return action end
    end
    if state.talent_crackling and not state.buff_ascendance and can_cast(api, book.voltaic_blaze) then
        action = spell_action(context, book.voltaic_blaze, state.target, "Crackling Fury Voltaic Blaze")
        if action then return action end
    end
    if can_cast(api, book.tempest) then
        return spell_action(context, book.tempest, state.target, "Tempest filler")
    end
    return spell_action(context, book.lightning_bolt, state.target, "Lightning Bolt filler")
end

local function two_target(context, api, state, now)
    local action = list_stormkeeper(context, api, state, now)
        or list_ancestral_swiftness(context, api, now)
        or list_voltaic(context, api, state, now, state.flame_shock_remains <= FS_REFRESH)
        or list_fire_elemental(context, api, state)
        or major_trinkets(context, api, state)
        or list_ascendance(context, api, state, now)
    if action then return action end
    if not state.buff_mote and state.flame_shock_remains > 0 and can_cast(api, book.lava_burst) then
        action = spell_action(context, book.lava_burst, state.target, "2t Lava Burst")
        if action then return action end
    end
    if can_cast(api, book.tempest) and (state.buff_mote or state.buff_stormkeeper) then
        return spell_action(context, book.tempest, state.target, "2t Tempest")
    end
    if state.buff_stormkeeper and state.buff_mote and state.hero_stormbringer
        and can_cast(api, book.lightning_bolt) then
        return spell_action(context, book.lightning_bolt, state.target, "2t Stormkeeper Lightning Bolt")
    end
    if state.buff_mote or near_cap(state) then
        action = two_target_spender(context, api, state)
        if action then return action end
    end
    if can_cast(api, book.tempest) then
        return spell_action(context, book.tempest, state.target, "2t Tempest filler")
    end
    if state.hero_stormbringer and state.buff_stormkeeper then
        return spell_action(context, book.lightning_bolt, state.target, "2t Lightning Bolt")
    end
    return spell_action(context, book.chain_lightning, state.target, "2t Chain Lightning")
        or spell_action(context, book.lightning_bolt, state.target, "2t Lightning Bolt")
end

local function three_target(context, api, state, now)
    local action = list_stormkeeper(context, api, state, now)
        or list_ancestral_swiftness(context, api, now)
        or list_voltaic(context, api, state, now, true)
        or list_fire_elemental(context, api, state)
        or major_trinkets(context, api, state)
        or list_ascendance(context, api, state, now)
    if action then return action end
    if state.talent_mote and state.talent_inferno and not state.buff_mote
        and state.flame_shock_remains <= FS_REFRESH and cd_remains(api, book.ascendance) > 5.0 then
        action = spell_action(context, book.flame_shock, state.target, "3t Inferno Arc Flame Shock")
        if action then return action end
    end
    if state.buff_overcharge then
        action = three_target_spender(context, api, state)
        if action then return action end
    end
    if state.buff_purging and (state.buff_lava_surge or cd_remains(api, book.voltaic_blaze) < 2.0)
        and can_cast(api, book.lava_burst) and state.flame_shock_remains > 0 then
        action = spell_action(context, book.lava_burst, state.target, "3t Purging Flames")
        if action then return action end
    end
    if state.buff_tempest and state.buff_lava_surge and state.talent_mote and not state.buff_mote
        and can_cast(api, book.lava_burst) and state.flame_shock_remains > 0 then
        action = spell_action(context, book.lava_burst, state.target, "3t Lava Surge before Tempest")
        if action then return action end
    end
    if can_cast(api, book.tempest) and state.buff_mote then
        return spell_action(context, book.tempest, state.target, "3t Tempest MotE")
    end
    if state.tempest_stacks >= 2 and state.stormkeeper_stacks < 4 and can_cast(api, book.tempest) then
        return spell_action(context, book.tempest, state.target, "3t Tempest cap")
    end
    if state.buff_stormkeeper and state.maelstrom_deficit > state.sk_cl_gain
        and can_cast(api, book.chain_lightning) then
        return spell_action(context, book.chain_lightning, state.target, "3t Stormkeeper Chain Lightning")
    end
    if near_cap(state) or state.buff_mote then
        action = three_target_spender(context, api, state)
        if action then return action end
    end
    if can_cast(api, book.tempest) then
        return spell_action(context, book.tempest, state.target, "3t Tempest filler")
    end
    return spell_action(context, book.chain_lightning, state.target, "3t Chain Lightning")
end

local function four_plus(context, api, state, now)
    local action = list_stormkeeper(context, api, state, now)
        or list_ancestral_swiftness(context, api, now)
        or list_voltaic(context, api, state, now, true)
        or list_fire_elemental(context, api, state)
        or major_trinkets(context, api, state)
        or list_ascendance(context, api, state, now)
    if action then return action end
    if state.buff_overcharge then
        action = aoe_spender(context, api, state)
        if action then return action end
    end
    if state.tempest_stacks < 2 then
        action = aoe_spender(context, api, state)
        if action then return action end
    end
    if state.buff_purging and (state.buff_lava_surge or cd_remains(api, book.voltaic_blaze) < 2.0)
        and can_cast(api, book.lava_burst) then
        action = spell_action(context, book.lava_burst, state.target, "AoE Purging Flames")
        if action then return action end
    end
    if state.tempest_stacks >= 2 and state.stormkeeper_stacks < 4 and can_cast(api, book.tempest) then
        return spell_action(context, book.tempest, state.target, "AoE Tempest cap")
    end
    if state.buff_stormkeeper and state.maelstrom_deficit > state.sk_cl_gain
        and can_cast(api, book.chain_lightning) then
        return spell_action(context, book.chain_lightning, state.target, "AoE Stormkeeper Chain Lightning")
    end
    action = aoe_spender(context, api, state)
    if action then return action end
    if can_cast(api, book.tempest) then
        return spell_action(context, book.tempest, state.target, "AoE Tempest filler")
    end
    return spell_action(context, book.chain_lightning, state.target, "AoE Chain Lightning")
end

local function damage_list(context, api, state, now)
    if state.band == 1 then return single_target(context, api, state, now) end
    if state.band == 2 then return two_target(context, api, state, now) end
    if state.band == 3 then return three_target(context, api, state, now) end
    return four_plus(context, api, state, now)
end

local function queued_filler(context, api, state)
    if state.band >= 3 then
        return spell_action(context, book.chain_lightning, state.target, "Queued Chain Lightning")
    end
    if state.band == 2 and not (state.hero_stormbringer and state.buff_stormkeeper) then
        return spell_action(context, book.chain_lightning, state.target, "Queued Chain Lightning")
    end
    return spell_action(context, book.lightning_bolt, state.target, "Queued Lightning Bolt")
end

local function maybe_debug(context, api, state, extra)
    if not cfg.debug_diagnostics then return end
    local now = num(api.get_game_time())
    if (now - runtime.last_debug) < DEBUG_INTERVAL then return end
    runtime.last_debug = now
    local line = string.format(
        "DBG v2.4.0 band=%d enemies=%d dummy=%s ms=%d/%d def=%d hero=%s eb=%s anc=%s rods=%s/%d gcd=%.2f %s",
        math.floor(num(state.band)),
        math.floor(num(state.enemies)),
        state.dummy and "1" or "0",
        math.floor(num(state.maelstrom)),
        math.floor(num(state.maelstrom_max)),
        math.floor(num(state.maelstrom_deficit)),
        state.hero_farseer and "FS" or (state.hero_stormbringer and "SB" or "?"),
        state.talent_elemental_blast and "1" or "0",
        state.ancestors_up == true and "1" or (state.ancestors_up == false and "0" or "?"),
        state.rods_known and "k" or "u",
        math.floor(num(state.lightning_rods)),
        gcd_remains(api),
        extra or ""
    )
    context.log(line)
end

local function combat_body(context)
    local api = context.api
    refresh_spellbook(api)
    observe_successes(api)
    local now = num(api.get_game_time())

    -- Hardcast: include_queue=false is the real clip boundary.
    if api.unit_is_casting_or_channeling("player", false) then
        maybe_debug(context, api, snapshot(api), "casting")
        return context.action.none("Casting")
    end

    local state = snapshot(api)
    maybe_debug(context, api, state, "")

    -- Native queue window: include=true is true while include=false is false.
    local in_queue = cfg.queue_window_casting
        and api.unit_is_casting_or_channeling("player", true) == true
    if in_queue then
        local info = api.get_unit_casting_info("player")
        local start_ms = 0
        if info ~= nil then start_ms = num(info.start_time_ms) end
        if start_ms <= 0 then
            return context.action.none("Queue window without cast id")
        end
        if runtime.queued_cast_start == start_ms then
            return context.action.none("Queue already used")
        end
        local filler = queued_filler(context, api, state)
        if filler ~= nil then
            runtime.queued_cast_start = start_ms
            return filler
        end
        return context.action.none("Queue window empty")
    end

    local action = defensive_action(context, api, state)
    if action then return action end
    if (cfg.utility_profile or 0) ~= 2 then
        action = interrupt_action(context, api)
        if action then return action end
        action = utility_action(context, api, state)
        if action then return action end
    end
    action = combat_recovery(context, api, state)
    if action then return action end
    if state.target == "" then
        return context.action.none("No valid enemy target")
    end
    action = independent_trinkets(context, api, state)
    if action then return action end

    local gcd = gcd_remains(api)
    if gcd > 0.05 then
        local wait_ms = math.floor(clamp(gcd * 1000.0, 50.0, 1500.0))
        return context.action.wait(wait_ms, "GCD")
    end

    if state.moving and not state.buff_swg then
        action = movement_action(context, api, state)
        if action then return action end
        return context.action.none("Moving - no instant")
    end

    action = damage_list(context, api, state, now)
    if action then return action end
    action = movement_action(context, api, state)
    if action then return action end
    return context.action.none("No Elemental action")
end

local function upkeep_action(context, api)
    refresh_spellbook(api)
    if can_cast(api, book.lightning_shield) then
        local up, _, _ = has_buff_named(api, "player", "Lightning Shield")
        if not up then
            return spell_action(context, book.lightning_shield, "player", "Lightning Shield")
        end
    end
    if can_cast(api, book.skyfury) then
        local up, _, _ = has_buff_named(api, "player", "Skyfury")
        if not up then
            return spell_action(context, book.skyfury, "player", "Skyfury")
        end
    end
    if cfg.auto_ghost_wolf and api.is_player_moving() and can_cast(api, book.ghost_wolf) then
        local wolf, _, _ = has_buff_named(api, "player", "Ghost Wolf")
        if not wolf then
            return spell_action(context, book.ghost_wolf, "player", "Ghost Wolf")
        end
    end
    return context.action.none("Out of combat")
end

local trinket_options = {
    { value = 0, label = "Disabled", tooltip = "Never use this trinket automatically." },
    { value = 1, label = "Smart", tooltip = "Choose between immediate use and burst alignment." },
    { value = 2, label = "With Major CDs", tooltip = "Use only inside the Ascendance burst package." },
    { value = 3, label = "Boss Only", tooltip = "Use only on bosses and training dummies." },
    { value = 4, label = "On Cooldown", tooltip = "Use whenever ready while cooldown automation is on." },
}

local groups = {
    { id = "automation", label = "Automation", icon = IDS.LIGHTNING_BOLT, collapsible = true, collapsed_default = false,
      description = "Core combat, defensive, and utility behavior." },
    { id = "cooldowns", label = "Cooldowns & Trinkets", icon = IDS.STORMKEEPER, collapsible = true, collapsed_default = false,
      description = "Control burst timing and on-use items." },
    { id = "advanced", label = "Advanced", icon = IDS.SKYFURY, collapsible = true, collapsed_default = true,
      description = "Overrides, niche automation, and diagnostics." },
}

local definitions = {
    { id = "automatic_target_recovery", type = "bool", label = "Auto Retarget", default = true, group = "automation",
      description = "Retargets only when your current enemy dies or becomes unreachable in combat." },
    { id = "auto_ghost_wolf", type = "bool", label = "Auto Ghost Wolf", default = true, group = "automation",
      description = "Automatically enters Ghost Wolf while moving between pulls." },
    { id = "survival_profile", type = "enum", label = "Defensive Profile", default = 1, group = "automation",
      description = "How early personal defensives and emergency healing fire.",
      enum_options = {
        { value = 0, label = "Aggressive", tooltip = "Delay personals to preserve damage globals" },
        { value = 1, label = "Balanced", tooltip = "Recommended automatic thresholds" },
        { value = 2, label = "Safe", tooltip = "Use personals and healing earlier" },
      } },
    { id = "utility_profile", type = "enum", label = "Utility Profile", default = 0, group = "automation",
      description = "How much interrupt and group-support automation to run.",
      enum_options = {
        { value = 0, label = "Smart", tooltip = "Automate interrupts, dispels, and efficient group support" },
        { value = 1, label = "Interrupts Only", tooltip = "Automate Wind Shear only" },
        { value = 2, label = "Manual", tooltip = "Leave utility manual" },
      } },
    { id = "cooldown_policy", type = "enum", label = "Cooldown Strategy", default = 0, group = "cooldowns",
      description = "When major cooldowns are allowed to fire.",
      enum_options = {
        { value = 0, label = "Smart", tooltip = "Use major cooldowns on bosses and worthwhile pulls." },
        { value = 1, label = "On Cooldown", tooltip = "Use major cooldowns whenever their toggles allow." },
        { value = 2, label = "Boss Only", tooltip = "Reserve major cooldowns for bosses and training dummies." },
      } },
    { id = "trinket_1_mode", type = "enum", label = "Trinket 1", default = 2, group = "cooldowns",
      description = "Automatic use rule for the first trinket slot.",
      enum_options = trinket_options },
    { id = "trinket_2_mode", type = "enum", label = "Trinket 2", default = 2, group = "cooldowns",
      description = "Automatic use rule for the second trinket slot.",
      enum_options = trinket_options },
    { id = "automatic_prepull", type = "bool", label = "Automatic Prepull", default = true, group = "cooldowns",
      description = "Uses Titan's pull timer to prepare Stormkeeper and Lava Burst." },
    { id = "content_mode", type = "enum", label = "Content Override", default = 0, group = "advanced",
      description = "Leave Automatic unless content detection is incorrect.",
      enum_options = {
        { value = 0, label = "Automatic", tooltip = "Detect Mythic+, raid, and solo play" },
        { value = 1, label = "Mythic+", tooltip = "Force dungeon pull logic" },
        { value = 2, label = "Raid", tooltip = "Force boss/raid logic" },
        { value = 3, label = "Solo", tooltip = "Force open-world logic" },
      } },
    { id = "hero_mode", type = "enum", label = "Hero Tree Override", default = 0, group = "advanced",
      description = "Leave Automatic unless testing a specific hero tree.",
      enum_options = {
        { value = 0, label = "Automatic", tooltip = "Detect the active hero talents" },
        { value = 1, label = "Farseer", tooltip = "Force Farseer sequencing" },
        { value = 2, label = "Stormbringer", tooltip = "Force Stormbringer sequencing" },
      } },
    { id = "auto_purge", type = "bool", label = "Automatic Purge", default = false, group = "advanced",
      description = "Automatically remove configured enemy magic buffs." },
    { id = "auto_stun", type = "bool", label = "Capacitor Backup Stun", default = false, group = "advanced",
      description = "Use Capacitor Totem as a configured Mythic+ backup stop." },
    { id = "queue_window_casting", type = "bool", label = "Queue Window Casting", default = true, group = "advanced",
      description = "Queue Lightning Bolt or Chain Lightning in Titan's spell queue window." },
    { id = "debug_diagnostics", type = "bool", label = "Debug Diagnostics", default = false, group = "advanced",
      description = "Enable lightweight rotation diagnostics for troubleshooting." },
}

local values = {}
for i = 1, #definitions do
    values[definitions[i].id] = definitions[i].default
end

local function apply_cfg()
    cfg.automatic_target_recovery = values.automatic_target_recovery ~= false
    cfg.auto_ghost_wolf = values.auto_ghost_wolf ~= false
    cfg.survival_profile = num(values.survival_profile)
    cfg.utility_profile = num(values.utility_profile)
    cfg.cooldown_policy = num(values.cooldown_policy)
    cfg.trinket_1_mode = num(values.trinket_1_mode)
    cfg.trinket_2_mode = num(values.trinket_2_mode)
    cfg.automatic_prepull = values.automatic_prepull ~= false
    cfg.content_mode = num(values.content_mode)
    cfg.hero_mode = num(values.hero_mode)
    cfg.auto_purge = values.auto_purge == true
    cfg.auto_stun = values.auto_stun == true
    cfg.queue_window_casting = values.queue_window_casting ~= false
    cfg.debug_diagnostics = values.debug_diagnostics == true
    cfg.use_racials = true
    cfg.use_natures_swiftness = true
end
apply_cfg()

return {
    identity = {
        name = "Llama's Elemental",
        author = "Llama",
        description = "Midnight Season 2 Elemental Shaman v2.4.0: native Lua",
        version = { major = 2, minor = 4, patch = 0 },
        class_id = 7,
        specialization_id = 262,
    },

    declarations = {
        macro_bindings = {
            { macro_body = EQ_CURSOR_MACRO, name = "Earthquake @cursor" },
            { macro_body = CAPACITOR_PLAYER_MACRO, name = "Capacitor Totem @player" },
        },
        friendly_spell_bindings = {
            { spell_id = IDS.HEALING_SURGE, name = "Healing Surge" },
            { spell_id = IDS.CLEANSE_SPIRIT, name = "Cleanse Spirit" },
        },
        custom_toggles = {
            { name = "Defensives", default_enabled = true },
            { name = "Utility", default_enabled = true },
            { name = "Mini CDs", default_enabled = true },
            { name = "Major CDs", default_enabled = true },
            { name = "Burst Now", default_enabled = false },
        },
        interrupt_spells = {
            { spell_id = IDS.WIND_SHEAR, range = 30.0, is_crowd_control = false, require_targeting = true, has_reticle = false },
            { spell_id = IDS.CAPACITOR_TOTEM, range = 8.0, is_crowd_control = true, require_targeting = false, has_reticle = true },
        },
        group_buff_spells = {
            { spell_id = IDS.SKYFURY, buff_id = IDS.SKYFURY, range = 100, name = "Skyfury" },
        },
    },

    settings = {
        groups = groups,
        definitions = definitions,
        values = values,
        apply = function(new_values)
            for i = 1, #definitions do
                local d = definitions[i]
                local v = new_values and new_values[d.id]
                if v == nil then v = d.default end
                values[d.id] = v
            end
            apply_cfg()
        end,
    },

    initialize = function()
        runtime.last_spellbook = -999.0
        runtime.last_success_index = 0
        runtime.last_sk_success = -999.0
        runtime.last_as_success = -999.0
        runtime.last_asc_success = -999.0
        runtime.last_vb_success = -999.0
        runtime.last_debug = -999.0
        runtime.last_recovery = -999.0
        runtime.unreachable_since = -999.0
        runtime.recovery_guid = ""
        runtime.queued_cast_start = 0
        runtime.last_setup_id = 0
        runtime.last_setup_time = -999.0
        book = {}
    end,

    shutdown = function()
    end,

    on_tick = function(context)
        local api = context.api
        if num(api.get_player_specialization_id()) ~= SPEC_ID then return end
        refresh_spellbook(api)
        observe_successes(api)
    end,

    get_combat_action = function(context)
        return combat_body(context)
    end,

    get_prepull_action = function(context)
        local api = context.api
        refresh_spellbook(api)
        if not cfg.automatic_prepull then
            return context.action.none("Prepull off")
        end
        local remain = num(context.prepull_seconds_remaining)
        if remain > 3.2 and can_cast(api, book.stormkeeper) then
            return spell_action(context, book.stormkeeper, "player", "Prepull Stormkeeper")
        end
        if remain > 0.2 and remain <= 1.7 and can_cast(api, book.lava_burst) then
            return spell_action(context, book.lava_burst, "target", "Prepull Lava Burst")
        end
        return context.action.none("Ready to pull")
    end,

    get_out_of_combat_action = function(context)
        local api = context.api
        -- Non-negotiable: a selected training dummy runs the full combat APL.
        if looks_like_dummy(api, "target") then
            return combat_body(context)
        end
        return upkeep_action(context, api)
    end,

    get_mounted_action = function(context)
        return context.action.none("Mounted")
    end,
}
