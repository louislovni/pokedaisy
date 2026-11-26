// GPL-3.0. Live Gen 4/5 reader adapted from the Android companion.
#pragma once
#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>
#include <string>

enum class DsGame { Unsupported, Platinum, Johto, Black, White, Black2, White2 };
bool ds_unova(DsGame game);
bool ds_unova_sequel(DsGame game);
DsGame ds_companion_game(const char* sha1);
// Capsule (iOS): the French carts the Android app's ports cover, by header code
// and revision. Sets the addresses the companion reads (US ones for anything
// else) and returns the cart's game, or Unsupported. *soulSilver is set for SoulSilver.
DsGame ds_select_cart(const char code[4],int revision,bool* soulSilver);
// Maps the digest of a known decrypted dump to the digest of the retail dump it
// is the same ROM as, so every digest check downstream sees one id per game.
const char* ds_canonical_sha1(const char* sha1);
// Origin HeartGold (v4.0.3, English patch 1.0.0-rc8; OriginHeartGold.kt): a Japanese HeartGold's header (IPKJ rev 0)
// told apart by SaveData_Get's pool word. It runs as DsGame::Johto at its own addresses, with its tables read from the
// player's cart (map_art.cpp's ds_origin_read): 1025 species and their forms to 1440, 920 moves, Fairy as type 9.
struct DsOrigin {
  std::vector<std::string> species, moves, items;
  std::vector<std::array<int8_t,2>> speciesTypes; // by species / form id
  std::vector<std::array<int16_t,2>> moveMeta;    // by move: type, power
  std::vector<uint8_t> basePp;
  std::vector<int> machines;                      // TM01 .. TM92, HM01 .. HM08, TM93 .. TM130
  std::vector<int32_t> wildRates;                 // map header, walking rate, surfing rate, ...
  std::vector<std::array<uint16_t,3>> forms;      // species, form, the form's own species id
  // Heartfelt Gold and Chaos Purple: hg-engine over the US HeartGold (the US cart's RAM, a save of its own, species ids
  // that are Dex numbers to ARCEUS and Dex + 50 after, ds_hg_national_species). Origin HeartGold otherwise.
  bool usEngine=false, chaos=false;
  int lastSpecies=1440;                          // its last personal record
  std::vector<uint8_t> iconPalettes;             // usEngine: each species' icon palette
  std::vector<int> dexOrder;                     // usEngine: National Dex number -> species id (0 first)
  const char* title="Pokémon Origin HeartGold";
  int form_species(int species,int form)const{
    if(form)for(const auto& f:forms)if(f[0]==species&&f[1]==form)return f[2];
    return species;
  }
};
inline constexpr int dsOriginLastNational=1025, dsOriginLastSpecies=1440;
// Heartfelt Gold / Chaos Purple: No.n's species id.
inline int ds_hg_national_species(int n){return n<1?0:n<=493?n:n+50;}
bool ds_origin_cart(const std::vector<uint8_t>& rom);
// Heartfelt Gold (v1.0.15) or Chaos Purple (the demo): the US HeartGold's header with hg-engine's ARM9 and overlay table.
bool ds_hg_engine_cart(const std::vector<uint8_t>& rom,bool* chaos);
// Selects Origin HeartGold (its addresses, its tables) after ds_select_cart; null clears it.
void ds_select_origin(const DsOrigin* tables);
const DsOrigin* ds_origin();
int ds_egg_icon(DsGame game);
struct DsMon {
  int species=0, level=0, hp=0, maxHp=0, heldItem=0;
  uint32_t personality=0, experience=0, status=0;
  bool egg=false;
  int gender=0, nature=0, friendship=0;
  // Live max HP, Attack, Defense, Speed, Sp. Atk, Sp. Def, in cartridge order.
  std::array<int,6> stats{},ivs{},evs{};
  std::array<int,4> moves{}, pp{};
  std::string nickname;
};
struct DsItem { int id, count, pocket; };
struct DsCompanion {
  DsGame game=DsGame::Platinum;
  bool valid=false, dexValid=false, nationalDex=false;
  std::vector<uint8_t> dexSeen, dexCaught;
  std::vector<uint8_t> eventFlags;
  int seenCount=0, caughtCount=0;
  int mapX=-1,mapY=-1,mapSkin=0,mapUnlock=0,playerGender=0;
  uint64_t arrivals=0;
  int repelSteps=0, money=0, badges=0, map=0;
  std::string trainer;
  std::vector<DsMon> party;
  std::vector<DsItem> bag;
};
bool ds_event_flag(const DsCompanion&,unsigned flag);
int ds_unova_month(const uint8_t* ram,size_t size,DsGame game);
bool ds_decode_mon(const uint8_t* raw, size_t size, DsMon& mon, DsGame game=DsGame::Platinum);
DsCompanion ds_read_companion(const uint8_t* ram, size_t size,DsGame game=DsGame::Platinum);
const char* ds_species(int id,DsGame game=DsGame::Platinum);
const char* ds_move(int id,DsGame game=DsGame::Platinum);
const char* ds_item(int id,DsGame game=DsGame::Platinum);
const char* ds_place(int id,DsGame game=DsGame::Platinum);

const char* ds_nature(int id);
const char* ds_status(const DsMon& mon);

int ds_move_type(int id,DsGame game=DsGame::Platinum);
int ds_move_power(int id,DsGame game=DsGame::Platinum);
int ds_species_type(int id,int slot,DsGame game=DsGame::Platinum);

enum class DsRepelResult { Used, NotAvailable, StillActive, NoneLeft };
DsRepelResult ds_use_repel(const uint8_t* ram,size_t size,DsGame game,int item,
                         void (*write8)(uint32_t,uint8_t));
