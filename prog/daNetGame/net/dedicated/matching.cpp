// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include "matching.h"
#include "matching_state_data.h"
#include <debug/dag_debug.h>

#include <startup/dag_globalSettings.h> // argv
#include "net/userid.h"
#include "main/appProfile.h"
#include "main/main.h"
#include <util/dag_base64.h>
#include <ecs/core/entityManager.h>

namespace dedicated_matching
{
using namespace dedicated_matching::state_data;

/*
Traffic encryption and client authentification

Backend service send hmac_id-sha256 (authKey) and encryption_key (encKey) to client, which are generated using sessionId,
userId, and room secret.
Dedicated game server (host) process should be started with -session_id:<session_id>
and -room_secret:<room_secret> commandline arguments.
Client send hmac_id and userid on connect (with launch_network_session({authKey, encKey, connect=List<string(ip:port)>})
from quirrel, or same bindings from daslang.
Host will accept connection only if everything is correct.

Room info on backend sample:
{
  "mode_info": {
    "gameName":"myGame", //needed for multgames on one matching instance, used for tests
  }
  "roomId":"uint64", //as STRING!
  "sessionId":"uint64", //as string!
  "public": {
    "groupSize": 0//max group size for autogroups
  },
  "members":[
    {
      "userId":"unsigned int63 (only postivies uint64)",
      "name":"any User Name",
      "public":{
        "mteam":0, //any integer. Mteam is matchmaking team, set by matchmaking. Mteam id that was set by match-matching
        "team":0, // any integer. Team id that was set by matching upon user request (like in lobby)
        "squadId":0, //squadId, to find groups(parties) of players
        "appId":0
      },
      "private": { //will be send only for user himself
        "authKey":<hmac_id>,
        "encKey":<encryption_key>,
      }
    }
  ]
}
Most of mulitplayer games use concept of 'team', 'group'.
Some games can have multiple sessions per room (lobby style).
Players usually have at least names and some unique id (for performance reason we prefer int64,
but probably that can be changed in future).
Game modes can have other options, like session duration, score limit, team sizes, and so on.
Such settings can be set durion session or in backend (lobby, matchmaking, etc).
In DNG eash game session has separate dedicated game server process.

Commandline arguments:

  dedicated:

    "-room_secret:<room_secret>" -- required for traffic encryption
    "-session_id:<session_id>" -- required for traffic encryption
    "-nomatching" - disable Gaijin mathcing
    "-nonetenc" - switch off traffic encryption
    "-nopeerauth" - swithc off client authentification and traffic enryption
    "-customPublicMatchingCfg:" - undocumented

  client (debug options):

    -force_player_group:int - debug option to for client squad(group)
*/

static void apply_custom_matching_public_config(const char *encoded_config)
{
  debug("applying custom matching public config");

  String config;
  Base64(encoded_config).decode(config);

  Json::Reader().parse(config.begin(), config.end(), room_info["public"], false);
}

static void merge_json(Json::Value const &what, Json::Value &where)
{
  for (const eastl::string &key : what.getMemberNames())
  {
    const Json::Value &v = what.get(key, Json::Value());
    if (v.isNull())
      where.removeMember(key.c_str());
    else
      where[key] = v;
  }
}

static Json::Value &get_room_member(matching::UserId user_id)
{
  static Json::Value null;
  auto it = room_members.find(user_id);
  if (it != room_members.end())
    return it->second;
  return null;
}

static void register_room_member(Json::Value const &member_info)
{
  matching::UserId userId = member_info["userId"].asUInt64();
  room_members[userId] = member_info;
  g_entity_mgr->broadcastEventImmediate(NetMatchingEventOnRegisterRoomMember{userId});
}

static eastl::string find_room_secret()
{
  return !room_info.isNull() ? room_info["private"]["room_secret"].asString() : eastl::string();
}

void apply_room_info_on_join(const Json::Value &params)
{
  current_room_id = params["roomId"].asUInt64();
  room_info = params;

  group_size = room_info["public"].get("groupSize", 0).asInt();

  room_secret = find_room_secret();
  debug("joined to matching room %lx with key '%s'", current_room_id, room_secret.c_str());
  const Json::Value &members = params["members"];
  for (Json::ArrayIndex i = 0; i < members.size(); ++i)
  {
    matching::UserId uid = members[i]["userId"].asUInt64();
    if (uid != net::get_user_id())
      register_room_member(members[i]);
  }
}
void apply_room_invite(Json::Value const &params, Json::Value &resp)
{
  if (current_room_id != matching::INVALID_ROOM_ID)
  {
    resp["accept"] = false;
    resp["reason"] = "already invited";
    return;
  }

  const Json::Value &inviteData = params["invite_data"];
  const Json::Value &modeInfo = inviteData["mode_info"];

  const char *requestedGame = modeInfo["gameName"].asCString();
  if (strcmp(requestedGame, get_game_name()) != 0)
  {
    debug("Room invite rejected due to game name mismatch. Current game is '%s'. Requested "
          "game is '%s'",
      get_game_name(), requestedGame);
    resp["accept"] = false;
    resp["reason"] = "game mismatch";
    return;
  }

  resp["accept"] = true;

  current_room_id = params["roomId"].asUInt64();
  g_entity_mgr->broadcastEventImmediate(NetMatchingEventOnJoinRoom{current_room_id});
}

void apply_room_member_leaved(const Json::Value &params)
{
  matching::UserId userId = params["userId"].asUInt64();
  if (userId == net::get_user_id())
    return;

  auto it = room_members.find(userId);
  if (it != room_members.end())
    debug("player %s[%ld] leaved from game room", params["name"].asCString(), userId);
}

void apply_room_member_joined(const Json::Value &params)
{
  matching::UserId userId = params["userId"].asUInt64();
  if (userId == net::get_user_id())
    return;
  register_room_member(params);
  debug("player %s[%ld] joined game room", params["name"].asCString(), userId);
}

void apply_room_member_attr_changed(const Json::Value &params)
{
  matching::UserId userId = params["userId"].asUInt64();
  Json::Value &member = get_room_member(userId);
  if (member.isNull())
    return;

  const Json::Value &pub = params["public"];
  const Json::Value &priv = params["private"];
  if (pub.isObject())
    merge_json(pub, member["public"]);
  if (priv.isObject())
    merge_json(priv, member["private"]);
}

void apply_room_destroyed(const Json::Value &) {}

void apply_room_member_kicked(const Json::Value &params)
{
  matching::UserId userId = params["userId"].asUInt64();
  debug("player [%ld] kicked by matching server", userId);
}

void apply_room_attr_changed(const Json::Value &params)
{
  const Json::Value &pub = params["public"];
  const Json::Value &priv = params["private"];

  if (pub.isObject())
    merge_json(pub, room_info["public"]);

  if (priv.isObject())
    merge_json(priv, room_info["private"]);
}

void init()
{
  if (dgs_get_argv("nomatching") || app_profile::get().disableRemoteNetServices)
  {
    debug("matching client disabled");
    if (const char *encodedConfig = dgs_get_argv("customPublicMatchingCfg"))
      apply_custom_matching_public_config(encodedConfig);
    return;
  }
  g_entity_mgr->broadcastEventImmediate(NetMatchingEventOnInit{});
}

void update() { g_entity_mgr->broadcastEventImmediate(NetMatchingEventOnUpdate{}); }

void shutdown() { g_entity_mgr->broadcastEventImmediate(NetMatchingEventOnTerm{}); }

void on_level_loaded() { g_entity_mgr->broadcastEventImmediate(NetMatchingEventOnLevelLoaded{}); }

void player_kick_from_room(matching::UserId user_id) { g_entity_mgr->broadcastEventImmediate(NetMatchingKickPlayerEvent{user_id}); }

void ban_player_in_room(matching::UserId user_id) { g_entity_mgr->broadcastEventImmediate(NetMatchingBanPlayerEvent{user_id}); }

void on_player_team_changed(matching::UserId user_id, int team)
{
  g_entity_mgr->broadcastEventImmediate(NetMatchingChangeTeamEvent{user_id, team});
}

int get_player_team(matching::UserId uid)
{
  const Json::Value &member = get_room_member(uid);
  if (member.isNull())
    return -1;
  const Json::Value &mteam = member["public"]["mteam"]; // team set by matchmaking
  if (mteam.isIntegral())
    return mteam.asInt();

  const Json::Value &team = member["public"]["team"]; // team set by user (in Lobby for example)
  if (team.isIntegral())
    return team.asInt();
  return -1;
}

int get_player_req_teams_num(matching::UserId uid)
{
  const Json::Value &member = get_room_member(uid);
  if (member.isNull())
    return -1;
  const Json::Value &reqTeamsNum = member["public"]["reqTeamsNum"];
  if (reqTeamsNum.isIntegral())
    return reqTeamsNum.asInt();

  return -1;
}

int get_player_app_id(matching::UserId uid)
{
  const Json::Value &member = get_room_member(uid);
  if (member.isNull())
    return app_profile::get().appId;
  const Json::Value &appId = member["public"]["appId"];
  if (appId.isIntegral())
    return appId.asInt();
  return app_profile::get().appId;
}

eastl::string get_player_name(matching::UserId uid)
{
  const Json::Value &member = get_room_member(uid);
  if (member.isNull())
    return "";
  return member["name"].asString();
}

int64_t get_player_group(matching::UserId uid)
{
  if (const char *player_group = dgs_get_argv("force_player_group"))
    if (*player_group)
      return atoi(player_group);

  if (group_size == 1) // solo game mode ignores squads
    return uid;

  const Json::Value &member = get_room_member(uid);
  if (member.isNull())
    return uid;
  const Json::Value &squadId = member["public"]["squadId"];
  if (!squadId.isIntegral())
    return uid;
  return squadId.asUInt64();
}

const Json::Value &get_mode_info() { return room_info["public"]; }

const eastl::string &get_room_secret()
{
  if (DAGOR_UNLIKELY(room_secret.empty()))
    if (const char *rsec = dgs_get_argv("room_secret"); rsec && *rsec)
      room_secret = rsec;
  return room_secret;
}

int get_room_members_count() { return room_members.size(); }

const char *get_player_custom_info(matching::UserId uid)
{
  const Json::Value &member = get_room_member(uid);
  if (member.isNull())
    return "";
  return member["public"]["custom"].asCString();
}

} // namespace dedicated_matching

#define NET_MATCHING_ECS_EVENT ECS_REGISTER_EVENT
NET_MATCHING_ECS_EVENTS
#undef NET_MATCHING_ECS_EVENT

Json::Value dedicated_matching::state_data::room_info;
matching::RoomId dedicated_matching::state_data::current_room_id = matching::INVALID_ROOM_ID;
eastl::string dedicated_matching::state_data::room_secret;
eastl::unordered_map<matching::UserId, Json::Value> dedicated_matching::state_data::room_members;
int dedicated_matching::state_data::group_size = 0; // 0 = unlimited
bool (*dedicated_matching::state_data::generate_peer_auth)(matching::UserId, void const *, size_t, matching::AuthKey &) = nullptr;
