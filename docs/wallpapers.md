# Zone wallpapers

Watermarked 4K zone screenshots made by `tools/zone_wallpapers.py`, written to `~/Pictures/Wallpapers/ffxi-linux/`
as `<zone>-<n>.jpg`. The HDMI monitor's Plasma slideshow cycles that folder. The 8 hand-framed `*.png` scenes there
come from `tools/make_wallpapers.sh`.

Status as of 2026-10-18: first full run stopped by choice at zone 83 of 222 (alphabetical; last finished zone
`garlaige_citadel`). 67 zones have images.

## Resuming

`python3 tools/zone_wallpapers.py` skips zones that already have 5 images, so re-running continues with the
unstarted zones and retries the partial and failed ones. `--only ZONE ...` limits a run to specific zones.

Eligible zones: every LSB `data/zones/*/zone.yaml` of type city, outdoors, dungeon or dynamis, plus the open untyped
zones in `UNTYPED_OPEN`. Instanced zones, battlefield arenas, ships and GM areas are excluded.

## Known issues to look at

- Zones with no usable shots were rejected by the scoring checks (void below the horizon, geometry within the near
  probe distance, no depth past the far probe, too dark). Cramped or dark dungeons are expected here, but
  `caedarva_mire` is outdoors and should work; check whether the zone renders at all or is all fog/void.
- Dungeon yields are low (often 1-3 of 80 candidates). More spots or dungeon-specific camera rules would help.

## Done (5 images) — 58

| zone | id | type | images |
|---|---|---|---|
| abdhaljs_isle_purgonorgo | 44 | outdoors | 5 |
| abyssea_altepa | 218 | outdoors | 5 |
| abyssea_attohwa | 215 | outdoors | 5 |
| abyssea_grauberg | 254 | outdoors | 5 |
| abyssea_konschtat | 15 | outdoors | 5 |
| abyssea_la_theine | 132 | outdoors | 5 |
| abyssea_misareaux | 216 | outdoors | 5 |
| abyssea_tahrongi | 45 | outdoors | 5 |
| abyssea_uleguerand | 253 | outdoors | 5 |
| abyssea_vunkerl | 217 | outdoors | 5 |
| aht_urhgan_whitegate | 50 | city | 5 |
| al_zahbi | 48 | city | 5 |
| altaieu | 33 | outdoors | 5 |
| arrapago_reef | 54 | outdoors | 5 |
| attohwa_chasm | 7 | outdoors | 5 |
| bastok_jeuno_airship | 224 | outdoors | 5 |
| bastok_markets | 235 | city | 5 |
| bastok_markets_s | 87 | city | 5 |
| bastok_mines | 234 | city | 5 |
| batallia_downs | 105 | outdoors | 5 |
| batallia_downs_s | 84 | outdoors | 5 |
| beaucedine_glacier | 111 | outdoors | 5 |
| beaucedine_glacier_s | 136 | outdoors | 5 |
| behemoths_dominion | 127 | outdoors | 5 |
| bhaflau_thickets | 52 | outdoors | 5 |
| bibiki_bay | 4 | outdoors | 5 |
| buburimu_peninsula | 118 | outdoors | 5 |
| cape_teriggan | 113 | outdoors | 5 |
| carpenters_landing | 2 | outdoors | 5 |
| castle_oztroja | 151 | dungeon | 5 |
| castle_oztroja_s | 99 | dungeon | 5 |
| castle_zvahl_baileys | 161 | dungeon | 5 |
| castle_zvahl_baileys_s | 138 | dungeon | 5 |
| castle_zvahl_keep | 162 | dungeon | 5 |
| castle_zvahl_keep_s | 155 | dungeon | 5 |
| crawlers_nest_s | 171 | dungeon | 5 |
| dangruf_wadi | 191 | dungeon | 5 |
| dho_gates | 272 | dungeon | 5 |
| diorama_abdhaljs_ghelsba | 43 | outdoors | 5 |
| dynamis_bastok | 186 | dynamis | 5 |
| dynamis_beaucedine | 134 | dynamis | 5 |
| dynamis_buburimu | 40 | dynamis | 5 |
| dynamis_jeuno | 188 | dynamis | 5 |
| dynamis_qufim | 41 | dynamis | 5 |
| dynamis_san_doria | 185 | dynamis | 5 |
| dynamis_valkurm | 39 | dynamis | 5 |
| dynamis_windurst | 187 | dynamis | 5 |
| dynamis_xarcabard | 135 | dynamis | 5 |
| east_ronfaure | 101 | outdoors | 5 |
| east_ronfaure_s | 81 | outdoors | 5 |
| east_sarutabaruta | 116 | outdoors | 5 |
| eastern_adoulin | 257 | city | 5 |
| eastern_altepa_desert | 114 | outdoors | 5 |
| escha_ruaun | 289 | outdoors | 5 |
| escha_zitah | 288 | outdoors | 5 |
| feretory | 285 | city | 5 |
| foret_de_hennetiel | 262 | outdoors | 5 |
| fort_karugo_narugo_s | 96 | dungeon | 5 |

## Partial (fewer than 5) — 9

| zone | id | type | images |
|---|---|---|---|
| altar_room | 152 | dungeon | 1 |
| aydeewa_subterrane | 68 | dungeon | 2 |
| beadeaux_s | 92 | dungeon | 3 |
| ceizak_battlegrounds | 261 | outdoors | 4 |
| chateau_doraguille | 233 | city | 3 |
| chocobo_circuit | 70 | city | 3 |
| cirdas_caverns | 270 | dungeon | 2 |
| davoi | 149 | dungeon | 4 |
| fort_ghelsba | 141 | dungeon | 3 |

## No usable shots — 11

| zone | id | type | images |
|---|---|---|---|
| abyssea_empyreal_paradox | 255 | outdoors | 0 |
| alzadaal_undersea_ruins | 72 | dungeon | 0 |
| beadeaux | 147 | dungeon | 0 |
| bostaunieux_oubliette | 167 | dungeon | 0 |
| caedarva_mire | 79 | outdoors | 0 |
| celennia_memorial_library | 284 | city | 0 |
| crawlers_nest | 197 | dungeon | 0 |
| den_of_rancor | 160 | dungeon | 0 |
| dragons_aery | 154 | dungeon | 0 |
| dynamis_tavnazia | 42 | dynamis | 0 |
| feiyin | 204 | dungeon | 0 |

## Not started — 144

| zone | id | type | images |
|---|---|---|---|
| dynamis_bastok_d | 295 | dynamis | 0 |
| dynamis_jeuno_d | 297 | dynamis | 0 |
| dynamis_san_doria_d | 294 | dynamis | 0 |
| dynamis_windurst_d | 296 | dynamis | 0 |
| garlaige_citadel | 200 | dungeon | 0 |
| garlaige_citadel_s | 164 | dungeon | 0 |
| ghelsba_outpost | 140 | dungeon | 0 |
| giddeus | 145 | dungeon | 0 |
| grand_palace_of_huxzoi | 34 | dungeon | 0 |
| grauberg_s | 89 | outdoors | 0 |
| gusgen_mines | 196 | dungeon | 0 |
| gustav_tunnel | 212 | dungeon | 0 |
| hall_of_the_gods | 251 | city | 0 |
| hall_of_transference | 14 | dungeon | 0 |
| halvung | 62 | dungeon | 0 |
| heavens_tower | 242 | city | 0 |
| ifrits_cauldron | 205 | dungeon | 0 |
| inner_horutoto_ruins | 192 | dungeon | 0 |
| jugner_forest | 104 | outdoors | 0 |
| jugner_forest_s | 82 | outdoors | 0 |
| kamihr_drifts | 267 | outdoors | 0 |
| kazham | 250 | city | 0 |
| kazham_jeuno_airship | 226 | outdoors | 0 |
| king_ranperres_tomb | 190 | dungeon | 0 |
| konschtat_highlands | 108 | outdoors | 0 |
| korroloka_tunnel | 173 | dungeon | 0 |
| kuftal_tunnel | 174 | dungeon | 0 |
| la_theine_plateau | 102 | outdoors | 0 |
| la_vaule_s | 85 | outdoors | 0 |
| labyrinth_of_onzozo | 213 | dungeon | 0 |
| leafallia | 281 | open | 0 |
| lower_delkfutts_tower | 184 | dungeon | 0 |
| lower_jeuno | 245 | city | 0 |
| lufaise_meadows | 24 | outdoors | 0 |
| mamook | 65 | dungeon | 0 |
| manaclipper | 3 | outdoors | 0 |
| marjami_ravine | 266 | outdoors | 0 |
| maze_of_shakhrami | 198 | dungeon | 0 |
| meriphataud_mountains | 119 | outdoors | 0 |
| meriphataud_mountains_s | 97 | outdoors | 0 |
| metalworks | 237 | city | 0 |
| mhaura | 249 | city | 0 |
| middle_delkfutts_tower | 157 | dungeon | 0 |
| misareaux_coast | 25 | outdoors | 0 |
| mog_garden | 280 | city | 0 |
| moh_gates | 269 | dungeon | 0 |
| monastic_cavern | 150 | dungeon | 0 |
| mordion_gaol | 131 | dungeon | 0 |
| morimar_basalt_fields | 265 | outdoors | 0 |
| mount_zhayolm | 61 | outdoors | 0 |
| nashmau | 53 | outdoors | 0 |
| newton_movalpolos | 12 | dungeon | 0 |
| norg | 252 | city | 0 |
| north_gustaberg | 106 | outdoors | 0 |
| north_gustaberg_s | 88 | outdoors | 0 |
| northern_san_doria | 231 | city | 0 |
| oldton_movalpolos | 11 | dungeon | 0 |
| open_sea_route_to_al_zahbi | 46 | outdoors | 0 |
| open_sea_route_to_mhaura | 47 | outdoors | 0 |
| ordelles_caves | 193 | dungeon | 0 |
| outer_horutoto_ruins | 194 | dungeon | 0 |
| outer_rakaznar | 274 | dungeon | 0 |
| outer_rakaznar_u3 | 189 | city | 0 |
| palborough_mines | 143 | dungeon | 0 |
| pashhow_marshlands | 109 | outdoors | 0 |
| pashhow_marshlands_s | 90 | outdoors | 0 |
| phanauet_channel | 1 | outdoors | 0 |
| phomiuna_aqueducts | 27 | dungeon | 0 |
| port_bastok | 236 | city | 0 |
| port_jeuno | 246 | city | 0 |
| port_san_doria | 232 | city | 0 |
| port_windurst | 240 | city | 0 |
| promyvion_dem | 18 | dungeon | 0 |
| promyvion_holla | 16 | dungeon | 0 |
| promyvion_mea | 20 | dungeon | 0 |
| promyvion_vahzl | 22 | dungeon | 0 |
| psoxja | 9 | dungeon | 0 |
| qufim_island | 126 | outdoors | 0 |
| quicksand_caves | 208 | dungeon | 0 |
| qulun_dome | 148 | dungeon | 0 |
| rabao | 247 | city | 0 |
| rala_waterways | 258 | dungeon | 0 |
| ranguemont_pass | 166 | dungeon | 0 |
| reisenjima | 291 | open | 0 |
| riverne_site_a01 | 30 | dungeon | 0 |
| riverne_site_b01 | 29 | dungeon | 0 |
| rolanberry_fields | 110 | outdoors | 0 |
| rolanberry_fields_s | 91 | outdoors | 0 |
| romaeve | 122 | outdoors | 0 |
| ruaun_gardens | 130 | outdoors | 0 |
| rulude_gardens | 243 | city | 0 |
| sacrarium | 28 | dungeon | 0 |
| san_doria_jeuno_airship | 223 | outdoors | 0 |
| sauromugue_champaign | 120 | outdoors | 0 |
| sauromugue_champaign_s | 98 | outdoors | 0 |
| sea_serpent_grotto | 176 | dungeon | 0 |
| sealions_den | 32 | city | 0 |
| selbina | 248 | city | 0 |
| ship_bound_for_mhaura_pirates | 228 | outdoors | 0 |
| ship_bound_for_selbina_pirates | 227 | outdoors | 0 |
| sih_gates | 268 | dungeon | 0 |
| silver_sea_route_to_al_zahbi | 59 | outdoors | 0 |
| silver_sea_route_to_nashmau | 58 | outdoors | 0 |
| south_gustaberg | 107 | outdoors | 0 |
| southern_san_doria | 230 | city | 0 |
| southern_san_doria_s | 80 | city | 0 |
| tahrongi_canyon | 117 | outdoors | 0 |
| tavnazian_safehold | 26 | city | 0 |
| temple_of_uggalepih | 159 | dungeon | 0 |
| the_boyahda_tree | 153 | dungeon | 0 |
| the_colosseum | 71 | city | 0 |
| the_eldieme_necropolis | 195 | dungeon | 0 |
| the_eldieme_necropolis_s | 175 | dungeon | 0 |
| the_garden_of_ruhmet | 35 | dungeon | 0 |
| the_sanctuary_of_zitah | 121 | outdoors | 0 |
| the_shrine_of_ruavitau | 178 | dungeon | 0 |
| toraimarai_canal | 169 | dungeon | 0 |
| uleguerand_range | 5 | outdoors | 0 |
| upper_delkfutts_tower | 158 | dungeon | 0 |
| upper_jeuno | 244 | city | 0 |
| valkurm_dunes | 103 | outdoors | 0 |
| valley_of_sorrows | 128 | outdoors | 0 |
| velugannon_palace | 177 | dungeon | 0 |
| vunkerl_inlet_s | 83 | outdoors | 0 |
| wajaom_woodlands | 51 | outdoors | 0 |
| west_ronfaure | 100 | outdoors | 0 |
| west_sarutabaruta | 115 | outdoors | 0 |
| west_sarutabaruta_s | 95 | outdoors | 0 |
| western_adoulin | 256 | city | 0 |
| western_altepa_desert | 125 | outdoors | 0 |
| windurst_jeuno_airship | 225 | outdoors | 0 |
| windurst_walls | 239 | city | 0 |
| windurst_waters | 238 | city | 0 |
| windurst_waters_s | 94 | city | 0 |
| windurst_woods | 241 | city | 0 |
| woh_gates | 273 | dungeon | 0 |
| xarcabard | 112 | outdoors | 0 |
| xarcabard_s | 137 | outdoors | 0 |
| yahse_hunting_grounds | 260 | outdoors | 0 |
| yhoator_jungle | 124 | outdoors | 0 |
| yorcia_weald | 263 | outdoors | 0 |
| yughott_grotto | 142 | dungeon | 0 |
| yuhtunga_jungle | 123 | outdoors | 0 |
| zeruhn_mines | 172 | dungeon | 0 |
