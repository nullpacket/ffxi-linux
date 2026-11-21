-- SPDX-License-Identifier: GPL-3.0-or-later
-- ffxi-linux: Ashita's memory and resource interfaces (IEntity, IParty, IPlayer, ITarget, IRecast, ICastBar, IInventory,
-- IAutoFollow, GetEntity / GetPlayerEntity, IResourceManager) over the per-frame state tables of src/ashita/lua_state.cpp.
-- Appended to prelude.lua in one chunk: it uses that file's locals (xi, bit, ffi, unpack, interface, warn_once, M) and
-- publishes M.state, M.entity, M.memory_manager and M.resource_manager.
--
-- The values follow Ashita's SDK (plugins/sdk/Ashita.h and ffxi/*.h, the same in Ashita 4.3.2.1 and HorizonXI's 4.3.1.2)
-- and the way the bundled addons read them (docs/ashita-compat.md). Where the game keeps a value the session does not
-- have, the getter answers what an idle client would (0, empty) and says so here.
-------------------------------------------------------------------------------------------------------------------
-- Game state: one snapshot a frame, read through Ashita's memory interfaces.
-------------------------------------------------------------------------------------------------------------------
local cache = { frame = -1 };
local function state()
    local f = xi.frame();
    if (cache.frame ~= f) then
        cache.frame = f;
        cache.world = xi.world();
        cache.entities = nil;
        cache.raw = {};
        cache.party = nil;
        cache.recast = nil;
    end
    return cache.world;
end
local function entities()
    state();
    if (cache.entities == nil) then
        cache.entities = xi.entities();
    end
    return cache.entities;
end
local function entity(i)
    return entities()[tonumber(i) or -1];
end

-- Party slots as Ashita lays them out: 0..5 our party (0 = us), 6..11 and 12..17 the alliance's other parties. Our own
-- slot carries our live vitals (the game updates it from S2C 0x0DF as the others' slots).
local function party_slots()
    local w = state();
    if (cache.party ~= nil) then
        return cache.party;
    end
    local slots = {};
    local mine, other = {}, { {}, {} };
    local me = nil;
    for _, m in ipairs(w.party or {}) do
        if (m.id == w.charId) then
            me = m;
        elseif (m.partyNo == 0) then
            mine[#mine + 1] = m;
        else
            table.insert(other[m.partyNo] or other[1], m);
        end
    end
    table.sort(mine, function (a, b) return a.order < b.order; end);
    if ((w.charId or 0) ~= 0) then
        local s = {
            id = w.charId, index = w.actIndex, name = w.charName, hp = w.hp, mp = w.mp, tp = w.tp, hpp = w.hpp, mpp = w.mpp,
            zone = w.zone, mainJob = w.mainJob, mainLevel = w.mainLevel, subJob = w.subJob, subLevel = w.subLevel, partyNo = 0,
            leader = me ~= nil and me.leader or false, flags = me ~= nil and me.flags or 0, order = me ~= nil and me.order or 0,
            buffs = {}, self = true,
        };
        table.insert(mine, 1, s);
    end
    for i, m in ipairs(mine) do slots[i - 1] = m; end
    for p = 1, 2 do
        table.sort(other[p], function (a, b) return a.order < b.order; end);
        for i, m in ipairs(other[p]) do slots[p * 6 + i - 1] = m; end
    end
    cache.party = slots;
    return slots;
end

-- SpawnFlags (entity.h): 0x01 PC, 0x02 NPC, 0x04 party, 0x08 alliance, 0x10 monster, 0x200 the local player. As the
-- bundled LuAshitacast's table reads them: 0x09 an alliance member, 0x0D a party member; we are 0x20D (INFERRED).
local function spawn_flags(e)
    if (e.self) then return 0x20D; end
    local f;
    if (e.isPc) then f = 0x01; elseif (e.isMob) then f = 0x10; else f = 0x02; end
    if (e.party) then f = bit.bor(f, 0x0C); elseif (e.alliance) then f = bit.bor(f, 0x08); end
    return f;
end
-- Render flags as addons test them (INFERRED from their checks): Flags0 0x200 drawn, 0x2000 targetable, 0x4000 hidden;
-- Flags1 0x1000000 targetable. An actor the client does not draw has none; one with the TargetOffFlag has no
-- targetable bits.
local function render_flags(e, which)
    if (e.hidden) then return 0; end
    if (which == 0) then return e.targetOff and 0x200 or 0x2200; end
    if (which == 1) then return e.targetOff and 0 or 0x1000000; end
    return 0;
end

local IEntity = interface('IEntity', {});
local entity_getters = {
    GetLocalPositionX = function (e) return e.x; end,
    GetLocalPositionY = function (e) return e.y; end,
    GetLocalPositionZ = function (e) return e.z; end,
    GetLocalPositionW = function () return 1; end,
    GetLocalPositionYaw = function (e) return e.heading; end,
    GetLocalPositionRoll = function () return 0; end,
    GetLocalPositionPitch = function () return 0; end,
    GetLastPositionX = function (e) return e.x; end,
    GetLastPositionY = function (e) return e.y; end,
    GetLastPositionZ = function (e) return e.z; end,
    GetLastPositionW = function () return 1; end,
    GetLastPositionYaw = function (e) return e.heading; end,
    GetLastPositionRoll = function () return 0; end,
    GetLastPositionPitch = function () return 0; end,
    GetMoveX = function (e) return e.x; end,
    GetMoveY = function (e) return e.y; end,
    GetMoveZ = function (e) return e.z; end,
    GetMoveW = function () return 1; end,
    GetMoveDeltaX = function () return 0; end,
    GetMoveDeltaY = function () return 0; end,
    GetMoveDeltaZ = function () return 0; end,
    GetMoveDeltaW = function () return 0; end,
    GetMoveDeltaYaw = function () return 0; end,
    GetMoveDeltaRoll = function () return 0; end,
    GetMoveDeltaPitch = function () return 0; end,
    GetTargetIndex = function (e) return e.index; end,
    GetServerId = function (e) return e.id; end,
    GetName = function (e) return e.name; end,
    GetMovementSpeed = function (e) return (e.speed or 50) / 10; end,
    GetMovementSpeed2 = function (e) return (e.speed or 50) / 10; end,
    GetAnimationSpeed = function () return 1; end,
    GetActorPointer = function (e) return e.hidden and 0 or 1; end, -- non-zero: an actor exists (addons test it)
    GetAttachment = function () return 0; end,
    GetEventPointer = function () return 0; end,
    GetDistance = function (e) return e.distsq; end, -- squared, as the game keeps it
    GetTurnSpeed = function () return 0; end,
    GetTurnSpeedHead = function () return 0; end,
    GetHeading = function (e) return e.heading; end,
    GetNext = function () return 0; end,
    GetHPPercent = function (e) return e.hpp; end,
    GetType = function (e) return e.type; end,
    GetRace = function (e) return e.race; end,
    GetLocalMoveCount = function () return 0; end,
    GetActorLockFlag = function () return 0; end,
    GetModelUpdateFlags = function () return 0; end,
    GetDoorId = function (e) return e.doorId; end,
    GetLookHair = function (e) return e.look[0]; end,
    GetLookHead = function (e) return e.look[1]; end,
    GetLookBody = function (e) return e.look[2]; end,
    GetLookHands = function (e) return e.look[3]; end,
    GetLookLegs = function (e) return e.look[4]; end,
    GetLookFeet = function (e) return e.look[5]; end,
    GetLookMain = function (e) return e.look[6]; end,
    GetLookSub = function (e) return e.look[7]; end,
    GetLookRanged = function (e) return e.look[8]; end,
    GetActionTimer1 = function () return 0; end,
    GetActionTimer2 = function () return 0; end,
    GetRenderFlags0 = function (e) return render_flags(e, 0); end,
    GetRenderFlags1 = function (e) return render_flags(e, 1); end,
    GetRenderFlags2 = function (e) return render_flags(e, 2); end,
    GetRenderFlags3 = function (e) return render_flags(e, 3); end,
    GetRenderFlags4 = function (e) return render_flags(e, 4); end,
    GetRenderFlags5 = function () return 0; end,
    GetRenderFlags6 = function () return 0; end,
    GetRenderFlags7 = function () return 0; end,
    GetRenderFlags8 = function () return 0; end,
    GetPopEffect = function () return 0; end,
    GetUpdateMask = function () return 0; end,
    GetInteractionTargetIndex = function () return 0; end,
    GetCostumeId = function () return 0; end,
    -- Status and StatusServer: the server status (0 idle, 1 engaged, 2 / 3 dead, 33 resting, 85 mounted ...).
    GetStatus = function (e) return e.status; end,
    GetStatusServer = function (e) return e.status; end,
    GetStatusEvent = function () return 0; end,
    GetModelTime = function () return 0; end,
    GetModelStartTime = function () return 0; end,
    GetClaimStatus = function (e) return e.claimId; end, -- the claimer's server id
    GetZoneId = function (e) return e.self and state().zone or 0; end,
    GetAnimation = function () return 0; end,
    GetAnimationTime = function () return 0; end,
    GetAnimationStep = function () return 0; end,
    GetAnimationPlay = function () return 0; end,
    GetEmoteTargetIndex = function () return 0; end,
    GetEmoteId = function () return 0; end,
    GetEmoteIdString = function () return 0; end,
    GetEmoteTargetActorPointer = function () return 0; end,
    GetSpawnFlags = spawn_flags,
    GetLinkshellColor = function (e) return e.lsColor; end,
    GetNameColor = function () return 0; end,
    GetCampaignNameFlag = function () return 0; end,
    GetMountId = function (e) return e.mount; end,
    GetLastActionId = function () return 0; end,
    GetMogMotionId = function () return 0; end,
    GetTargetedIndex = function (e) return e.targetIndex; end,
    GetPetTargetIndex = function (e)
        if (e.self) then return state().petIndex or 0; end
        return 0;
    end,
    GetUpdateRequestDelay = function () return 0; end,
    GetIsDirty = function () return 0; end,
    GetBallistaFlags = function () return 0; end,
    GetPankrationEnabled = function () return 0; end,
    GetPankrationFlagFlip = function () return 0; end,
    GetModelSize = function () return -1; end, -- "generally -1" (entity.h)
    GetModelHitboxSize = function (e) return e.hitbox; end,
    GetEnvironmentAreaId = function () return 0; end,
    GetMonstrosityFlag = function () return 0; end,
    GetMonstrosityNameId = function () return 0; end,
    GetMonstrosityName = function () return ''; end,
    GetMonstrosityNameAbbr = function () return ''; end,
    GetFellowTargetIndex = function () return 0; end,
    GetWarpTargetIndex = function () return 0; end,
    GetTrustOwnerTargetIndex = function () return 0; end,
    GetAreaDisplayTargetIndex = function () return 0; end,
};
for k, f in pairs(entity_getters) do
    IEntity[k] = function (self, i, ...)
        local e = entity(i);
        if (e == nil) then
            if (k == 'GetName') then return nil; end
            return 0;
        end
        return f(e, ...);
    end
end
IEntity.GetEntityMapSize = function () return 0x900; end
IEntity.GetRawEntity = function (self, i) return GetEntity(i); end
-- Setters write the game's memory in Ashita; nothing here is writable.
setmetatable(IEntity, { __index = function (t, k)
    if (type(k) ~= 'string') then return nil; end
    local f;
    if (k:match('^Set')) then f = function () warn_once('IEntity:' .. k); end
    else f = function () warn_once('IEntity:' .. k); return 0; end end
    rawset(t, k, f);
    return f;
end });

-- GetEntity(index): the raw entity_t as a table (the fields addons read), one per entity per frame.
function GetEntity(i)
    i = tonumber(i) or -1;
    local e = entity(i);
    if (e == nil) then
        return nil;
    end
    local raw = cache.raw[i];
    if (raw ~= nil) then
        return raw;
    end
    local pos = { X = e.x, Y = e.y, Z = e.z, W = 1, Yaw = e.heading, Roll = 0, Pitch = 0 };
    raw = {
        TargetIndex = e.index, ServerId = e.id, Name = e.name, Distance = e.distsq, HPPercent = e.hpp, Heading = e.heading,
        Status = e.status, StatusServer = e.status, StatusEvent = 0, SpawnFlags = spawn_flags(e), Type = e.type, Race = e.race,
        ClaimStatus = e.claimId, TargetedIndex = e.targetIndex, PetTargetIndex = entity_getters.GetPetTargetIndex(e),
        MovementSpeed = (e.speed or 50) / 10, MovementSpeed2 = (e.speed or 50) / 10, AnimationSpeed = 1, ModelSize = -1,
        ModelHitboxSize = e.hitbox, MountId = e.mount, LinkshellColor = e.lsColor, NameColor = 0, DoorId = e.doorId,
        ActorPointer = e.hidden and 0 or 1, EventPointer = 0, ZoneId = e.self and state().zone or 0,
        FellowTargetIndex = 0, WarpTargetIndex = 0, TrustOwnerTargetIndex = 0, CostumeId = 0, EmoteId = 0, EmoteTargetIndex = 0,
        Movement = { LocalPosition = pos, LastPosition = { X = e.x, Y = e.y, Z = e.z, W = 1, Yaw = e.heading, Roll = 0, Pitch = 0 },
                     Move = { X = e.x, Y = e.y, Z = e.z, W = 1, DeltaX = 0, DeltaY = 0, DeltaZ = 0, DeltaW = 0, DeltaRoll = 0,
                              DeltaYaw = 0, DeltaPitch = 0 } },
        Look = { Hair = e.look[0], Head = e.look[1], Body = e.look[2], Hands = e.look[3], Legs = e.look[4], Feet = e.look[5],
                 Main = e.look[6], Sub = e.look[7], Ranged = e.look[8] },
        Render = { Flags0 = render_flags(e, 0), Flags1 = render_flags(e, 1), Flags2 = 0, Flags3 = 0, Flags4 = 0, Flags5 = 0,
                   Flags6 = 0, Flags7 = 0, Flags8 = 0 },
    };
    cache.raw[i] = raw;
    return raw;
end
function GetPlayerEntity()
    local w = state();
    if ((w.login or 0) ~= 2) then
        return nil;
    end
    return GetEntity(w.actIndex);
end

-------------------------------------------------------------------------------------------------------------------
-- IParty
-------------------------------------------------------------------------------------------------------------------
local IParty = interface('IParty', {});
local function member(i) return party_slots()[tonumber(i) or -1]; end
local party_getters = {
    GetMemberIndex = function (m) return m.partyNo; end,     -- the party it is in (party.h: "Index of their current party")
    GetMemberNumber = function (m) return m.order; end,
    GetMemberName = function (m) return m.name; end,
    GetMemberServerId = function (m) return m.id; end,
    GetMemberServerId2 = function (m) return m.id; end,
    GetMemberTargetIndex = function (m) return m.index; end,
    GetMemberLastUpdatedTimestamp = function () return os.time(); end,
    GetMemberHP = function (m) return m.hp; end,
    GetMemberMP = function (m) return m.mp; end,
    GetMemberTP = function (m) return m.tp; end,
    GetMemberHPPercent = function (m) return m.hpp; end,
    GetMemberMPPercent = function (m) return m.mpp; end,
    GetMemberHPPercent2 = function (m) return m.hpp; end,
    GetMemberMPPercent2 = function (m) return m.mpp; end,
    GetMemberZone = function (m) return m.zone; end,
    -- Zone2: only set while the member is in another zone than ours (party.h).
    GetMemberZone2 = function (m) return (m.zone ~= state().zone) and m.zone or 0; end,
    GetMemberFlagMask = function (m) return m.flags or 0; end,
    GetMemberMainJob = function (m) return m.mainJob; end,
    GetMemberMainJobLevel = function (m) return m.mainLevel; end,
    GetMemberSubJob = function (m) return m.subJob; end,
    GetMemberSubJobLevel = function (m) return m.subLevel; end,
    GetMemberMasterLevel = function () return 0; end,
    GetMemberMasterBreaker = function () return 0; end,
    GetMemberMonstrosityItemId = function () return 0; end,
    GetMemberMonstrosityPrefixFlag1 = function () return 0; end,
    GetMemberMonstrosityPrefixFlag2 = function () return 0; end,
    GetMemberMonstrosityName = function () return ''; end,
    GetMemberIsActive = function () return 1; end,
};
for k, f in pairs(party_getters) do
    IParty[k] = function (self, i)
        local m = member(i);
        if (m == nil) then
            if (k == 'GetMemberName' or k == 'GetMemberMonstrosityName') then return ''; end
            return 0;
        end
        return f(m);
    end
end
-- Our own lots are the only ones the client knows (the pool's lot column); others' come as 0.
IParty.GetMemberTreasureLot = function (self, i, slot)
    local m = member(i);
    if (m == nil or not m.self) then return 0; end
    local t = (state().treasure or {})[slot];
    return (t and t.id ~= 0 and t.lot >= 0) and t.lot or 0;
end
local function party_count(p)
    local n = 0;
    for i = p * 6, p * 6 + 5 do if (party_slots()[i] ~= nil) then n = n + 1; end end
    return n;
end
local function leader_of(p)
    for i = p * 6, p * 6 + 5 do local m = party_slots()[i]; if (m and bit.band(m.flags or 0, 0x04) ~= 0) then return m.id; end end
    return 0;
end
IParty.GetAlliancePartyMemberCount1 = function () return party_count(0); end
IParty.GetAlliancePartyMemberCount2 = function () return party_count(1); end
IParty.GetAlliancePartyMemberCount3 = function () return party_count(2); end
IParty.GetAlliancePartyVisible1 = function () return party_count(0) > 0 and 1 or 0; end
IParty.GetAlliancePartyVisible2 = function () return party_count(1) > 0 and 1 or 0; end
IParty.GetAlliancePartyVisible3 = function () return party_count(2) > 0 and 1 or 0; end
IParty.GetAlliancePartyLeaderServerId1 = function () return leader_of(0); end
IParty.GetAlliancePartyLeaderServerId2 = function () return leader_of(1); end
IParty.GetAlliancePartyLeaderServerId3 = function () return leader_of(2); end
IParty.GetAllianceLeaderServerId = function ()
    for i = 0, 17 do local m = party_slots()[i]; if (m and bit.band(m.flags or 0, 0x08) ~= 0) then return m.id; end end
    return 0;
end
IParty.GetAllianceInvited = function () return 0; end
IParty.GetAllianceInviteParty = function () return 0; end
-- The other members' status icons (S2C 0x076), five entries: the low byte of each id (0xFF none) at 1..32 and the high
-- bits two per icon in BitMask (exact to icon 26; a double holds 53 bits).
local function icon_entry(i)
    local m = member((tonumber(i) or -1) + 1);
    if (m == nil or m.partyNo ~= 0) then return nil; end
    return m;
end
IParty.GetStatusIconsServerId = function (self, i) local m = icon_entry(i); return m and m.id or 0; end
IParty.GetStatusIconsTargetIndex = function (self, i) local m = icon_entry(i); return m and m.index or 0; end
IParty.GetStatusIcons = function (self, i)
    local m, t = icon_entry(i), {};
    for k = 1, 32 do
        local id = m and m.buffs and m.buffs[k];
        t[k] = id and bit.band(id, 0xFF) or 0xFF;
    end
    return t;
end
IParty.GetStatusIconsBitMask = function (self, i)
    local m, mask = icon_entry(i), 0;
    for k = 1, 32 do
        local id = m and m.buffs and m.buffs[k];
        if (id ~= nil) then mask = mask + bit.band(bit.rshift(id, 8), 3) * 2 ^ (2 * (k - 1)); end
    end
    return mask;
end

-------------------------------------------------------------------------------------------------------------------
-- IPlayer
-------------------------------------------------------------------------------------------------------------------
local IPlayer = interface('IPlayer', {});
IPlayer.GetHPMax = function () return state().hpMax or 0; end
IPlayer.GetMPMax = function () return state().mpMax or 0; end
IPlayer.GetMainJob = function () return state().mainJob or 0; end
IPlayer.GetMainJobLevel = function () return state().mainLevel or 0; end
IPlayer.GetSubJob = function () return state().subJob or 0; end
IPlayer.GetSubJobLevel = function () return state().subLevel or 0; end
IPlayer.GetExpCurrent = function () return state().exp or 0; end
IPlayer.GetExpNeeded = function () return state().expNext or 0; end
IPlayer.GetStat = function (self, i) local s = state().statBase; return s and s[i] or 0; end
IPlayer.GetStatModifier = function (self, i) local s = state().statBonus; return s and s[i] or 0; end
IPlayer.GetAttack = function () return state().attack or 0; end
IPlayer.GetDefense = function () return state().defense or 0; end
IPlayer.GetResist = function (self, i) local s = state().resist; return s and s[i] or 0; end
IPlayer.GetTitle = function () return state().title or 0; end
IPlayer.GetRank = function () return state().rank or 0; end
IPlayer.GetRankPoints = function () return state().rankBar or 0; end
IPlayer.GetHomepoint = function () return state().bindZone or 0; end
IPlayer.GetNation = function () return state().nation or 0; end
IPlayer.GetResidence = function () return 0; end
IPlayer.GetSuLevel = function () return state().suLevel or 0; end
IPlayer.GetItemLevel = function () return state().itemLevel or 0; end
IPlayer.GetHighestItemLevel = function () return state().itemLevel or 0; end
IPlayer.GetMainHandItemLevel = function () return state().itemLevel or 0; end
IPlayer.GetRangedItemLevel = function () return 0; end
IPlayer.GetUnityFaction = function () return state().unity or 0; end
IPlayer.GetUnityPoints = function () return 0; end
IPlayer.GetMasteryJob = function () return 0; end
IPlayer.GetMasteryJobLevel = function () return 0; end
IPlayer.GetMasteryFlags = function () return 0; end
IPlayer.GetMasteryExp = function () return 0; end
IPlayer.GetMasteryExpNeeded = function () return 0; end
IPlayer.GetJobLevel = function (self, j) local l = state().jobLevels; return l and l[j] or 0; end
IPlayer.GetJobMasterFlags = function () return 0; end
IPlayer.GetJobMasterLevel = function () return 0; end
-- 0 not logged in, 1 loading, 2 in the game.
IPlayer.GetLoginStatus = function () return state().login or 0; end
IPlayer.GetIsZoning = function () return (state().login == 2) and 0 or 1; end
IPlayer.GetPetTP = function () return state().petTp or 0; end
IPlayer.GetPetMPPercent = function () return state().petMpp or 0; end
IPlayer.GetPetHPPercent = function () return state().petHpp or 0; end
IPlayer.GetDataLoadedFlags = function () return 0xFF; end
-- Buffs: our status effect ids, -1 where empty; StatusIcons the same ids with 255 where empty, and StatusTimers their
-- expiry as S2C 0x063 sent it: (seconds since the Vana'diel epoch 1009810800) x 60, wrapping, 0x7FFFFFFF none.
IPlayer.GetBuffs = function ()
    local t, b = {}, state().buffs or {};
    for k = 1, 32 do t[k] = b[k] and b[k][1] or -1; end
    return t;
end
IPlayer.GetStatusIcons = function ()
    local t, b = {}, state().buffs or {};
    for k = 1, 32 do t[k] = b[k] and b[k][1] or 255; end
    return t;
end
IPlayer.GetStatusTimers = function ()
    local t, b = {}, state().buffs or {};
    for k = 1, 32 do t[k] = b[k] and b[k][3] or 0x7FFFFFFF; end
    return t;
end
local function skill_object(v, craft)
    v = v or 0;
    if (craft) then
        return { GetSkill = function () return bit.band(bit.rshift(v, 5), 0x3FF); end, GetRank = function () return bit.band(v, 0x1F); end,
                 IsCapped = function () return bit.band(v, 0x8000) ~= 0; end, GetRaw = function () return v; end };
    end
    return { GetSkill = function () return bit.band(v, 0x7FFF); end, IsCapped = function () return bit.band(v, 0x8000) ~= 0; end,
             GetRaw = function () return v; end };
end
IPlayer.GetCombatSkill = function (self, i) local s = state().skills; return skill_object(s and s[i], false); end
IPlayer.GetCraftSkill = function (self, i) local s = state().skills; return skill_object(s and s[48 + i], true); end
IPlayer.HasSpell = function (self, id) local s = state().spells; return s ~= nil and s[id] == true; end
IPlayer.HasKeyItem = function (self, id) local s = state().keyItems; return s ~= nil and s[id] == true; end
IPlayer.HasAbility = function (self, id)
    local w = state();
    if (id >= 512) then return (w.jobAbilities or {})[id - 512] == true; end
    return (w.weaponSkills or {})[id] == true;
end
IPlayer.HasWeaponSkill = function (self, id) return (state().weaponSkills or {})[id] == true; end
IPlayer.HasTrait = function (self, id) return (state().traits or {})[id] == true; end
IPlayer.HasPetCommand = function (self, id) return (state().petAbilities or {})[id] == true; end
IPlayer.HasAbilityData = function () return true; end
IPlayer.HasSpellData = function () return true; end
-- The player's own copy of the ability timers (seconds, by slot; INFERRED to match IRecast's).
local function recast_slot(i) return (state().abilityRecasts or {})[tonumber(i) or -1]; end
IPlayer.GetAbilityRecast = function (self, i) local r = recast_slot(i); return r and math.ceil(r.left) or 0; end
IPlayer.GetAbilityRecastTimerId = function (self, i) local r = recast_slot(i); return r and r.id or 0; end
IPlayer.GetAbilityRecastCalc1 = function (self, i) local r = recast_slot(i); return r and r.calc1 or 0; end
IPlayer.GetAbilityRecastCalc2 = function (self, i) local r = recast_slot(i); return r and r.calc2 or 0; end
IPlayer.GetMountRecast = function () return 0; end
IPlayer.GetMountRecastTimerId = function () return 0; end
IPlayer.GetMeritPoints = function () return 0; end
IPlayer.GetMeritPointsMax = function () return 0; end
IPlayer.GetLimitPoints = function () return 0; end
IPlayer.GetAssimilationPoints = function () return 0; end
IPlayer.GetIsLimitBreaker = function () return false; end
IPlayer.GetIsExperiencePointsLocked = function () return false; end
IPlayer.GetIsLimitModeEnabled = function () return false; end
IPlayer.GetJobPoints = function () return 0; end
IPlayer.GetJobPointsSpent = function () return 0; end
IPlayer.GetCapacityPoints = function () return 0; end
IPlayer.GetSubMapNum = function () return 0; end

-------------------------------------------------------------------------------------------------------------------
-- ITarget: two entries. With the sub-target cursor up, entry 0 is the cursor's and entry 1 the main target (how the
-- bundled LuAshitacast, libs/ffxi/targets.lua and statustimers read it); otherwise entry 0 is the target.
-------------------------------------------------------------------------------------------------------------------
local ITarget = interface('ITarget', {});
local function target_index(i)
    local t = state().target or {};
    if (t.subActive) then
        if (i == 1) then return t.index or 0; end
        return t.sub or 0;
    end
    if (i == 1) then return 0; end
    return t.index or 0;
end
ITarget.GetTargetIndex = function (self, i) return target_index(tonumber(i) or 0); end
ITarget.GetServerId = function (self, i)
    local e = entity(target_index(tonumber(i) or 0));
    return e and e.id or 0;
end
ITarget.GetEntityPointer = function (self, i) return entity(target_index(tonumber(i) or 0)) and 1 or 0; end
ITarget.GetActorPointer = function (self, i) local e = entity(target_index(tonumber(i) or 0)); return (e and not e.hidden) and 1 or 0; end
ITarget.GetIsActive = function (self, i) return target_index(tonumber(i) or 0) ~= 0 and 1 or 0; end
ITarget.GetIsModelActor = function (self, i) return target_index(tonumber(i) or 0) ~= 0 and 1 or 0; end
ITarget.GetIsArrowActive = function (self, i) return target_index(tonumber(i) or 0) ~= 0 and 1 or 0; end
ITarget.GetIsSubTargetActive = function () return (state().target or {}).subActive and 1 or 0; end
ITarget.GetIsMenuOpen = function () return 0; end
ITarget.GetLockedOnFlags = function () return 0; end
ITarget.GetSubTargetFlags = function () return 0; end
ITarget.GetIsPlayerMoving = function () return 0; end
ITarget.GetIsWalk = function () return 0; end
ITarget.GetIsAutoNotice = function () return 0; end
ITarget.GetActionTargetActive = function () return 0; end
ITarget.GetFocusTargetIndex = function () return 0; end
ITarget.GetFocusTargetServerId = function () return 0; end
ITarget.SetTarget = function (self, index) xi.set_target(index); return true; end
-- The target window shows the main target.
local function window_entity() local t = state().target or {}; return entity(t.index or 0); end
ITarget.GetWindowName = function () local e = window_entity(); return e and e.name or ''; end
ITarget.GetWindowHPPercent = function () local e = window_entity(); return e and e.hpp or 0; end
ITarget.GetLastTargetName = function () local e = window_entity(); return e and e.name or ''; end
ITarget.GetLastTargetIndex = function () local e = window_entity(); return e and e.index or 0; end
ITarget.GetLastTargetServerId = function () local e = window_entity(); return e and e.id or 0; end

-------------------------------------------------------------------------------------------------------------------
-- IRecast: S2C 0x119's 31 ability timers in the order the server sent them (slot 0: the two-hour, id 0), counted in the
-- game's 1/60 s ticks; spell timers by spell id, from the recast in our own 0x028 actions.
-------------------------------------------------------------------------------------------------------------------
local IRecast = interface('IRecast', {});
IRecast.GetAbilityTimerId = function (self, i) local r = recast_slot(i); return r and r.id or 0; end
IRecast.GetAbilityTimer = function (self, i) local r = recast_slot(i); return r and math.floor(r.left * 60) or 0; end
IRecast.GetAbilityRecast = IRecast.GetAbilityTimer;
IRecast.GetAbilityCalc1 = function (self, i) local r = recast_slot(i); return r and r.calc1 or 0; end
IRecast.GetAbilityCalc2 = function (self, i) local r = recast_slot(i); return r and r.calc2 or 0; end
IRecast.GetSpellTimer = function (self, id) local r = state().spellRecast or {}; return math.floor((r[id] or 0) * 60); end

-------------------------------------------------------------------------------------------------------------------
-- ICastBar: our own cast, from the spell's cast time.
-------------------------------------------------------------------------------------------------------------------
local ICastBar = interface('ICastBar', {});
ICastBar.GetPercent = function ()
    local w = state();
    if ((w.castSpell or 0) == 0) then return 0; end
    local sp = xi.spell(w.castSpell);
    local total = sp and (sp.CastTime / 4) or 1;
    return math.min(1, (w.castElapsed or 0) / math.max(total, 0.01));
end
ICastBar.GetCount = function () return math.floor(ICastBar.GetPercent() * 100); end
ICastBar.GetMax = function () return 100; end
ICastBar.GetCastType = function () return 0; end

-------------------------------------------------------------------------------------------------------------------
-- IInventory
-------------------------------------------------------------------------------------------------------------------
local IInventory = interface('IInventory', {});
local function blank_extra(s) s = s or ''; return s .. string.rep('\0', 28 - #s); end
-- item_t: Id, Index, Count, Flags (the LockFlg: 5 equipped, 19 bazaar ...), Price (bazaar), Extra (28 bytes). Every slot
-- of a container answers, empty ones with Id 0; slot 0 of the inventory is the gil (Id 0xFFFF).
IInventory.GetContainerItem = function (self, c, i)
    local box = (state().inventory or {})[c];
    local it = box and box[i];
    if (it == nil) then
        return { Id = 0, Index = i, Count = 0, Flags = 0, Price = 0, Extra = blank_extra() };
    end
    local id = it.id;
    if (c == 0 and i == 0) then id = 0xFFFF; end
    return { Id = id, Index = i, Count = it.count, Flags = it.flags or 0, Price = it.price or 0, Extra = blank_extra(it.extra) };
end
-- ContainerMaxCapacity: S2C 0x01C's ItemNum2 (1 + the usable slots, 0 for a container the character cannot use; the bundled
-- LuAshitacast clamps it to 80). INFERRED: Ashita answers this copy rather than ItemNum; they differ only for a locked
-- container. Before 0x01C arrives: 0.
IInventory.GetContainerCountMax = function (self, c)
    local w = state();
    if (not w.containerSizeValid) then return 0; end
    local v = (w.containerUsable or {})[c];
    return v or 0;
end
IInventory.GetContainerCount = function (self, c)
    local n, box = 0, (state().inventory or {})[c] or {};
    for k, v in pairs(box) do if ((k > 0 or c ~= 0) and v.id ~= 0) then n = n + 1; end end
    return n;
end
IInventory.GetEquippedItem = function (self, slot)
    local eq = (state().equipment or {})[slot];
    if (eq == nil or eq[2] == 0) then
        return { Slot = slot, Index = 0 };
    end
    return { Slot = slot, Index = eq[1] * 256 + eq[2] };
end
-- treasureitem_t; empty slots answer with ItemId 0. Lot: ours (0 none, 0xFFFF passed).
IInventory.GetTreasurePoolItem = function (self, i)
    local t = (state().treasure or {})[tonumber(i) or -1];
    if (t == nil or t.id == 0) then
        return { Use = 0, ItemId = 0, Count = 0, Flags = 0, Price = 0, Extra = blank_extra(), Status = 0, Lot = 0, WinningLot = 0,
                 WinningEntityServerId = 0, WinningEntityTargetIndex = 0, WinningEntityName = '', TimeToLive = 0, DropTime = 0 };
    end
    return { Use = 1, ItemId = t.id, Count = t.count, Flags = 0, Price = 0, Extra = blank_extra(), Status = 0,
             Lot = (t.lot < 0) and 0xFFFF or t.lot, WinningLot = t.bestLot, WinningEntityServerId = 0, WinningEntityTargetIndex = 0,
             WinningEntityName = t.bestName, TimeToLive = math.floor(t.ttl), DropTime = t.dropTime };
end
IInventory.GetTreasurePoolItemCount = function ()
    local n = 0;
    for _, t in pairs(state().treasure or {}) do if (t.id ~= 0) then n = n + 1; end end
    return n;
end
IInventory.GetTreasurePoolStatus = function () return 1; end -- 1: loaded
IInventory.GetContainerUpdateCounter = function () return state().inventoryCounter or 0; end
IInventory.GetCheckTargetIndex = function () return 0; end
IInventory.GetCheckServerId = function () return 0; end
IInventory.GetSearchComment = function () return ''; end
IInventory.GetCraftStatus = function () return 0; end

local IAutoFollow = interface('IAutoFollow', {});
IAutoFollow.GetIsAutoRunning = function () return 0; end
IAutoFollow.GetFollowTargetIndex = function () return 0; end
IAutoFollow.GetFollowTargetServerId = function () return 0; end

local memory_manager = interface('IMemoryManager', {
    GetEntity = function () return IEntity; end,
    GetParty = function () return IParty; end,
    GetPlayer = function () return IPlayer; end,
    GetTarget = function () return ITarget; end,
    GetRecast = function () return IRecast; end,
    GetCastBar = function () return ICastBar; end,
    GetInventory = function () return IInventory; end,
    GetAutoFollow = function () return IAutoFollow; end,
});

-------------------------------------------------------------------------------------------------------------------
-- Resources
-------------------------------------------------------------------------------------------------------------------
local job_names = { [0] = 'None', 'Warrior', 'Monk', 'White Mage', 'Black Mage', 'Red Mage', 'Thief', 'Paladin', 'Dark Knight', 'Beastmaster',
    'Bard', 'Ranger', 'Samurai', 'Ninja', 'Dragoon', 'Summoner', 'Blue Mage', 'Corsair', 'Puppetmaster', 'Dancer', 'Scholar', 'Geomancer', 'Rune Fencer' };
local job_abbr = { [0] = 'NON', 'WAR', 'MNK', 'WHM', 'BLM', 'RDM', 'THF', 'PLD', 'DRK', 'BST', 'BRD', 'RNG', 'SAM', 'NIN', 'DRG', 'SMN', 'BLU', 'COR',
    'PUP', 'DNC', 'SCH', 'GEO', 'RUN' };
local race_names = { [0] = 'Unknown', 'Hume', 'Hume', 'Elvaan', 'Elvaan', 'Tarutaru', 'Tarutaru', 'Mithra', 'Galka' };
local days = { [0] = 'Firesday', 'Earthsday', 'Watersday', 'Windsday', 'Iceday', 'Lightningday', 'Lightsday', 'Darksday' };
local function nonempty(s) return (s ~= nil and s ~= '') and s or nil; end
local function item_field(i, k) local it = xi.item(i); return it and nonempty(it[k][1]) or nil; end
local string_tables = {
    ['jobs.names'] = function (i) return job_names[i]; end,
    ['jobs.names_abbr'] = function (i) return job_abbr[i]; end,
    ['races.names'] = function (i) return race_names[i]; end,
    ['days.names'] = function (i) return days[i]; end,
    ['zones.names'] = function (i) return nonempty(xi.zone_name(i)); end,
    ['areas'] = function (i) return nonempty(xi.zone_name(i)); end,
    ['buffs.names'] = function (i) return nonempty(xi.status(i)); end,
    ['buffs.names_log'] = function (i) return nonempty(xi.status(i)); end, -- INFERRED: the log uses the same names
    ['statusnames'] = function (i) return nonempty(xi.status(i)); end,
    ['keyitems.names'] = function (i) return nonempty(xi.key_item(i)); end,
    ['items.names'] = function (i) return item_field(i, 'Name'); end,
    ['items.descriptions'] = function (i) return item_field(i, 'Description'); end,
    -- Monster TP moves: Ashita's index is the move's id less 256 (the bundled LuAshitacast and rWarnings subtract it);
    -- file 7035 (ROM/27/80) holds them by that index.
    ['monsters.abilities'] = function (i) return nonempty(xi.monster_ability(i)); end,
};
-- The largest index each table answers (for searches by string).
local string_limits = { ['items.names'] = 0xFFFF, ['items.descriptions'] = 0xFFFF, ['keyitems.names'] = 4096,
    ['monsters.abilities'] = 4096, ['buffs.names'] = 1024, ['buffs.names_log'] = 1024, ['statusnames'] = 1024 };
-- Icons (Bitmap, ImageSize, ImageType, ImageName) are made when first read: addons ask for items far more often than
-- they draw them.
local function with_icon(t, bitmap)
    if (t == nil) then return nil; end
    return setmetatable(t, { __index = function (self, k)
        if (k == 'Bitmap' or k == 'ImageSize' or k == 'ImageType' or k == 'ImageName') then
            local b = bitmap();
            rawset(self, 'Bitmap', b);
            rawset(self, 'ImageSize', b and #b or 0);
            rawset(self, 'ImageType', 0); -- not decoded
            rawset(self, 'ImageName', '');
            return rawget(self, k);
        end
        return nil;
    end });
end
local function item_with_icon(it) return it and with_icon(it, function () return xi.item_bitmap(it.Id); end); end
local resource_manager;
resource_manager = interface('IResourceManager', {
    GetItemById = function (self, id) return item_with_icon(xi.item(id)); end,
    GetItemByName = function (self, name) return item_with_icon(xi.item_by_name(name)); end,
    GetSpellById = function (self, id) return xi.spell(id); end,
    GetSpellByName = function (self, name)
        name = tostring(name):lower();
        for id = 1, 1024 do local s = xi.spell(id); if (s and s.Name[1]:lower() == name) then return s; end end
        return nil;
    end,
    GetAbilityById = function (self, id) return xi.ability(id); end,
    GetAbilityByName = function (self, name)
        name = tostring(name):lower();
        for id = 1, 2048 do local a = xi.ability(id); if (a and a.Name[1]:lower() == name) then return a; end end
        return nil;
    end,
    GetAbilityByTimerId = function (self, id) return xi.ability_by_timer(id); end,
    -- IStatusIcon. INFERRED: CanCancel / HideTimer are not read from the game's data (0).
    GetStatusIconById = function (self, id)
        id = tonumber(id) or -1;
        if (id < 0) then return nil; end
        local s = xi.status(id);
        return with_icon({ Index = id, Id = id, CanCancel = 0, HideTimer = 0, Description = { [0] = s, s, s, s } },
            function () return xi.status_bitmap(id); end);
    end,
    GetStatusIconByIndex = function (self, i) return resource_manager.GetStatusIconById(self, i); end,
    -- GetString(table, index [, language]) -> string or nil; GetString(table, string [, language]) -> index or -1. The
    -- language (1 Japanese, 2 English) is ignored: only English is loaded.
    GetString = function (self, tbl, v)
        local name = tostring(tbl):lower();
        local f = string_tables[name];
        if (f == nil) then warn_once('GetString table ' .. tostring(tbl)); return (type(v) == 'string') and -1 or nil; end
        if (type(v) == 'string') then
            local want = v:lower();
            for i = 0, string_limits[name] or 1024 do local s = f(i); if (s ~= nil and s:lower() == want) then return i; end end
            return -1;
        end
        v = tonumber(v);
        if (v == nil or v < 0) then return nil; end
        return f(v);
    end,
    GetStringLength = function (self, tbl, i) local s = resource_manager.GetString(self, tbl, i); return (type(s) == 'string') and #s or 0; end,
    GetAbilityRange = function (self, id) local a = xi.ability(id); return a and a.Range or 0; end,
    GetSpellRange = function (self, id) local s = xi.spell(id); return s and s.Range or 0; end,
    GetFilePath = function () return nil; end,
    GetTexture = function () return nil; end,
});


M.state = state;
M.entity = entity;
M.memory_manager = memory_manager;
M.resource_manager = resource_manager;
