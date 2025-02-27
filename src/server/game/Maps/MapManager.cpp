/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "MapManager.h"
#include "Battleground.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Group.h"
#include "InstanceSaveMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldStateMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#ifdef ELUNA
#include "LuaEngine.h"
#endif
#include <boost/dynamic_bitset.hpp>
#include <numeric>

MapManager::MapManager()
    : _freeInstanceIds(std::make_unique<InstanceIds>()), _nextInstanceId(0), _scheduledScripts(0)
{
    i_gridCleanUpDelay = sWorld->getIntConfig(CONFIG_INTERVAL_GRIDCLEAN);
    i_timer.SetInterval(sWorld->getIntConfig(CONFIG_INTERVAL_MAPUPDATE));
}

MapManager::~MapManager() = default;

void MapManager::Initialize()
{
    Map::InitStateMachine();

    int num_threads(sWorld->getIntConfig(CONFIG_NUMTHREADS));

#if ELUNA
    if (num_threads > 1)
    {
        // Force 1 thread for Eluna as lua is single threaded. By default thread count is 1
        // This should allow us not to use mutex locks
        TC_LOG_ERROR("maps", "Map update threads set to %i, when Eluna only allows 1, changing to 1", num_threads);
        num_threads = 1;
    }
#endif
    // Start mtmaps if needed.
    if (num_threads > 0)
        m_updater.activate(num_threads);
}

void MapManager::InitializeVisibilityDistanceInfo()
{
    for (auto iter = i_maps.begin(); iter != i_maps.end(); ++iter)
        iter->second->InitVisibilityDistance();
}

MapManager* MapManager::instance()
{
    static MapManager instance;
    return &instance;
}

Map* MapManager::FindMap_i(uint32 mapId, uint32 instanceId) const
{
    return Trinity::Containers::MapGetValuePtr(i_maps, { mapId, instanceId });
}

Map* MapManager::CreateWorldMap(uint32 mapId, uint32 instanceId)
{
    Map* map = new Map(mapId, i_gridCleanUpDelay, instanceId, REGULAR_DIFFICULTY);
    map->LoadRespawnTimes();
    map->LoadCorpseData();

    if (sWorld->getBoolConfig(CONFIG_BASEMAP_LOAD_GRIDS))
        map->LoadAllCells();

    return map;
}

InstanceMap* MapManager::CreateInstance(uint32 mapId, uint32 instanceId, InstanceSave* save, Difficulty difficulty, TeamId team)
{
    // make sure we have a valid map id
    MapEntry const* entry = sMapStore.LookupEntry(mapId);
    if (!entry)
    {
        TC_LOG_ERROR("maps", "CreateInstance: no entry for map %d", mapId);
        ABORT();
    }

    // some instances only have one difficulty
    sDBCManager.GetDownscaledMapDifficultyData(mapId, difficulty);

    TC_LOG_DEBUG("maps", "MapInstanced::CreateInstance: %s map instance %d for %d created with difficulty %u", save ? "" : "new ", instanceId, mapId, static_cast<uint32>(difficulty));

    InstanceMap* map = new InstanceMap(mapId, i_gridCleanUpDelay, instanceId, difficulty, team);
    ASSERT(map->IsDungeon());

    map->LoadRespawnTimes();
    map->LoadCorpseData();

    bool load_data = save != nullptr;
    map->CreateInstanceData(load_data);

    if (sWorld->getBoolConfig(CONFIG_INSTANCEMAP_LOAD_GRIDS))
        map->LoadAllCells();

    return map;
}

BattlegroundMap* MapManager::CreateBattleground(uint32 mapId, uint32 instanceId, Battleground* bg)
{
    TC_LOG_DEBUG("maps", "MapInstanced::CreateBattleground: map bg %d for %d created.", instanceId, mapId);

    BattlegroundMap* map = new BattlegroundMap(mapId, i_gridCleanUpDelay, instanceId, REGULAR_DIFFICULTY);
    ASSERT(map->IsBattlegroundOrArena());
    map->SetBG(bg);
    bg->SetBgMap(map);
    return map;
}

/*
- return the right instance for the object, based on its InstanceId
- create the instance if it's not created already
- the player is not actually added to the instance (only in InstanceMap::Add)
*/
Map* MapManager::CreateMap(uint32 mapId, Player* player, uint32 loginInstanceId /*= 0*/)
{
    if (!player)
        return nullptr;

    MapEntry const* entry = sMapStore.LookupEntry(mapId);
    if (!entry)
        return nullptr;

    std::unique_lock<std::shared_mutex> lock(_mapsLock);

    Map* map = nullptr;
    uint32 newInstanceId = 0;                       // instanceId of the resulting map

    if (entry->IsBattlegroundOrArena())
    {
        // instantiate or find existing bg map for player
        // the instance id is set in battlegroundid
        newInstanceId = player->GetBattlegroundId();
        if (!newInstanceId)
            return nullptr;

        map = FindMap_i(mapId, newInstanceId);
        if (!map)
        {
            if (Battleground* bg = player->GetBattleground())
                map = CreateBattleground(mapId, newInstanceId, bg);
            else
            {
                player->TeleportToBGEntryPoint();
                return nullptr;
            }
        }
    }
    else if (entry->IsDungeon())
    {
        InstancePlayerBind* pBind = player->GetBoundInstance(mapId, player->GetDifficulty(entry->IsRaid()));
        InstanceSave* pSave = pBind ? pBind->save : nullptr;

        // priority:
        // 1. player's permanent bind
        // 2. player's current instance id if this is at login
        // 3. group's current bind
        // 4. player's current bind
        if (!pBind || !pBind->perm)
        {
            if (loginInstanceId) // if the player has a saved instance id on login, we either use this instance or relocate him out (return null)
            {
                map = FindMap_i(mapId, loginInstanceId);
                if (!map && pSave && pSave->GetInstanceId() == loginInstanceId)
                {
                    map = CreateInstance(mapId, loginInstanceId, pSave, pSave->GetDifficulty(), player->GetTeamId());
                    i_maps[{ map->GetId(), map->GetInstanceId() }] = map;
                }
                return map;
            }

            InstanceGroupBind* groupBind = nullptr;
            Group* group = player->GetGroup();
            // use the player's difficulty setting (it may not be the same as the group's)
            if (group)
            {
                groupBind = group->GetBoundInstance(entry);
                if (groupBind)
                {
                    // solo saves should be reset when entering a group's instance
                    player->UnbindInstance(mapId, player->GetDifficulty(entry->IsRaid()));
                    pSave = groupBind->save;
                }
            }
        }
        if (pSave)
        {
            // solo/perm/group
            newInstanceId = pSave->GetInstanceId();
            map = FindMap_i(mapId, newInstanceId);
            // it is possible that the save exists but the map doesn't
            if (!map)
                map = CreateInstance(mapId, newInstanceId, pSave, pSave->GetDifficulty(), player->GetTeamId());
        }
        else
        {
            Difficulty diff = player->GetGroup() ? player->GetGroup()->GetDifficulty(entry->IsRaid()) : player->GetDifficulty(entry->IsRaid());

            // if no instanceId via group members or instance saves is found
            // the instance will be created for the first time
            newInstanceId = GenerateInstanceId();

            //Seems it is now possible, but I do not know if it should be allowed
            //ASSERT(!FindInstanceMap(NewInstanceId));
            map = FindMap_i(mapId, newInstanceId);
            if (!map)
                map = CreateInstance(mapId, newInstanceId, nullptr, diff, player->GetTeamId());
        }
    }
    else
    {
        newInstanceId = 0;
        if (entry->IsSplitByFaction())
            newInstanceId = player->GetTeamId();

        map = FindMap_i(mapId, newInstanceId);
        if (!map)
            map = CreateWorldMap(mapId, newInstanceId);
    }

    if (map)
        i_maps[{ map->GetId(), map->GetInstanceId() }] = map;

    return map;
}

Map* MapManager::FindMap(uint32 mapId, uint32 instanceId) const
{
    std::shared_lock<std::shared_mutex> lock(_mapsLock);
    return FindMap_i(mapId, instanceId);
}

/*
Map::EnterState MapManager::PlayerCannotEnter(uint32 mapid, Player* player, bool loginCheck)
{
    MapEntry const* entry = sMapStore.LookupEntry(mapid);

    // No entry found in Map.dbc
    if (!entry)
        return Map::CANNOT_ENTER_NO_ENTRY;

    // Non-instanced maps can always be entered
    if (!entry->IsDungeon())
        return Map::CAN_ENTER;

    InstanceTemplate const* instance = sObjectMgr->GetInstanceTemplate(mapid);

    // Cannot enter instance maps when no instance_template entry is present
    if (!instance)
        return Map::CANNOT_ENTER_UNINSTANCED_DUNGEON;

    // Cannot enter instance if MapDifficulty data for highest available difficulty does not exist
    Difficulty targetDifficulty = player->GetDifficulty(entry->IsRaid());
    MapDifficulty const* mapDiff = sDBCManager.GetDownscaledMapDifficultyData(entry->ID, targetDifficulty);
    if (!mapDiff)
        return Map::CANNOT_ENTER_DIFFICULTY_UNAVAILABLE;

    // Game Masters can always enter
    if (player->IsGameMaster())
        return Map::CAN_ENTER;

    char const* mapName = entry->MapName;
    Group* group = player->GetGroup();

    // Cannot enter raid maps when not in a raid-group. Ignore rule when CONFIG_INSTANCE_IGNORE_RAID = true
    if (entry->IsRaid())
        if ((!group || !group->isRaidGroup()) && !sWorld->getBoolConfig(CONFIG_INSTANCE_IGNORE_RAID))
            return Map::CANNOT_ENTER_NOT_IN_RAID;

    // Cannot enter instance if player's corpse is in a different instance
    if (!player->IsAlive())
    {
        if (player->HasCorpse())
        {
            // let enter in ghost mode in instance that connected to inner instance with corpse
            uint32 corpseMap = player->GetCorpseLocation().GetMapId();
            do
            {
                if (corpseMap == mapid)
                    break;

                InstanceTemplate const* corpseInstance = sObjectMgr->GetInstanceTemplate(corpseMap);
                corpseMap = corpseInstance ? corpseInstance->Parent : 0;
            } while (corpseMap);

            if (!corpseMap)
                return Map::CANNOT_ENTER_CORPSE_IN_DIFFERENT_INSTANCE;

            TC_LOG_DEBUG("maps", "MAP: Player '%s' has corpse in instance '%s' and can enter.", player->GetName().c_str(), mapName);
        }
        else
            TC_LOG_DEBUG("maps", "Map::CanPlayerEnter - player '%s' is dead but does not have a corpse!", player->GetName().c_str());
    }

    // Get instance where player's group is bound & its map
    if (!loginCheck && group)
    {
        InstanceGroupBind* boundInstance = group->GetBoundInstance(entry);
        if (boundInstance && boundInstance->save)
            if (Map* boundMap = sMapMgr->FindMap(mapid, boundInstance->save->GetInstanceId()))
                if (Map::EnterState denyReason = boundMap->CannotEnter(player))
                    return denyReason;
    }

    // Players are only allowed to enter 5 instances per hour
    if (entry->IsDungeon() && (!player->GetGroup() || (player->GetGroup() && !player->GetGroup()->isLFGGroup())))
    {
        uint32 instanceIdToCheck = 0;
        if (InstanceSave* save = player->GetInstanceSave(mapid, entry->IsRaid()))
            instanceIdToCheck = save->GetInstanceId();

        // instanceId can never be 0 - will not be found
        if (!player->CheckInstanceCount(instanceIdToCheck) && !player->isDead())
            return Map::CANNOT_ENTER_TOO_MANY_INSTANCES;
    }

    // Other requirements
    if (player->Satisfy(sObjectMgr->GetAccessRequirement(mapid, targetDifficulty), mapid, true))
        return Map::CAN_ENTER;
    else
        return Map::CANNOT_ENTER_UNSPECIFIED_REASON;
}
*/
void MapManager::Update(uint32 diff)
{
    i_timer.Update(diff);
    if (!i_timer.Passed())
        return;

    MapMapType::iterator iter = i_maps.begin();
    while (iter != i_maps.end())
    {
        if (iter->second->CanUnload(diff))
        {
            if (DestroyMap(iter->second))
                iter = i_maps.erase(iter);
            else
                ++iter;

            continue;
        }

        if (m_updater.activated())
            m_updater.schedule_update(*iter->second, uint32(i_timer.GetCurrent()));
        else
            iter->second->Update(uint32(i_timer.GetCurrent()));

        ++iter;
    }
    if (m_updater.activated())
        m_updater.wait();

    for (iter = i_maps.begin(); iter != i_maps.end(); ++iter)
        iter->second->DelayedUpdate(uint32(i_timer.GetCurrent()));

    i_timer.SetCurrent(0);
}

bool MapManager::DestroyMap(Map* map)
{
    map->RemoveAllPlayers();
    if (map->HavePlayers())
        return false;

    map->UnloadAll();

    // Free up the instance id and allow it to be reused for bgs and arenas (other instances are handled in the InstanceSaveMgr)
    if (map->IsBattlegroundOrArena())
        sMapMgr->FreeInstanceId(map->GetInstanceId());

    // erase map
    delete map;
    return true;
}

bool MapManager::IsValidMAP(uint32 mapId)
{
    return sMapStore.LookupEntry(mapId) != nullptr;
}

void MapManager::UnloadAll()
{
    // first unload maps
    for (auto iter = i_maps.begin(); iter != i_maps.end(); ++iter)
        iter->second->UnloadAll();

    // then delete them
    for (auto iter = i_maps.begin(); iter != i_maps.end(); ++iter)
        delete iter->second;

    i_maps.clear();

    if (m_updater.activated())
        m_updater.deactivate();

    Map::DeleteStateMachine();
}

uint32 MapManager::GetNumInstances() const
{
    std::shared_lock<std::shared_mutex> lock(_mapsLock);
    return std::count_if(i_maps.begin(), i_maps.end(), [](MapMapType::value_type const& value) { return value.second->IsDungeon(); });
}

uint32 MapManager::GetNumPlayersInInstances() const
{
    std::shared_lock<std::shared_mutex> lock(_mapsLock);
    return std::accumulate(i_maps.begin(), i_maps.end(), 0u, [](uint32 total, MapMapType::value_type const& value) { return total + (value.second->IsDungeon() ? value.second->GetPlayers().getSize() : 0); });
}

void MapManager::InitInstanceIds()
{
    _nextInstanceId = 1;

    if (QueryResult result = CharacterDatabase.Query("SELECT IFNULL(MAX(id), 0) FROM instance"))
        _freeInstanceIds->resize((*result)[0].GetUInt64() + 2, true); // make space for one extra to be able to access [_nextInstanceId] index in case all slots are taken
    else
        _freeInstanceIds->resize(_nextInstanceId + 1, true);

    // never allow 0 id
    _freeInstanceIds->set(0, false);
}

void MapManager::RegisterInstanceId(uint32 instanceId)
{
    // Allocation and sizing was done in InitInstanceIds()
    _freeInstanceIds->set(instanceId, false);

    // Instances are pulled in ascending order from db and nextInstanceId is initialized with 1,
    // so if the instance id is used, increment until we find the first unused one for a potential new instance
    if (_nextInstanceId == instanceId)
        ++_nextInstanceId;
}

uint32 MapManager::GenerateInstanceId()
{
    if (_nextInstanceId == 0xFFFFFFFF)
    {
        TC_LOG_ERROR("maps", "Instance ID overflow!! Can't continue, shutting down server. ");
        World::StopNow(ERROR_EXIT_CODE);
        return _nextInstanceId;
    }

    uint32 newInstanceId = _nextInstanceId;
    ASSERT(newInstanceId < _freeInstanceIds->size());
    _freeInstanceIds->set(newInstanceId, false);

    // Find the lowest available id starting from the current NextInstanceId (which should be the lowest according to the logic in FreeInstanceId()
    size_t nextFreedId = _freeInstanceIds->find_next(_nextInstanceId++);
    if (nextFreedId == InstanceIds::npos)
    {
        _nextInstanceId = uint32(_freeInstanceIds->size());
        _freeInstanceIds->push_back(true);
    }
    else
        _nextInstanceId = uint32(nextFreedId);

    return newInstanceId;
}

void MapManager::FreeInstanceId(uint32 instanceId)
{
    // If freed instance id is lower than the next id available for new instances, use the freed one instead
    _nextInstanceId = std::min(instanceId, _nextInstanceId);
    _freeInstanceIds->set(instanceId, true);

#ifdef ELUNA
    sEluna->FreeInstanceId(instanceId);
#endif

}

// hack to allow conditions to access what faction owns the map (these worldstates should not be set on these maps)
class SplitByFactionMapScript : public WorldMapScript
{
public:
    SplitByFactionMapScript(char const* name, uint32 mapId) : WorldMapScript(name, mapId)
    {
    }

    void OnCreate(Map* map) override
    {
        sWorldStateMgr->SetValue(WS_TEAM_IN_INSTANCE_ALLIANCE, map->GetInstanceId() == TEAM_ALLIANCE, false, map);
        sWorldStateMgr->SetValue(WS_TEAM_IN_INSTANCE_HORDE, map->GetInstanceId() == TEAM_HORDE, false, map);
    }
};

void MapManager::AddSC_BuiltInScripts()
{
    for (MapEntry const* mapEntry : sMapStore)
        if (mapEntry->IsWorldMap() && mapEntry->IsSplitByFaction())
            new SplitByFactionMapScript(Trinity::StringFormat("world_map_set_faction_worldstates_%u", mapEntry->ID).c_str(), mapEntry->ID);
}
