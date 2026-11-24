#include "Core/Slippi/LocalTeamsCoordinator.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/OnScreenDisplay.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

LocalTeamsCoordinator::LocalTeamsCoordinator(uintptr_t device_, SlippiUser* user_, std::function<u16()> randomStage_)
    : randomStage(std::move(randomStage_)), device(device_), user(user_)
{
  IniFile ini;
  ini.Load(File::GetUserPath(D_CONFIG_IDX) + "local-teams.ini");
  auto section = ini.GetOrCreateSection("LocalTeams");
  section->Get("Enabled", &enabled, true);
  section->Get("Count", &state.count, 1u);
  section->Get("BasePort", &basePort, 49000u);
  section->Get("LoopbackTest", &loopback, false);
  section->Get("NativeCSS", &nativeCSS, true);
  section->Get("LobbyCode", &lobbyCode, std::string());
  for (unsigned i = 0; i < 4; ++i)
  {
    unsigned port = i + 1;
    section->Get("Controller" + std::to_string(i + 1), &port, port);
    state.ports[i] = port - 1;
    section->Get("Identity" + std::to_string(i + 1), &identityFiles[i], std::string());
  }
  if (enabled && (!state.Valid() || basePort < 1024 || basePort > 65532 ||
                  (loopback && state.count != 4)))
  {
    enabled = false;
    OSD::AddMessage("Local teams disabled: check local-teams.ini (1-4 players, distinct controller ports)", 15000, OSD::Color::RED);
  }
}

void LocalTeamsCoordinator::Fail(const std::string& message)
{
  error = message;
  state.phase = LocalTeams::Phase::Error;
  ERROR_LOG(SLIPPI_ONLINE, "[LocalTeams] %s", message.c_str());
  ShowStatus();
}

void LocalTeamsCoordinator::ShowStatus()
{
  std::ostringstream msg;
  if (state.phase == LocalTeams::Phase::Selecting && NativeCSS())
  {
    msg << "Local Teams: ";
    for (unsigned i = 0; i < state.count; ++i)
      msg << (i ? " | " : "") << "P" << i + 1 << ((state.confirmed & (1u << i)) ? " READY" : " pick + Start");
  }
  else if (state.phase == LocalTeams::Phase::Selecting)
    msg << "Local player " << state.active + 1 << "/" << state.count << " (controller "
        << state.ports[state.active] + 1 << "): pick, then press Start";
  else if (state.phase == LocalTeams::Phase::Error) msg << "Local teams: " << error << " | Z to reset";
  else if (state.phase == LocalTeams::Phase::EnteringCode) msg << "Local teams: enter the room code | Start to reopen, Z to reset";
  else if (state.phase == LocalTeams::Phase::Searching) msg << (loopback ? "Local teams: connecting localhost test" : "Local teams: searching for " + lobbyCode);
  else if (state.phase == LocalTeams::Phase::InGame) msg << "Local teams: in game";
  else msg << "Local teams: waiting for ready players";
  for (unsigned i = 0; i < state.count; ++i)
    if (state.confirmed & (1u << i)) msg << " | L" << i + 1 << " char " << unsigned(state.selections[i].character)
                                       << " team " << unsigned(state.selections[i].team) + 1;
  if (msg.str() != lastStatus)
  {
    lastStatus = msg.str();
    if (!NativeCSS() || state.phase == LocalTeams::Phase::Error)
      OSD::AddMessage(lastStatus, 30000, OSD::Color::CYAN);
    INFO_LOG(SLIPPI_ONLINE, "[LocalTeams] %s", lastStatus.c_str());
  }
}

std::array<u8, LocalTeams::StatusSize> LocalTeamsCoordinator::Poll(u8 mode, const u8* buttonBytes, const u8* reports, unsigned enteringPort, u32 frame)
{
  active = (mode & 0x1F) == SlippiMatchmaking::TEAMS;
  std::array<u8, LocalTeams::StatusSize> result{};
  if (!Active()) return result;
  if ((mode & 0x20) && NativeCSS())
  {
    // The same session-reset command binds an entering port or, with port 4,
    // clears cards on a real mode exit. Connection cancellation retains picks.
    if (enteringPort == 4) state.ClearCards();
    else if (!validated) state.BeginJoining(enteringPort);
  }
  const bool returnedFromGame = state.phase == LocalTeams::Phase::InGame;
  std::array<std::uint16_t, 4> buttons{};
  for (unsigned i = 0; i < 4; ++i) buttons[i] = (u16(buttonBytes[2 * i]) << 8) | buttonBytes[2 * i + 1];
  if ((mode & 0x40) && !(mode & 0x80))
  {
    // The code keyboard can release/press Start repeatedly. Those inputs must
    // never arm CSS confirmation or immediately reopen a cancelled keyboard.
    state.startArmed = false;
    state.nativeArmed = state.nativeHeld = 0;
  }
  else if (!(mode & 0xA0))
  {
    state.PollJoining(reports, frame, !validated);
    state.Poll(buttons);
    if (NativeCSS() && state.phase == LocalTeams::Phase::Selecting)
      for (unsigned i = 0; i < state.count; ++i)
        if (state.InputEnabled(i) && (buttons[state.ports[i]] & 0x200)) state.UnreadyPlayer(i);
    if (returnedFromGame) published = false;
  }
  result = {{1, u8(state.count), u8(state.active), u8(state.ports[state.active]), u8(state.confirmed),
             u8(state.phase), u8(state.startArmed), u8(state.clearToken), u8(loopback)}};
  result[9] = u8(NativeCSS()); result[10] = u8(state.nativeArmed); result[11] = u8(state.nativeHeld);
  for (unsigned i = 0; i < 4; ++i)
  {
    result[12 + i] = u8(state.ports[i]);
    result[16 + 4*i] = state.selections[i].character;
    result[17 + 4*i] = state.selections[i].costume;
    result[18 + 4*i] = state.selections[i].team;
    result[19 + 4*i] = u8((state.saved >> i) & 1);
  }
  result[32] = u8(state.JoinedMask());
  result[33] = u8(state.SuppressedMask());
  result[34] = u8(state.rosterCount); result[35] = u8(state.rosterAction);
  state.clearToken = false;
  ShowStatus();
  return result;
}

bool LocalTeamsCoordinator::Confirm(const u8* payload, u32 rng)
{
  if (!Active()) return false;
  LocalTeams::Selection selection;
  selection.team = payload[0]; selection.character = payload[1]; selection.costume = payload[2];
  selection.altStage = payload[8]; selection.rng = rng;
  if (payload[7] & 0x80)
  {
    if (!NativeCSS() || !state.ConfirmPlayer(payload[7] & 3, selection, payload[3] == 1)) return false;
  }
  else if (!state.Confirm(selection, payload[3] != 0)) return false;
  // One proposal per local group, independent of which teammate readies last.
  if (state.AllConfirmed()) RefreshStage();
  if (state.AllConfirmed() && !loopback && !validated) state.phase = LocalTeams::Phase::EnteringCode;
  ShowStatus();
  return true;
}

bool LocalTeamsCoordinator::ChangeCount(const u8* payload)
{
  // A connected rematch also uses Selecting: validated guards its live roster.
  const unsigned previous = state.count;
  if (!Active() || !NativeCSS()) return false;
  const unsigned action = state.rosterAction;
  if (action == 0x40 && payload[0] == 0 && state.phase != LocalTeams::Phase::InGame)
  {
    // Leaving the mode is allowed while searching/connected. Scene teardown
    // still owns cancelling all the local clients; roster changes stay gated.
    state.ClearCards(); state.count = 1; state.joined = 0; state.ports.fill(4);
    published = false;
    ShowStatus();
    return true;
  }
  if (validated || state.phase != LocalTeams::Phase::Selecting) return false;
  if (action && payload[0] != state.rosterCount) return false;
  if (!action && !state.Resize(payload[0])) return false;
  for (unsigned i = 0; i < previous; ++i)
  {
    if (state.joining && !state.Claimed(i)) continue;
    const u8* pick = payload + 1 + 4 * i;
    if (pick[3] && pick[0] < 26 && pick[2] <= 2)
    {
      state.selections[i].character = pick[0];
      state.selections[i].costume = pick[1];
      state.selections[i].team = pick[2];
      state.saved |= 1u << i;
    }
    else state.saved &= ~(1u << i);
  }
  if (action & 0x80)
  {
    if (!state.Resize(payload[0])) return false;
    state.ClaimAddedPort(action & 3);
  }
  else if (action & 0x40)
  {
    state.RemovePlayer(action & 3);
  }
  published = false;
  ShowStatus();
  return true;
}

void LocalTeamsCoordinator::RefreshStage()
{
  stageId = randomStage();
}

SlippiPlayerSelections LocalTeamsCoordinator::Selection(unsigned index) const
{
  const auto& value = state.selections[index];
  SlippiPlayerSelections result;
  result.characterId = value.character; result.characterColor = value.costume;
  result.teamId = value.team; result.alt_stage_mode = value.altStage;
  result.rngOffset = value.rng;
  result.isCharacterSelected = true;
  result.stageId = stageId; result.isStageSelected = true;
  return result;
}
SlippiPlayerSelections LocalTeamsCoordinator::PrimarySelection() const { return Selection(0); }

std::string LocalTeamsCoordinator::SearchCode() const
{
  // Match the stock connect-code UI's full-width Shift-JIS hashtag.
  return ReplaceAll(UTF8ToSHIFTJIS(lobbyCode), "#", std::string("\x81\x94", 2));
}

void LocalTeamsCoordinator::SetSearchCode(const std::string& shiftJisCode)
{
  // The native Slippi code-entry callback supplies the same Shift-JIS payload
  // that stock matchmaking uses, including its full-width hashtag.
  lobbyCode = SHIFTJISToUTF8(shiftJisCode);
}

namespace
{
std::string StripTrailingSeparators(std::string path)
{
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
  {
#ifdef _WIN32
    if (path.size() == 3 && path[1] == ':')
      break;
#endif
    path.pop_back();
  }
  return path;
}

std::string TrimWhitespaceAndBom(std::string text)
{
  auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  auto trimBoth = [&](std::string& s) {
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front())))
      s.erase(s.begin());
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back())))
      s.pop_back();
  };
  trimBoth(text);
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
  {
    text.erase(0, 3);
    trimBoth(text);
  }
  return text;
}

std::string ToLowerAscii(std::string text)
{
  for (char& ch : text)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

bool IsJsonFileName(const std::string& name)
{
  const std::string lower = ToLowerAscii(name);
  return lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".json") == 0;
}

std::string ParentDirectory(const std::string& path)
{
  const std::string clean = StripTrailingSeparators(path);
  const size_t slash = clean.find_last_of("/\\");
  if (slash == std::string::npos)
    return {};
  if (slash == 0)
    return clean.substr(0, 1);
#ifdef _WIN32
  if (slash == 2 && clean[1] == ':')
    return clean.substr(0, 3);
#endif
  return clean.substr(0, slash);
}

std::string IdentitySearchDirectory(const std::string& overrideDir)
{
  if (!overrideDir.empty())
  {
    const std::string clean = StripTrailingSeparators(overrideDir);
    return File::IsDirectory(clean) ? clean : std::string();
  }

  const std::string slippiDir = StripTrailingSeparators(File::GetSlippiUserConfigFolder());
  if (slippiDir.empty())
    return {};

  const std::string localUserJson = slippiDir + DIR_SEP "user.json";
  if (File::Exists(localUserJson) && !File::IsDirectory(localUserJson))
    return slippiDir;

  const std::string externalFile = slippiDir + DIR_SEP "external-user-json.txt";
  if (File::Exists(externalFile))
  {
    std::string target;
    if (!File::ReadFileToString(externalFile, target))
      return {};
    target = TrimWhitespaceAndBom(std::move(target));
    if (target.empty())
      return {};
    const std::string parent = ParentDirectory(target);
    return (!parent.empty() && File::IsDirectory(parent)) ? parent : std::string();
  }

  return File::IsDirectory(slippiDir) ? slippiDir : std::string();
}

bool ReadIdentityString(const json& value, const char* primaryKey, const char* aliasKey, std::string& dst)
{
  auto tryKey = [&](const char* key) {
    if (!key)
      return false;
    auto it = value.find(key);
    if (it == value.end() || !it->is_string())
      return false;
    dst = it->get<std::string>();
    return true;
  };
  return tryKey(primaryKey) || tryKey(aliasKey);
}

enum class IdentityParseResult
{
  Ok,
  OpenFailed,
  Invalid,
};

IdentityParseResult ParseIdentityFile(const std::string& path, const SlippiUser::UserInfo& fallback,
                                        SlippiUser::UserInfo& out)
{
  if (File::IsDirectory(path))
    return IdentityParseResult::OpenFailed;

  std::ifstream input(path);
  if (!input)
    return IdentityParseResult::OpenFailed;

  json value = json::parse(input, nullptr, false);
  if (value.is_discarded() || !value.is_object())
    return IdentityParseResult::Invalid;

  auto identity = fallback;
  if (!ReadIdentityString(value, "uid", nullptr, identity.uid) ||
      !ReadIdentityString(value, "playKey", "play_key", identity.playKey) ||
      !ReadIdentityString(value, "displayName", "display_name", identity.displayName) ||
      !ReadIdentityString(value, "connectCode", "connect_code", identity.connectCode))
    return IdentityParseResult::Invalid;

  if (identity.uid.empty() || identity.playKey.empty())
    return IdentityParseResult::Invalid;

  out = std::move(identity);
  return IdentityParseResult::Ok;
}
}  // namespace

bool LocalTeamsCoordinator::Identity(unsigned index, const SlippiUser::UserInfo& fallback,
                                     SlippiUser::UserInfo& out, std::string& errorOut)
{
  if (identityFiles[index].empty())
  {
    out = fallback;
    return true;
  }
  const IdentityParseResult result = ParseIdentityFile(identityFiles[index], fallback, out);
  if (result == IdentityParseResult::OpenFailed)
  {
    errorOut = "Could not open configured identity " + std::to_string(index + 1);
    return false;
  }
  if (result == IdentityParseResult::Invalid)
  {
    errorOut = "Invalid configured identity " + std::to_string(index + 1);
    return false;
  }
  return true;
}

bool LocalTeamsCoordinator::ResolveIdentities(const SlippiUser::UserInfo& fallback,
                                              std::array<SlippiUser::UserInfo, 4>& out,
                                              std::string& errorOut, const std::string& discoveryDir)
{
  const unsigned count = std::min<unsigned>(state.count, 4);
  if (count == 0)
    return true;

  std::array<bool, 4> assigned{};
  std::set<std::string> usedUids;

  // Pass 1: Explicit Identity1..Identity<count> take precedence; unreadable/invalid paths fall back below.
  for (unsigned i = 0; i < count; ++i)
  {
    if (identityFiles[i].empty())
      continue;
    std::string slotError;
    if (!Identity(i, fallback, out[i], slotError))
    {
      WARN_LOG(SLIPPI_ONLINE, "[LocalTeams] %s (%s); falling back to autodiscovery/primary user", slotError.c_str(),
               identityFiles[i].c_str());
      continue;
    }
    assigned[i] = true;
    usedUids.insert(out[i].uid);
  }

  // Pass 2: Player 1 defaults to fallback if Identity1 is omitted and fallback UID was not claimed in Pass 1.
  if (!assigned[0] && identityFiles[0].empty() && !fallback.uid.empty() && !fallback.playKey.empty() &&
      usedUids.count(fallback.uid) == 0)
  {
    out[0] = fallback;
    assigned[0] = true;
    usedUids.insert(fallback.uid);
  }

  // Pass 3: Autodiscover .json files placed next to user.json for remaining unassigned slots.
  bool needDiscovery = false;
  for (unsigned i = 0; i < count; ++i)
  {
    if (!assigned[i])
    {
      needDiscovery = true;
      break;
    }
  }

  if (needDiscovery)
  {
    const std::string dir = IdentitySearchDirectory(discoveryDir);
    if (!dir.empty())
    {
      File::FSTEntry tree = File::ScanDirectoryTree(dir, false);
      struct DiscoveredFile
      {
        std::string lowerName;
        std::string virtualName;
        std::string physicalName;
      };
      std::vector<DiscoveredFile> candidates;
      for (const auto& entry : tree.children)
      {
        if (entry.isDirectory || !IsJsonFileName(entry.virtualName))
          continue;
        std::string lower = ToLowerAscii(entry.virtualName);
        if (lower == "user.json")
          continue;
        candidates.push_back({std::move(lower), entry.virtualName, entry.physicalName});
      }
      std::sort(candidates.begin(), candidates.end(), [](const DiscoveredFile& a, const DiscoveredFile& b) {
        if (a.lowerName != b.lowerName)
          return a.lowerName < b.lowerName;
        if (a.virtualName != b.virtualName)
          return a.virtualName < b.virtualName;
        return a.physicalName < b.physicalName;
      });

      unsigned nextSlot = 0;
      for (const auto& candidate : candidates)
      {
        while (nextSlot < count && assigned[nextSlot])
          ++nextSlot;
        if (nextSlot >= count)
          break;

        SlippiUser::UserInfo parsed;
        if (ParseIdentityFile(candidate.physicalName, fallback, parsed) != IdentityParseResult::Ok)
          continue;
        if (usedUids.count(parsed.uid) != 0)
          continue;

        out[nextSlot] = std::move(parsed);
        assigned[nextSlot] = true;
        usedUids.insert(out[nextSlot].uid);
        INFO_LOG(SLIPPI_ONLINE, "[LocalTeams] Autodiscovered identity %s for player %u (%s)",
                 candidate.virtualName.c_str(), nextSlot + 1, out[nextSlot].uid.c_str());
      }
    }
  }

  // Pass 4: Fallback to primary user for any remaining slots and verify required auth fields.
  for (unsigned i = 0; i < count; ++i)
  {
    if (!assigned[i])
      out[i] = fallback;
    if (out[i].uid.empty() || out[i].playKey.empty())
    {
      errorOut = "Log in before searching";
      return false;
    }
  }

  return true;
}

bool LocalTeamsCoordinator::BeginSearch(std::unique_ptr<SlippiMatchmaking>& primary)
{
  if (!Active() || !state.AllConfirmed()) return false;
  RequestStop();
  for (auto& guest : guests)
  {
    guest.matchmaking.reset();
    guest.netplay.reset();
    guest.previous.fill(0);
  }
  if (loopback)
  {
    validated = published = false;
    primary = std::make_unique<SlippiMatchmaking>(device, user);
    primary->SetupLocalTeamsLoopback(0, u16(basePort));
    for (unsigned i = 1; i < 4; ++i)
    {
      guests[i - 1].matchmaking = std::make_unique<SlippiMatchmaking>(device, user);
      guests[i - 1].matchmaking->SetupLocalTeamsLoopback(u8(i), u16(basePort));
    }
    return true;
  }
  if (SearchCode().empty() || SearchCode().size() > 18)
  { Fail("Enter a room code of at most 18 Shift-JIS bytes"); return false; }
  const auto fallback = user->GetUserInfo();
  // Read every identity before starting any thread. Never log play keys.
  std::array<SlippiUser::UserInfo, 4> identities;
  std::string identityError;
  if (!ResolveIdentities(fallback, identities, identityError))
  {
    Fail(identityError);
    return false;
  }
  validated = published = false;
  state.phase = LocalTeams::Phase::Searching;
  state.startArmed = false;
  state.nativeArmed = state.nativeHeld = 0;
  ShowStatus();
  SlippiMatchmaking::MatchSearchSettings search;
  search.mode = SlippiMatchmaking::TEAMS;
  search.connectCode = SearchCode();
  primary = std::make_unique<SlippiMatchmaking>(device, user, u16(basePort), &identities[0]);
  for (unsigned i = 1; i < state.count; ++i)
    guests[i - 1].matchmaking = std::make_unique<SlippiMatchmaking>(device, user, u16(basePort + i), &identities[i]);
  primary->FindMatch(search);
  for (unsigned i = 1; i < state.count; ++i) guests[i - 1].matchmaking->FindMatch(search);
  return true;
}

SlippiMatchmaking::ProcessState LocalTeamsCoordinator::Tick(SlippiMatchmaking& primary, SlippiNetplayClient* netplay)
{
  using MM = SlippiMatchmaking;
  if (!Active()) return primary.GetMatchmakeState();
  if (state.phase == LocalTeams::Phase::Error) return MM::ERROR_ENCOUNTERED;
  if (primary.GetMatchmakeState() == MM::IDLE) return MM::IDLE;
  std::array<SlippiMatchmaking*, 4> clients{{&primary, nullptr, nullptr, nullptr}};
  for (unsigned i = 1; i < state.count; ++i) clients[i] = guests[i - 1].matchmaking.get();
  bool complete = true;
  auto progress = MM::CONNECTION_SUCCESS;
  for (unsigned i = 0; i < state.count; ++i)
  {
    if (!clients[i]) { Fail("A local session is missing; reset with Z"); return MM::ERROR_ENCOUNTERED; }
    const auto status = clients[i]->GetMatchmakeState();
    if (status == MM::ERROR_ENCOUNTERED)
    { Fail("Player " + std::to_string(i + 1) + ": " + clients[i]->GetErrorMessage()); return MM::ERROR_ENCOUNTERED; }
    complete &= status == MM::CONNECTION_SUCCESS;
    if (status < progress) progress = status;
  }
  if (!complete) return progress; // preserve stock Searching -> Connecting UI states
  if (!validated)
  {
    const auto reference = primary.GetMatchmakeResult();
    unsigned assigned = 0;
    for (unsigned i = 0; i < state.count; ++i)
    {
      const auto result = clients[i]->GetMatchmakeResult();
      const int slot = clients[i]->LocalPlayerIndex();
      if (slot < 0 || slot > 3 || (assigned & (1u << slot)) || result.id != reference.id || result.players.size() != 4)
      { Fail("Sessions received different lobbies or duplicate slots"); return MM::ERROR_ENCOUNTERED; }
      assigned |= 1u << slot;
      localSlots[i] = unsigned(slot);
      std::array<std::string, 4> a{}, b{};
      for (const auto& player : reference.players)
      {
        if (player.port < 1 || player.port > 4) { Fail("Invalid primary roster"); return MM::ERROR_ENCOUNTERED; }
        a[player.port - 1] = player.uid + ":" + player.connectCode;
      }
      for (const auto& player : result.players)
      {
        if (player.port < 1 || player.port > 4) { Fail("Invalid roster"); return MM::ERROR_ENCOUNTERED; }
        b[player.port - 1] = player.uid + ":" + player.connectCode;
      }
      if (!LocalTeams::CompatibleRoster(a, b)) { Fail("Local sessions disagree about the roster"); return MM::ERROR_ENCOUNTERED; }
      INFO_LOG(SLIPPI_ONLINE, "[LocalTeams] local %u controller %u -> Melee slot %d", i + 1, state.ports[i] + 1, slot + 1);
    }
    for (unsigned i = 1; i < state.count; ++i) guests[i - 1].netplay = clients[i]->GetNetplayClient();
    validated = true;
    state.phase = LocalTeams::Phase::Waiting;
    ShowStatus();
  }
  for (unsigned i = 0; i < state.count; ++i)
  {
    auto* client = i ? guests[i - 1].netplay.get() : netplay;
    if (!client && i == 0) continue; // Primary ownership transfers just after initial Tick.
    if (!client) { Fail("Local P" + std::to_string(i + 1) + " session is missing"); return MM::ERROR_ENCOUNTERED; }
    const auto activePlayers = client->GetActivePlayerIndices();
    if (activePlayers.size() == 3 && client->GetSlippiConnectStatus() == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_CONNECTED) continue;
    for (unsigned slot = 0; slot < 4; ++slot)
    {
      if (slot == localSlots[i] || activePlayers.count(u8(slot))) continue;
      const auto label = primary.GetPlayerName(u8(slot));
      bool isLocal = false;
      for (unsigned local = 0; local < state.count; ++local) isLocal |= localSlots[local] == slot;
      const auto detail = "Connection to " + (label.empty() ? "player " + std::to_string(slot + 1) : label) + " ended";
      WARN_LOG(SLIPPI_ONLINE, "[LocalTeams] local %u observed slot %u missing; status %u, reason %u",
               i + 1, slot + 1, unsigned(client->GetSlippiConnectStatus()), unsigned(client->GetDisconnectReason()));
      if (isLocal) { Fail(detail + " on Local P" + std::to_string(i + 1)); return MM::ERROR_ENCOUNTERED; }
      // Stock Teams closes the lobby after a participant leaves, including
      // after a game finishes with someone disconnected. This is not a local fault.
      lobbyDisconnected = true;
      OSD::AddMessage(detail + "; lobby closed", 8000, OSD::Color::YELLOW);
      return MM::IDLE;
    }
    Fail("Local P" + std::to_string(i + 1) + " network session ended");
    return MM::ERROR_ENCOUNTERED;
  }
  if (netplay && !published && state.AllConfirmed()) PublishSelections(*netplay);
  return MM::CONNECTION_SUCCESS;
}

void LocalTeamsCoordinator::PublishSelections(SlippiNetplayClient& primary)
{
  if (!validated || published || !state.AllConfirmed()) return;
  auto selection = Selection(0);
  primary.SetMatchSelections(selection);
  for (unsigned i = 1; i < state.count; ++i)
  { selection = Selection(i); guests[i - 1].netplay->SetMatchSelections(selection); }
  published = true;
  state.phase = LocalTeams::Phase::Waiting;
}

bool LocalTeamsCoordinator::AllReady(SlippiNetplayClient& primary)
{
  if (!published || !state.AllConfirmed()) return false;
  for (unsigned i = 0; i < state.count; ++i)
  {
    auto* client = i == 0 ? &primary : guests[i - 1].netplay.get();
    if (!client || client->GetActivePlayerIndices().size() != 3) return false;
    auto info = client->GetMatchInfoSnapshot();
    if (!info.localPlayerSelections.isCharacterSelected) return false;
    for (const auto& selection : info.remotePlayerSelections)
      if (!selection.isCharacterSelected) return false;
  }
  return true;
}

void LocalTeamsCoordinator::StartGame()
{
  state.phase = LocalTeams::Phase::InGame;
  state.lastSentFrame = 0;
  for (unsigned i = 1; i < state.count; ++i)
  { guests[i - 1].previous.fill(0); guests[i - 1].netplay->StartSlippiGame(); }
}

SlippiNetplayClient* LocalTeamsCoordinator::GuestNetplay(unsigned index) const
{
  return (index >= 1 && index < state.count) ? guests[index - 1].netplay.get() : nullptr;
}

void LocalTeamsCoordinator::FillRumblePorts(std::array<u8, 4>& slotToPortPlusOne) const
{
  slotToPortPlusOne.fill(0);
  if (!Active() || !validated)
    return;
  for (unsigned i = 0; i < state.count; ++i)
  {
    if (localSlots[i] < 4 && state.ports[i] < 4)
      slotToPortPlusOne[localSlots[i]] = static_cast<u8>(state.ports[i] + 1);
  }
}

void LocalTeamsCoordinator::SendInputs(s32 frame, u8 delay, s32 finalized, u32 checksum, const u8* reports, bool skip,
                                       std::array<LocalTeams::Pad, 3>* sentPads, bool* wasSkipped)
{
  if (!Active() || state.phase != LocalTeams::Phase::InGame)
  {
    if (wasSkipped) *wasSkipped = true;
    return;
  }
  if (!skip && !state.AcceptFrame(frame)) skip = true;
  if (wasSkipped) *wasSkipped = skip;
  for (unsigned i = 1; i < state.count; ++i)
  {
    auto& guest = guests[i - 1];
    guest.netplay->DropOldRemoteInputs(finalized);
    LocalTeams::Pad pad{};
    std::copy_n(reports + 12 * state.ports[i], 12, pad.begin());
    if (skip)
    {
      if (sentPads)
      {
        LocalTeams::Pad prevCopy = guest.previous;
        (*sentPads)[i - 1] = LocalTeams::NormalizePad(pad, prevCopy);
      }
      guest.netplay->SendSlippiPad(nullptr);
      continue;
    }
    if (frame == 1)
      for (s32 initial = 1; initial <= delay; ++initial) guest.netplay->SendSlippiPad(std::make_unique<SlippiPad>(initial));
    pad = LocalTeams::NormalizePad(pad, guest.previous);
    if (sentPads) (*sentPads)[i - 1] = pad;
    guest.netplay->SendSlippiPad(std::make_unique<SlippiPad>(frame + delay, finalized, checksum, pad.data()));
  }
}

void LocalTeamsCoordinator::Cleanup()
{
  RequestStop();
  // Join all workers before the EXI/Rust user owner is destroyed or ports reused.
  for (auto& guest : guests) { guest.matchmaking.reset(); guest.netplay.reset(); guest.previous.fill(0); }
  state.ResetSelection();
  validated = published = false;
  lobbyDisconnected = false;
  localSlots.fill(4);
  error.clear(); lastStatus.clear();
}

void LocalTeamsCoordinator::RequestStop()
{
  for (auto& guest : guests)
  {
    if (guest.matchmaking) guest.matchmaking->RequestStop();
    if (guest.netplay) guest.netplay->RequestStop();
  }
}
