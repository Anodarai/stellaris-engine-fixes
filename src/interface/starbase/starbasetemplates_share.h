// Template share codes: a template as one line of text that can be copied, pasted into chat and
// loaded again, in this game or another. Nothing here touches game memory.
//
//   SB1:<level>:<modules>:<buildings>:<name>:<check>
//
// - Slots are separated by ',' and an empty slot is an empty entry ("4,,4"). Trailing empty
//   slots are never written.
// - A base game level, module or building is written as its index in the tables below, in base
//   36 ("0" ... "z", "10" ...). Anything else (a mod's) is written in full after a '.':
//   ".giga_ring_module".
// - Keys and the name keep [A-Za-z0-9_.-]; a space is '+', any other byte %XX.
// - <check> is 4 hex digits of a hash of everything before it, so a truncated or mistyped code
//   is refused rather than loaded wrong.
//
// THE TABLES ARE PART OF THE FORMAT. Never reorder or remove an entry: a code made earlier would
// load as something else. A key a later game version adds goes at the end of its table. A
// different encoding gets another prefix ("SB2").
#pragma once
#include <cctype>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>
#include "starbasetemplates.h"

namespace starbasetemplates {
// Stellaris 4.5.4's own keys: the normal starbase's first, then orbital rings, deep space
// citadels and waystations, then everything else, in the order of the game's files.
inline constexpr const char* kShareLevels[] = {
    "starbase_level_outpost", "starbase_level_starport", "starbase_level_starhold", "starbase_level_starfortress",
    "starbase_level_citadel", "starbase_level_swarm", "starbase_level_ai", "starbase_level_exd_0",
    "starbase_level_exd", "starbase_level_marauder", "starbase_level_gatebuilders", "starbase_level_synth_queen",
    "starbase_level_synth_queen_big", "starbase_level_caravaneer", "starbase_level_fe_outpost",
    "starbase_level_voidworm_nest", "starbase_level_hatchery", "starbase_level_orbital_ring_tier_1",
    "starbase_level_orbital_ring_tier_2", "starbase_level_orbital_ring_tier_3", "starbase_level_deep_space_citadel_1",
    "starbase_level_deep_space_citadel_2", "starbase_level_deep_space_citadel_3", "starbase_level_waystation_1",
    "starbase_level_waystation_2", "starbase_level_waystation_3", "starbase_level_juggernaut",
    "starbase_level_enclave_mercenary_station", "starbase_level_enclave_salvager_station",
    "starbase_level_enclave_shroudwalker_station", "starbase_level_enclave_mindwarden_station",
    "starbase_level_civilian_arkship_1", "starbase_level_civilian_arkship_2", "starbase_level_civilian_arkship_3",
    "starbase_level_science_arkship_1", "starbase_level_science_arkship_2", "starbase_level_science_arkship_3",
    "starbase_level_military_arkship_1", "starbase_level_military_arkship_2", "starbase_level_military_arkship_3",
    "starbase_level_military_arkship_champions_forge",
};
inline constexpr const char* kShareModules[] = {
    "shipyard", "anchorage", "solar_panel_network", "trading_hub", "detection_array", "archaeo_rampart",
    "astromining_bay", "beastport", "vivarium_tank", "starbase_laser_battery", "starbase_archaeo_laser_battery",
    "starbase_particle_launcher", "starbase_psionic_torpedo", "starbase_mass_driver_battery",
    "starbase_archaeo_mass_driver_battery", "starbase_swarmer_missile_battery", "starbase_scourge_missile_battery",
    "starbase_hangar_bay", "starbase_archaeo_hangar_bay", "starbase_psionic_hangar_bay", "starbase_amoeba_hangar_bay",
    "starbase_swarm_hangar_bay", "orbital_ring_habitation", "orbital_ring_shipyard", "orbital_ring_anchorage",
    "orbital_ring_beastport", "orbital_ring_vivarium_tank", "orbital_ring_laser_battery",
    "orbital_ring_archaeo_laser_battery", "orbital_ring_particle_launcher", "orbital_ring_psionic_torpedo",
    "orbital_ring_mass_driver_battery", "orbital_ring_archaeo_mass_driver_battery",
    "orbital_ring_swarmer_missile_battery", "orbital_ring_scourge_missile_battery", "orbital_ring_hangar_bay",
    "orbital_ring_archaeo_hangar_bay", "orbital_ring_psionic_hangar_bay", "orbital_ring_amoeba_hangar_bay",
    "orbital_ring_swarm_hangar_bay", "waystation_energy_module", "waystation_mining_module",
    "waystation_hydroponics_module", "waystation_refinery_module", "waystation_research_module",
    "waystation_trade_module", "waystation_treasure_vault", "waystation_logistics_hub", "waystation_repair_bay",
    "waystation_resource_silo", "waystation_nebula_refinery", "waystation_observatory",
    "waystation_dimensional_shrine", "waystation_recruitment_office", "waystation_aura_emitter",
    "waystation_art_college", "waystation_curator_think_tank", "waystation_trader_proxy_office",
    "waystation_salvager_workshop", "waystation_cybrex_mining_hub", "waystation_yuht_detection_array",
    "waystation_hyperlane_detailer", "waystation_pirate_hideout", "waystation_concealment_field",
    "waystation_reaver_stronghold", "waystation_cloaking", "waystation_nanotech_module",
    "waystation_pyroclastic_resonator", "waystation_zroni_storm_caster",
    "waystation_cordyceptic_reanimation_facility", "waystation_growth_chamber", "waystation_aetherophasic_anchor",
    "waystation_gravitational_centrifuge", "waystation_voidlure_tiyanki", "waystation_voidlure_space_amoeba",
    "waystation_voidlure_crystalline_entity", "waystation_voidlure_voidworm", "waystation_voidlure_cutholoid",
    "waystation_laser_battery", "waystation_archaeo_laser_battery", "waystation_mass_driver_battery",
    "waystation_archaeo_mass_driver_battery", "waystation_swarmer_missile_battery", "waystation_particle_launcher",
    "waystation_psionic_torpedo", "waystation_scourge_missile_battery", "waystation_hangar_bay",
    "waystation_archaeo_hangar_bay", "waystation_psionic_hangar_bay", "waystation_amoeba_hangar_bay",
    "waystation_swarm_hangar_bay", "arkship_spinal_devastator_mount", "arkship_capital_shipyard",
    "arkship_planetary_refinery", "arkship_stellar_igniter", "arkship_system_scanner", "arkship_exodus_jump",
    "arkship_engineering_bay", "arkship_beastport", "arkship_docking_bay", "arkship_habitation_module",
    "arkship_sensor_array", "arkship_mining_array", "arkship_grand_archive", "arkship_voidlure", "arkship_hangar_bay",
    "arkship_archaeo_hangar_bay", "arkship_psionic_hangar_bay", "arkship_amoeba_hangar_bay",
    "arkship_swarm_hangar_bay", "arkship_laser_battery", "arkship_archaeo_laser_battery", "arkship_lance_battery",
    "arkship_particle_launcher", "arkship_psionic_torpedo", "arkship_arc_emitter_battery",
    "arkship_psionic_lightning_battery", "arkship_mass_driver_battery", "arkship_archaeo_mass_driver_battery",
    "arkship_kinetic_artillery_battery", "arkship_lm_accelerator", "arkship_mass_accelerator",
    "arkship_archaeo_mass_accelerator", "arkship_missile_battery", "arkship_archaeo_missile_battery",
    "arkship_swamer_missile_battery", "arkship_scourge_missile_battery", "arkship_flak_battery", "arkship_pd_battery",
    "arkship_archaeo_pd_battery",
};
inline constexpr const char* kShareBuildings[] = {
    "recruitment_office", "crew_quarters", "hydroponics_bay", "resource_silo", "nebula_refinery", "listening_post",
    "offworld_trading_company", "fleet_academy", "naval_logistics_office", "capital_yards", "black_hole_observatory",
    "target_uplink_computer", "defense_grid", "deep_space_black_site", "curator_think_tank", "art_college",
    "salvager_workshop", "shroud_beacon", "cybrex_mining_hub", "irassian_naval_yards", "yuht_detection_array",
    "zroni_storm_caster", "archaeo_overcharger", "feeder", "astromining_hub", "nanite_harvester",
    "communications_jammer", "disruption_field", "warp_fluctuator", "transit_hub", "command_center",
    "trader_proxy_office", "dragon_hatchery", "ice_mining_station", "offspring_outlook", "mercenary_garrison",
    "cordyceptic_reanimation_facility", "dark_matter_detector", "dark_matter_listening_post", "dimensional_shrine",
    "reloading_bay", "growth_chamber", "storm_attraction_computer", "storm_repelling_computer", "hyperlane_detailer",
    "voidlure_tiyanki", "voidlure_space_amoeba", "voidlure_crystalline_entity", "voidlure_voidworm",
    "voidlure_cutholoid", "pyroclastic_resonator", "aura_emitter", "gravitational_centrifuge", "ring_minerals_hub",
    "ring_energy_hub", "ring_food_hub", "ring_trade_hub", "ring_bureaucracy_hub", "ring_synapse_hub",
    "ring_maintenance_hub", "ring_consumer_goods_hub", "ring_alloys_hub", "ring_psi_corps",
    "ring_orbital_shield_generator", "ring_galactic_stock_exchange", "ring_embassy_complex", "ring_noble_estates",
    "ring_slave_processing_facility", "ring_feeder", "ring_growth_chamber", "ring_aura_emitter", "arkship_energy_hub",
    "arkship_food_hub", "arkship_trade_hub", "arkship_bureaucracy_hub", "arkship_synapse_hub",
    "arkship_maintenance_hub", "arkship_consumer_goods_hub", "arkship_alloys_hub", "arkship_psi_corps",
    "arkship_shield_generator", "arkship_galactic_stock_exchange", "arkship_embassy_complex", "arkship_noble_estates",
    "arkship_slave_processing_facility", "arkship_resource_silo", "arkship_listening_post", "arkship_fleet_academy",
    "arkship_deep_space_black_site", "arkship_irassian_naval_yards", "arkship_yuht_detection_array",
    "arkship_archaeo_overcharger", "arkship_warp_fluctuator", "arkship_detection_array", "arkship_hydroponics_bay",
    "arkship_harvest_processing_arrays", "arkship_dark_matter_detector", "arkship_armor", "arkship_lm_armor",
    "arkship_archaeo_armor", "arkship_shield", "arkship_psi_shield", "arkship_archaeo_shield",
    "arkship_crystal_plating", "arkship_afterburner", "arkship_armor_hardener", "arkship_shield_hardener",
    "arkship_growth_chamber", "arkship_quantum_destabilizer", "arkship_subspace_snare", "arkship_shield_dampener",
    "arkship_aura_strike_craft", "arkship_aura_hyperlane", "arkship_aura_pd_penetration", "arkship_aura_bombardment",
    "arkship_aura_range", "arkship_offspring_outlook",
};

inline constexpr char kSharePrefix[] = "SB1";
inline constexpr size_t kShareMaxSlots = 32;  // as many as the editor shows
inline constexpr size_t kShareMaxName = 128;  // as for a template's name
inline constexpr size_t kShareMaxText = 4096; // a pasted text longer than this is not a code

struct ShareTable {
    const char* const* keys;
    size_t count;
};
enum ShareField { kShareLevel, kShareModule, kShareBuilding };
inline constexpr ShareTable kShareTables[3] = {{kShareLevels, std::size(kShareLevels)},
                                               {kShareModules, std::size(kShareModules)},
                                               {kShareBuildings, std::size(kShareBuildings)}};

inline std::string ShareCheck(std::string_view body) {
    uint32_t h = 2166136261u; // FNV-1a, folded to 16 bits
    for (unsigned char c : body) {
        h ^= c;
        h *= 16777619u;
    }
    h = (h ^ (h >> 16)) & 0xFFFF;
    static const char hex[] = "0123456789abcdef";
    return {hex[(h >> 12) & 15], hex[(h >> 8) & 15], hex[(h >> 4) & 15], hex[h & 15]};
}
inline bool ShareRawChar(unsigned char c) {
    return (c < 0x80 && std::isalnum(c)) || c == '_' || c == '-' || c == '.';
}
inline std::string ShareEscape(std::string_view text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (ShareRawChar(c))
            out += static_cast<char>(c);
        else if (c == ' ')
            out += '+';
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
inline bool ShareUnescape(std::string_view text, std::string& out) {
    out.clear();
    auto digit = [](char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                      : -1;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%') {
            if (i + 2 >= text.size())
                return false; // needs two more characters
            const int hi = digit(text[i + 1]), lo = digit(text[i + 2]);
            if (hi < 0 || lo < 0)
                return false;
            out += static_cast<char>(hi * 16 + lo);
            i += 2;
        } else if (ShareRawChar(static_cast<unsigned char>(c))) {
            out += c;
        } else {
            return false;
        }
    }
    return true;
}
inline std::string ShareBase36(size_t n) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string out;
    do {
        out.insert(out.begin(), digits[n % 36]);
        n /= 36;
    } while (n);
    return out;
}
// The table index of a key, or -1.
inline int ShareIndex(ShareField field, const std::string& key) {
    const auto& t = kShareTables[field];
    for (size_t i = 0; i < t.count; ++i)
        if (key == t.keys[i])
            return static_cast<int>(i);
    return -1;
}
inline std::string ShareToken(ShareField field, const std::string& key) {
    if (key.empty())
        return {};
    const int index = ShareIndex(field, key);
    return index >= 0 ? ShareBase36(static_cast<size_t>(index)) : "." + ShareEscape(key);
}
// One slot or level: "" (empty), a table index in base 36, or '.' and an escaped key.
inline bool ShareKey(ShareField field, std::string_view token, std::string& key) {
    key.clear();
    if (token.empty())
        return true;
    if (token[0] == '.')
        return token.size() > 1 && ShareUnescape(token.substr(1), key) && !key.empty() && key.size() <= 256;
    if (token.size() > 2)
        return false;
    size_t index = 0;
    for (char c : token) {
        const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const int d = l >= '0' && l <= '9' ? l - '0' : l >= 'a' && l <= 'z' ? l - 'a' + 10 : -1;
        if (d < 0)
            return false;
        index = index * 36 + static_cast<size_t>(d);
    }
    if (index >= kShareTables[field].count)
        return false;
    key = kShareTables[field].keys[index];
    return true;
}
inline std::vector<std::string_view> ShareSplit(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (;;) {
        const size_t at = text.find(separator, start);
        if (at == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

// The code for a layout; empty when it can't have one (no level, too many slots, a long name).
inline std::string EncodeShare(const Layout& layout, const std::string& name) {
    if (layout.level.empty() || !Valid(layout) || name.size() > kShareMaxName)
        return {};
    std::string body = std::string(kSharePrefix) + ":" + ShareToken(kShareLevel, layout.level);
    for (ShareField field : {kShareModule, kShareBuilding}) {
        const auto& slots = field == kShareModule ? layout.modules : layout.buildings;
        size_t count = slots.size();
        while (count > 0 && slots[count - 1].empty())
            --count;
        if (count > kShareMaxSlots)
            return {};
        body += ':';
        for (size_t i = 0; i < count; ++i)
            body += (i ? "," : "") + ShareToken(field, slots[i]);
    }
    body += ":" + ShareEscape(name);
    return body + ":" + ShareCheck(body);
}

struct SharedTemplate {
    std::string name; // may be empty
    Layout layout;
};
// Reads a code. Whitespace anywhere is ignored, so a code that a chat window wrapped, or that was
// pasted with a line break, still loads; so are the prefix's and the hex digits' case. `error`
// says why a text is refused.
inline bool DecodeShare(std::string_view text, SharedTemplate& out, std::string& error) {
    if (text.size() > kShareMaxText) {
        error = "Text is too long for a template code";
        return false;
    }
    std::string code;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)))
            code += c;
    auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    if (code.size() < 4 || lower(code[0]) != 's' || lower(code[1]) != 'b' ||
        !std::isdigit(static_cast<unsigned char>(code[2]))) {
        error = "Not a template code (they start with SB1:)";
        return false;
    }
    const auto parts = ShareSplit(code, ':');
    if (parts[0].size() != 3 || parts[0][2] != kSharePrefix[2]) {
        error = "Code is from another version of the mod";
        return false;
    }
    if (parts.size() != 6) {
        error = "Code is incomplete or damaged";
        return false;
    }
    std::string check(parts[5]);
    for (auto& c : check)
        c = lower(c);
    const std::string body = kSharePrefix + code.substr(3, code.size() - 3 - parts[5].size() - 1);
    if (check != ShareCheck(body)) {
        error = "Code is damaged (its check does not match)";
        return false;
    }
    SharedTemplate parsed;
    if (!ShareKey(kShareLevel, parts[1], parsed.layout.level) || parsed.layout.level.empty()) {
        error = "Code has an unreadable level";
        return false;
    }
    for (ShareField field : {kShareModule, kShareBuilding}) {
        auto& slots = field == kShareModule ? parsed.layout.modules : parsed.layout.buildings;
        const auto list = parts[field == kShareModule ? 2 : 3];
        if (list.empty())
            continue;
        const auto tokens = ShareSplit(list, ',');
        if (tokens.size() > kShareMaxSlots) {
            error = "Code has too many slots";
            return false;
        }
        for (const auto token : tokens) {
            std::string key;
            if (!ShareKey(field, token, key)) {
                error = std::string("Code has an unreadable ") + (field == kShareModule ? "module" : "building");
                return false;
            }
            slots.push_back(std::move(key));
        }
        while (!slots.empty() && slots.back().empty())
            slots.pop_back();
    }
    if (!ShareUnescape(parts[4], parsed.name) || parsed.name.size() > kShareMaxName ||
        parsed.name.find_first_of(std::string("\r\n\t\0", 4)) != std::string::npos) {
        error = "Code has an unreadable name";
        return false;
    }
    if (!Valid(parsed.layout)) {
        error = "Template in the code is too large";
        return false;
    }
    out = std::move(parsed);
    return true;
}
} // namespace starbasetemplates
